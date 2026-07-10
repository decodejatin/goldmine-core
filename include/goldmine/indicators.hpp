
#pragma once
// =============================================================================
// indicators.hpp  — Heap-free, AVX2-vectorized technical indicators
//
// All calculations operate directly on SoATickStore arrays (contiguous memory)
// to maximize CPU prefetch efficiency and enable auto-vectorization.
//
// Indicators provided:
//   - VWAP (daily, cumulative)
//   - ATR (14-period, Wilder's smoothing)
//   - Bollinger Bands (20-period, 2 stddev)
//   - Keltner Channel (20-period EMA ± 1.5 ATR)
//   - Z-Score (rolling 20-period)
//   - RSI (14-period, Wilder's EMA variant)
//   - EMA (generic, used by KC and RSI)
//   - Rolling Pearson Correlation (XAU vs DXY, 15-min window)
//
// Compile: g++ -std=c++20 -O3 -march=native -mavx2 -ffast-math
// =============================================================================

#include "goldmine/types.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <immintrin.h>
#include <numeric>
#include <span>

namespace goldmine {
namespace indicators {

// =============================================================================
// Internal helpers
// =============================================================================

namespace detail {

// ---------------------------------------------------------------------------
// avx2_dot_product
//
// Computes sum( a[i] ) over N doubles using 256-bit AVX2 SIMD.
// N must be a multiple of 4 for full vectorization.
// Remainder handled scalar. Used for mean and variance loops.
// ---------------------------------------------------------------------------
[[nodiscard]] inline double avx2_sum(const double* __restrict__ a,
                                      std::size_t n) noexcept {
    __m256d acc = _mm256_setzero_pd();
    const std::size_t vec_end = n & ~std::size_t(3);  // floor to multiple-of-4

    for (std::size_t i = 0; i < vec_end; i += 4) {
        __m256d v = _mm256_loadu_pd(a + i);
        acc = _mm256_add_pd(acc, v);
    }
    // Horizontal sum of 4 doubles in acc
    __m128d lo  = _mm256_castpd256_pd128(acc);
    __m128d hi  = _mm256_extractf128_pd(acc, 1);
    __m128d sum = _mm_add_pd(lo, hi);
    sum = _mm_hadd_pd(sum, sum);
    double result = _mm_cvtsd_f64(sum);

    // Scalar tail
    for (std::size_t i = vec_end; i < n; ++i) result += a[i];
    return result;
}

// Compute sum(a[i]^2) using AVX2
[[nodiscard]] inline double avx2_sum_sq(const double* __restrict__ a,
                                          std::size_t n) noexcept {
    __m256d acc = _mm256_setzero_pd();
    const std::size_t vec_end = n & ~std::size_t(3);

    for (std::size_t i = 0; i < vec_end; i += 4) {
        __m256d v = _mm256_loadu_pd(a + i);
        acc = _mm256_fmadd_pd(v, v, acc);  // FMA: acc += v * v
    }
    __m128d lo  = _mm256_castpd256_pd128(acc);
    __m128d hi  = _mm256_extractf128_pd(acc, 1);
    __m128d sum = _mm_add_pd(lo, hi);
    sum = _mm_hadd_pd(sum, sum);
    double result = _mm_cvtsd_f64(sum);
    for (std::size_t i = vec_end; i < n; ++i) result += a[i] * a[i];
    return result;
}

// Compute sum(a[i]*b[i]) using AVX2 FMA — used for Pearson correlation
[[nodiscard]] inline double avx2_dot(const double* __restrict__ a,
                                      const double* __restrict__ b,
                                      std::size_t n) noexcept {
    __m256d acc = _mm256_setzero_pd();
    const std::size_t vec_end = n & ~std::size_t(3);

    for (std::size_t i = 0; i < vec_end; i += 4) {
        __m256d va = _mm256_loadu_pd(a + i);
        __m256d vb = _mm256_loadu_pd(b + i);
        acc = _mm256_fmadd_pd(va, vb, acc);
    }
    __m128d lo  = _mm256_castpd256_pd128(acc);
    __m128d hi  = _mm256_extractf128_pd(acc, 1);
    __m128d sum = _mm_add_pd(lo, hi);
    sum = _mm_hadd_pd(sum, sum);
    double result = _mm_cvtsd_f64(sum);
    for (std::size_t i = vec_end; i < n; ++i) result += a[i] * b[i];
    return result;
}

// Wilder's Exponential Smoothing multiplier
[[nodiscard]] constexpr double wilder_alpha(std::size_t period) noexcept {
    return 1.0 / static_cast<double>(period);
}

} // namespace detail

// =============================================================================
// Fast polynomial approximations — SIMD-friendly, no <cmath> on hot path
// =============================================================================

// Fast Gaussian CDF approximation (Abramowitz & Stegun, max error ~1.5e-7)
[[nodiscard]] inline double fast_gaussian_cdf(double x) noexcept {
    // Constants for rational approximation
    constexpr double a1 = 0.254829592, a2 = -0.284496736, a3 = 1.421413741;
    constexpr double a4 = -1.453152027, a5 = 1.061405429, p = 0.3275911;
    const double sign = (x < 0.0) ? -1.0 : 1.0;
    x = std::abs(x);
    const double t = 1.0 / (1.0 + p * x);
    const double y = 1.0 - (((((a5*t + a4)*t) + a3)*t + a2)*t + a1)*t
                     * std::exp(-x * x * 0.5) * 0.3989422804014327;  // 1/sqrt(2π)
    return 0.5 * (1.0 + sign * y);
}

// Branchless sign: returns +1.0 or -1.0 (no branch)
[[nodiscard]] inline double branchless_sign(double x) noexcept {
    // Uses bit manipulation: if x >= 0 → +1, else -1
    return static_cast<double>(1 - 2 * static_cast<int>(x < 0));
}

// Branchless conditional: returns a if cond, else b
[[nodiscard]] inline double branchless_select(bool cond, double a, double b) noexcept {
    return b + static_cast<double>(cond) * (a - b);
}

// =============================================================================
// VWAP — Volume Weighted Average Price (daily, cumulative)
//
// VWAP = Σ(price_i * volume_i) / Σ(volume_i)
// Resets at session open (managed externally via SessionContext).
//
// Input:  SoATickStore reference, number of ticks in current session
// Output: vwap price, deviation from current last price
// =============================================================================
struct VwapResult { Price vwap; Price deviation; Price daily_stddev; };

[[nodiscard]] inline VwapResult compute_vwap(const TickStore& store,
                                              std::size_t session_ticks) noexcept {
    if (session_ticks == 0) return {0.0, 0.0, 0.0};

    const std::size_t n    = std::min(session_ticks, store.filled());
    const std::size_t base = (store.count - n) % INDICATOR_WIN;  // ring start

    double pv_sum = 0.0;
    double v_sum  = 0.0;
    double p_sum  = 0.0;
    double p2_sum = 0.0;

    // Linear scan over contiguous ring segment — compiler will auto-vectorize
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t idx = (base + i) % INDICATOR_WIN;
        const double p = store.lasts[idx];
        const double v = store.volumes[idx];
        pv_sum += p * v;
        v_sum  += v;
        p_sum  += p;
        p2_sum += p * p;
    }

