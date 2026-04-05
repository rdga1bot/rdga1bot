#pragma once
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <optional>
#include <vector>
#include <utility>
#include <cstdint>
#include <unordered_map>
#include <chrono>

// ── GeodataWorker ─────────────────────────────────────────────────────────────
// Виконує A* FindPath() у окремому потоці щоб не блокувати Brain (50мс+).
// Geodata stateless для FindPath() → thread-safe.
//
// Використання:
//   1. g_geodata_worker.Start(geodata_ptr, core_id)
//   2. g_geodata_worker.RequestPath(sx,sy,sz, ex,ey,ez, range, id)
//   3. Наступний тік: g_geodata_worker.TryGetResult() → optional<PathResult>

class Geodata; // forward declaration

struct PathRequest {
    float    sx, sy, sz;
    float    ex, ey, ez;
    float    maxRange = 5000.f;
    uint64_t id       = 0;
};

struct PathResult {
    uint64_t                         id      = 0;
    std::vector<std::pair<float,float>> path;
    bool                             success = false;
    float                            distance = 0.f;
    int                              time_ms  = 0;
};

class GeodataWorker {
public:
    GeodataWorker();
    ~GeodataWorker();

    // Запустити воркер з вказівником на завантажену Geodata.
    // geo не може бути nullptr. core_id=-1 → без affinity.
    void Start(const Geodata* geo, int core_id = -1);
    void Stop();
    bool IsRunning() const { return m_running.load(); }

    // Запросити пошук шляху (non-blocking).
    // Якщо воркер зайнятий — новий запит замінює старий (старий шлях не потрібен).
    void RequestPath(const PathRequest& req);

    // Отримати готовий результат (non-blocking).
    std::optional<PathResult> TryGetResult();

    int LastTimeMs() const { return m_last_time_ms.load(); }

private:
    void WorkerLoop();

    const Geodata*          m_geo = nullptr;
    std::thread             m_thread;
    std::mutex              m_mutex;
    std::condition_variable m_cv;
    std::atomic<bool>       m_running{false};

    std::optional<PathRequest>  m_pending;
    std::optional<PathResult>   m_result;
    std::atomic<int>            m_last_time_ms{0};
    int                         m_core_id = -1;

    // Path cache
    struct CachedPath {
        std::vector<std::pair<float,float>> path;
        std::chrono::steady_clock::time_point ts;
    };
    std::unordered_map<uint64_t, CachedPath> m_cache;
    static constexpr int CACHE_TTL_MS = 500;
    static constexpr int CACHE_MAX    = 32;

    static uint64_t PathHash(float sx, float sy, float ex, float ey) {
        // Точність до 256 L2u (1 блок)
        auto q = [](float v){ return (uint64_t)((int)(v / 256.0f) + 1280); };
        return q(sx) | (q(sy) << 12) | (q(ex) << 24) | (q(ey) << 36);
    }
};
