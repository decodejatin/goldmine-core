#pragma once
// =============================================================================
// pipeline.hpp — 4-Thread Core-Pinned Pipeline Architecture
//
// Thread 1 (Ingestion)   : External feed → normalize → SPSC Q1
// Thread 2 (Indicators)  : SPSC Q1 → SoA store → compute indicators → PingPong
// Thread 3 (Logic Core)  : PingPong → WideBitmask evaluation → score → SPSC Q2
// Thread 4 (Risk & Exec) : SPSC Q2 → pre-trade risk checks → order/output
//
// All inter-thread communication is lock-free. Zero dynamic allocation.
// =============================================================================

#include "goldmine/types.hpp"
#include "goldmine/lockfree_queue.hpp"
#include "goldmine/indicators.hpp"
#include "goldmine/bitmask_engine.hpp"
#include "goldmine/rule_registry.hpp"
#include "goldmine/gold_rules.hpp"
#include "goldmine/online_learner.hpp"
#include "goldmine/risk_engine.hpp"
#include "goldmine/cpu_affinity.hpp"

#include <atomic>
#include <cstdio>
#include <functional>
#include <immintrin.h>
#include <thread>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

namespace goldmine {

// ---------------------------------------------------------------------------
// Shared state — all SPSC queues between pipeline stages, pre-allocated.
// ---------------------------------------------------------------------------
struct alignas(CACHE_LINE) PipelineState {
    // Feed → Indicator thread
    SpscQueue<Tick, TICK_BUFFER_SZ>    gold_queue{};
    SpscQueue<DxyTick, TICK_BUFFER_SZ> dxy_queue{};
    SpscQueue<Price, TICK_BUFFER_SZ>   xag_queue{};
    SpscQueue<Price, TICK_BUFFER_SZ>   yield_queue{};

    // Indicator → Logic thread (latest-value semantics)
    SpscPingPong<IndicatorSnapshot>    snapshot_pp{};

    // Logic → Risk thread (raw signals before risk check)
    SpscQueue<EngineSignal, 256>       raw_signal_queue{};

    // Risk → Output (final, risk-checked signals)
    SpscQueue<EngineSignal, 256>       final_signal_queue{};
    OutcomeQueue                       outcome_queue{};

    // Shared latest tick (atomic publish from ingestion → logic for MarketState)
    alignas(64) std::atomic<Nanos> latest_tick_ts{0};
    alignas(64) Tick               latest_tick{};     // relaxed read OK
    alignas(64) DxyTick            latest_dxy{};

