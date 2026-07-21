#pragma once
// =============================================================================
// feature_exporter.hpp — Binary feature export for walk-forward calibration
//
// Thread 3 exports per-tick records: timestamp, full WideBitmask state, and
// future return labels. A fixed ring buffer accumulates lookback for computing
// future returns without allocation.
// =============================================================================
#include "goldmine/types.hpp"
#include <array>
#include <cstdio>
#include <cstring>

namespace goldmine {

// ---------------------------------------------------------------------------
// FeatureRecord — one row of training data (exported to binary)
// ---------------------------------------------------------------------------
struct alignas(64) FeatureRecord {
    std::uint64_t timestamp_ns;
    WideBitmask   bitmask;            // 128 bytes (1024 bits)
    float         conviction;         // raw score from engine
    float         future_return_10t;  // % return 10 ticks ahead
    float         future_return_50t;  // % return 50 ticks ahead
    float         _pad = 0.0f;
};

// ---------------------------------------------------------------------------
// FeatureExporter — accumulates price lookback and writes features to disk
//
// Usage:
//   FeatureExporter exp("features.bin");
//   // In Thread 3 hot path:
//   exp.record_tick(price, timestamp, mask, conviction);
//   // At shutdown:
//   exp.flush();
// ---------------------------------------------------------------------------
inline constexpr std::size_t FEATURE_LOOKBACK = 64;  // must be > 50

class FeatureExporter {
public:
    FeatureExporter() = default;

    bool open(const char* path) noexcept {
        fp_ = std::fopen(path, "wb");
        return fp_ != nullptr;
    }

    // Called every tick from Thread 3. Stores current price/mask and
    // retroactively computes future returns for ticks 10 and 50 ago.
    void record_tick(Price price, Nanos ts, const WideBitmask& mask,
                     float conviction) noexcept {
        if (!fp_) return;

        const std::size_t idx = write_pos_ % FEATURE_LOOKBACK;

        // Store current tick data
        prices_[idx]   = price;
        stamps_[idx]   = ts;
        masks_[idx]    = mask;
        convictions_[idx] = conviction;

        ++write_pos_;

        // Once we have 50+ ticks of future data, export the tick from 50 ago
        if (write_pos_ >= FEATURE_LOOKBACK) {
            const std::size_t export_idx = (write_pos_ - FEATURE_LOOKBACK) % FEATURE_LOOKBACK;
            const Price base_price = prices_[export_idx];

            if (base_price > 1e-8) {
                FeatureRecord rec{};
                rec.timestamp_ns = static_cast<std::uint64_t>(stamps_[export_idx]);
                rec.bitmask      = masks_[export_idx];
                rec.conviction   = convictions_[export_idx];

                // 10-tick future return
                const std::size_t idx10 = (export_idx + 10) % FEATURE_LOOKBACK;
                rec.future_return_10t = static_cast<float>(
                    (prices_[idx10] - base_price) / base_price * 100.0);

                // 50-tick future return
                const std::size_t idx50 = (export_idx + 50) % FEATURE_LOOKBACK;
                rec.future_return_50t = static_cast<float>(
                    (prices_[idx50] - base_price) / base_price * 100.0);

                std::fwrite(&rec, sizeof(FeatureRecord), 1, fp_);
                ++records_written_;
            }
        }
    }

    void flush() noexcept {
        if (fp_) std::fflush(fp_);
    }

    void close() noexcept {
        if (fp_) { flush(); std::fclose(fp_); fp_ = nullptr; }
    }

    [[nodiscard]] std::size_t records_written() const noexcept { return records_written_; }

    ~FeatureExporter() { close(); }

private:
    FILE* fp_ = nullptr;
    std::size_t write_pos_ = 0;
    std::size_t records_written_ = 0;

    std::array<Price, FEATURE_LOOKBACK>        prices_{};
    std::array<Nanos, FEATURE_LOOKBACK>        stamps_{};
    std::array<WideBitmask, FEATURE_LOOKBACK>  masks_{};
    std::array<float, FEATURE_LOOKBACK>        convictions_{};
};

} // namespace goldmine