    if (v_sum < 1e-10) return {0.0, 0.0, 0.0};

    const double vwap = pv_sum / v_sum;
    const double last = store.lasts[(store.count - 1) % INDICATOR_WIN];
    const double mean = p_sum / static_cast<double>(n);
    const double var  = (p2_sum / static_cast<double>(n)) - (mean * mean);
    const double sdev = (var > 0.0) ? std::sqrt(var) : 0.0;

    return {vwap, last - vwap, sdev};
}

// =============================================================================
// ATR — Average True Range (14-period, Wilder's smoothing)
//
// True Range = max(High-Low, |High-PrevClose|, |Low-PrevClose|)
// For tick data we approximate: High = ask, Low = bid, Close = last
// =============================================================================
struct AtrResult { Price atr; Price prev_atr; bool expanding; };

[[nodiscard]] inline AtrResult compute_atr(const TickStore& store) noexcept {
    const std::size_t n = store.filled();
    if (n < ATR_PERIOD + 1) return {0.0, 0.0, false};

    const double alpha = detail::wilder_alpha(ATR_PERIOD);
    double atr = 0.0;

    // Seed with simple average of first ATR_PERIOD true ranges
    {
        double init_sum = 0.0;
        const std::size_t seed_start = (store.count - n) % INDICATOR_WIN;
        for (std::size_t i = 1; i <= ATR_PERIOD; ++i) {
            const std::size_t cur  = (seed_start + i) % INDICATOR_WIN;
            const std::size_t prev = (seed_start + i - 1) % INDICATOR_WIN;
            const double hi  = store.asks[cur];
            const double lo  = store.bids[cur];
            const double pc  = store.lasts[prev];
            const double tr  = std::max({hi - lo,
                                          std::abs(hi - pc),
                                          std::abs(lo - pc)});
            init_sum += tr;
        }
        atr = init_sum / static_cast<double>(ATR_PERIOD);
    }

    // Wilder's smoothing for remaining bars
    double prev_atr = atr;
    const std::size_t seed_start = (store.count - n) % INDICATOR_WIN;
    for (std::size_t i = ATR_PERIOD + 1; i < n; ++i) {
        const std::size_t cur  = (seed_start + i) % INDICATOR_WIN;
        const std::size_t prev = (seed_start + i - 1) % INDICATOR_WIN;
        const double hi  = store.asks[cur];
        const double lo  = store.bids[cur];
        const double pc  = store.lasts[prev];
        const double tr  = std::max({hi - lo,
                                      std::abs(hi - pc),
                                      std::abs(lo - pc)});
        if (i == n - 2) prev_atr = atr;
        atr = alpha * tr + (1.0 - alpha) * atr;
    }

    return {atr, prev_atr, atr > prev_atr};
}

// =============================================================================
// EMA — Exponential Moving Average (generic, used by Keltner + RSI base)
//
// Uses a fixed-size scratch array to avoid heap allocation.
// period must be <= INDICATOR_WIN.
// =============================================================================
[[nodiscard]] inline Price compute_ema(const TickStore& store,
                                        std::size_t period) noexcept {
    const std::size_t n = std::min(store.filled(), period * 3);
    if (n < period) return 0.0;

    const double alpha = 2.0 / static_cast<double>(period + 1);
    const std::size_t base = (store.count - n) % INDICATOR_WIN;

    // Seed with SMA of first 'period' bars
    double ema = 0.0;
    for (std::size_t i = 0; i < period; ++i) {
        ema += store.lasts[(base + i) % INDICATOR_WIN];
    }
    ema /= static_cast<double>(period);

    // Apply EMA for remaining bars
    for (std::size_t i = period; i < n; ++i) {
        const double p = store.lasts[(base + i) % INDICATOR_WIN];
        ema = alpha * p + (1.0 - alpha) * ema;
    }
    return ema;
}

// =============================================================================
// Bollinger Bands (20-period, 2 standard deviations)
//
// Uses AVX2 vectorized mean and variance over the window.
// =============================================================================
struct BollingerResult {
    Price upper, mid, lower, width;
};

[[nodiscard]] inline BollingerResult compute_bollinger(const TickStore& store,
                                                         std::size_t period = 20,
                                                         double multiplier  = 2.0) noexcept {
    const std::size_t n = std::min(store.filled(), period);
    if (n < period) return {0.0, 0.0, 0.0, 0.0};

    // Extract contiguous window into aligned scratch — no heap, stack only
    alignas(32) std::array<double, INDICATOR_WIN> scratch{};
    const std::size_t base = (store.count - period) % INDICATOR_WIN;
    for (std::size_t i = 0; i < period; ++i) {
        scratch[i] = store.lasts[(base + i) % INDICATOR_WIN];
    }

    const double sum  = detail::avx2_sum(scratch.data(), period);
    const double mean = sum / static_cast<double>(period);
    const double sq   = detail::avx2_sum_sq(scratch.data(), period);
    const double var  = sq / static_cast<double>(period) - mean * mean;
    const double sdev = (var > 0.0) ? std::sqrt(var) : 0.0;

    const double upper = mean + multiplier * sdev;
    const double lower = mean - multiplier * sdev;
    return {upper, mean, lower, upper - lower};
}

// =============================================================================
// Keltner Channel (20-period EMA ± 1.5 * ATR14)
// =============================================================================
struct KeltnerResult {
    Price upper, mid, lower, width;
};

[[nodiscard]] inline KeltnerResult compute_keltner(const TickStore& store,
                                                     Price atr,
                                                     std::size_t period  = 20,
                                                     double multiplier   = 1.5) noexcept {
    const Price ema   = compute_ema(store, period);
    const double band = multiplier * atr;
    const Price upper = ema + band;
    const Price lower = ema - band;
    return {upper, ema, lower, upper - lower};
}

// =============================================================================
// Z-Score — rolling (price - mean) / stddev over a window
// =============================================================================
struct ZScoreResult { double zscore; double mean; double stddev; };

[[nodiscard]] inline ZScoreResult compute_zscore(const TickStore& store,
                                                   std::size_t period = 20) noexcept {
    const std::size_t n = std::min(store.filled(), period);
    if (n < 2) return {0.0, 0.0, 0.0};

    alignas(32) std::array<double, INDICATOR_WIN> scratch{};
    const std::size_t base = (store.count - n) % INDICATOR_WIN;
    for (std::size_t i = 0; i < n; ++i) {
        scratch[i] = store.lasts[(base + i) % INDICATOR_WIN];
    }

    const double sum   = detail::avx2_sum(scratch.data(), n);
    const double mean  = sum / static_cast<double>(n);
    const double sq    = detail::avx2_sum_sq(scratch.data(), n);
    const double var   = sq / static_cast<double>(n) - mean * mean;
    const double sdev  = (var > 0.0) ? std::sqrt(var) : 1e-10;
    const double last  = store.lasts[(store.count - 1) % INDICATOR_WIN];
    return {(last - mean) / sdev, mean, sdev};
}

// =============================================================================
// RSI — 14-period (Wilder's Smoothed MA variant)
//
// RSI = 100 - 100 / (1 + RS)
// RS  = avg_gain / avg_loss  (Wilder smoothing)
// =============================================================================
[[nodiscard]] inline Price compute_rsi(const TickStore& store,
                                        std::size_t period = RSI_PERIOD) noexcept {
    const std::size_t n = store.filled();
    if (n < period + 1) return 50.0;

    double avg_gain = 0.0;
    double avg_loss = 0.0;
    const std::size_t base = (store.count - n) % INDICATOR_WIN;

    // Seed: simple average of first 'period' changes
    for (std::size_t i = 1; i <= period; ++i) {
        const double cur  = store.lasts[(base + i) % INDICATOR_WIN];
        const double prev = store.lasts[(base + i - 1) % INDICATOR_WIN];
        const double diff = cur - prev;
        if (diff > 0.0) avg_gain += diff;
        else            avg_loss += -diff;
    }
    avg_gain /= static_cast<double>(period);
    avg_loss /= static_cast<double>(period);

    // Wilder smoothing for remaining bars
    const double alpha = detail::wilder_alpha(period);
    for (std::size_t i = period + 1; i < n; ++i) {
        const double cur  = store.lasts[(base + i) % INDICATOR_WIN];
        const double prev = store.lasts[(base + i - 1) % INDICATOR_WIN];
        const double diff = cur - prev;
        const double gain = (diff > 0.0) ?  diff : 0.0;
        const double loss = (diff < 0.0) ? -diff : 0.0;
        avg_gain = alpha * gain + (1.0 - alpha) * avg_gain;
        avg_loss = alpha * loss + (1.0 - alpha) * avg_loss;
    }

    if (avg_loss < 1e-10) return 100.0;
    const double rs = avg_gain / avg_loss;
    return 100.0 - (100.0 / (1.0 + rs));
}

// =============================================================================
// Pearson Rolling Correlation — XAU vs DXY over a rolling window of ticks
//
// corr(X, Y) = [Σxy - n*mean_x*mean_y] / [n * stddev_x * stddev_y]
//
// Uses AVX2 dot product for inner loop acceleration.
// =============================================================================
struct CorrResult {
    double correlation;  // in [-1.0, +1.0]
    double xau_delta;    // price change of XAU over window
    double dxy_delta;    // price change of DXY over window
};

[[nodiscard]] inline CorrResult compute_xau_dxy_correlation(
        const TickStore& store, std::size_t window = CORR_WINDOW) noexcept {

    const std::size_t n = std::min(store.filled(), window);
    if (n < 4) return {0.0, 0.0, 0.0};

    // Extract into contiguous aligned scratch buffers — no heap
    alignas(32) std::array<double, INDICATOR_WIN> xau_buf{};
    alignas(32) std::array<double, INDICATOR_WIN> dxy_buf{};
    const std::size_t base = (store.count - n) % INDICATOR_WIN;

    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t idx = (base + i) % INDICATOR_WIN;
        xau_buf[i] = store.lasts[idx];
        dxy_buf[i] = store.dxy_prices[idx];
    }