    std::atomic<bool> running{false};
};

// ---------------------------------------------------------------------------
// Core pinning utility
// ---------------------------------------------------------------------------
inline void pin_to_core([[maybe_unused]] int core_id) noexcept {
    auto_pin_thread(core_id);
}

// ---------------------------------------------------------------------------
// Thread 2 — Indicator Computation
// ---------------------------------------------------------------------------
inline void indicator_thread_fn(PipelineState* ps) noexcept {
    pin_to_core(2);
    TickStore                   store{};
    indicators::SessionContext  ctx{};
    Price dxy_last=0, xag_last=0, yield_last=0;
    Nanos day_open_ns=0;

    while (ps->running.load(std::memory_order_relaxed)) {
        bool did_work = false;
        if (auto d = ps->dxy_queue.try_pop()) {
            dxy_last = d->price; ps->latest_dxy = *d; did_work = true;
        }
        if (auto x = ps->xag_queue.try_pop()) {
            xag_last = *x; did_work = true;
        }
        if (auto y = ps->yield_queue.try_pop()) {
            yield_last = *y; did_work = true;
        }
        if (auto t = ps->gold_queue.try_pop()) {
            did_work = true;
            ps->latest_tick = *t;
            ps->latest_tick_ts.store(t->timestamp_ns, std::memory_order_release);
            Nanos sod = utc_sod_seconds(t->timestamp_ns);

            if (day_open_ns==0 || t->timestamp_ns-day_open_ns>86400LL*1'000'000'000LL) {
                ctx = indicators::SessionContext{};
                day_open_ns = t->timestamp_ns;
            }
            if (t->bid < ctx.swing_low) ctx.swing_low = t->bid;

            // Asian session range tracking (00:00-06:00 UTC)
            if (sod >= session::ASIAN_OPEN && sod < session::ASIAN_CLOSE) {
                if (t->last > ctx.asian_high) ctx.asian_high = t->last;
                if (t->last < ctx.asian_low)  ctx.asian_low  = t->last;
            } else if (!ctx.asian_finalized && sod >= session::ASIAN_CLOSE) {
                ctx.asian_finalized = true;
            }
            // Opening range (first 15 ticks of London)
            if (sod >= session::LDN_OPEN && ctx.opening_ticks < 15) {
                if (t->last > ctx.opening_hi) ctx.opening_hi = t->last;
                if (t->last < ctx.opening_lo) ctx.opening_lo = t->last;
                ++ctx.opening_ticks;
            }

            store.push(*t, dxy_last, xag_last, yield_last);
            ++ctx.session_tick_count;

            auto snap = indicators::compute_all(store, ctx, t->timestamp_ns);
            ctx.prev_velocity = snap.price_velocity;
            ctx.prev_garch    = snap.garch_vol;
            ps->snapshot_pp.store(snap);
        }
        if (!did_work) _mm_pause();
    }
}

// ---------------------------------------------------------------------------
// Thread 3 — Hot Logic Core (bitmask evaluation + scoring)
// ---------------------------------------------------------------------------
inline void logic_thread_fn(PipelineState* ps,
                             RuleRegistry* registry) noexcept {
    pin_to_core(3);

    IndicatorSnapshot snap{};
    OnlineLearner learner{};
    learner.initialize(registry->weights(), registry->count());

    while (ps->running.load(std::memory_order_relaxed)) {
        // Drain trade outcomes from Thread 4 and apply online SGD updates
        bool adapted = false;
        while (auto outcome = ps->outcome_queue.try_pop()) {
            learner.update(*outcome);
            adapted = true;
        }
        if (adapted) {
            learner.apply_to(registry->mutable_weights(), registry->count());
        }

        if (!ps->snapshot_pp.load(snap)) {
            _mm_pause();
            continue;
        }

        // Build MarketState from latest atomic data
        MarketState state{};
        state.indicators = snap;
        state.last_tick   = ps->latest_tick;
        state.last_dxy    = ps->latest_dxy;
        state.utc_sod_s   = utc_sod_seconds(snap.computed_at_ns);
        state.is_valid    = true;

        // --- BITMASK EVALUATION (hot path) ---
        const WideBitmask mask = registry->evaluate_all(state);

        // --- SCORING ---
        const SessionFlags sess = classify_session(state.utc_sod_s);
        const Score conviction  = registry->score(mask, sess);

        // Build raw signal
        EngineSignal sig{};
        sig.active_rules = mask;
        sig.conviction   = conviction;
        sig.signal_ns    = snap.computed_at_ns;
        sig.suggested_sl = (snap.atr14 > 1e-5) ? 1.5 * snap.atr14 : 5.0;
        sig.suggested_tp = (snap.atr14 > 1e-5) ? 3.0 * snap.atr14 : 10.0; // 2:1 RR minimum

        // Direction set if conviction exceeds ±0.40 threshold.
        // Signal quality gate is now in expert_engine.cpp via 5-tick persistence.
        // The pipeline just needs to pass through any meaningful signal.
        const auto cfg_thresh_l = +0.40;
        const auto cfg_thresh_s = -0.40;

        if (conviction >= cfg_thresh_l)       sig.direction = Direction::LONG;
        else if (conviction <= cfg_thresh_s)  sig.direction = Direction::SHORT;

        // ALWAYS push for UI telemetry.
        [[maybe_unused]] bool ok = ps->raw_signal_queue.try_push(sig);
    }
}

// ---------------------------------------------------------------------------
// PaperEngine — dry-run execution simulator (Thread 4)
// Tracks simulated fills, PnL, drawdowns in pre-allocated ring buffer.
// ---------------------------------------------------------------------------
class PaperEngine {
public:
    void on_signal(PipelineState* ps, const EngineSignal& sig, const SizingResult& sizing, Price current_bid, Price current_ask) noexcept {
        Nanos now = sig.signal_ns;
        // Check if we need to close existing position
        if (pos_.is_open) {
            Price mark = (pos_.direction==Direction::LONG) ? current_bid : current_ask;
            if ((pos_.direction==Direction::LONG  && mark <= pos_.sl_price) ||
                (pos_.direction==Direction::SHORT && mark >= pos_.sl_price) ||
                (pos_.direction==Direction::LONG  && mark >= pos_.tp_price) ||
                (pos_.direction==Direction::SHORT && mark <= pos_.tp_price)) {
                close_position(ps, mark, now);
            }
        }
        // Open new position if no open position and signal is actionable
        if (!pos_.is_open && sig.direction != Direction::NONE && !sig.risk_blocked) {
            Price entry = (sig.direction==Direction::LONG) ? current_ask : current_bid;
            double slip = entry * 0.00005; // 0.5 bps simulated slippage
            entry += (sig.direction==Direction::LONG) ? slip : -slip;
            pos_.is_open = true;
            pos_.direction = sig.direction;
            pos_.entry_price = entry;
            pos_.sl_price = (sig.direction==Direction::LONG) ? entry-sig.suggested_sl : entry+sig.suggested_sl;
            pos_.tp_price = (sig.direction==Direction::LONG) ? entry+sig.suggested_tp : entry-sig.suggested_tp;
            pos_.open_ns = now;
            pos_.lot_size = sizing.lot_size;
            pos_.open_rules = sig.active_rules;
            pos_.open_conviction = sig.conviction;
            // Log fill
            auto& f = fills_[fill_count_ % PAPER_LOG_SZ];
            f.fill_ns = now; f.direction = sig.direction;
            f.entry_price = entry; f.sl_price = pos_.sl_price;
            f.tp_price = pos_.tp_price; f.conviction = sig.conviction;
            f.rules_active = sig.active_rules;
            f.slippage_bps = 0.5; f.commission = 3.50;
            ++fill_count_;
        }
    }

