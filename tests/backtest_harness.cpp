// =============================================================================
// backtest_harness.cpp — Full Single-Threaded Backtest with v3 Strategy
//
// This is a SINGLE-THREADED deterministic backtest that replicates exactly
// the same strategy as expert_engine.cpp but without the multi-threaded
// pipeline. This avoids race conditions in signal draining and produces
// reproducible results.
//
// Usage: ./backtest_harness [data/gold_ticks.bin]
//    or: ./backtest_harness (uses synthetic data)
// =============================================================================
#include "goldmine/types.hpp"
#include "goldmine/indicators.hpp"
#include "goldmine/tick_packer.hpp"
#include "goldmine/replayer.hpp"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include <deque>
#include <algorithm>

using namespace goldmine;

// ---------------------------------------------------------------------------
// Generate realistic synthetic PAXG tick data for testing
// Mean-reverting random walk with occasional trends
// ---------------------------------------------------------------------------
static const char* SYNTH_PATH = "/tmp/goldmine_synth.bin";

static void generate_synthetic_ticks(std::size_t count) {
    FILE* f = std::fopen(SYNTH_PATH, "wb");
    if (!f) return;

    Price xau = 2650.0;   // Starting price
    // Start at London open (08:00 UTC)
    Nanos ts = 8LL * 3600 * 1'000'000'000LL;

    // Ornstein-Uhlenbeck mean-reverting process
    // dX = theta * (mu - X) * dt + sigma * dW
    const double theta = 0.02;  // mean reversion speed
    const double mu    = 2650.0; // long-run mean
    const double sigma = 0.15;  // volatility per tick

    // Simple LCG-based pseudo-random (deterministic)
    uint64_t seed = 123456789ULL;
    auto next_rand = [&]() -> double {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        // Convert to [-1, 1]
        return (static_cast<double>(seed >> 33) / static_cast<double>(1ULL << 31)) - 1.0;
    };

    for (std::size_t i = 0; i < count; ++i) {
        // Mean-reverting step
        double dW = next_rand();
        double drift = theta * (mu - xau);
        double noise = sigma * dW;

        // Occasional trend (every ~500 ticks, sustained for ~100 ticks)
        if (i % 500 < 100 && i > 500) {
            drift += ((i / 500) % 2 == 0) ? 0.05 : -0.05;
        }

        xau += drift + noise;

        // Keep price reasonable
        xau = std::max(xau, 2600.0);
        xau = std::min(xau, 2700.0);

        ts += 100'000'000LL;  // 100ms per tick

        double spread = 0.10 + 0.05 * std::abs(dW); // variable spread
        double vol = 1.0 + 2.0 * std::abs(dW);       // volume correlated with moves

        Tick t{};
        t.timestamp_ns = ts;
        t.bid = xau - spread * 0.5;
        t.ask = xau + spread * 0.5;
        t.last = xau;
        t.volume = vol;

        BinaryTick bt = BinaryTick::from_tick(t);
        std::fwrite(&bt, sizeof(BinaryTick), 1, f);
    }
    std::fclose(f);
}

// ===========================================================================
// Single-threaded backtest engine — deterministic, no race conditions
// ===========================================================================
struct BacktestPosition {
    bool   is_open       = false;
    bool   is_long       = false;
    double entry_price   = 0.0;
    double sl            = 0.0;
    double tp            = 0.0;
    double trailing_stop = 0.0;
    double best_price    = 0.0;
    double lot_size      = 0.0;
    uint64_t entry_tick  = 0;
    double entry_atr     = 0.0;
};

struct BacktestResult {
    int    total_trades   = 0;
    int    winners        = 0;
    int    losers         = 0;
    int    tp_exits       = 0;
    int    sl_exits       = 0;
    int    trail_exits    = 0;
    int    time_exits     = 0;
    double total_pnl      = 0.0;
    double max_drawdown   = 0.0;
    double peak_equity    = 10000.0;
    double current_equity = 10000.0;
    double gross_profit   = 0.0;
    double gross_loss     = 0.0;
};

