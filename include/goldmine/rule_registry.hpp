#pragma once
// =============================================================================
// rule_registry.hpp — Scalable Rule Registration Framework
//
// Allows adding rules at init-time without modifying engine core code.
// Each rule is a function pointer — no virtual dispatch, no vtable lookup.
//
// Usage:
//   RuleRegistry reg;
//   reg.add_rule("squeeze_breakout_up", +0.35,
//       [](const MarketState& s, const IndicatorSnapshot& ind) -> bool {
//           return ind.squeeze_breakout_up;
//       });
//
//   // Hot path:
//   WideBitmask mask = reg.evaluate_all(state);
//   Score score = reg.score(mask, session_flags);
// =============================================================================

#include "goldmine/types.hpp"
#include "goldmine/bitmask_engine.hpp"
#include <cstdio>

namespace goldmine {

// ---------------------------------------------------------------------------
// RuleEvalFn — the function signature for every rule evaluator.
// No dynamic dispatch. The compiler can inline lambdas stored as fn ptrs.
// ---------------------------------------------------------------------------
using RuleEvalFn = bool(*)(const MarketState&, const IndicatorSnapshot&);

// ---------------------------------------------------------------------------
// RuleEntry — metadata for a single registered rule
// ---------------------------------------------------------------------------
struct RuleEntry {
    const char*  name      = nullptr;
    double       weight    = 0.0;
    RuleEvalFn   evaluate  = nullptr;
    std::size_t  bit_index = 0;
    bool         active    = false;
};

// ---------------------------------------------------------------------------
// RuleRegistry — pre-allocated registry for up to MAX_RULES rules.
//
// All memory is statically sized. add_rule() is init-time only.
// evaluate_all() and score() are the hot-path methods.
// ---------------------------------------------------------------------------
class RuleRegistry {
public:
    RuleRegistry() noexcept { weights_.fill(0.0); }

    // Non-copyable (owns pre-allocated arrays)
    RuleRegistry(const RuleRegistry&)            = delete;
    RuleRegistry& operator=(const RuleRegistry&) = delete;

    // -----------------------------------------------------------------------
    // add_rule — register a new rule. Returns assigned bit index.
    // Called at init-time ONLY. Not thread-safe.
    // -----------------------------------------------------------------------
    std::size_t add_rule(
            const char* name, double weight, RuleEvalFn fn) noexcept {
        if (count_ >= MAX_RULES) return MAX_RULES;  // overflow guard

        const std::size_t idx = count_;
        rules_[idx] = {name, weight, fn, idx, true};
        weights_[idx] = weight;
        ++count_;
        return idx;
    }

    // -----------------------------------------------------------------------
    // evaluate_all — branchless evaluation of all registered rules.
    //
    // Each rule's boolean result is shifted into its bit position using
    // WideBitmask::set_if(), which compiles to a single OR instruction
    // with no branch. For 1000 rules, this is ~1000 OR instructions on
    // the same cache-hot WideBitmask — fully pipelined by the CPU.
    //
    // THIS IS THE HOT PATH.
    // -----------------------------------------------------------------------
    [[nodiscard]] WideBitmask evaluate_all(
            const MarketState& state) const noexcept {

        WideBitmask mask{};
        const IndicatorSnapshot& ind = state.indicators;

        for (std::size_t i = 0; i < count_; ++i) {
            // Branchless: set_if compiles to conditional OR, no branch
            mask.set_if(i, rules_[i].evaluate(state, ind));
        }
        return mask;
    }

    // -----------------------------------------------------------------------
    // score — dot-product active bits against weight vector
    // Uses AVX2-optimized sparse iteration from bitmask_ops.
    // -----------------------------------------------------------------------
    [[nodiscard]] Score score(
            const WideBitmask& mask,
            const SessionFlags& sess) const noexcept {

        const Score raw = bitmask_ops::score_mask(mask, weights_.data());
        return bitmask_ops::apply_session_scaling(raw, sess);
    }

