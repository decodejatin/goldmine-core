#pragma once
// =============================================================================
// risk_engine.hpp — Fractional Kelly Sizing & Adaptive Order Routing
//
// Calculates dynamic lot sizes using the Kelly Criterion, capped at
// configurable maximum risk-per-trade. Routes orders as Limit vs Market
// based on conviction score thresholds.
// =============================================================================
#include "goldmine/types.hpp"
#include "goldmine/param_shm.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace goldmine {

// ---------------------------------------------------------------------------
// OrderType — execution routing decision
// ---------------------------------------------------------------------------
enum class OrderType : std::uint8_t {
    NONE          = 0,
    LIMIT_PASSIVE = 1,  // conviction 0.35–0.60: capture spread
    MARKET_AGGRO  = 2,  // conviction ≥ 0.60: momentum fill
};

// ---------------------------------------------------------------------------
// KellyConfig — tunable risk parameters
// ---------------------------------------------------------------------------
struct KellyConfig {
    double kelly_fraction    = 0.25;   // f* multiplier (quarter-Kelly)
    double max_risk_pct      = 0.02;   // max 2% equity per trade
    double min_lot           = 0.01;   // minimum lot size
    double max_lot           = 5.0;    // maximum lot size
    double base_lot          = 0.10;   // fallback lot size
    double passive_threshold = 0.35;   // conviction for limit orders
    double aggro_threshold   = 0.60;   // conviction for market orders
    double pip_value         = 10.0;   // USD per pip for 1 standard lot XAU
};

// ---------------------------------------------------------------------------
// SizingResult — output of Kelly calculation
// ---------------------------------------------------------------------------
struct SizingResult {
    double    lot_size;
    OrderType order_type;
    double    raw_kelly_f;     // unscaled Kelly fraction
    double    risk_amount_usd; // dollar risk for this trade
};

// ---------------------------------------------------------------------------
// RiskEngine — Hardware-gated position sizing
// ---------------------------------------------------------------------------
class RiskEngine {
public:
    static constexpr double MAX_NOTIONAL_PER_ORDER = 1000000.0; // 1M USD limit
    static constexpr int    MAX_ORDERS_PER_SECOND  = 5;
    static constexpr double FAT_FINGER_PRICE_BOUND = 0.03;      // 3% max deviation

    // Control Plane Safety Boundaries
    static constexpr double MAX_ALLOWABLE_RISK_PCT = 5.0;
    static constexpr double MIN_TP_MULTIPLIER = 0.10;

    explicit RiskEngine(const KellyConfig& cfg = {}) noexcept : cfg_(cfg) {}

    // -----------------------------------------------------------------------
    // sync_parameters — strictly validates updates from RL cluster
    // -----------------------------------------------------------------------
    void sync_parameters(const DynamicParams* p) noexcept {
        if (!p) return;
        
        uint64_t new_version = p->version.load(std::memory_order_acquire);
        if (new_version > last_version_id_) {
            double new_risk = p->risk_pct.load(std::memory_order_acquire);
            if (new_risk > MAX_ALLOWABLE_RISK_PCT) [[unlikely]] {
                std::printf("[SAFETY BREACH] Rejected risk_pct %.2f%% > %.2f%%\n", new_risk, MAX_ALLOWABLE_RISK_PCT);
                new_risk = MAX_ALLOWABLE_RISK_PCT;
            }
            
            double new_tp = p->tp_multiplier.load(std::memory_order_acquire);
            if (new_tp < MIN_TP_MULTIPLIER) [[unlikely]] {
                std::printf("[SAFETY BREACH] Rejected tp_multiplier %.2f < %.2f\n", new_tp, MIN_TP_MULTIPLIER);
                new_tp = MIN_TP_MULTIPLIER;
            }

            cfg_.max_risk_pct = new_risk / 100.0;
            last_version_id_ = new_version;
            std::printf("[CONTROL-PLANE] Applied valid RL parameters (version %llu)\n", (unsigned long long)new_version);
        }
    }

    // -----------------------------------------------------------------------
    // update_market_state — tracks EMA for fat-finger detection
    // -----------------------------------------------------------------------
    void update_market_state(double mid_price) noexcept {
        if (ema_price_ < 1e-8) {
            ema_price_ = mid_price;
        } else {
            // 50-tick EMA approximation (alpha = 2 / 51 = ~0.039)
            ema_price_ = ema_price_ * 0.961 + mid_price * 0.039;
        }
    }