    // --- means (vectorized) ---
    const double dn   = static_cast<double>(n);
    const double mx   = detail::avx2_sum(xau_buf.data(), n) / dn;
    const double my   = detail::avx2_sum(dxy_buf.data(), n) / dn;

    // Centre the arrays in-place (scalar; branch-free subtract)
    for (std::size_t i = 0; i < n; ++i) {
        xau_buf[i] -= mx;
        dxy_buf[i] -= my;
    }

    // --- variance and covariance (vectorized) ---
    const double cov  = detail::avx2_dot(xau_buf.data(), dxy_buf.data(), n);
    const double var_x = detail::avx2_sum_sq(xau_buf.data(), n);
    const double var_y = detail::avx2_sum_sq(dxy_buf.data(), n);

    if (var_x < 1e-12 || var_y < 1e-12) return {0.0, 0.0, 0.0};

    const double corr = cov / std::sqrt(var_x * var_y);

    // Delta: last vs first in window
    const double first_idx = base % INDICATOR_WIN;
    const double last_idx  = (store.count - 1) % INDICATOR_WIN;
    const double xau_delta = store.lasts[last_idx]      - store.lasts[first_idx];
    const double dxy_delta = store.dxy_prices[last_idx] - store.dxy_prices[first_idx];

    return {corr, xau_delta, dxy_delta};
}

