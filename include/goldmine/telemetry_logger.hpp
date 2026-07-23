#pragma once
// =============================================================================
// telemetry_logger.hpp — Thread 5: Zero-allocation async binary audit logger
//
// Hot path threads push fixed-size TelemetryRecords into an SPSC queue in
// <15ns. Thread 5 drains the queue and writes to a buffered binary log file.
// =============================================================================
#include "goldmine/types.hpp"
#include "goldmine/lockfree_queue.hpp"
#include <cstdio>
#include <cstring>
#include <thread>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

namespace goldmine {

// ---------------------------------------------------------------------------
// TelemetryRecord — fixed-size audit record (192 bytes, cache-aligned)
// ---------------------------------------------------------------------------
enum class TelemetryEvent : std::uint8_t {
    SIGNAL_GENERATED = 1,
    SIGNAL_BLOCKED   = 2,
    TRADE_OPENED     = 3,
    TRADE_CLOSED     = 4,
    TICK_PROCESSED   = 5,
};

struct alignas(CACHE_LINE) TelemetryRecord {
    std::uint64_t   timestamp_ns;
    TelemetryEvent  event;
    Direction       direction;
    double          conviction;
    double          price;
    double          sl;
    double          tp;
    double          pnl;
    std::uint32_t   rules_active;    // popcount of mask
    std::uint32_t   latency_ns;      // tick-to-signal latency
    WideBitmask     mask;            // full 1024-bit state
};

// ---------------------------------------------------------------------------
// TelemetryLogger — Thread 5 async logger
// ---------------------------------------------------------------------------
inline constexpr std::size_t TELEMETRY_QUEUE_SZ = 4096;

class TelemetryLogger {
public:
    TelemetryLogger() = default;
    ~TelemetryLogger() { stop(); }

    TelemetryLogger(const TelemetryLogger&) = delete;
    TelemetryLogger& operator=(const TelemetryLogger&) = delete;

    bool open(const char* path) noexcept {
        fp_ = std::fopen(path, "wb");
        if (!fp_) return false;

        // Write header: version + record size
        std::uint32_t version = 1;
        std::uint32_t rec_size = sizeof(TelemetryRecord);
        std::fwrite(&version, 4, 1, fp_);
        std::fwrite(&rec_size, 4, 1, fp_);
        return true;
    }

    // Called from hot path threads — must complete in <15ns
    bool log(const TelemetryRecord& rec) noexcept {
        return queue_.try_push(rec);
    }

    // Convenience: log a signal event
    void log_signal(const EngineSignal& sig, Price price, Nanos latency) noexcept {
        TelemetryRecord rec{};
        rec.timestamp_ns = static_cast<std::uint64_t>(sig.signal_ns);
        rec.event = sig.risk_blocked ? TelemetryEvent::SIGNAL_BLOCKED
                                      : TelemetryEvent::SIGNAL_GENERATED;
        rec.direction = sig.direction;
        rec.conviction = sig.conviction;
        rec.price = price;
        rec.sl = sig.suggested_sl;
        rec.tp = sig.suggested_tp;
        rec.rules_active = static_cast<std::uint32_t>(sig.active_rules.popcount());
        rec.latency_ns = static_cast<std::uint32_t>(latency);
        rec.mask = sig.active_rules;
        (void)queue_.try_push(rec);
    }

    void start() noexcept {
        if (running_) return;
        running_ = true;
        thread_ = std::thread([this]() { drain_loop(); });
    }

    void stop() noexcept {
        if (!running_) return;
        running_ = false;
        if (thread_.joinable()) thread_.join();
        // Final drain
        drain_remaining();
        if (fp_) { std::fflush(fp_); std::fclose(fp_); fp_ = nullptr; }
    }

    [[nodiscard]] std::size_t records_written() const noexcept { return written_; }

private:
    void drain_loop() noexcept {
#ifdef __linux__
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(5, &cpuset);
        ::pthread_setaffinity_np(::pthread_self(), sizeof(cpuset), &cpuset);
#endif
        while (running_) {
            if (auto rec = queue_.try_pop()) {
                if (fp_) {
                    std::fwrite(&(*rec), sizeof(TelemetryRecord), 1, fp_);
                    ++written_;
                    // Flush every 1024 records to balance throughput vs durability
                    if ((written_ & 1023) == 0) std::fflush(fp_);
                }
            } else {
                _mm_pause();
            }
        }
    }

    void drain_remaining() noexcept {
        while (auto rec = queue_.try_pop()) {
            if (fp_) {
                std::fwrite(&(*rec), sizeof(TelemetryRecord), 1, fp_);
                ++written_;
            }
        }
    }

    SpscQueue<TelemetryRecord, TELEMETRY_QUEUE_SZ> queue_{};
    FILE* fp_ = nullptr;
    std::thread thread_;
    bool running_ = false;
    std::size_t written_ = 0;
};

} // namespace goldmine
