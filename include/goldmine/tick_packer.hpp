#pragma once
// =============================================================================
// tick_packer.hpp — CSV → dense binary tick converter + fixed-point format
//
// BinaryTick uses uint32_t fixed-point prices (5 decimal places) to halve
// cache bandwidth vs double. 20 bytes per tick → 50M ticks fit in 1 GB.
// =============================================================================
#include "goldmine/types.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace goldmine {

// ---------------------------------------------------------------------------
// BinaryTick — cache-friendly fixed-point tick (20 bytes, padded to 32)
// ---------------------------------------------------------------------------
struct alignas(32) BinaryTick {
    std::uint64_t timestamp_ns;
    std::uint32_t bid_raw;     // price * 100000 (5 decimal places)
    std::uint32_t ask_raw;
    std::uint32_t volume_raw;  // volume * 1000
    std::uint32_t _pad = 0;

    [[nodiscard]] Tick to_tick() const noexcept {
        return {
            .timestamp_ns = static_cast<Nanos>(timestamp_ns),
            .bid    = static_cast<double>(bid_raw) / 100000.0,
            .ask    = static_cast<double>(ask_raw) / 100000.0,
            .volume = static_cast<double>(volume_raw) / 1000.0,
            .last   = static_cast<double>((bid_raw + ask_raw) / 2) / 100000.0,
        };
    }

    static BinaryTick from_tick(const Tick& t) noexcept {
        BinaryTick bt{};
        bt.timestamp_ns = static_cast<std::uint64_t>(t.timestamp_ns);
        bt.bid_raw      = static_cast<std::uint32_t>(t.bid * 100000.0 + 0.5);
        bt.ask_raw      = static_cast<std::uint32_t>(t.ask * 100000.0 + 0.5);
        bt.volume_raw   = static_cast<std::uint32_t>(t.volume * 1000.0 + 0.5);
        return bt;
    }
};

static_assert(sizeof(BinaryTick) == 32);

// ---------------------------------------------------------------------------
// pack_csv_to_binary — reads CSV file, writes dense binary .bin file
//
// CSV format: Timestamp_ns,Bid,Ask,Volume  (header line skipped)
// Returns number of ticks written, or -1 on error.
// ---------------------------------------------------------------------------
inline long pack_csv_to_binary(const char* csv_path,
                               const char* bin_path) noexcept {
    FILE* fin = std::fopen(csv_path, "r");
    if (!fin) return -1;
    FILE* fout = std::fopen(bin_path, "wb");
    if (!fout) { std::fclose(fin); return -1; }

    // Skip header
    char line[512];
    if (!std::fgets(line, sizeof(line), fin)) {
        std::fclose(fin); std::fclose(fout); return -1;
    }

    long count = 0;
    while (std::fgets(line, sizeof(line), fin)) {
        std::uint64_t ts = 0;
        double bid = 0, ask = 0, vol = 0;
        if (std::sscanf(line, "%lu,%lf,%lf,%lf", &ts, &bid, &ask, &vol) != 4)
            continue;

        Tick t{};
        t.timestamp_ns = static_cast<Nanos>(ts);
        t.bid = bid; t.ask = ask; t.volume = vol;
        t.last = (bid + ask) * 0.5;

        BinaryTick bt = BinaryTick::from_tick(t);
        std::fwrite(&bt, sizeof(BinaryTick), 1, fout);
        ++count;
    }

    std::fclose(fin);
    std::fclose(fout);
    return count;
}

} // namespace goldmine