// =============================================================================
// Phase 2 — Advanced Statistical Indicators
// =============================================================================

struct DistStats { double mean,stddev,skew,kurt,p99,p01,tail_ratio,last_ret; };

[[nodiscard]] inline DistStats compute_return_distribution(
        const TickStore& store, std::size_t period = 50) noexcept {
    DistStats d{};
    const std::size_t n = std::min(store.filled(), period);
    if (n < 4) return d;
    alignas(32) std::array<double, INDICATOR_WIN> rets{};
    const std::size_t base = (store.count - n) % INDICATOR_WIN;
    std::size_t nr = 0;
    for (std::size_t i = 1; i < n; ++i) {
        double prev = store.lasts[(base+i-1)%INDICATOR_WIN];
        double cur  = store.lasts[(base+i)%INDICATOR_WIN];
        if (prev > 1e-8) rets[nr++] = (cur - prev) / prev;
    }
    if (nr < 3) return d;
    double dn = static_cast<double>(nr);
    d.mean = detail::avx2_sum(rets.data(), nr) / dn;
    double m2=0,m3=0,m4=0;
    for (std::size_t i=0;i<nr;++i){
        double x=rets[i]-d.mean; double x2=x*x;
        m2+=x2; m3+=x2*x; m4+=x2*x2;
    }
    m2/=dn; m3/=dn; m4/=dn;
    d.stddev = (m2>0) ? std::sqrt(m2) : 1e-10;
    d.skew = (m2>1e-15) ? m3/(d.stddev*d.stddev*d.stddev) : 0.0;
    d.kurt = (m2>1e-15) ? m4/(m2*m2) - 3.0 : 0.0;
    d.last_ret = rets[nr-1];
    // Approximate percentiles via sorted partial scan
    // Use insertion sort on small array (no heap)
    alignas(32) std::array<double, INDICATOR_WIN> sorted{};
    for(std::size_t i=0;i<nr;++i) sorted[i]=rets[i];
    for(std::size_t i=1;i<nr;++i){
        double k=sorted[i]; std::size_t j=i;
        while(j>0 && sorted[j-1]>k){sorted[j]=sorted[j-1];--j;}
        sorted[j]=k;
    }
    d.p01 = sorted[nr/100]; d.p99 = sorted[nr - 1 - nr/100];
    double upper_tail=0, lower_tail=0;
    for(std::size_t i=0;i<nr;++i){
        if(rets[i]>2.0*d.stddev) upper_tail+=rets[i];
        if(rets[i]<-2.0*d.stddev) lower_tail+=std::abs(rets[i]);
    }
    d.tail_ratio=(lower_tail>1e-12)?upper_tail/lower_tail:1.0;
    return d;
}

