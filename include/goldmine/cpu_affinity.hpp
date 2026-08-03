#pragma once
// =============================================================================
// cpu_affinity.hpp — Hardware Optimization & Core Pinning
//
// Pins threads to specific physical CPU cores to eliminate context switching
// and maximize L1/L2 cache residency.
// =============================================================================

#include <pthread.h>
#include <sched.h>
#include <cstdio>
#include <cstring>
#include <cerrno>

namespace goldmine {

inline void pin_thread_to_core(int core_id) noexcept {
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    
    // Check available cores granted to the process
    if (sched_getaffinity(0, sizeof(cpu_set_t), &cpuset) == 0) {
        int available_cores = CPU_COUNT(&cpuset);
        
        if (core_id >= available_cores || !CPU_ISSET(core_id, &cpuset)) {
            std::fprintf(stderr, "[AFFINITY WARN] Core %d unavailable (Max available: %d). Running unpinned on OS scheduler.\n", 
                         core_id, available_cores);
            return;
        }
    }

    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);

    pthread_t current_thread = pthread_self();
    int rc = pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);
    if (rc != 0) {
        std::fprintf(stderr, "[AFFINITY WARN] Failed to pin thread to core %d (Error %d: %s). Running unpinned on OS scheduler.\n", 
                     core_id, rc, std::strerror(rc));
    } else {
        std::printf("[AFFINITY] Thread successfully pinned to CPU core %d.\n", core_id);
    }
#else
    // Windows/Mac fallback if needed, but Goldmine is Linux-native
    (void)core_id;
#endif
}

inline void auto_pin_thread(int thread_role_index) noexcept {
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    
    if (sched_getaffinity(0, sizeof(cpu_set_t), &cpuset) == 0) {
        int available_cores = CPU_COUNT(&cpuset);
        if (available_cores > 0) {
            int target_core = thread_role_index % available_cores;
            pin_thread_to_core(target_core);
        }
    }
#else
    (void)thread_role_index;
#endif
}

} // namespace goldmine