    // -----------------------------------------------------------------------
    // compute_sizing — Fractional Kelly with hard caps
    //
    // Inputs:
    //   conviction : net conviction score from bitmask evaluation
    //   win_rate   : rolling empirical win probability [0, 1]
    //   avg_win    : average winning trade size (dollars)
    //   avg_loss   : average losing trade size (dollars, positive)
    //   equity     : current account equity
    //   atr        : current ATR for stop-loss calibration
    // -----------------------------------------------------------------------
    [[nodiscard]] SizingResult compute_sizing(
            double conviction,
            double win_rate,
            double avg_win,
            double avg_loss,
            double equity,
            double atr,
            const DynamicParams* params) const noexcept {

        SizingResult result{};
        
        // ML Confidence Gate (Module 4)
        if (params && params->p_profitable_gate_bps.load(std::memory_order_relaxed) < 6000) [[unlikely]] {
            std::printf("[ML-GATE] Rejected: P(profitable) < 60.00%%\n");
            result.order_type = OrderType::NONE;
            result.lot_size = 0.0;
            return result;
        }

        // Fat-Finger Check (Module 3)
        // Note: entry_price not passed here directly, assuming we check notional instead or 
        // we can check if conviction is insanely high without merit. 
        // We'll enforce fat-finger in `validate_execution` instead, but for now we enforce 
        // MAX_NOTIONAL_PER_ORDER below.

        result.order_type = route_order(conviction);

        if (result.order_type == OrderType::NONE) {
            result.lot_size = 0.0;
            return result;
        }

        // Kelly formula: f* = (p * b - q) / b
        // where p = win_rate, q = 1-p, b = avg_win/avg_loss (odds ratio)
        const double p = std::clamp(win_rate, 0.01, 0.99);
        const double q = 1.0 - p;
        const double b = (avg_loss > 1e-8) ? avg_win / avg_loss : 1.0;

        double kelly_f = (p * b - q) / b;
        result.raw_kelly_f = kelly_f;

        // Apply fractional Kelly (e.g., quarter-Kelly for risk reduction)
        kelly_f *= cfg_.kelly_fraction;

        // Ensure non-negative
        kelly_f = std::max(kelly_f, 0.0);

        // Scale by conviction: higher conviction → closer to full Kelly
        const double conviction_scale = std::clamp(std::abs(conviction), 0.0, 1.0);
        kelly_f *= conviction_scale;

        // Dollar risk = kelly_f * equity, capped at max_risk_pct
        double risk_usd = kelly_f * equity;
        const double max_risk_usd = cfg_.max_risk_pct * equity;
        risk_usd = std::min(risk_usd, max_risk_usd);
        result.risk_amount_usd = risk_usd;

        // Convert to lot size: risk_usd / (atr_stop_distance * pip_value)
        const double stop_distance = 1.5 * atr;
        if (stop_distance < 1e-8 || cfg_.pip_value < 1e-8) {
            result.lot_size = cfg_.base_lot;
            return result;
        }

        double lots = risk_usd / (stop_distance * cfg_.pip_value);

        // Clamp to min/max lot
        lots = std::clamp(lots, cfg_.min_lot, cfg_.max_lot);

        // Hard Physical Risk Gate: MAX_NOTIONAL_PER_ORDER
        const double notional_usd = lots * 100.0 * ema_price_; // roughly
        if (notional_usd > MAX_NOTIONAL_PER_ORDER) [[unlikely]] {
            std::printf("[RISK-GATE] Rejected: Notional $%.2f exceeds 1M USD limit\n", notional_usd);
            result.order_type = OrderType::NONE;
            result.lot_size = 0.0;
            return result;
        }

        result.lot_size = lots;
        return result;
    }

    // -----------------------------------------------------------------------
    // validate_execution — final pre-trade physical gates
    // -----------------------------------------------------------------------
    [[nodiscard]] bool validate_execution(double entry_price, uint64_t timestamp_ns) noexcept {
        // Fat Finger Price Bound
        if (ema_price_ > 1e-8) {
            const double deviation = std::abs(entry_price - ema_price_) / ema_price_;
            if (deviation > FAT_FINGER_PRICE_BOUND) [[unlikely]] {
                std::printf("[FAT-FINGER] Rejected: Price %.2f deviates %.2f%% from EMA %.2f\n", 
                            entry_price, deviation * 100.0, ema_price_);
                return false;
            }
        }

        // Orders per second rate-limiting
        uint64_t current_sec = timestamp_ns / 1'000'000'000ULL;
        if (current_sec == last_order_sec_) {
            if (++orders_this_sec_ > MAX_ORDERS_PER_SECOND) [[unlikely]] {
                std::printf("[RATE-LIMIT] Rejected: Exceeded %d orders per second\n", MAX_ORDERS_PER_SECOND);
                return false;
            }
        } else {
            last_order_sec_ = current_sec;
            orders_this_sec_ = 1;
        }

        return true;
    }

    // -----------------------------------------------------------------------
    // route_order — conviction-based execution type selection
    // -----------------------------------------------------------------------
    [[nodiscard]] OrderType route_order(double conviction) const noexcept {
        const double abs_conv = std::abs(conviction);
        // Branchless selection using comparison masks
        const bool is_aggro   = (abs_conv >= cfg_.aggro_threshold);
        const bool is_passive = (abs_conv >= cfg_.passive_threshold);

        // aggro=true → MARKET, passive=true && !aggro → LIMIT, else NONE
        return is_aggro   ? OrderType::MARKET_AGGRO :
               is_passive ? OrderType::LIMIT_PASSIVE :
                            OrderType::NONE;
    }

    // -----------------------------------------------------------------------
    // Getters
    // -----------------------------------------------------------------------
    [[nodiscard]] const KellyConfig& cfg() const noexcept { return cfg_; }

private:
    KellyConfig cfg_;
    double ema_price_ = 0.0;
    uint64_t last_order_sec_ = 0;
    int orders_this_sec_ = 0;
    uint64_t last_version_id_ = 0;
};

} // namespace goldmine