struct KinematicResult { double vel,accel,jerk,prev_vel; };

[[nodiscard]] inline KinematicResult compute_kinematics(
        const TickStore& store, double prev_vel_in=0.0,
        std::size_t smooth=5) noexcept {
    const std::size_t n = store.filled();
    if (n < smooth+3) return {0,0,0,prev_vel_in};
    auto p=[&](std::size_t ago) -> double {
        return store.lasts[(store.count-1-ago)%INDICATOR_WIN];
    };
    // EMA-smoothed velocity (finite difference over smooth ticks)
    double vel = (p(0) - p(smooth)) / static_cast<double>(smooth);
    double prev_v = (p(1) - p(smooth+1)) / static_cast<double>(smooth);
    double prev_v2= (p(2) - p(smooth+2)) / static_cast<double>(smooth);
    double accel = vel - prev_v;
    double jerk  = accel - (prev_v - prev_v2);
    return {vel, accel, jerk, prev_vel_in};
}

[[nodiscard]] inline double compute_vwap_gradient(
        const TickStore& store, std::size_t session_ticks,
        std::size_t lookback = 20) noexcept {
    if (session_ticks < lookback+5) return 0.0;
    auto vwap_at=[&](std::size_t ticks) -> double {
        double pv=0,v=0;
        std::size_t base=(store.count-ticks)%INDICATOR_WIN;
        for(std::size_t i=0;i<ticks;++i){
            std::size_t idx=(base+i)%INDICATOR_WIN;
            pv+=store.lasts[idx]*store.volumes[idx];
            v+=store.volumes[idx];
        }
        return (v>1e-10)?pv/v:0.0;
    };
    double v_now = vwap_at(session_ticks);
    double v_prev= vwap_at(session_ticks - lookback);
    return v_now - v_prev;
}

struct OlsResult { double slope,intercept,residual,mse; };

[[nodiscard]] inline OlsResult compute_rolling_ols(
        const TickStore& store, std::size_t period=50) noexcept {
    const std::size_t n = std::min(store.filled(), period);
    if (n < 5) return {0,0,0,0};
    alignas(32) std::array<double,INDICATOR_WIN> x{},y{};
    const std::size_t base=(store.count-n)%INDICATOR_WIN;
    for(std::size_t i=0;i<n;++i){
        std::size_t idx=(base+i)%INDICATOR_WIN;
        x[i]=store.yield_10y[idx]; y[i]=store.lasts[idx];
    }
    double dn=static_cast<double>(n);
    double mx=detail::avx2_sum(x.data(),n)/dn;
    double my=detail::avx2_sum(y.data(),n)/dn;
    for(std::size_t i=0;i<n;++i){x[i]-=mx;y[i]-=my;}
    double sxy=detail::avx2_dot(x.data(),y.data(),n);
    double sxx=detail::avx2_sum_sq(x.data(),n);
    double slope=(sxx>1e-12)?sxy/sxx:0.0;
    double intercept=my-slope*mx;
    double last_y=store.lasts[(store.count-1)%INDICATOR_WIN];
    double last_x=store.yield_10y[(store.count-1)%INDICATOR_WIN];
    double residual=last_y-(slope*last_x+intercept);
    double mse=0;
    for(std::size_t i=0;i<n;++i){
        std::size_t idx=(base+i)%INDICATOR_WIN;
        double pred=slope*store.yield_10y[idx]+intercept;
        double e=store.lasts[idx]-pred; mse+=e*e;
    }
    mse/=dn;
    return {slope,intercept,residual,mse};
}

struct GarchResult { double vol; double prev_vol; };

[[nodiscard]] inline GarchResult compute_garch11(
        const TickStore& store, double prev_garch=0.0,
        double omega=1e-6, double alpha=0.10, double beta=0.85,
        std::size_t period=50) noexcept {
    const std::size_t n=std::min(store.filled(),period);
    if(n<5) return {0,prev_garch};
    const std::size_t base=(store.count-n)%INDICATOR_WIN;
    double var=0;
    for(std::size_t i=1;i<=std::min(n,std::size_t(10));++i){
        double r=store.lasts[(base+i)%INDICATOR_WIN]-store.lasts[(base+i-1)%INDICATOR_WIN];
        var+=r*r;
    }
    var/=10.0;
    double prev=var;
    for(std::size_t i=11;i<n;++i){
        double r=store.lasts[(base+i)%INDICATOR_WIN]-store.lasts[(base+i-1)%INDICATOR_WIN];
        prev=var;
        var=omega+alpha*r*r+beta*var;
    }
    return {std::sqrt(var),std::sqrt(prev)};
}

