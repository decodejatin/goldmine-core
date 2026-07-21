#pragma once
// =============================================================================
// param_shm.hpp — Zero-Syscall Dynamic Parameter Updates (Module 2 & 4)
//
// Mmaps a 64-byte shared memory region for C++ to instantly read parameter
// updates from Python ML processes with zero I/O and zero locks.
// =============================================================================
#include <atomic>
#include <cstdint>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdexcept>
#include <cstdio>

namespace goldmine {

struct alignas(64) DynamicParams {
    alignas(64) std::atomic<uint64_t> version{0};
    alignas(64) std::atomic<double> risk_pct{2.0};
    alignas(64) std::atomic<double> tp_multiplier{0.35};
    alignas(64) std::atomic<double> sl_multiplier{1.5};
    alignas(64) std::atomic<double> conv_threshold{0.50};
    alignas(64) std::atomic<uint32_t> p_profitable_gate_bps{6000}; // 60.00%
    alignas(64) std::atomic<uint8_t> regime_id{0};
};

class ParamServerSHM {
public:
    ParamServerSHM() = default;

    ~ParamServerSHM() {
        if (params_ != MAP_FAILED && params_ != nullptr) {
            munmap(params_, sizeof(DynamicParams));
        }
        if (fd_ != -1) {
            close(fd_);
        }
    }

    bool init(const char* shm_name = "/goldmine_param_shm", bool create = false) {
        int oflag = O_RDWR;
        if (create) {
            oflag |= O_CREAT;
        }

        fd_ = shm_open(shm_name, oflag, 0666);
        if (fd_ < 0) {
            std::fprintf(stderr, "Failed to open SHM for parameters: %s\n", shm_name);
            return false;
        }

        if (create) {
            if (ftruncate(fd_, sizeof(DynamicParams)) == -1) {
                std::fprintf(stderr, "Failed to truncate SHM.\n");
                return false;
            }
        }

#ifndef MAP_HUGETLB
#define MAP_HUGETLB 0x40000
#endif
        void* ptr = mmap(nullptr, sizeof(DynamicParams), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_HUGETLB, fd_, 0);
        if (ptr == MAP_FAILED) {
            ptr = mmap(nullptr, sizeof(DynamicParams), PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
            if (ptr == MAP_FAILED) {
                std::fprintf(stderr, "Failed to mmap SHM for parameters.\n");
                return false;
            }
            std::printf("[SHM] HugePages unavailable. Fallback to standard 4KB RAM paging.\n");
        } else {
            std::printf("[SHM] HugePages allocation successful\n");
        }

        params_ = static_cast<DynamicParams*>(ptr);

        if (create) {
            // Initialize with default values using placement new to setup atomics
            new (params_) DynamicParams();
        }

        return true;
    }

    DynamicParams* get() const noexcept {
        return params_;
    }

private:
    int fd_{-1};
    DynamicParams* params_{nullptr};
};

} // namespace goldmine
