#pragma once
// =============================================================================
// account_state.hpp — Dynamic Equity & Kelly-Inspired Sizing
//
// Tracks equity, peak equity, realized PnL, and computes position sizing using
// Kelly-inspired fractional risk logic. Implements a circuit breaker.
// Zero-allocation, hot-path ready.
// =============================================================================
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace goldmine {

struct alignas(64) AccountState {
    double starting_equity;
    double current_equity;
    double peak_equity;
    double realized_pnl;

    // The hard dollar floor at which trading is permanently halted
    double circuit_breaker_equity;

    // -------------------------------------------------------------------------
    // Constructor
    // -------------------------------------------------------------------------
    explicit AccountState(double start_eq = 10.0,
                          double breaker_eq = 7.0) noexcept
        : starting_equity(start_eq)
        , current_equity(start_eq)
        , peak_equity(start_eq)
        , realized_pnl(0.0)
        , circuit_breaker_equity(breaker_eq)
    {}

    // -------------------------------------------------------------------------
    // compute_position_size
    //
    // Kelly-criterion inspired sizing:
    // Risk amount = current_equity * risk_pct (where risk_pct is fractional Kelly)
    // Quantity = Risk Amount / SL Distance (price terms)
    // 
    // Caps the position size so we never exceed available equity (no leverage).
    // -------------------------------------------------------------------------
    [[nodiscard]] double compute_position_size(double entry_price,
                                                double sl_dist,
                                                double risk_pct) const noexcept {
        if (entry_price < 1e-8 || sl_dist < 1e-8 || risk_pct < 1e-8) return 0.0;

        // Dollar amount we are willing to lose if SL is hit
        const double risk_usd = current_equity * risk_pct;

        // Required quantity to lose exactly risk_usd if price drops by sl_dist
        double qty = risk_usd / sl_dist;

        // Cap at 1x leverage (no margin)
        const double max_qty = current_equity / entry_price;
        qty = std::min(qty, max_qty);

        // Ensure minimum precision
        return std::max(qty, 0.0);
    }

    // -------------------------------------------------------------------------
    // is_trading_halted
    //
    // Returns true if current equity drops below the circuit breaker threshold.
    // -------------------------------------------------------------------------
    [[nodiscard]] bool is_trading_halted() const noexcept {
        if (current_equity <= circuit_breaker_equity) [[unlikely]] {
            return true;
        }
        return false;
    }

    // -------------------------------------------------------------------------
    // record_trade
    //
    // Update equity metrics after a trade closes.
    // -------------------------------------------------------------------------
    void record_trade(double net_pnl) noexcept {
        current_equity += net_pnl;
        realized_pnl += net_pnl;
        if (current_equity > peak_equity) {
            peak_equity = current_equity;
        }
    }

    // -------------------------------------------------------------------------
    // drawdown_pct
    // -------------------------------------------------------------------------
    [[nodiscard]] double drawdown_pct() const noexcept {
        if (peak_equity < 1e-8) return 0.0;
        return ((peak_equity - current_equity) / peak_equity) * 100.0;
    }
};

} // namespace goldmine