[[nodiscard]] inline double compute_hurst_approx(
        const TickStore& store, std::size_t period=100) noexcept {
    const std::size_t n=std::min(store.filled(),period);
    if(n<20) return 0.5;
    const std::size_t base=(store.count-n)%INDICATOR_WIN;
    // R/S analysis approximation over two sub-periods
    auto rs_stat=[&](std::size_t start,std::size_t len) -> double {
        double sum=0;
        for(std::size_t i=1;i<len;++i){
            double r=store.lasts[(base+start+i)%INDICATOR_WIN]
                    -store.lasts[(base+start+i-1)%INDICATOR_WIN];
            sum+=r;
        }
        double mean=sum/static_cast<double>(len-1);
        double cum=0,mx=-1e18,mn=1e18,sq=0;
        for(std::size_t i=1;i<len;++i){
            double r=store.lasts[(base+start+i)%INDICATOR_WIN]
                    -store.lasts[(base+start+i-1)%INDICATOR_WIN];
            cum+=(r-mean);
            if(cum>mx) mx=cum;
            if(cum<mn) mn=cum;
            sq+=(r-mean)*(r-mean);
        }
        double s=std::sqrt(sq/static_cast<double>(len-1));
        return (s>1e-12)?(mx-mn)/s:0.0;
    };
    double rs_full=rs_stat(0,n);
    double rs_half=(rs_stat(0,n/2)+rs_stat(n/2,n-n/2))/2.0;
    if(rs_half<1e-12||rs_full<1e-12) return 0.5;
    return std::log(rs_full/rs_half)/std::log(2.0);
}

[[nodiscard]] inline double compute_realized_vol(
        const TickStore& store, std::size_t period=20) noexcept {
    const std::size_t n=std::min(store.filled(),period);
    if(n<3) return 0.0;
    double sq=0;
    const std::size_t base=(store.count-n)%INDICATOR_WIN;
    for(std::size_t i=1;i<n;++i){
        double r=store.lasts[(base+i)%INDICATOR_WIN]-store.lasts[(base+i-1)%INDICATOR_WIN];
        sq+=r*r;
    }
    return std::sqrt(sq/static_cast<double>(n-1));
}

[[nodiscard]] inline double compute_trend_strength(
        const TickStore& store, std::size_t period=20) noexcept {
    const std::size_t n=std::min(store.filled(),period);
    if(n<3) return 0.0;
    const std::size_t base=(store.count-n)%INDICATOR_WIN;
    double sum_abs=0, sum_net=0;
    for(std::size_t i=1;i<n;++i){
        double d=store.lasts[(base+i)%INDICATOR_WIN]-store.lasts[(base+i-1)%INDICATOR_WIN];
        sum_abs+=std::abs(d); sum_net+=d;
    }
    return (sum_abs>1e-12)?100.0*std::abs(sum_net)/sum_abs:0.0;
}

struct GoldSilverResult { double ratio,zscore,velocity; };

[[nodiscard]] inline GoldSilverResult compute_gold_silver_ratio(
        const TickStore& store, std::size_t period=50) noexcept {
    const std::size_t n=std::min(store.filled(),period);
    if(n<5) return {0,0,0};
    alignas(32) std::array<double,INDICATOR_WIN> ratios{};
    const std::size_t base=(store.count-n)%INDICATOR_WIN;
    std::size_t valid=0;
    for(std::size_t i=0;i<n;++i){
        std::size_t idx=(base+i)%INDICATOR_WIN;
        double xag=store.xag_prices[idx];
        if(xag>1e-8) ratios[valid++]=store.lasts[idx]/xag;
    }
    if(valid<3) return {0,0,0};
    double dn=static_cast<double>(valid);
    double mean=detail::avx2_sum(ratios.data(),valid)/dn;
    double sq=detail::avx2_sum_sq(ratios.data(),valid);
    double var=sq/dn-mean*mean;
    double sd=(var>0)?std::sqrt(var):1e-10;
    double last_r=ratios[valid-1];
    double prev_r=(valid>1)?ratios[valid-2]:last_r;
    return {last_r,(last_r-mean)/sd,last_r-prev_r};
}

// =============================================================================
// Phase 4 — Order Flow Microstructure Indicators
// =============================================================================

struct OfiResult {
    double ofi_sum, ofi_stddev, ofi_zscore, ofi_accel;
    double voi_sum, voi_velocity;
    double micro_price, micro_delta;
};

