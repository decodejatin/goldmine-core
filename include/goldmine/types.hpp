#pragma once
// =============================================================================
// types.hpp — Foundation types for the Goldmine XAU/USD Expert System
// =============================================================================

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace goldmine {

// ---------------------------------------------------------------------------
// Primitive aliases
// ---------------------------------------------------------------------------
using Price    = double;
using Volume   = double;
using Nanos    = std::int64_t;
using Score    = double;

// ---------------------------------------------------------------------------
// Compile-time constants
// ---------------------------------------------------------------------------
inline constexpr std::size_t CACHE_LINE      = 64;
inline constexpr std::size_t TICK_BUFFER_SZ  = 4096;
inline constexpr std::size_t INDICATOR_WIN   = 512;
inline constexpr std::size_t ATR_PERIOD      = 14;
inline constexpr std::size_t RSI_PERIOD      = 14;
inline constexpr std::size_t CORR_WINDOW     = 15 * 60;

// Bitmask scaling — 16 words × 64 bits = 1024 rule capacity
inline constexpr std::size_t BITMASK_WORDS   = 16;
inline constexpr std::size_t MAX_RULES       = BITMASK_WORDS * 64;

static_assert((TICK_BUFFER_SZ & (TICK_BUFFER_SZ - 1)) == 0);
static_assert(INDICATOR_WIN % 8 == 0);

// ---------------------------------------------------------------------------
// Session windows (UTC seconds-of-day)
// ---------------------------------------------------------------------------
namespace session {
    inline constexpr Nanos ASIAN_OPEN  = 0  * 3600;
    inline constexpr Nanos ASIAN_CLOSE = 6  * 3600;
    inline constexpr Nanos LDN_OPEN   = 8  * 3600;
    inline constexpr Nanos NY_CLOSE   = 21 * 3600;
    inline constexpr Nanos OVERLAP_S  = 13 * 3600;
    inline constexpr Nanos OVERLAP_E  = 17 * 3600;
    // ICT Silver Bullet window (10:00-11:00 AM EST = 14:00-15:00 UTC)
    inline constexpr Nanos ICT_SB_S   = 14 * 3600;
    inline constexpr Nanos ICT_SB_E   = 15 * 3600;
    // Late NY fade (19:00-21:00 UTC)
    inline constexpr Nanos LATE_NY_S  = 19 * 3600;
    inline constexpr Nanos LATE_NY_E  = 21 * 3600;
}

// ---------------------------------------------------------------------------
// WideBitmask — scalable rule bitmask (basic ops; SIMD ops in bitmask_engine)
// ---------------------------------------------------------------------------
struct alignas(32) WideBitmask {
    static constexpr std::size_t NUM_WORDS = BITMASK_WORDS;
    static constexpr std::size_t NUM_BITS  = NUM_WORDS * 64;

    std::array<std::uint64_t, NUM_WORDS> words{};

    void set(std::size_t bit) noexcept {
        words[bit >> 6] |= (1ULL << (bit & 63));
    }
    void clear(std::size_t bit) noexcept {
        words[bit >> 6] &= ~(1ULL << (bit & 63));
    }
    [[nodiscard]] bool test(std::size_t bit) const noexcept {
        return (words[bit >> 6] >> (bit & 63)) & 1ULL;
    }
    void reset() noexcept { words.fill(0); }

    [[nodiscard]] std::size_t popcount() const noexcept {
        std::size_t c = 0;
        for (auto w : words) c += static_cast<std::size_t>(__builtin_popcountll(w));
        return c;
    }

    // Branchless conditional set: set bit if cond is true
    void set_if(std::size_t bit, bool cond) noexcept {
        words[bit >> 6] |= (static_cast<std::uint64_t>(cond) << (bit & 63));
    }
};

static_assert(sizeof(WideBitmask) == BITMASK_WORDS * 8);

// ---------------------------------------------------------------------------
// Tick — atomic unit of market data (40 bytes)
// ---------------------------------------------------------------------------
struct alignas(8) Tick {
    Nanos   timestamp_ns;
    Price   bid;
    Price   ask;
    Volume  volume;
    Price   last;

    [[nodiscard]] constexpr Price mid()    const noexcept { return (bid + ask) * 0.5; }
    [[nodiscard]] constexpr Price spread() const noexcept { return ask - bid; }
};
static_assert(sizeof(Tick) == 40);

// ---------------------------------------------------------------------------
// DXY Tick — US Dollar Index co-feed
// ---------------------------------------------------------------------------
struct alignas(8) DxyTick {
    Nanos   timestamp_ns;
    Price   price;
    Volume  volume;
};

// ---------------------------------------------------------------------------
// SoA tick store — contiguous arrays for CPU prefetcher
// ---------------------------------------------------------------------------
template <std::size_t N>
struct alignas(CACHE_LINE) SoATickStore {
    alignas(CACHE_LINE) std::array<Nanos,  N> timestamps{};
    alignas(CACHE_LINE) std::array<Price,  N> bids{};
    alignas(CACHE_LINE) std::array<Price,  N> asks{};
    alignas(CACHE_LINE) std::array<Price,  N> lasts{};
    alignas(CACHE_LINE) std::array<Volume, N> volumes{};
    alignas(CACHE_LINE) std::array<Price,  N> dxy_prices{};
    alignas(CACHE_LINE) std::array<Price,  N> xag_prices{};
    alignas(CACHE_LINE) std::array<Price,  N> yield_10y{};

    std::size_t count = 0;

    constexpr void push(const Tick& t, Price dxy,
                        Price xag = 0.0, Price y10 = 0.0) noexcept {
        const std::size_t idx = count % N;
        timestamps[idx] = t.timestamp_ns;
        bids[idx]       = t.bid;
        asks[idx]       = t.ask;
        lasts[idx]      = t.last;
        volumes[idx]    = t.volume;
        dxy_prices[idx] = dxy;
        xag_prices[idx] = xag;
        yield_10y[idx]  = y10;
        ++count;
    }

    [[nodiscard]] constexpr std::size_t filled() const noexcept {
        return count < N ? count : N;
    }
};

using TickStore = SoATickStore<INDICATOR_WIN>;

// ---------------------------------------------------------------------------
// IndicatorSnapshot — computed by indicator thread, read by logic thread
// ---------------------------------------------------------------------------
struct alignas(CACHE_LINE) IndicatorSnapshot {
    // --- Phase 1 indicators ---
    Price vwap = 0.0; Price vwap_deviation = 0.0; Price atr14 = 0.0;
    Price bb_upper = 0.0; Price bb_lower = 0.0; Price bb_mid = 0.0; Price bb_width = 0.0;
    Price kc_upper = 0.0; Price kc_lower = 0.0; Price kc_mid = 0.0; Price kc_width = 0.0;
    Price zscore = 0.0; Price rsi14 = 50.0;
    Price corr_xau_dxy = 0.0; Price dxy_delta_15m = 0.0; Price xau_delta_15m = 0.0;
    bool squeeze_active = false; bool squeeze_breakout_up = false;
    bool squeeze_breakout_down = false; bool atr_expanding = false;

    // --- Phase 2: Probability & Distributions ---
    double skewness_20       = 0.0;  // rolling skewness of returns
    double kurtosis_20       = 0.0;  // rolling excess kurtosis
    double return_mean       = 0.0;  // mean of tick-to-tick returns
    double return_stddev     = 0.0;  // stddev of tick returns
    double return_99pct      = 0.0;  // 99th percentile return value
    double return_01pct      = 0.0;  // 1st percentile return value
    double tail_ratio        = 1.0;  // upper_tail / lower_tail mass
    double last_return       = 0.0;  // most recent tick return

    // --- Phase 2: Calculus & Kinematics ---
    double price_velocity    = 0.0;  // dp/dt (first derivative, EMA-smoothed)
    double price_acceleration= 0.0;  // d²p/dt² (second derivative)
    double price_jerk        = 0.0;  // d³p/dt³ (third derivative)
    double vwap_gradient     = 0.0;  // slope of VWAP over window
    double prev_velocity     = 0.0;  // for zero-crossing detection

    // --- Phase 2: Cross-Asset ---
    double gold_silver_ratio = 0.0;  // XAU / XAG
    double gs_ratio_zscore   = 0.0;  // Z-score of G/S ratio
    double gs_ratio_velocity = 0.0;  // d(ratio)/dt
    double ols_residual      = 0.0;  // Gold vs 10Y yield OLS residual
    double ols_slope         = 0.0;  // regression slope
    double ols_mse           = 0.0;  // mean squared error
    double xag_last          = 0.0;  // latest silver price
    double yield_last        = 0.0;  // latest 10Y yield

    // --- Phase 2: Regime & GARCH ---
    double garch_vol         = 0.0;  // GARCH(1,1) conditional vol forecast
    double prev_garch_vol    = 0.0;  // previous forecast (for direction)
    double hurst_exponent    = 0.5;  // Hurst exponent [0,1]
    double vol_of_vol        = 0.0;  // volatility of ATR (meta-vol)
    double realized_vol_20   = 0.0;  // 20-tick realized volatility
    double trend_strength    = 0.0;  // directional movement strength [0,100]
    double squeeze_intensity = 0.0;  // bb_width / kc_width ratio

    // --- Session microstructure ---
    Price  asian_high       = 0.0;
    Price  asian_low        = 1e9;
    bool   asian_range_set  = false;
    Price  opening_range_hi = 0.0;
    Price  opening_range_lo = 1e9;
    bool   opening_range_set= false;
    double vol_mean_20      = 0.0;  // 20-tick average volume

    // --- Phase 4: Order Flow Microstructure ---
    double ofi_cumulative    = 0.0;  // Order Flow Imbalance (20-tick sum)
    double ofi_stddev        = 0.0;  // OFI rolling stddev
    double ofi_zscore        = 0.0;  // OFI z-score
    double ofi_acceleration  = 0.0;  // d(OFI)/dt
    double voi_cumulative    = 0.0;  // Volume Order Imbalance
    double voi_velocity      = 0.0;  // d(VOI)/dt
    double micro_price       = 0.0;  // Volume-weighted imbalance price
    double micro_price_delta = 0.0;  // micro_price - mid_price

    Nanos computed_at_ns    = 0;
};

// ---------------------------------------------------------------------------
// MarketState — full snapshot per evaluation cycle
// ---------------------------------------------------------------------------
struct alignas(CACHE_LINE) MarketState {
    Tick              last_tick{};
    DxyTick           last_dxy{};
    IndicatorSnapshot indicators{};
    Nanos             utc_sod_s  = 0;
    bool              is_valid   = false;
};

// ---------------------------------------------------------------------------
// SessionFlags
// ---------------------------------------------------------------------------
struct SessionFlags {
    bool is_overlap;
    bool is_asian;
    bool is_london_only;
};

[[nodiscard]] inline SessionFlags classify_session(Nanos sod_s) noexcept {
    using namespace session;
    return {
        .is_overlap     = (sod_s >= OVERLAP_S && sod_s < OVERLAP_E),
        .is_asian       = (sod_s >= ASIAN_OPEN && sod_s < ASIAN_CLOSE),
        .is_london_only = (sod_s >= LDN_OPEN   && sod_s < OVERLAP_S),
    };
}

// ---------------------------------------------------------------------------
// Engine config — tunable thresholds
// ---------------------------------------------------------------------------
struct EngineConfig {
    double zscore_threshold        = 2.5;
    double rsi_ob                  = 75.0;
    double rsi_os                  = 25.0;
    double corr_break_threshold    = 0.30;
    double dxy_drop_threshold      = -0.20;
    double atr_stop_multiplier     = 1.5;
    double conviction_long_thresh  = 0.25;
    double conviction_short_thresh = -0.25;
};

// ---------------------------------------------------------------------------
// EngineSignal — output of one evaluation cycle
// ---------------------------------------------------------------------------
enum class Direction : std::int8_t { NONE = 0, LONG = 1, SHORT = -1 };

struct alignas(CACHE_LINE) EngineSignal {
    Direction   direction    = Direction::NONE;
    Score       conviction   = 0.0;
    WideBitmask active_rules{};
    bool        risk_blocked = false;
    Price       suggested_sl = 0.0;
    Price       suggested_tp = 0.0;
    Nanos       signal_ns    = 0;
};

// ---------------------------------------------------------------------------
// Time helpers
// ---------------------------------------------------------------------------
[[nodiscard]] inline Nanos utc_sod_seconds(Nanos epoch_ns) noexcept {
    return (epoch_ns / 1'000'000'000LL) % 86400LL;
}

// ---------------------------------------------------------------------------
// Paper Trading types (Thread 4 execution simulation)
// ---------------------------------------------------------------------------
struct PaperFill {
    Nanos       fill_ns       = 0;
    Direction   direction     = Direction::NONE;
    Price       entry_price   = 0.0;
    Price       sl_price      = 0.0;
    Price       tp_price      = 0.0;
    Score       conviction    = 0.0;
    WideBitmask rules_active{};
    double      slippage_bps  = 0.0;   // simulated slippage in basis points
    double      commission    = 0.0;   // simulated commission USD
};

struct PaperPosition {
    bool        is_open       = false;
    Direction   direction     = Direction::NONE;
    Price       entry_price   = 0.0;
    Price       sl_price      = 0.0;
    Price       tp_price      = 0.0;
    Nanos       open_ns       = 0;
    double      unrealized_pnl= 0.0;
    double      lot_size      = 0.0;
    WideBitmask open_rules{};
    double      open_conviction = 0.0;
};

inline constexpr std::size_t PAPER_LOG_SZ = 1024;

struct PaperTradeSummary {
    int         total_trades  = 0;
    int         winners       = 0;
    int         losers        = 0;
    double      total_pnl     = 0.0;
    double      max_drawdown  = 0.0;
    double      peak_equity   = 0.0;
    double      current_equity= 10000.0;  // starting capital USD
    double      avg_latency_ns= 0.0;
    Nanos       last_trade_ns = 0;
};

} // namespace goldmine
