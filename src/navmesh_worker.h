#pragma once
#include "navmesh_builder.h"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <queue>
#include <optional>
#include <memory>

struct NavPathRequest {
    float sx, sy, sz, ex, ey, ez;
    uint64_t id = 0;
};

struct NavPathResult {
    uint64_t id = 0;
    std::vector<std::pair<float,float>> path;
    bool success = false;
};

// ── NavMeshWorker ─────────────────────────────────────────────────────────────
// Async pathfinding через NavMeshBuilder на окремому потоці (Core 4 або будь-який).
// API аналогічний GeodataWorker.
class NavMeshWorker {
public:
    NavMeshWorker();
    ~NavMeshWorker();

    void Start(std::shared_ptr<NavMeshBuilder> builder, int core_id = -1);
    void Stop();
    bool IsRunning() const { return m_running.load(); }

    void RequestPath(const NavPathRequest& req);
    std::optional<NavPathResult> TryGetResult();

private:
    void WorkerLoop();

    std::shared_ptr<NavMeshBuilder>  m_builder;
    std::thread                      m_thread;
    std::mutex                       m_mutex;
    std::condition_variable          m_cv;
    std::atomic<bool>                m_running{false};
    std::queue<NavPathRequest>       m_requests;
    std::queue<NavPathResult>        m_results;
};