    void close_position(PipelineState* ps, Price exit_price, Nanos now) noexcept {
        if (!pos_.is_open) return;
        double pnl = (pos_.direction==Direction::LONG)
            ? (exit_price - pos_.entry_price)
            : (pos_.entry_price - exit_price);
        pnl -= 3.50; // commission
        summary_.total_pnl += pnl;
        summary_.current_equity += pnl;
        summary_.total_trades++;
        if (pnl > 0) summary_.winners++; else summary_.losers++;
        if (summary_.current_equity > summary_.peak_equity)
            summary_.peak_equity = summary_.current_equity;
        double dd = summary_.peak_equity - summary_.current_equity;
        if (dd > summary_.max_drawdown) summary_.max_drawdown = dd;
        double lat = static_cast<double>(now - pos_.open_ns);
        summary_.avg_latency_ns = (summary_.avg_latency_ns*(summary_.total_trades-1)+lat)/summary_.total_trades;
        summary_.last_trade_ns = now;
        
        // Push outcome to online learner (Thread 3)
        if (ps) {
            TradeOutcome outcome{};
            outcome.label = (pnl > 0.0) ? 1.0 : -1.0;
            outcome.active_rules = pos_.open_rules;
            outcome.conviction = pos_.open_conviction;
            outcome.close_ns = now;
            (void)ps->outcome_queue.try_push(outcome);
        }
        
        pos_.is_open = false;
    }

