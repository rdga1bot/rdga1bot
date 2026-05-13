#ifdef RDGA1BOT_ALLOC_STATS
#include "AllocStats.h"
#include <new>
#include <cstdlib>
#include <sstream>

namespace AllocStats {
    std::atomic<uint64_t> total_count{0};
    std::atomic<uint64_t> total_bytes{0};

    std::string report() {
        std::ostringstream oss;
        oss << "[AllocStats] allocs=" << total_count.load(std::memory_order_relaxed)
            << " total_kb=" << total_bytes.load(std::memory_order_relaxed) / 1024;
        return oss.str();
    }

    void reset() {
        total_count.store(0, std::memory_order_relaxed);
        total_bytes.store(0, std::memory_order_relaxed);
    }
} // namespace AllocStats

void* operator new(std::size_t sz) {
    void* p = std::malloc(sz ? sz : 1);
    if (!p) throw std::bad_alloc{};
    AllocStats::total_count.fetch_add(1, std::memory_order_relaxed);
    AllocStats::total_bytes.fetch_add(static_cast<uint64_t>(sz), std::memory_order_relaxed);
    return p;
}

void* operator new[](std::size_t sz) { return ::operator new(sz); }

void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }

// Sized deallocation forms (C++14) — required to suppress linker warnings with -fsanitize
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
#endif
