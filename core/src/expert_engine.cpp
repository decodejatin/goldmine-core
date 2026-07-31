// =============================================================================
// expert_engine.cpp — Goldmine PAXG/USDT Production Paper Trader v6
//
// PRODUCTION-GRADE FEATURES (PHASE 1):
//   - CostModel: dynamic slippage and exact hurdle rates.
//   - AccountState: Kelly-inspired sizing and strict circuit breaker.
//   - Zero-allocation C++20 hot path.
// =============================================================================

#include "goldmine/pipeline.hpp"
#include "goldmine/live_ingestion.hpp"
#include "goldmine/cost_model.hpp"
#include "goldmine/account_state.hpp"
#include "goldmine/config_loader.hpp"
#include "goldmine/trade_store.hpp"
#include "goldmine/param_shm.hpp"
#include "goldmine/cpu_affinity.hpp"
#include "goldmine/replay_engine.hpp"
#include "goldmine/security_guard.hpp"
#include "goldmine/execution/paper_connector.hpp"
#include "goldmine/execution/binance_connector.hpp"
#include "goldmine/execution/order_manager.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <sstream>
#include <thread>
#include <atomic>
#include <cmath>
#include <deque>
#include <ctime>

using namespace goldmine;

// ===========================================================================
// CLI
// ===========================================================================
std::atomic<bool>   cli_force_close{false};
std::atomic<bool>   cli_force_buy{false};
std::atomic<bool>   cli_force_sell{false};
std::atomic<double> cli_override_sl{0.0};
std::atomic<double> cli_override_tp{0.0};
std::atomic<double> cli_limit_buy{0.0};
std::atomic<double> cli_limit_sell{0.0};
std::atomic<double> cli_max_risk{0.02};  // 2% base risk

void cli_thread_fn() {
    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;
        if (cmd == "close") { cli_force_close = true; std::cout << "[CLI] Close.\n"; }
        else if (cmd == "buy")  { cli_force_buy = true;  std::cout << "[CLI] BUY.\n"; }
        else if (cmd == "sell") { cli_force_sell = true;  std::cout << "[CLI] SELL.\n"; }
        else if (cmd == "limit") {
            std::string dir; double price; iss >> dir >> price;
            if (dir == "buy")  { cli_limit_buy = price;  std::cout << "[CLI] Limit BUY at " << price << "\n"; }
            if (dir == "sell") { cli_limit_sell = price; std::cout << "[CLI] Limit SELL at " << price << "\n"; }
        }
        else if (cmd == "sl") { double v; iss >> v; cli_override_sl = v; std::cout << "[CLI] SL=" << v << "\n"; }
        else if (cmd == "tp") { double v; iss >> v; cli_override_tp = v; std::cout << "[CLI] TP=" << v << "\n"; }
        else if (cmd == "risk") { double v; iss >> v; cli_max_risk = v; std::cout << "[CLI] Risk=" << v << "\n"; }
        else if (cmd == "help") {
            std::cout << "Commands: close, buy, sell, limit buy/sell <price>, "
                         "sl <$>, tp <$>, risk <pct>, help\n";
        }
        else std::cout << "[CLI] Unknown. Type 'help'.\n";
    }
}

