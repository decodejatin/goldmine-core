#pragma once
// =============================================================================
// bitmask_engine.hpp — AVX2-accelerated WideBitmask operations
//
// Processes 256 bits (4 uint64_t words) per SIMD instruction.
// For BITMASK_WORDS=16 (1024 rules), the entire mask is processed in
// exactly 4 AVX2 iterations — sub-nanosecond bulk logic operations.
// =============================================================================

#include "goldmine/types.hpp"
#include <immintrin.h>

namespace goldmine {
namespace bitmask_ops {

// ---------------------------------------------------------------------------
// Bulk AND — mask &= other (e.g., apply session gate masks)
// ---------------------------------------------------------------------------
inline void and_inplace(WideBitmask& a, const WideBitmask& b) noexcept {
    static_assert(WideBitmask::NUM_WORDS % 4 == 0);
    for (std::size_t i = 0; i < WideBitmask::NUM_WORDS; i += 4) {
        __m256i va = _mm256_load_si256(
            reinterpret_cast<const __m256i*>(a.words.data() + i));
        __m256i vb = _mm256_load_si256(
            reinterpret_cast<const __m256i*>(b.words.data() + i));
        _mm256_store_si256(
            reinterpret_cast<__m256i*>(a.words.data() + i),
            _mm256_and_si256(va, vb));
    }
}

// ---------------------------------------------------------------------------
// Bulk OR — mask |= other (e.g., merge rule groups)
// ---------------------------------------------------------------------------
inline void or_inplace(WideBitmask& a, const WideBitmask& b) noexcept {
    for (std::size_t i = 0; i < WideBitmask::NUM_WORDS; i += 4) {
        __m256i va = _mm256_load_si256(
            reinterpret_cast<const __m256i*>(a.words.data() + i));
        __m256i vb = _mm256_load_si256(
            reinterpret_cast<const __m256i*>(b.words.data() + i));
        _mm256_store_si256(
            reinterpret_cast<__m256i*>(a.words.data() + i),
            _mm256_or_si256(va, vb));
    }
}

// ---------------------------------------------------------------------------
// Bulk XOR — mask ^= other
// ---------------------------------------------------------------------------
inline void xor_inplace(WideBitmask& a, const WideBitmask& b) noexcept {
    for (std::size_t i = 0; i < WideBitmask::NUM_WORDS; i += 4) {
        __m256i va = _mm256_load_si256(
            reinterpret_cast<const __m256i*>(a.words.data() + i));
        __m256i vb = _mm256_load_si256(
            reinterpret_cast<const __m256i*>(b.words.data() + i));
        _mm256_store_si256(
            reinterpret_cast<__m256i*>(a.words.data() + i),
            _mm256_xor_si256(va, vb));
    }
}

// ---------------------------------------------------------------------------
// Bulk ANDNOT — mask = a & ~b (e.g., remove blocked rules)
// ---------------------------------------------------------------------------
inline void andnot_inplace(WideBitmask& a, const WideBitmask& b) noexcept {
    for (std::size_t i = 0; i < WideBitmask::NUM_WORDS; i += 4) {
        __m256i va = _mm256_load_si256(
            reinterpret_cast<const __m256i*>(a.words.data() + i));
        __m256i vb = _mm256_load_si256(
            reinterpret_cast<const __m256i*>(b.words.data() + i));
        _mm256_store_si256(
            reinterpret_cast<__m256i*>(a.words.data() + i),
            _mm256_andnot_si256(vb, va));  // note: andnot(b, a) = a & ~b
    }
}

// ---------------------------------------------------------------------------
// Test if any bit is set (zero-test)
// ---------------------------------------------------------------------------
[[nodiscard]] inline bool any_set(const WideBitmask& m) noexcept {
    for (std::size_t i = 0; i < WideBitmask::NUM_WORDS; i += 4) {
        __m256i v = _mm256_load_si256(
            reinterpret_cast<const __m256i*>(m.words.data() + i));
        if (!_mm256_testz_si256(v, v)) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Score — dot-product of active bits against a contiguous weight array
//
// Iterates only over set bits using __builtin_ctzll (optimal for sparse
// bitmasks typical in trading — usually 5-30 of 1024 rules fire).
//
// weights[] must have at least MAX_RULES entries, aligned to 32 bytes.
// ---------------------------------------------------------------------------
[[nodiscard]] inline Score score_mask(
        const WideBitmask& mask,
        const double* __restrict__ weights) noexcept {

    Score total = 0.0;
    for (std::size_t w = 0; w < WideBitmask::NUM_WORDS; ++w) {
        std::uint64_t bits = mask.words[w];
        const std::size_t base = w * 64;
        while (bits) {
            const int bit = __builtin_ctzll(bits);
            total += weights[base + static_cast<std::size_t>(bit)];
            bits &= bits - 1;   // clear lowest set bit
        }
    }
    return total;
}

// ---------------------------------------------------------------------------
// Apply session multiplier and clamp to [-1.0, +1.0]
// ---------------------------------------------------------------------------
[[nodiscard]] inline Score apply_session_scaling(
        Score raw, const SessionFlags& sess) noexcept {

    // No session scaling — raw rule-agreement score is used directly.
    // Session-specific filters are implemented at the rule level instead,
    // allowing each rule to decide if its setup is valid in the current session.
    // Clamping to [-1.0, +1.0] is still enforced to normalize the output.
    (void)sess;
    return (raw > 1.0) ? 1.0 : (raw < -1.0) ? -1.0 : raw;
}

} // namespace bitmask_ops
} // namespace goldmine
