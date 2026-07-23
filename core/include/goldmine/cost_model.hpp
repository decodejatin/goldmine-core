#pragma once
// =============================================================================
// cost_model.hpp — Zero-Allocation Transaction Cost Model
//
// Computes the true hurdle rate for every trade. A signal is worthless if its
// TP cannot clear the round-trip cost of:
//   1. Exchange fees (maker + taker, Binance VIP 0 default)
//   2. Dynamic slippage (base + volume-penalty for large orders)
//   3. Half-spread cost (priced from live bid/ask EMA)
//
// All functions are constexpr-capable, noexcept, zero-allocation.
// =============================================================================
#include <algorithm>
#include <cmath>

namespace goldmine {

struct CostModel {
    // -- Exchange fee schedule (basis points = 1/100th of 1%) -----------------
    // Binance VIP 0:  Maker 0.1000%, Taker 0.1000%
    // With BNB:       Maker 0.0750%, Taker 0.0750%
    double maker_fee_bps     = 10.0;
    double taker_fee_bps     = 10.0;
    double bnb_discount      = 0.75;   // 25% discount when paying with BNB
    bool   use_bnb           = false;

    // -- Dynamic slippage model -----------------------------------------------
    // slippage(qty) = base_slippage_bps + volume_penalty_bps_per_unit × qty
    //
    // Rationale: larger orders eat deeper into the order book, suffering
    // progressively worse fills. For PAXG/USDT on Binance, typical book depth
    // is thin — a 0.01 PAXG order has negligible impact, but a 1.0 PAXG order
    // will move the market measurably.
    double base_slippage_bps         = 1.5;   // ~1.5 bps baseline slippage
    double volume_penalty_bps_per_unit = 50.0; // +50 bps per full PAXG unit

    // -- Spread cost ----------------------------------------------------------
    // Dynamically tracked from live bid/ask via EMA. Represents half the spread
    // cost since we cross it once on entry (taker) and once on exit.
    double spread_cost_bps   = 0.0;

    // =========================================================================
    // Fee calculations
    // =========================================================================

    [[nodiscard]] double effective_maker_bps() const noexcept {
        return use_bnb ? maker_fee_bps * bnb_discount : maker_fee_bps;
    }

    [[nodiscard]] double effective_taker_bps() const noexcept {
        return use_bnb ? taker_fee_bps * bnb_discount : taker_fee_bps;
    }

    // -------------------------------------------------------------------------
    // slippage_bps — volume-dependent slippage estimate
    //
    // Small orders (~0.001 PAXG):  ~1.5 bps
    // Medium orders (~0.01 PAXG):  ~2.0 bps
    // Large orders (~0.1 PAXG):    ~6.5 bps
    // -------------------------------------------------------------------------
    [[nodiscard]] double slippage_bps(double qty) const noexcept {
        return base_slippage_bps + volume_penalty_bps_per_unit * std::abs(qty);
    }

    // -------------------------------------------------------------------------
    // total_round_trip_bps — all costs for a complete open → close cycle
    //
    // Entry: taker fee + slippage(qty) + half-spread
    // Exit:  maker fee + slippage(qty) + half-spread
    // -------------------------------------------------------------------------
    [[nodiscard]] double total_round_trip_bps(double qty) const noexcept {
        const double slip = slippage_bps(qty);
        return effective_taker_bps()    // entry fee
             + effective_maker_bps()    // exit fee
             + 2.0 * slip              // slippage both legs
             + spread_cost_bps;        // spread crossing cost
    }

    // -------------------------------------------------------------------------
    // round_trip_cost_usd — total fees in USD for a given entry + qty
    // -------------------------------------------------------------------------
    [[nodiscard]] double round_trip_cost_usd(double entry_price,
                                              double qty) const noexcept {
        const double notional = entry_price * std::abs(qty);
        return notional * total_round_trip_bps(qty) / 10000.0;
    }

    // =========================================================================
    // min_profit_price — the absolute minimum price movement required to
    //                    break even after all fees and slippage.
    //
    // This is the HURDLE RATE. If the signal's TP distance is less than
    // this value, the trade should be rejected.
    //
    // Derivation:
    //   gross_profit = qty × Δprice
    //   fees         = notional × total_round_trip_bps / 10000
    //   breakeven:  qty × Δprice = notional × bps / 10000
    //               Δprice = entry_price × bps / 10000
    // =========================================================================
    [[nodiscard]] double min_profit_price(double entry_price,
                                           double qty) const noexcept {
        if (std::abs(qty) < 1e-15 || entry_price < 1e-8) return 1e9;
        // Δprice = entry_price × total_bps / 10000
        return entry_price * total_round_trip_bps(qty) / 10000.0;
    }

    // =========================================================================
    // net_pnl — profit/loss after deducting all transaction costs
    //
    // Used at trade close to compute the exact realised PnL.
    // =========================================================================
    [[nodiscard]] double net_pnl(double entry_price,
                                  double exit_price,
                                  double qty,
                                  bool is_long) const noexcept {
        const double gross = is_long
            ? (exit_price - entry_price) * qty
            : (entry_price - exit_price) * qty;
        return gross - round_trip_cost_usd(entry_price, qty);
    }

    // =========================================================================
    // update_spread_cost — call every tick to maintain live spread EMA
    //
    // Alpha = 0.02 → roughly 50-tick half-life for stable tracking.
    // =========================================================================
    void update_spread_cost(double bid, double ask,
                            double mid_price) noexcept {
        if (mid_price > 1e-8) {
            const double live_bps = ((ask - bid) / mid_price) * 10000.0;
            spread_cost_bps = spread_cost_bps < 1e-12
                ? live_bps                                     // cold start
                : 0.02 * live_bps + 0.98 * spread_cost_bps;   // EMA
        }
    }
};

} // namespace goldmine