[[nodiscard]] inline OfiResult compute_order_flow(
        const TickStore& store, std::size_t period=20) noexcept {
    OfiResult r{};
    const std::size_t n = std::min(store.filled(), period);
    if (n < 3) return r;
    const std::size_t base = (store.count - n) % INDICATOR_WIN;

    alignas(32) std::array<double, INDICATOR_WIN> ofi_arr{}, voi_arr{};

    for (std::size_t i = 1; i < n; ++i) {
        const std::size_t cur  = (base + i) % INDICATOR_WIN;
        const std::size_t prev = (base + i - 1) % INDICATOR_WIN;

        const double db = store.bids[cur] - store.bids[prev];  // ΔP_bid
        const double da = store.asks[cur] - store.asks[prev];  // ΔP_ask
        const double vb_cur  = store.volumes[cur];
        const double vb_prev = store.volumes[prev];

        // OFI: branchless using sign indicators
        // I{ΔPb>=0}·vb - I{ΔPb<=0}·vb_prev - I{ΔPa<=0}·va + I{ΔPa>=0}·va_prev
        const double ofi_t =
            static_cast<double>(db >= 0) * vb_cur
          - static_cast<double>(db <= 0) * vb_prev
          - static_cast<double>(da <= 0) * vb_cur   // using volume as proxy
          + static_cast<double>(da >= 0) * vb_prev;

        ofi_arr[i - 1] = ofi_t;

        // VOI: volume × sign(price_change)
        const double dp = store.lasts[cur] - store.lasts[prev];
        voi_arr[i - 1] = vb_cur * branchless_sign(dp);
    }

    const std::size_t m = n - 1;
    const double dn = static_cast<double>(m);

    // OFI cumulative sum and statistics
    r.ofi_sum = detail::avx2_sum(ofi_arr.data(), m);
    const double ofi_mean = r.ofi_sum / dn;
    double ofi_var = 0;
    for (std::size_t i = 0; i < m; ++i) {
        double d = ofi_arr[i] - ofi_mean;
        ofi_var += d * d;
    }
    ofi_var /= dn;
    r.ofi_stddev = (ofi_var > 0) ? std::sqrt(ofi_var) : 1e-10;
    r.ofi_zscore = (r.ofi_stddev > 1e-10) ? ofi_mean / r.ofi_stddev : 0.0;

    // OFI acceleration (last - second-to-last)
    if (m >= 2) r.ofi_accel = ofi_arr[m - 1] - ofi_arr[m - 2];

    // VOI cumulative and velocity
    r.voi_sum = detail::avx2_sum(voi_arr.data(), m);
    if (m >= 2) r.voi_velocity = voi_arr[m - 1] - voi_arr[m - 2];

    // Micro-price: P_micro = P_bid × (V_ask/(V_bid+V_ask)) + P_ask × (V_bid/(V_bid+V_ask))
    const std::size_t last_idx = (store.count - 1) % INDICATOR_WIN;
    const double pb = store.bids[last_idx];
    const double pa = store.asks[last_idx];
    const double vl = store.volumes[last_idx];
    // Use volume as proxy for both sides (true L2 data not available in tick feed)
    const double vb_proxy = vl * static_cast<double>(pb > store.bids[(store.count - 2) % INDICATOR_WIN]);
    const double va_proxy = vl * static_cast<double>(pa < store.asks[(store.count - 2) % INDICATOR_WIN]);
    const double vtotal = vb_proxy + va_proxy;
    if (vtotal > 1e-10) {
        r.micro_price = pb * (va_proxy / vtotal) + pa * (vb_proxy / vtotal);
    } else {
        r.micro_price = (pb + pa) * 0.5;
    }
    r.micro_delta = r.micro_price - (pb + pa) * 0.5;

    return r;
}

// =============================================================================
// IndicatorEngine — aggregates all calculations
// =============================================================================
struct SessionContext {
    std::size_t session_tick_count = 0;
    Price       swing_low          = std::numeric_limits<Price>::max();
    Price       ema20              = 0.0;
    Price       asian_high         = 0.0;
    Price       asian_low          = 1e9;
    bool        asian_finalized    = false;
    Price       opening_hi         = 0.0;
    Price       opening_lo         = 1e9;
    std::size_t opening_ticks      = 0;
    double      prev_velocity      = 0.0;
    double      prev_garch         = 0.0;
};

