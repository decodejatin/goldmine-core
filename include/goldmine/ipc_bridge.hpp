#pragma once
// =============================================================================
// ipc_bridge.hpp — POSIX Shared Memory IPC Bridge for MetaTrader 5
//
// Lock-free double-buffered ring of TradeCommands via shm_open/mmap.
// Thread 4 writes commands, MT5 Python bridge reads them.
// Sub-microsecond latency — no kernel copies, no syscalls on hot path.
// =============================================================================
#include "goldmine/types.hpp"
#include <atomic>
#include <cstring>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace goldmine {

// ---------------------------------------------------------------------------
// TradeCommand — cache-line-aligned IPC command structure
// ---------------------------------------------------------------------------
struct alignas(64) TradeCommand {
    std::uint64_t sequence_id;
    std::uint64_t timestamp_ns;
    std::uint8_t  action;       // 1=BUY, 2=SELL, 3=CLOSE
    double        lot_size;
    double        stop_loss;
    double        take_profit;
    double        entry_price;
    double        conviction;
    std::atomic<std::uint8_t> status;  // 0=PENDING, 1=EXECUTED, 2=REJECTED
    char          _pad[5];
};

static_assert(sizeof(TradeCommand) <= 128);

// ---------------------------------------------------------------------------
// SharedMemoryHeader — placed at the start of the mapped region
// ---------------------------------------------------------------------------
inline constexpr std::size_t IPC_RING_SIZE = 64;  // power of 2
inline constexpr const char* SHM_NAME = "/goldmine_mt5_bridge";

struct alignas(CACHE_LINE) SharedMemoryHeader {
    alignas(CACHE_LINE) std::atomic<std::uint64_t> write_seq{0};
    alignas(CACHE_LINE) std::atomic<std::uint64_t> read_seq{0};
    alignas(CACHE_LINE) std::uint64_t ring_size = IPC_RING_SIZE;
    alignas(CACHE_LINE) std::uint64_t magic = 0x474F4C444D494E45ULL; // "GOLDMINE"
};

// ---------------------------------------------------------------------------
// IpcBridge — producer side (Thread 4 writes commands)
// ---------------------------------------------------------------------------
class IpcBridge {
public:
    IpcBridge() = default;
    ~IpcBridge() { close(); }

    IpcBridge(const IpcBridge&) = delete;
    IpcBridge& operator=(const IpcBridge&) = delete;

    bool open() noexcept {
        const std::size_t total_size = sizeof(SharedMemoryHeader)
            + IPC_RING_SIZE * sizeof(TradeCommand);

        fd_ = ::shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
        if (fd_ < 0) return false;

        if (::ftruncate(fd_, static_cast<off_t>(total_size)) < 0) {
            close(); return false;
        }

        base_ = static_cast<std::uint8_t*>(
            ::mmap(nullptr, total_size, PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd_, 0));

        if (base_ == MAP_FAILED) { base_ = nullptr; close(); return false; }

        total_size_ = total_size;
        header_ = reinterpret_cast<SharedMemoryHeader*>(base_);
        ring_ = reinterpret_cast<TradeCommand*>(base_ + sizeof(SharedMemoryHeader));

        // Initialize header (only if we're first)
        header_->ring_size = IPC_RING_SIZE;
        header_->magic = 0x474F4C444D494E45ULL;

        return true;
    }

    bool push_command(const EngineSignal& sig, Price entry, double lot) noexcept {
        if (!header_) return false;

        std::uint64_t seq = header_->write_seq.load(std::memory_order_relaxed);
        std::uint64_t read = header_->read_seq.load(std::memory_order_acquire);

        // Full check
        if (seq - read >= IPC_RING_SIZE) return false;

        TradeCommand& cmd = ring_[seq % IPC_RING_SIZE];
        cmd.sequence_id  = seq;
        cmd.timestamp_ns = static_cast<std::uint64_t>(sig.signal_ns);
        cmd.action       = (sig.direction == Direction::LONG) ? 1 :
                           (sig.direction == Direction::SHORT) ? 2 : 3;
        cmd.lot_size     = lot;
        cmd.stop_loss    = sig.suggested_sl;
        cmd.take_profit  = sig.suggested_tp;
        cmd.entry_price  = entry;
        cmd.conviction   = sig.conviction;
        cmd.status.store(0, std::memory_order_release);  // PENDING

        header_->write_seq.store(seq + 1, std::memory_order_release);
        return true;
    }

    void close() noexcept {
        if (base_) {
            ::munmap(base_, total_size_);
            base_ = nullptr; header_ = nullptr; ring_ = nullptr;
        }
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    static void unlink() noexcept { ::shm_unlink(SHM_NAME); }

private:
    int                   fd_         = -1;
    std::uint8_t*         base_       = nullptr;
    std::size_t           total_size_ = 0;
    SharedMemoryHeader*   header_     = nullptr;
    TradeCommand*         ring_       = nullptr;
};

} // namespace goldmine