int main(int argc, char* argv[]) {
    const char* tick_file = (argc > 1) ? argv[1] : nullptr;

    if (!tick_file) {
        std::printf("[BACKTEST] No tick file specified, generating 100000 synthetic mean-reverting ticks...\n");
        generate_synthetic_ticks(100000);
        tick_file = SYNTH_PATH;
    }

    MappedTickFile file;
    if (!file.open(tick_file)) {
        std::fprintf(stderr, "[ERROR] Cannot open tick file: %s\n", tick_file);
        return 1;
    }
    std::printf("[BACKTEST] Loaded %zu ticks (%.2f MB) from %s\n",
                file.count(),
                static_cast<double>(file.bytes()) / (1024.0 * 1024.0),
                tick_file);

    // -----------------------------------------------------------------------
    // Single-threaded: compute indicators directly, no pipeline threads
    // -----------------------------------------------------------------------
    TickStore store{};
    indicators::SessionContext ctx{};
    Nanos day_open_ns = 0;

    BacktestPosition pos{};
    BacktestResult result{};
    std::deque<double> recent_pnl;
    static constexpr int MAX_RECENT = 30;

    uint64_t last_trade_tick = 0;
    int long_confirm  = 0;
    int short_confirm = 0;
    double spread_ema = 0.0;
    bool spread_init  = false;

    // Strategy parameters (same as expert_engine.cpp)
    static constexpr uint64_t WARMUP         = 100;
    static constexpr uint64_t COOLDOWN       = 30;
    static constexpr uint64_t LOSS_COOLDOWN  = 300;
    static constexpr int      SIGNAL_PERSIST = 8;
    static constexpr uint64_t TIME_EXIT      = 400;
    static constexpr uint64_t HARD_TIME_EXIT  = 1200;  // forced close regardless
    static constexpr double   TRAIL_ACTIVATION = 0.30;
    static constexpr double   TRAIL_FACTOR   = 0.40;
    static constexpr double   MIN_SL         = 3.0;
    static constexpr double   MAX_SL         = 15.0;
    static constexpr double   MIN_TP         = 1.00;
    static constexpr double   SPREAD_MULT    = 2.0;
    static constexpr double   CONV_THRESHOLD = 0.55;
    static constexpr int      MAX_CONSEC     = 3;

    auto start_time = std::chrono::high_resolution_clock::now();

    for (std::size_t i = 0; i < file.count(); ++i) {
        Tick t = file[i].to_tick();

        // Day boundary reset
        Nanos sod = utc_sod_seconds(t.timestamp_ns);
        if (day_open_ns == 0 || t.timestamp_ns - day_open_ns > 86400LL * 1'000'000'000LL) {
            ctx = indicators::SessionContext{};
            day_open_ns = t.timestamp_ns;
        }
        if (t.bid < ctx.swing_low) ctx.swing_low = t.bid;

        // Asian session range
        if (sod >= session::ASIAN_OPEN && sod < session::ASIAN_CLOSE) {
            if (t.last > ctx.asian_high) ctx.asian_high = t.last;
            if (t.last < ctx.asian_low)  ctx.asian_low  = t.last;
        } else if (!ctx.asian_finalized && sod >= session::ASIAN_CLOSE) {
            ctx.asian_finalized = true;
        }
        // Opening range
        if (sod >= session::LDN_OPEN && ctx.opening_ticks < 15) {
            if (t.last > ctx.opening_hi) ctx.opening_hi = t.last;
            if (t.last < ctx.opening_lo) ctx.opening_lo = t.last;
            ++ctx.opening_ticks;
        }

        store.push(t, 0.0, 0.0, 0.0);  // no DXY/XAG/Yield in single-asset mode
        ++ctx.session_tick_count;

        // Compute indicators
        auto snap = indicators::compute_all(store, ctx, t.timestamp_ns);
        ctx.prev_velocity = snap.price_velocity;
        ctx.prev_garch    = snap.garch_vol;

        double current_spread = t.ask - t.bid;
        if (!spread_init) { spread_ema = current_spread; spread_init = true; }
        else { spread_ema = 0.05 * current_spread + 0.95 * spread_ema; }

        // ===================================================================
        // CONVICTION SCORING — replicate the rule weights from expert_engine
        // ===================================================================
        double conviction = 0.0;

        // Mean reversion Z-score
        if (snap.zscore < -1.2 && snap.vwap > 10.0) conviction += 0.20;
        if (snap.zscore < -1.8 && snap.vwap > 10.0) conviction += 0.15;
        if (snap.zscore > 1.2  && snap.vwap > 10.0) conviction -= 0.20;
        if (snap.zscore > 1.8  && snap.vwap > 10.0) conviction -= 0.15;

        // RSI confirmation
        if (snap.rsi14 < 35.0) conviction += 0.15;
        if (snap.rsi14 > 65.0) conviction -= 0.15;

        // VWAP position
        if (t.last < snap.vwap && snap.vwap > 10.0) conviction += 0.10;
        if (t.last > snap.vwap && snap.vwap > 10.0) conviction -= 0.10;

        // Velocity deceleration
        if (snap.price_velocity < 0.0 && snap.price_acceleration > 0.0) conviction += 0.15;
        if (snap.price_velocity > 0.0 && snap.price_acceleration < 0.0) conviction -= 0.15;

        // Bollinger Band pierce
        if (t.last < snap.bb_lower && snap.bb_lower > 10.0) conviction += 0.15;
        if (t.last > snap.bb_upper && snap.bb_upper > 10.0) conviction -= 0.15;

        // Squeeze breakout
        if (snap.squeeze_breakout_up)   conviction += 0.25;
        if (snap.squeeze_breakout_down) conviction -= 0.25;

        // Trend strength
        if (snap.trend_strength > 50.0 && snap.price_velocity > 0.0) conviction += 0.10;
        if (snap.trend_strength > 50.0 && snap.price_velocity < 0.0) conviction -= 0.10;

        // OFI confirmation
        if (snap.ofi_zscore > 1.5 && snap.micro_price_delta > 0.0) conviction += 0.10;
        if (snap.ofi_zscore < -1.5 && snap.micro_price_delta < 0.0) conviction -= 0.10;

        // Clamp
        conviction = std::clamp(conviction, -1.0, 1.0);

        // ===================================================================
        // SIGNAL PERSISTENCE
        // ===================================================================
        if (conviction >= CONV_THRESHOLD) {
            long_confirm++;
            short_confirm = 0;
        } else if (conviction <= -CONV_THRESHOLD) {
            short_confirm++;
            long_confirm = 0;
        } else {
            long_confirm = 0;
            short_confirm = 0;
        }

        // ===================================================================
        // MANAGE OPEN TRADE
        // ===================================================================
        if (pos.is_open) {
            double mark = pos.is_long ? t.bid : t.ask;
            double raw_pnl = pos.is_long ? (mark - pos.entry_price) : (pos.entry_price - mark);
            bool hit_sl = pos.is_long ? (mark <= pos.sl) : (mark >= pos.sl);
            bool hit_tp = pos.is_long ? (mark >= pos.tp) : (mark <= pos.tp);

            // Update best price
            if (pos.is_long) { if (mark > pos.best_price) pos.best_price = mark; }
            else             { if (mark < pos.best_price) pos.best_price = mark; }

            // Trailing stop
            double tp_dist = std::abs(pos.tp - pos.entry_price);
            double move = pos.is_long ? (mark - pos.entry_price) : (pos.entry_price - mark);
            if (move > TRAIL_ACTIVATION * tp_dist) {
                if (pos.is_long) {
                    double trail = pos.best_price - TRAIL_FACTOR * (pos.best_price - pos.entry_price);
                    if (trail > pos.trailing_stop) pos.trailing_stop = trail;
                } else {
                    double trail = pos.best_price + TRAIL_FACTOR * (pos.entry_price - pos.best_price);
                    if (pos.trailing_stop == 0.0 || trail < pos.trailing_stop) pos.trailing_stop = trail;
                }
            }

            bool hit_trail = false;
            if (pos.trailing_stop > 0.0) {
                hit_trail = pos.is_long ? (mark <= pos.trailing_stop) : (mark >= pos.trailing_stop);
            }

            uint64_t ticks_open = i - pos.entry_tick;
            bool time_exit = (ticks_open >= TIME_EXIT) && (raw_pnl >= 0.05);
            bool hard_exit = (ticks_open >= HARD_TIME_EXIT);  // forced close

            if (hit_sl || hit_tp || hit_trail || time_exit || hard_exit) {
                double pnl = raw_pnl * pos.lot_size;
                result.total_pnl += pnl;
                result.current_equity += pnl;
                result.total_trades++;

                if (pnl > 0) {
                    result.winners++;
                    result.gross_profit += pnl;
                } else {
                    result.losers++;
                    result.gross_loss += std::abs(pnl);
                }

                if (result.current_equity > result.peak_equity)
                    result.peak_equity = result.current_equity;
                double dd = result.peak_equity - result.current_equity;
                if (dd > result.max_drawdown) result.max_drawdown = dd;

                if (hit_tp) result.tp_exits++;
                else if (hit_trail) result.trail_exits++;
                else if (hit_sl) result.sl_exits++;
                else result.time_exits++;

                if (static_cast<int>(recent_pnl.size()) >= MAX_RECENT) recent_pnl.pop_front();
                recent_pnl.push_back(pnl);

                last_trade_tick = i;
                pos.is_open = false;
                pos.trailing_stop = 0.0;
            }
        }

        // ===================================================================
        // ENTRY LOGIC
        // ===================================================================
        bool warmed_up = (i >= WARMUP);

        // Consecutive losses check
        int consec_losses = 0;
        for (auto it = recent_pnl.rbegin(); it != recent_pnl.rend(); ++it) {
            if (*it <= 0) consec_losses++;
            else break;
        }
        uint64_t required_cooldown = (consec_losses >= MAX_CONSEC) ? LOSS_COOLDOWN : COOLDOWN;
        bool cooled = (i - last_trade_tick >= required_cooldown);

        bool spread_ok = (spread_ema < 0.01) || (current_spread <= SPREAD_MULT * spread_ema);

        bool sig_long  = (long_confirm  >= SIGNAL_PERSIST) && (conviction >= CONV_THRESHOLD);
        bool sig_short = (short_confirm >= SIGNAL_PERSIST) && (conviction <= -CONV_THRESHOLD);

        if (!pos.is_open && warmed_up && cooled && spread_ok && (sig_long || sig_short)) {
            pos.is_open = true;
            pos.is_long = sig_long;
            pos.entry_price = pos.is_long ? t.ask : t.bid;

            double atr_sl = snap.atr14 > 0.5 ? 1.5 * snap.atr14 : 5.0;
            double sl_dist = std::clamp(atr_sl, MIN_SL, MAX_SL);

            double vol_tp = snap.realized_vol_20 > 0.01 ? 2.5 * snap.realized_vol_20 * 20.0 : 2.0;
            double tp_dist = std::max(MIN_TP, std::min(vol_tp, sl_dist * 0.6));

            if (sl_dist < 2.0 * tp_dist) {
                sl_dist = 2.0 * tp_dist;
                sl_dist = std::min(sl_dist, MAX_SL);
            }

            pos.sl = pos.is_long ? pos.entry_price - sl_dist : pos.entry_price + sl_dist;
            pos.tp = pos.is_long ? pos.entry_price + tp_dist : pos.entry_price - tp_dist;
            pos.best_price = pos.entry_price;
            pos.trailing_stop = 0.0;

            double equity = 100000.0;
            double risk_dollars = equity * 0.01;
            pos.lot_size = risk_dollars / sl_dist;
            pos.lot_size = std::min(pos.lot_size, 2.0);
            pos.lot_size = std::max(pos.lot_size, 0.01);

            pos.entry_tick = i;
            pos.entry_atr  = snap.atr14;

            long_confirm = 0;
            short_confirm = 0;
        }
    }

    // Close any open position at end of data
    if (pos.is_open) {
        Tick last_t = file[file.count()-1].to_tick();
        double mark = pos.is_long ? last_t.bid : last_t.ask;
        double raw_pnl = pos.is_long ? (mark - pos.entry_price) : (pos.entry_price - mark);
        double pnl = raw_pnl * pos.lot_size;
        result.total_pnl += pnl;
        result.current_equity += pnl;
        result.total_trades++;
        if (pnl > 0) { result.winners++; result.gross_profit += pnl; }
        else { result.losers++; result.gross_loss += std::abs(pnl); }
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    double elapsed = std::chrono::duration<double>(end_time - start_time).count();

    // -----------------------------------------------------------------------
    // Print Results
    // -----------------------------------------------------------------------
    double win_rate = (result.total_trades > 0)
        ? 100.0 * static_cast<double>(result.winners) / result.total_trades
        : 0.0;

    double profit_factor = (result.gross_loss > 0.01)
        ? result.gross_profit / result.gross_loss
        : (result.gross_profit > 0 ? 999.0 : 0.0);

    double max_dd_pct = (result.peak_equity > 0)
        ? 100.0 * result.max_drawdown / result.peak_equity
        : 0.0;

    double avg_win = (result.winners > 0) ? result.gross_profit / result.winners : 0.0;
    double avg_loss = (result.losers > 0) ? result.gross_loss / result.losers : 0.0;
    double expectancy = (result.total_trades > 0) ? result.total_pnl / result.total_trades : 0.0;
    double ticks_per_sec = static_cast<double>(file.count()) / elapsed;

    std::printf("\n");
    std::printf("╔══════════════════════════════════════════════════════════╗\n");
    std::printf("║          GOLDMINE v3 BACKTEST RESULTS                   ║\n");
    std::printf("╠══════════════════════════════════════════════════════════╣\n");
    std::printf("║  Ticks Replayed  : %-10zu                            ║\n", file.count());
    std::printf("║  Elapsed         : %.3f sec                            ║\n", elapsed);
    std::printf("║  Throughput      : %.0f ticks/sec                      ║\n", ticks_per_sec);
    std::printf("╠══════════════════════════════════════════════════════════╣\n");
    std::printf("║  Total Trades    : %-6d                                ║\n", result.total_trades);
    std::printf("║  Winners         : %-6d                                ║\n", result.winners);
    std::printf("║  Losers          : %-6d                                ║\n", result.losers);
    std::printf("║  ✅ Win Rate     : %.1f%%                              ║\n", win_rate);
    std::printf("╠══════════════════════════════════════════════════════════╣\n");
    std::printf("║  Exit Breakdown:                                        ║\n");
    std::printf("║    TP Exits      : %-6d                                ║\n", result.tp_exits);
    std::printf("║    Trail Exits   : %-6d                                ║\n", result.trail_exits);
    std::printf("║    SL Exits      : %-6d                                ║\n", result.sl_exits);
    std::printf("║    Time Exits    : %-6d                                ║\n", result.time_exits);
    std::printf("╠══════════════════════════════════════════════════════════╣\n");
    std::printf("║  Total PnL       : $%.2f                               ║\n", result.total_pnl);
    std::printf("║  Profit Factor   : %.2f                                ║\n", profit_factor);
    std::printf("║  Avg Win         : $%.4f                               ║\n", avg_win);
    std::printf("║  Avg Loss        : $%.4f                               ║\n", avg_loss);
    std::printf("║  Expectancy/Trade: $%.4f                               ║\n", expectancy);
    std::printf("║  Max Drawdown    : $%.2f (%.2f%%)                      ║\n", result.max_drawdown, max_dd_pct);
    std::printf("║  End Equity      : $%.2f                               ║\n", result.current_equity);
    std::printf("╚══════════════════════════════════════════════════════════╝\n");

    return 0;
}
