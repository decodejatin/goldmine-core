#pragma once
// =============================================================================
// online_learner.hpp — O(N) Zero-Allocation Online Weight Calibration
//
// Implements Stochastic Gradient Descent (SGD) with L2 decay on the hot path.
// Thread 4 pushes completed trade outcomes; Thread 3 reads them to update
// the live weight vector W_t in-place — no locks, no allocation.
//
// Update rule:
//   W_t = W_{t-1} + η · (y_t - W_{t-1}^T · X_t) · X_t - λ · W_{t-1}
//
// Where X_t is the active WideBitmask (binary features), y_t ∈ {-1, +1}.
// =============================================================================
#include "goldmine/types.hpp"
#include "goldmine/lockfree_queue.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <immintrin.h>

namespace goldmine {

// ---------------------------------------------------------------------------
// TradeOutcome — feedback from Thread 4 → Thread 3 via SPSC queue00000
// ---------------------------------------------------------------------------
struct alignas(CACHE_LINE) TradeOutcome {
    double      label;          // +1.0 = profitable, -1.0 = loss
    WideBitmask active_rules;   // X_t: which rules fired
    double      conviction;     // raw score at signal time
    Nanos       close_ns;       // when the trade was closed
};

// ---------------------------------------------------------------------------
// OnlineLearner — SGD weight updater, zero-allocation, O(N) per update
// ---------------------------------------------------------------------------
class OnlineLearner {
public:
    static constexpr double DEFAULT_LR     = 0.001;  // learning rate η
    static constexpr double DEFAULT_DECAY  = 0.0001; // L2 regularization λ
    static constexpr double MAX_WEIGHT     = 2.0;    // clamp to prevent divergence
    static constexpr double MIN_WEIGHT     = -2.0;

    OnlineLearner() noexcept { weights_.fill(0.0); }

    void set_learning_rate(double lr) noexcept { lr_ = lr; }
    void set_decay(double lambda) noexcept { decay_ = lambda; }

    // -----------------------------------------------------------------------
    // initialize — copy default weights from RuleRegistry at startup
    // -----------------------------------------------------------------------
    void initialize(const double* default_weights, std::size_t count) noexcept {
        count_ = std::min(count, MAX_RULES);
        for (std::size_t i = 0; i < count_; ++i) {
            weights_[i] = default_weights[i];
        }
    }

    // -----------------------------------------------------------------------
    // update — O(N) SGD step on a single trade outcome
    //
    // Iterates only set bits in the bitmask (sparse), making this O(k)
    // where k = number of active rules, typically 10-30 out of 120.
    // -----------------------------------------------------------------------
    void update(const TradeOutcome& outcome) noexcept {
        const double y = outcome.label;

        // Compute prediction: W^T · X (dot product over active bits)
        double prediction = 0.0;
        for (std::size_t w = 0; w < WideBitmask::NUM_WORDS; ++w) {
            std::uint64_t word = outcome.active_rules.words[w];
            while (word) {
                const int bit = __builtin_ctzll(word);
                const std::size_t idx = w * 64 + static_cast<std::size_t>(bit);
                if (idx < count_) {
                    prediction += weights_[idx];
                }
                word &= word - 1;  // clear lowest set bit
            }
        }

        // Error signal
        const double error = y - prediction;

        // Update weights: w_i += η · error · x_i - λ · w_i
        // Only update weights for active rules (sparse)
        for (std::size_t w = 0; w < WideBitmask::NUM_WORDS; ++w) {
            std::uint64_t word = outcome.active_rules.words[w];
            while (word) {
                const int bit = __builtin_ctzll(word);
                const std::size_t idx = w * 64 + static_cast<std::size_t>(bit);
                if (idx < count_) {
                    weights_[idx] += lr_ * error - decay_ * weights_[idx];
                    // Clamp to prevent divergence
                    weights_[idx] = std::clamp(weights_[idx], MIN_WEIGHT, MAX_WEIGHT);
                }
                word &= word - 1;
            }
        }

        // Apply decay to ALL weights (dense, rare — every 100 updates)
        ++update_count_;
        if ((update_count_ & 127) == 0) {
            for (std::size_t i = 0; i < count_; ++i) {
                weights_[i] *= (1.0 - decay_);
            }
        }
    }

    // -----------------------------------------------------------------------
    // apply_to — copy online weights back into the scoring weight array
    //
    // Called by Thread 3 after processing outcomes. The RuleRegistry
    // scores against this weight vector via bitmask_ops::score_mask().
    // -----------------------------------------------------------------------
    void apply_to(double* target_weights, std::size_t count) const noexcept {
        const std::size_t n = std::min(count, count_);
        for (std::size_t i = 0; i < n; ++i) {
            target_weights[i] = weights_[i];
        }
    }

    [[nodiscard]] const double* weights()     const noexcept { return weights_.data(); }
    [[nodiscard]] std::size_t   update_count() const noexcept { return update_count_; }

private:
    alignas(32) std::array<double, MAX_RULES> weights_{};
    std::size_t count_        = 0;
    std::size_t update_count_ = 0;
    double      lr_           = DEFAULT_LR;
    double      decay_        = DEFAULT_DECAY;
};

// SPSC queue for trade outcomes: Thread 4 → Thread 3
using OutcomeQueue = SpscQueue<TradeOutcome, 256>;

} // namespace goldmine