[[nodiscard]] inline IndicatorSnapshot compute_all(
        const TickStore& store,
        const SessionContext& ctx,
        Nanos now_ns) noexcept {

    IndicatorSnapshot snap{};
    snap.computed_at_ns = now_ns;
    if (store.filled() < ATR_PERIOD + 1) return snap;

    // --- Phase 1 core ---
    const auto atr_res = compute_atr(store);
    snap.atr14 = atr_res.atr; snap.atr_expanding = atr_res.expanding;
    const auto vwap_res = compute_vwap(store, ctx.session_tick_count);
    snap.vwap = vwap_res.vwap; snap.vwap_deviation = vwap_res.deviation;
    const auto bb = compute_bollinger(store,20,2.0);
    snap.bb_upper=bb.upper; snap.bb_lower=bb.lower; snap.bb_mid=bb.mid; snap.bb_width=bb.width;
    const auto kc = compute_keltner(store,snap.atr14,20,1.5);
    snap.kc_upper=kc.upper; snap.kc_lower=kc.lower; snap.kc_mid=kc.mid; snap.kc_width=kc.width;
    snap.squeeze_active = (bb.upper<kc.upper)&&(bb.lower>kc.lower);
    snap.squeeze_intensity = (kc.width>1e-10)?bb.width/kc.width:1.0;
    const double last = store.lasts[(store.count-1)%INDICATOR_WIN];
    const double vol_last = store.volumes[(store.count-1)%INDICATOR_WIN];
    double vm=0; std::size_t nv=std::min(store.filled(),std::size_t(20));
    std::size_t bv=(store.count-nv)%INDICATOR_WIN;
    for(std::size_t i=0;i<nv;++i) vm+=store.volumes[(bv+i)%INDICATOR_WIN];
    snap.vol_mean_20 = (nv>0)?vm/static_cast<double>(nv):1.0;
    // Volume-based check OR price-velocity check (for feeds with constant volume)
    bool high_vol=(vol_last>2.0*snap.vol_mean_20);
    // Fallback: if all volumes are ~1.0 (constant), use price velocity as proxy
    const auto kin_pre = compute_kinematics(store, 0.0);
    bool vel_breakout = std::abs(kin_pre.vel) > 0.02;  // meaningful price movement
    bool breakout_confirmed = high_vol || vel_breakout;
    snap.squeeze_breakout_up   = snap.squeeze_active&&(last>bb.upper)&&breakout_confirmed;
    snap.squeeze_breakout_down = snap.squeeze_active&&(last<bb.lower)&&breakout_confirmed;
    const auto zs = compute_zscore(store,20);
    snap.zscore = zs.zscore;
    snap.rsi14 = compute_rsi(store,RSI_PERIOD);
    const auto corr = compute_xau_dxy_correlation(store,CORR_WINDOW);
    snap.corr_xau_dxy=corr.correlation; snap.xau_delta_15m=corr.xau_delta; snap.dxy_delta_15m=corr.dxy_delta;

    // --- Phase 2: Return distribution ---
    const auto dist = compute_return_distribution(store,50);
    snap.skewness_20=dist.skew; snap.kurtosis_20=dist.kurt;
    snap.return_mean=dist.mean; snap.return_stddev=dist.stddev;
    snap.return_99pct=dist.p99; snap.return_01pct=dist.p01;
    snap.tail_ratio=dist.tail_ratio; snap.last_return=dist.last_ret;

    // --- Phase 2: Kinematics ---
    const auto kin = compute_kinematics(store,ctx.prev_velocity);
    snap.price_velocity=kin.vel; snap.price_acceleration=kin.accel;
    snap.price_jerk=kin.jerk; snap.prev_velocity=kin.prev_vel;

    // --- Phase 2: VWAP gradient ---
    snap.vwap_gradient = compute_vwap_gradient(store,ctx.session_tick_count);

    // --- Phase 2: Rolling OLS (Gold vs 10Y yield) ---
    const auto ols = compute_rolling_ols(store,50);
    snap.ols_residual=ols.residual; snap.ols_slope=ols.slope; snap.ols_mse=ols.mse;

    // --- Phase 2: GARCH(1,1) ---
    const auto garch = compute_garch11(store,ctx.prev_garch);
    snap.garch_vol=garch.vol; snap.prev_garch_vol=garch.prev_vol;

    // --- Phase 2: Hurst, realized vol, trend strength ---
    snap.hurst_exponent = compute_hurst_approx(store,100);
    snap.realized_vol_20 = compute_realized_vol(store,20);
    snap.trend_strength = compute_trend_strength(store,20);
    snap.vol_of_vol = (snap.prev_garch_vol>1e-12)?std::abs(snap.garch_vol-snap.prev_garch_vol)/snap.prev_garch_vol:0.0;

    // --- Phase 2: Gold/Silver ratio ---
    const auto gs = compute_gold_silver_ratio(store,50);
    snap.gold_silver_ratio=gs.ratio; snap.gs_ratio_zscore=gs.zscore; snap.gs_ratio_velocity=gs.velocity;
    snap.xag_last = store.xag_prices[(store.count-1)%INDICATOR_WIN];
    snap.yield_last = store.yield_10y[(store.count-1)%INDICATOR_WIN];

    // --- Session microstructure (passed from ctx) ---
    snap.asian_high=ctx.asian_high; snap.asian_low=ctx.asian_low;
    snap.asian_range_set=ctx.asian_finalized;
    snap.opening_range_hi=ctx.opening_hi; snap.opening_range_lo=ctx.opening_lo;
    snap.opening_range_set=(ctx.opening_ticks>=15);

    // --- Phase 4: Order Flow Microstructure ---
    const auto ofi = compute_order_flow(store, 20);
    snap.ofi_cumulative=ofi.ofi_sum; snap.ofi_stddev=ofi.ofi_stddev;
    snap.ofi_zscore=ofi.ofi_zscore; snap.ofi_acceleration=ofi.ofi_accel;
    snap.voi_cumulative=ofi.voi_sum; snap.voi_velocity=ofi.voi_velocity;
    snap.micro_price=ofi.micro_price; snap.micro_price_delta=ofi.micro_delta;

    return snap;
}

} // namespace indicators
} // namespace goldmine
