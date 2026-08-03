#pragma once
// =============================================================================
// replayer.hpp — Zero-copy mmap tick replayer with benchmark timer
//
// Memory-maps a .bin file of BinaryTick records and streams them directly
// into Thread 1's SPSC queue. No heap allocation, no read() syscalls on
// the hot path — the OS page cache does all the work.
// =============================================================================
#include "goldmine/tick_packer.hpp"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <chrono>

namespace goldmine {

// ---------------------------------------------------------------------------
// MappedTickFile — mmap wrapper for BinaryTick arrays
// ---------------------------------------------------------------------------
class MappedTickFile {
public:
    MappedTickFile() = default;
    ~MappedTickFile() { close(); }

    MappedTickFile(const MappedTickFile&) = delete;
    MappedTickFile& operator=(const MappedTickFile&) = delete;

    bool open(const char* path) noexcept {
        fd_ = ::open(path, O_RDONLY);
        if (fd_ < 0) return false;

        struct stat st{};
        if (::fstat(fd_, &st) < 0) { close(); return false; }

        file_size_ = static_cast<std::size_t>(st.st_size);
        tick_count_ = file_size_ / sizeof(BinaryTick);

        if (tick_count_ == 0) { close(); return false; }

        data_ = static_cast<const BinaryTick*>(
            ::mmap(nullptr, file_size_, PROT_READ, MAP_PRIVATE | MAP_POPULATE, fd_, 0));

        if (data_ == MAP_FAILED) { data_ = nullptr; close(); return false; }

        // Advise sequential access for prefetch optimization
        ::madvise(const_cast<BinaryTick*>(data_), file_size_, MADV_SEQUENTIAL);
        return true;
    }

    void close() noexcept {
        if (data_) { ::munmap(const_cast<BinaryTick*>(data_), file_size_); data_ = nullptr; }
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
        file_size_ = 0; tick_count_ = 0;
    }

    [[nodiscard]] const BinaryTick* data()  const noexcept { return data_; }
    [[nodiscard]] std::size_t       count() const noexcept { return tick_count_; }
    [[nodiscard]] std::size_t       bytes() const noexcept { return file_size_; }

    [[nodiscard]] const BinaryTick& operator[](std::size_t i) const noexcept {
        return data_[i];
    }

private:
    int              fd_         = -1;
    const BinaryTick* data_     = nullptr;
    std::size_t      file_size_ = 0;
    std::size_t      tick_count_= 0;
};

// ---------------------------------------------------------------------------
// ReplayBenchmark — high-resolution throughput measurement
// ---------------------------------------------------------------------------
struct ReplayStats {
    std::size_t ticks_replayed   = 0;
    double      elapsed_seconds  = 0.0;
    double      ticks_per_second = 0.0;
    double      us_per_100k      = 0.0;  // microseconds per 100,000 ticks
};

class ReplayTimer {
public:
    void start() noexcept {
        start_ = std::chrono::high_resolution_clock::now();
    }

    [[nodiscard]] ReplayStats finish(std::size_t ticks) const noexcept {
        auto end = std::chrono::high_resolution_clock::now();
        double elapsed = std::chrono::duration<double>(end - start_).count();
        ReplayStats s{};
        s.ticks_replayed = ticks;
        s.elapsed_seconds = elapsed;
        s.ticks_per_second = (elapsed > 0) ? static_cast<double>(ticks) / elapsed : 0;
        s.us_per_100k = (ticks > 0) ? (elapsed * 1e6 * 100000.0) / static_cast<double>(ticks) : 0;
        return s;
    }

private:
    std::chrono::high_resolution_clock::time_point start_{};
};

// ---------------------------------------------------------------------------
// replay_into_pipeline — streams mapped ticks through the full pipeline
//
// Template parameter PipelineT must expose feed_gold_tick(Tick).
// DXY/XAG/Yield prices are passed as constants (or from co-feed files).
// ---------------------------------------------------------------------------
template<typename PipelineT>
ReplayStats replay_into_pipeline(const MappedTickFile& file,
                                  PipelineT& pipeline,
                                  Price dxy_price  = 104.50,
                                  Price xag_price  = 28.50,
                                  Price yield_price= 4.30) noexcept {
    ReplayTimer timer;
    timer.start();

    for (std::size_t i = 0; i < file.count(); ++i) {
        Tick t = file[i].to_tick();

        while (!pipeline.feed_gold_tick(t))
            __mm_pause();

        // Feed co-assets at 1/10 rate to avoid queue saturation
        if (i % 10 == 0) {
            DxyTick d{};
            d.timestamp_ns = t.timestamp_ns;
            d.price = dxy_price;
            d.volume = 50.0;
            while (!pipeline.feed_dxy_tick(d)) _mm_pause();
            while (!pipeline.feed_xag_tick(xag_price)) _mm_pause();
            while (!pipeline.feed_yield_tick(yield_price)) _mm_pause();
        }
    }

    return timer.finish(file.count());
}

} // namespace goldmine
