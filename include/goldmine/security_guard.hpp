#pragma once
// =============================================================================
// security_guard.hpp — Memory-Locked API Key Vault (Module 3)
//
// Prevents sensitive API keys from being swapped to disk.
// =============================================================================
#include <string>
#include <vector>
#include <cstring>
#include <sys/mman.h>
#include <stdexcept>
#include <cstdio>

namespace goldmine {

class SecureKeyVault {
public:
    explicit SecureKeyVault(const std::string& key) {
        length_ = key.length();
        // Allocate page-aligned memory for mlock
        if (posix_memalign(&memory_, 4096, ((length_ + 4095) / 4096) * 4096) != 0) {
            throw std::runtime_error("SecureKeyVault: posix_memalign failed");
        }

        if (mlock(memory_, length_) != 0) {
            std::fprintf(stderr, "[WARNING] SecureKeyVault: mlock failed, keys may swap to disk!\n");
        }

        std::memcpy(memory_, key.c_str(), length_);
    }

    ~SecureKeyVault() {
        if (memory_) {
            // Zero out memory before unlocking
            std::memset(memory_, 0, length_);
            munlock(memory_, length_);
            free(memory_);
        }
    }

    // Disable copy/move
    SecureKeyVault(const SecureKeyVault&) = delete;
    SecureKeyVault& operator=(const SecureKeyVault&) = delete;

    [[nodiscard]] const char* data() const noexcept {
        return static_cast<const char*>(memory_);
    }

    [[nodiscard]] size_t length() const noexcept {
        return length_;
    }

private:
    void* memory_ = nullptr;
    size_t length_ = 0;
};

} // namespace goldmine