// ===========================================================================
// main
// ===========================================================================
int main(int argc, char* argv[]) {
    auto_pin_thread(1); // Auto-pin Thread 1 (Ingestion) to an available core

    std::string config_path = "config/goldmine.toml";
    std::string replay_file = "";
    
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--replay" && i + 1 < argc) {
            replay_file = argv[++i];
        } else if (arg.ends_with(".toml")) {
            config_path = arg;
        }
    }
    
    auto sys_cfg = ConfigLoader::load(config_path);
    double start_equity = sys_cfg.starting_equity;

    EngineConfig cfg{};
    Pipeline pipeline;

    // -----------------------------------------------------------------------
    // PRICE-ACTION RULES (Simplified for Phase 1 testing)
    // -----------------------------------------------------------------------
    pipeline.registry().add_rule("pa_zscore_long", +0.30,
        [](const MarketState&, const IndicatorSnapshot& ind) -> bool {
            return ind.zscore < -1.5;
        });
    pipeline.registry().add_rule("pa_zscore_short", -0.30,
        [](const MarketState&, const IndicatorSnapshot& ind) -> bool {
            return ind.zscore > 1.5;
        });

    pipeline.start(cfg);
    
    std::unique_ptr<goldmine::LiveTickReader> reader;
    std::unique_ptr<goldmine::ReplayEngine> replay;
    
    if (!replay_file.empty()) {
        replay = std::make_unique<goldmine::ReplayEngine>(replay_file);
    } else {
        reader = std::make_unique<goldmine::LiveTickReader>();
    }

    // -- PHASE 1 & 2 Initialization --
    AccountState account(start_equity, sys_cfg.circuit_breaker_equity); // dynamic drawdown breaker
    
    CostModel cost_model;
    cost_model.maker_fee_bps = sys_cfg.maker_fee_bps;
    cost_model.taker_fee_bps = sys_cfg.taker_fee_bps;
    cost_model.base_slippage_bps = sys_cfg.slippage_bps;
    cost_model.volume_penalty_bps_per_unit = sys_cfg.volume_penalty_bps_per_unit;
    cost_model.use_bnb = false;
    
    TradeStore trade_store;
    if (!trade_store.init("trades.db")) {
        std::fprintf(stderr, "Failed to initialize SQLite trade store!\n");
        return 1;
    }

    ParamServerSHM param_server;
    if (!param_server.init("/goldmine_param_shm", true)) {
        std::fprintf(stderr, "Failed to initialize Param SHM Server!\n");
        return 1;
    }

    cli_max_risk.store(sys_cfg.base_risk_pct / 100.0);

    std::printf("╔═══════════════════════════════════════════════════════════════╗\n");
    std::printf("║   GOLDMINE Production Paper Trader v6 — PHASE 1              ║\n");
    std::printf("╠═══════════════════════════════════════════════════════════════╣\n");
    std::printf("║  Starting Capital:  $%.2f                                    \n", start_equity);
    std::printf("╚═══════════════════════════════════════════════════════════════╝\n");

    std::thread cli_thread(cli_thread_fn);
    cli_thread.detach();

    struct Position {
        bool   is_open       = false;
        bool   is_long       = false;
        double entry_price   = 0.0;
        double sl            = 0.0;
        double tp            = 0.0;
        double trailing_stop = 0.0;
        double best_price    = 0.0;
        double paxg_qty      = 0.0;
        uint64_t entry_tick  = 0;
    } pos;

    uint64_t tick_count       = 0;
    double   algo_conv        = 0.0;

    static constexpr int      SIGNAL_PERSIST = 8;
    static constexpr double   CONV_THRESHOLD = 0.25;
    int long_confirm = 0;
    int short_confirm = 0;

    RiskEngine risk_engine_gate;

    while (true) {
        Tick tick{};
        if (replay) {
            SharedTick st{};
            if (!replay->read_next(st)) {
                std::printf("[REPLAY] End of file reached.\n");
                break;
            }
            tick.timestamp_ns = st.timestamp_ms * 1'000'000LL;
            tick.bid          = st.bid;
            tick.ask          = st.ask;
            tick.volume       = st.volume;
            tick.last         = (st.bid + st.ask) * 0.5;
        } else {
            tick = reader->spin_wait_next();
        }
        
        tick_count++;

        cost_model.update_spread_cost(tick.bid, tick.ask, tick.last);

        // Status
        if (tick_count % 50 == 0 || pos.is_open) {
            double upnl_net = 0.0;
            if (pos.is_open) {
                double mark = pos.is_long ? tick.bid : tick.ask;
                upnl_net = cost_model.net_pnl(pos.entry_price, mark, pos.paxg_qty, pos.is_long);
            }
            if (tick_count % 50 == 0) {
                std::printf("[💰$%.2f] #%llu | %.2f | %s | Net: $%+.4f\n",
                    account.current_equity + (pos.is_open ? upnl_net : 0.0),
                    static_cast<unsigned long long>(tick_count), tick.bid,
                    pos.is_open ? (pos.is_long ? "LONG" : "SHORT") : "FLAT",
                    upnl_net);
            }
        }

        while (!pipeline.feed_gold_tick(tick)) _mm_pause();

        // -- Manage open trade --
        if (pos.is_open) {
            double mark = pos.is_long ? tick.bid : tick.ask;
            bool hit_sl = pos.is_long ? (mark <= pos.sl) : (mark >= pos.sl);
            bool hit_tp = pos.is_long ? (mark >= pos.tp) : (mark <= pos.tp);
            bool force  = cli_force_close.exchange(false);

            if (force || hit_sl || hit_tp) {
                double net_pnl = cost_model.net_pnl(pos.entry_price, mark, pos.paxg_qty, pos.is_long);
                
                account.record_trade(net_pnl);

                const char* reason = hit_tp ? "TP" : (hit_sl ? "SL" : "FORCE");
                
                TelemetryFrame tr{};
                tr.type = TelemetryType::TRADE;
                tr.open_time = pos.entry_tick; // approx
                tr.close_time = tick_count;
                std::strncpy(tr.direction, pos.is_long ? "LONG" : "SHORT", 7);
                tr.entry_price = pos.entry_price;
                tr.exit_price = mark;
                tr.qty = pos.paxg_qty;
                tr.pnl_usd = net_pnl;
                tr.fees = cost_model.round_trip_cost_usd(pos.entry_price, pos.paxg_qty);
                tr.sl = pos.sl;
                tr.tp = pos.tp;
                tr.conviction = algo_conv;
                tr.rules_mask = 0; // TODO
                tr.duration_ticks = tick_count - pos.entry_tick;
                trade_store.log_trade(tr);
                
                TelemetryFrame eq{};
                eq.type = TelemetryType::EQUITY;
                eq.timestamp = tick_count;
                eq.equity = account.current_equity;
                eq.drawdown_pct = account.drawdown_pct();
                eq.trade_count = 1; // TODO: properly track count
                trade_store.log_equity(eq);

                std::printf("\n[TRADE CLOSED] %s | Exit: %.2f | NET PnL: $%+.6f | "
                    "Equity: $%.4f | DD: %.1f%% | %s\n\n",
                    pos.is_long ? "LONG" : "SHORT", mark, net_pnl,
                    account.current_equity, account.drawdown_pct(), reason);

                pos.is_open = false;

                if (account.is_trading_halted()) {
                    std::printf("\n[CIRCUIT BREAKER TRIGGERED] Equity dropped to $%.2f (Breaker at $%.2f). Halting engine.\n",
                        account.current_equity, account.circuit_breaker_equity);
                    break; // Halt engine
                }
            }
        }

        // -- CLI overrides --
        bool manual_buy  = cli_force_buy.exchange(false);
        bool manual_sell = cli_force_sell.exchange(false);

        // -- Drain pipeline --
        double pipeline_atr = 2.0; // fallback
        while (auto sig = pipeline.poll_signal()) {
            algo_conv = sig->conviction;
            if (sig->suggested_sl > 0.5) pipeline_atr = sig->suggested_sl;
        }

        if (algo_conv >= CONV_THRESHOLD) { long_confirm++; short_confirm = 0; }
        else if (algo_conv <= -CONV_THRESHOLD) { short_confirm++; long_confirm = 0; }
        else { long_confirm = 0; short_confirm = 0; }

        bool sig_long  = (long_confirm  >= SIGNAL_PERSIST);
        bool sig_short = (short_confirm >= SIGNAL_PERSIST);

        bool can_trade = !account.is_trading_halted() && !pos.is_open &&
                         (manual_buy || manual_sell || sig_long || sig_short);

        if (can_trade) {
            bool is_long = manual_buy ? true : (manual_sell ? false : sig_long);
            double entry_price = is_long ? tick.ask : tick.bid;

            DynamicParams* p = param_server.get();
            if (p) {
                // Pass through strict validation function (Module 2)
                risk_engine_gate.sync_parameters(p);
                cli_max_risk.store(risk_engine_gate.cfg().max_risk_pct);
                
                // ML Gate
                if (!manual_buy && !manual_sell && p->p_profitable_gate_bps.load(std::memory_order_relaxed) < 6000) {
                    std::printf("[ML-GATE] Rejected: P(profitable) < 60.00%%\n");
                    long_confirm = 0;
                    short_confirm = 0;
                    continue;
                }
            }

            double sl_dist = cli_override_sl.load() > 0.0 ? cli_override_sl.load() : 1.5 * pipeline_atr;
            double tp_dist_raw = cli_override_tp.load() > 0.0 ? cli_override_tp.load() : 2.0 * sl_dist;
            double risk_pct = cli_max_risk.load();

            // Compute exact qty using Kelly-inspired sizing
            double paxg_qty = account.compute_position_size(entry_price, sl_dist, risk_pct);
            
            // Check minimum profit price
            double breakeven_dist = cost_model.min_profit_price(entry_price, paxg_qty);
            
            if (tp_dist_raw < breakeven_dist) {
                if (!manual_buy && !manual_sell) {
                    std::printf("[REJECTED] TP too tight for transaction costs. Required: %.2f, Got: %.2f\n", 
                                breakeven_dist, tp_dist_raw);
                    // Reset confirm to avoid spamming
                    long_confirm = 0; 
                    short_confirm = 0;
                    continue;
                }
            }

            double tp_dist = std::max(tp_dist_raw, breakeven_dist * 1.5); // Ensure buffer if manual override

            // Open position
            pos.is_open     = true;
            pos.is_long     = is_long;
            pos.entry_price = entry_price;
            pos.sl = is_long ? entry_price - sl_dist : entry_price + sl_dist;
            pos.tp = is_long ? entry_price + tp_dist : entry_price - tp_dist;
            pos.paxg_qty      = paxg_qty;
            pos.entry_tick    = tick_count;

            long_confirm = 0;
            short_confirm = 0;

            std::printf("\n[OPENED] %s | Entry: %.2f | Qty: %.6f | SL: %.2f | TP: %.2f | Risk: %.1f%%\n\n",
                is_long ? "LONG" : "SHORT", entry_price, paxg_qty, pos.sl, pos.tp, risk_pct * 100.0);
        }

        std::fflush(stdout);
    }
    return 0;
}
