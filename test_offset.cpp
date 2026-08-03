#include <iostream>
#include <atomic>
#include <cstdint>
#include <cstddef>
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
int main() {
    std::cout << "seq: " << offsetof(SharedTick, sequence_id) << "\n";
    std::cout << "ts: " << offsetof(SharedTick, timestamp_ms) << "\n";
    std::cout << "bid: " << offsetof(SharedTick, bid) << "\n";
    std::cout << "magic: " << offsetof(SharedTick, magic_header) << "\n";
    return 0;
}