    // -----------------------------------------------------------------------
    // Accessors
    // -----------------------------------------------------------------------
    [[nodiscard]] std::size_t count()                 const noexcept { return count_; }
    [[nodiscard]] const RuleEntry& rule(std::size_t i) const noexcept { return rules_[i]; }
    [[nodiscard]] const double* weights()             const noexcept { return weights_.data(); }
    [[nodiscard]] double* mutable_weights()                 noexcept { return weights_.data(); }

    // -----------------------------------------------------------------------
    // load_weights_from_file — override default weights with calibrated values
    //
    // Binary format: array of double[count_], little-endian.
    // Returns true on success. Called at init-time only.
    // -----------------------------------------------------------------------
    bool load_weights_from_file(const char* path) noexcept {
        FILE* f = std::fopen(path, "rb");
        if (!f) return false;

        // Read exactly count_ doubles
        std::size_t read = std::fread(weights_.data(), sizeof(double), count_, f);
        std::fclose(f);

        if (read != count_) return false;

        // Sync rule entries
        for (std::size_t i = 0; i < count_; ++i) {
            rules_[i].weight = weights_[i];
        }
        return true;
    }

private:
    std::array<RuleEntry, MAX_RULES>  rules_{};
    alignas(32) std::array<double, MAX_RULES> weights_{};  // contiguous for SIMD scoring
    std::size_t count_ = 0;
};

// ===========================================================================
// register_default_gold_rules — the 25 Gold-specific archetypes
//
// This is called once at startup. Each rule is a stateless lambda
// stored as a function pointer — zero overhead vs hand-coded if-blocks.
// ===========================================================================
inline void register_default_gold_rules(
        RuleRegistry& reg, const EngineConfig& cfg) noexcept {

    // --- Session ---
    reg.add_rule("session_overlap", 0.0,
        [](const MarketState& s, const IndicatorSnapshot&) -> bool {
            return classify_session(s.utc_sod_s).is_overlap;
        });
    reg.add_rule("session_asian", 0.0,
        [](const MarketState& s, const IndicatorSnapshot&) -> bool {
            return classify_session(s.utc_sod_s).is_asian;
        });
    reg.add_rule("session_london_only", 0.0,
        [](const MarketState& s, const IndicatorSnapshot&) -> bool {
            return classify_session(s.utc_sod_s).is_london_only;
        });

    // --- Volatility Squeeze ---
    reg.add_rule("squeeze_active", 0.0,
        [](const MarketState&, const IndicatorSnapshot& ind) -> bool {
            return ind.squeeze_active;
        });
    reg.add_rule("squeeze_break_up", +0.35,
        [](const MarketState&, const IndicatorSnapshot& ind) -> bool {
            return ind.squeeze_breakout_up;
        });
    reg.add_rule("squeeze_break_down", -0.35,
        [](const MarketState&, const IndicatorSnapshot& ind) -> bool {
            return ind.squeeze_breakout_down;
        });

    // --- Z-Score ---
    reg.add_rule("zscore_overbought", -0.20,
        [](const MarketState&, const IndicatorSnapshot& ind) -> bool {
            return ind.zscore > 2.5;  // uses default threshold
        });
    reg.add_rule("zscore_oversold", +0.20,
        [](const MarketState&, const IndicatorSnapshot& ind) -> bool {
            return ind.zscore < -2.5;
        });

    // --- RSI ---
    reg.add_rule("rsi_overbought", -0.15,
        [](const MarketState&, const IndicatorSnapshot& ind) -> bool {
            return ind.rsi14 > 75.0;
        });
    reg.add_rule("rsi_oversold", +0.15,
        [](const MarketState&, const IndicatorSnapshot& ind) -> bool {
            return ind.rsi14 < 25.0;
        });

    // --- VWAP ---
    reg.add_rule("vwap_above", +0.05,
        [](const MarketState& s, const IndicatorSnapshot& ind) -> bool {
            return s.last_tick.last > ind.vwap && ind.vwap > 0.0;
        });
    reg.add_rule("vwap_below", -0.05,
        [](const MarketState& s, const IndicatorSnapshot& ind) -> bool {
            return s.last_tick.last < ind.vwap && ind.vwap > 0.0;
        });
    reg.add_rule("vwap_extreme_up", -0.25,
        [](const MarketState& s, const IndicatorSnapshot& ind) -> bool {
            return ind.zscore > 2.5 && s.last_tick.last > ind.vwap;
        });
    reg.add_rule("vwap_extreme_down", +0.25,
        [](const MarketState& s, const IndicatorSnapshot& ind) -> bool {
            return ind.zscore < -2.5 && s.last_tick.last < ind.vwap;
        });

    // --- Cross-Asset Divergence ---
    reg.add_rule("dxy_drop_xau_flat", -0.30,
        [](const MarketState&, const IndicatorSnapshot& ind) -> bool {
            return ind.dxy_delta_15m < -0.20 && ind.xau_delta_15m < 0.30
                   && ind.corr_xau_dxy > -0.30;
        });
    reg.add_rule("dxy_rise_xau_flat", +0.15,
        [](const MarketState&, const IndicatorSnapshot& ind) -> bool {
            return ind.dxy_delta_15m > 0.30 && ind.xau_delta_15m > 0.30;
        });
    reg.add_rule("corr_break_bear", -0.20,
        [](const MarketState&, const IndicatorSnapshot& ind) -> bool {
            return ind.corr_xau_dxy > -0.30 && ind.dxy_delta_15m < -0.20;
        });
    reg.add_rule("corr_break_bull", +0.20,
        [](const MarketState&, const IndicatorSnapshot& ind) -> bool {
            return ind.corr_xau_dxy > -0.30 && ind.dxy_delta_15m >= -0.20;
        });

    // --- ATR Risk Gate ---
    reg.add_rule("atr_stop_too_wide", 0.0,
        [](const MarketState& s, const IndicatorSnapshot& ind) -> bool {
            return s.last_tick.spread() > 1.5 * ind.atr14 && ind.atr14 > 0.0;
        });

    // --- Trend ---
    reg.add_rule("price_above_20ema", +0.10,
        [](const MarketState& s, const IndicatorSnapshot& ind) -> bool {
            return s.last_tick.last > ind.kc_mid && ind.kc_mid > 0.0;
        });
    reg.add_rule("price_below_20ema", -0.10,
        [](const MarketState& s, const IndicatorSnapshot& ind) -> bool {
            return s.last_tick.last < ind.kc_mid && ind.kc_mid > 0.0;
        });
    reg.add_rule("atr_expanding", +0.05,
        [](const MarketState&, const IndicatorSnapshot& ind) -> bool {
            return ind.atr_expanding;
        });
    reg.add_rule("atr_contracting", -0.05,
        [](const MarketState&, const IndicatorSnapshot& ind) -> bool {
            return !ind.atr_expanding;
        });

    // --- Rubber Band Composites ---
    reg.add_rule("rubber_band_long", +0.40,
        [](const MarketState& s, const IndicatorSnapshot& ind) -> bool {
            return ind.zscore < -2.5 && ind.rsi14 < 25.0
                   && s.last_tick.last < ind.vwap && ind.vwap > 0.0;
        });
    reg.add_rule("rubber_band_short", -0.40,
        [](const MarketState& s, const IndicatorSnapshot& ind) -> bool {
            return ind.zscore > 2.5 && ind.rsi14 > 75.0
                   && s.last_tick.last > ind.vwap;
        });

    // Suppress unused parameter warning for cfg (thresholds baked into lambdas above)
    (void)cfg;
}

} // namespace goldmine
