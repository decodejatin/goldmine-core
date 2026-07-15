#pragma once
// =============================================================================
// replay_engine.hpp — Deterministic Offline Playback (Module 4)
//
// Records raw binary ticks and plays them back at maximum speed for
// deterministic offline simulation.
// =============================================================================

#include "goldmine/live_ingestion.hpp"
#include <string>
#include <vector>
#include <fstream>
#include <stdexcept>
#include <cstdio>

namespace goldmine {

class ReplayEngine {
public:
    explicit ReplayEngine(const std::string& bin_file) {
        file_ = std::fopen(bin_file.c_str(), "rb");
        if (!file_) {
            throw std::runtime_error("ReplayEngine: Failed to open " + bin_file);
        }
        std::printf("[REPLAY] Loaded deterministic playback file: %s\n", bin_file.c_str());
    }

    ~ReplayEngine() {
        if (file_) {
            std::fclose(file_);
        }
    }

    // Disable copy/move
    ReplayEngine(const ReplayEngine&) = delete;
    ReplayEngine& operator=(const ReplayEngine&) = delete;

    [[nodiscard]] bool read_next(SharedTick& out_tick) noexcept {
        if (!file_) return false;
        size_t read_bytes = std::fread(&out_tick, 1, sizeof(SharedTick), file_);
        return read_bytes == sizeof(SharedTick);
    }

private:
    std::FILE* file_ = nullptr;
};

class ReplayRecorder {
public:
    explicit ReplayRecorder(const std::string& bin_file) {
        file_ = std::fopen(bin_file.c_str(), "wb");
        if (!file_) {
            std::fprintf(stderr, "[WARNING] ReplayRecorder: Failed to open %s\n", bin_file.c_str());
        }
    }

    ~ReplayRecorder() {
        if (file_) {
            std::fflush(file_);
            std::fclose(file_);
        }
    }

    ReplayRecorder(const ReplayRecorder&) = delete;
    ReplayRecorder& operator=(const ReplayRecorder&) = delete;

    void record(const SharedTick& tick) noexcept {
        if (file_) {
            std::fwrite(&tick, 1, sizeof(SharedTick), file_);
        }
    }

private:
    std::FILE* file_ = nullptr;
};

} // namespace goldmine
