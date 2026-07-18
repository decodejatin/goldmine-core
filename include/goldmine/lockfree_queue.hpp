
#pragma once
// =============================================================================
// lockfree_queue.hpp  — Single-Producer Single-Consumer (SPSC) Ring Buffer
//
// Implements a zero-allocation, lock-free queue using:
//   - std::atomic<std::size_t> with acquire/release ordering
//   - Cache-line padding between head and tail to prevent false sharing
//   - Power-of-2 capacity for modulo-free indexing via bitmask
//
// Compile: g++ -std=c++20 -O3 -march=native
// =============================================================================

#include "goldmine/types.hpp"
#include <array>
#include <atomic>
#include <cstddef>
#include <optional>
#include <type_traits>

namespace goldmine {

// ---------------------------------------------------------------------------
// SpscQueue<T, N>
//
// T    — element type, must be trivially copyable (no hidden allocations)
// N    — capacity, MUST be a power of 2
//
// Thread safety model:
//   - Exactly ONE producer thread calls try_push()
//   - Exactly ONE consumer thread calls try_pop()
//   - No other synchronization needed
// ---------------------------------------------------------------------------
template <typename T, std::size_t N>
    requires (std::is_trivially_copyable_v<T> &&
             (N & (N - 1)) == 0 &&     // power of 2
              N >= 2)
class SpscQueue {
public:
    static constexpr std::size_t CAPACITY = N;
    static constexpr std::size_t MASK     = N - 1;
    static_assert((CAPACITY & (CAPACITY - 1)) == 0, "Capacity must be a power of 2");

    SpscQueue() noexcept = default;

    // Non-copyable, non-movable — queue owns its storage
    SpscQueue(const SpscQueue&)            = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;
    SpscQueue(SpscQueue&&)                 = delete;
    SpscQueue& operator=(SpscQueue&&)      = delete;

    // -----------------------------------------------------------------------
    // try_push — called exclusively by the PRODUCER thread
    //
    // Returns true if element was enqueued.
    // Returns false if the queue is full (back-pressure signal).
    // Zero heap allocation. The element is copied into the ring buffer slot.
    // -----------------------------------------------------------------------
    [[nodiscard]] bool try_push(const T& item) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t next = (head + 1) & MASK;

        // Full? tail is cached to avoid cross-core traffic on every push
        if (next == tail_cache_) [[unlikely]] {
            tail_cache_ = tail_.load(std::memory_order_acquire);
            if (next == tail_cache_) return false;
        }

        buffer_[head] = item;
        // Release: make the write to buffer_[head] visible to the consumer
        head_.store(next, std::memory_order_release);
        return true;
    }

    // -----------------------------------------------------------------------
    // try_pop — called exclusively by the CONSUMER thread
    //
    // Returns the element if available, std::nullopt if queue is empty.
    // -----------------------------------------------------------------------
    [[nodiscard]] std::optional<T> try_pop() noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);

        // Empty? head is cached to reduce cross-core acquire traffic
        if (tail == head_cache_) [[unlikely]] {
            head_cache_ = head_.load(std::memory_order_acquire);
            if (tail == head_cache_) return std::nullopt;
        }

        T item = buffer_[tail];
        // Release: update tail so producer can reuse the slot
        tail_.store((tail + 1) & MASK, std::memory_order_release);
        return item;
    }

    // -----------------------------------------------------------------------
    // Diagnostic helpers — single-threaded use only
    // -----------------------------------------------------------------------
    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::size_t approx_size() const noexcept {
        const std::size_t h = head_.load(std::memory_order_acquire);
        const std::size_t t = tail_.load(std::memory_order_acquire);
        return (h - t + N) & MASK;
    }

private:
    // -----------------------------------------------------------------------
    // Memory layout: each atomic and its producer/consumer-side cache lives
    // on its own 64-byte cache line to prevent false sharing between threads.
    // -----------------------------------------------------------------------

    // --- Producer-side (written by producer, occasionally read by consumer) ---
    alignas(CACHE_LINE) std::atomic<std::size_t> head_{0};
    alignas(CACHE_LINE) std::size_t tail_cache_ = 0;  // producer's cached tail

    // --- Consumer-side (written by consumer, occasionally read by producer) ---
    alignas(CACHE_LINE) std::atomic<std::size_t> tail_{0};
    alignas(CACHE_LINE) std::size_t head_cache_ = 0;  // consumer's cached head

    // --- Shared ring buffer (read/write by both, but at different indices) ---
    alignas(CACHE_LINE) std::array<T, N> buffer_{};
};

// ---------------------------------------------------------------------------
// Concrete queue types used by the engine
// ---------------------------------------------------------------------------

// Feed queue: raw Gold ticks from market data adapter → indicator thread
using GoldTickQueue = SpscQueue<Tick, TICK_BUFFER_SZ>;

// DXY feed queue: Dollar Index ticks → indicator thread
using DxyTickQueue  = SpscQueue<DxyTick, TICK_BUFFER_SZ>;

// Signal queue: evaluated signals from logic engine → order router
using SignalQueue   = SpscQueue<EngineSignal, 256>;

// Snapshot queue: indicator snapshots → logic engine (single slot ping-pong)
// We use a small ring to avoid the engine reading a partially-written snapshot.
using SnapshotQueue = SpscQueue<IndicatorSnapshot, 16>;

// ---------------------------------------------------------------------------
// SpscPingPong<T>
//
// A minimal double-buffer for the case where the producer always wants the
// consumer to read the *latest* value, discarding stale ones.
// Used to publish IndicatorSnapshot from indicator thread → engine thread
// without queue buildup during indicator computation spikes.
// ---------------------------------------------------------------------------
template <typename T>
    requires std::is_trivially_copyable_v<T>
class SpscPingPong {
public:
    // Producer writes
    void store(const T& val) noexcept {
        const std::size_t idx = write_idx_.load(std::memory_order_relaxed);
        buf_[idx & 1] = val;
        // Increment index — consumer now sees the new slot
        write_idx_.store(idx + 1, std::memory_order_release);
    }

    // Consumer reads latest
    [[nodiscard]] bool load(T& out) noexcept {
        const std::size_t idx = write_idx_.load(std::memory_order_acquire);
        if (idx == last_seen_) return false;  // nothing new
        out = buf_[(idx - 1) & 1];            // read the latest completed slot
        last_seen_ = idx;
        return true;
    }

private:
    alignas(CACHE_LINE) std::atomic<std::size_t> write_idx_{0};
    alignas(CACHE_LINE) std::array<T, 2>         buf_{};
    std::size_t last_seen_ = 0;  // consumer-private, no sharing
};

using SnapshotPingPong = SpscPingPong<IndicatorSnapshot>;

} // namespace goldmine
