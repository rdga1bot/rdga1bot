#pragma once
#ifdef RDGA1BOT_ALLOC_STATS
#include <atomic>
#include <cstddef>
#include <string>

namespace AllocStats {
    extern std::atomic<uint64_t> total_count;
    extern std::atomic<uint64_t> total_bytes;
    std::string report();
    void reset();
} // namespace AllocStats
#endif