    const PaperTradeSummary& summary() const noexcept { return summary_; }
    const PaperPosition& position()    const noexcept { return pos_; }
    std::size_t fill_count()           const noexcept { return fill_count_; }
private:
    PaperPosition pos_{};
    PaperTradeSummary summary_{};
    std::array<PaperFill, PAPER_LOG_SZ> fills_{};
    std::size_t fill_count_ = 0;
};

inline void risk_thread_fn(PipelineState* ps,
                            const EngineConfig& cfg,
                            PaperEngine* paper) noexcept {
    pin_to_core(4);
    
    RiskEngine risk_engine{};

    while (ps->running.load(std::memory_order_relaxed)) {
        auto maybe = ps->raw_signal_queue.try_pop();
        if (!maybe) { _mm_pause(); continue; }
        EngineSignal sig = *maybe;
        
        // Fixed 1% risk sizing — avoids runaway Kelly formula during warmup
        SizingResult sizing{};
        if (!sig.risk_blocked && sig.direction != Direction::NONE) {
            const double CAPITAL    = 100000.0;
            const double RISK_PCT   = 0.01;     // 1% risk per trade
            double sl_dist          = sig.suggested_sl > 0.5 ? sig.suggested_sl : 5.0;
            sizing.lot_size         = (CAPITAL * RISK_PCT) / sl_dist;
            sizing.lot_size         = std::min(sizing.lot_size, 5.0);  // hard cap: 5 oz
            sizing.lot_size         = std::max(sizing.lot_size, 0.01); // minimum 0.01 oz
            sizing.order_type       = OrderType::MARKET_AGGRO;
        }

        // Paper trading execution
        if (paper) {
            paper->on_signal(ps, sig, sizing, ps->latest_tick.bid, ps->latest_tick.ask);
        }
        [[maybe_unused]] bool ok = ps->final_signal_queue.try_push(sig);
    }
    (void)cfg;
}

// ===========================================================================
// Pipeline — top-level orchestrator
// ===========================================================================
class Pipeline {
public:
    Pipeline() : state_(std::make_unique<PipelineState>()) {}
    ~Pipeline() { stop(); }
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    void start(const EngineConfig& cfg) {
        if (state_->running.load()) return;
        register_all_gold_rules(registry_, cfg);
        state_->running.store(true, std::memory_order_release);
        t_indicator_ = std::thread(indicator_thread_fn, state_.get());
        t_logic_ = std::thread(logic_thread_fn, state_.get(), &registry_);
        t_risk_ = std::thread(risk_thread_fn, state_.get(), std::cref(cfg), &paper_);
    }

    [[nodiscard]] __attribute__((always_inline)) inline bool feed_gold_tick(const Tick& t) noexcept { return state_->gold_queue.try_push(t); }
    [[nodiscard]] __attribute__((always_inline)) inline bool feed_dxy_tick(const DxyTick& t) noexcept { return state_->dxy_queue.try_push(t); }
    [[nodiscard]] __attribute__((always_inline)) inline bool feed_xag_tick(Price p) noexcept { return state_->xag_queue.try_push(p); }
    [[nodiscard]] __attribute__((always_inline)) inline bool feed_yield_tick(Price p) noexcept { return state_->yield_queue.try_push(p); }

    [[nodiscard]] __attribute__((always_inline)) inline std::optional<EngineSignal> poll_signal() noexcept {
        return state_->final_signal_queue.try_pop();
    }

    RuleRegistry& registry() noexcept { return registry_; }
    const PaperEngine& paper() const noexcept { return paper_; }
    PaperEngine& paper() noexcept { return paper_; }

    void stop() {
        if (!state_->running.load()) return;
        state_->running.store(false, std::memory_order_release);
        if (t_indicator_.joinable()) t_indicator_.join();
        if (t_logic_.joinable())     t_logic_.join();
        if (t_risk_.joinable())      t_risk_.join();
    }

private:
    std::unique_ptr<PipelineState> state_;
    RuleRegistry    registry_{};
    PaperEngine     paper_{};
    std::thread     t_indicator_;
    std::thread     t_logic_;
    std::thread     t_risk_;
};

} // namespace goldmine
