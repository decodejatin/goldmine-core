#pragma once
// =============================================================================
// live_ingestion.hpp — POSIX Shared Memory Tick Ingestion (Thread 1)
// Native Linux Version (Single-Asset XAU/USD)
// =============================================================================

#include "goldmine/types.hpp"
#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <immintrin.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <new>

namespace goldmine {

// ---------------------------------------------------------------------------
// Lock-free Shared Tick (updated by Python WebSocket Bridge)
// ---------------------------------------------------------------------------
struct alignas(64) SharedTick {
    alignas(64) std::atomic<uint64_t> sequence_id;
    uint64_t timestamp_ms;
    double bid;
    double ask;
    uint32_t volume; 
    uint32_t magic_header; // 0x474F4C44
    uint32_t checksum;
    char _pad[12];
};
static_assert(sizeof(SharedTick) <= 64);

// ---------------------------------------------------------------------------
// LiveTickReader — Sub-millisecond spin-wait reader for SHM
// ---------------------------------------------------------------------------
class LiveTickReader {
public:
    explicit LiveTickReader(const char* shm_name = "/goldmine_tick_shm") {
        // Creates or opens /dev/shm/goldmine_tick_shm
        fd_ = shm_open(shm_name, O_CREAT | O_RDWR, 0666);
        if (fd_ == -1) {
            throw std::runtime_error("LiveTickReader: Failed to shm_open");
        }

        // Set the size exactly to 64 bytes (cache line)
        if (ftruncate(fd_, sizeof(SharedTick)) == -1) {
            close(fd_);
            throw std::runtime_error("LiveTickReader: Failed to ftruncate");
        }

        // Map into memory, attempt HugePages first
#ifndef MAP_HUGETLB
#define MAP_HUGETLB 0x40000
#endif
        void* ptr = mmap(nullptr, sizeof(SharedTick), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_HUGETLB, fd_, 0);
        if (ptr == MAP_FAILED) {
            // Fallback to standard pages
            ptr = mmap(nullptr, sizeof(SharedTick), PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
            if (ptr == MAP_FAILED) {
                close(fd_);
                throw std::runtime_error("LiveTickReader: Failed to mmap");
            }
            std::printf("[SHM] HugePages unavailable. Fallback to standard 4KB RAM paging.\n");
        } else {
            std::printf("[SHM] HugePages allocation successful\n");
        }

        // Map the pointer directly to our atomic struct
        shared_tick_ = reinterpret_cast<SharedTick*>(ptr);
    }

    ~LiveTickReader() {
        if (shared_tick_) {
            munmap(shared_tick_, sizeof(SharedTick));
        }
        if (fd_ != -1) {
            close(fd_);
        }
    }

    // Delete copy/move to enforce single reader
    LiveTickReader(const LiveTickReader&) = delete;
    LiveTickReader& operator=(const LiveTickReader&) = delete;
    LiveTickReader(LiveTickReader&&) = delete;
    LiveTickReader& operator=(LiveTickReader&&) = delete;

    // -----------------------------------------------------------------------
    // spin_wait_next() — Polls sequence_id, returns Tick when updated
    // -----------------------------------------------------------------------
    [[nodiscard]] __attribute__((always_inline)) inline Tick spin_wait_next() noexcept {
        uint64_t current_seq = last_seq_;
        while (true) {
            // Memory order acquire ensures we read the updated payload after sequence_id changes
            uint64_t seq = shared_tick_->sequence_id.load(std::memory_order_acquire);
            
            if (seq > current_seq) [[likely]] {
                last_seq_ = seq;
                
                // SHM Memory Guard (Module 3)
                if (shared_tick_->magic_header != 0x474F4C44) [[unlikely]] {
                    // Invalid tick, skip
                    continue;
                }
                
                // (Optional checksum validation could go here)
                // uint32_t expected_crc = ...;
                // if (shared_tick_->checksum != expected_crc) [[unlikely]] continue;
                
                Tick t{};
                t.timestamp_ns = shared_tick_->timestamp_ms * 1'000'000LL;
                t.bid          = shared_tick_->bid;
                t.ask          = shared_tick_->ask;
                t.volume       = shared_tick_->volume;
                t.last         = (t.bid + t.ask) * 0.5; // Mid price for XAU
                
                return t;
            }
            
            // Sub-millisecond spin-wait pause (AVX2 intrinsic)
            _mm_pause(); 
        }
    }

private:
    int fd_ = -1;
    SharedTick* shared_tick_ = nullptr;
    uint64_t last_seq_ = 0;
};

} // namespace goldmine
