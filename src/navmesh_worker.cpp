#include "navmesh_worker.h"
#include <iostream>
#ifdef __linux__
#include <sched.h>
#include <pthread.h>
#endif

NavMeshWorker::NavMeshWorker() = default;

NavMeshWorker::~NavMeshWorker() { Stop(); }

void NavMeshWorker::Start(std::shared_ptr<NavMeshBuilder> builder, int core_id) {
    if (m_running.load()) return;
    m_builder = builder;
    m_running = true;
    m_thread  = std::thread(&NavMeshWorker::WorkerLoop, this);
#ifdef __linux__
    if (core_id >= 0) {
        cpu_set_t cs; CPU_ZERO(&cs); CPU_SET((size_t)core_id, &cs);
        pthread_setaffinity_np(m_thread.native_handle(), sizeof(cs), &cs);
        std::cerr << "[NAV-W] Started on Core " << core_id << "\n";
    }
#endif
}

void NavMeshWorker::Stop() {
    if (!m_running.load()) return;
    { std::lock_guard<std::mutex> lk(m_mutex); m_running = false; }
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
}

void NavMeshWorker::RequestPath(const NavPathRequest& req) {
    if (!m_running.load()) return;
    { std::lock_guard<std::mutex> lk(m_mutex); m_requests.push(req); }
    m_cv.notify_one();
}

std::optional<NavPathResult> NavMeshWorker::TryGetResult() {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_results.empty()) return std::nullopt;
    auto r = std::move(m_results.front());
    m_results.pop();
    return r;
}

void NavMeshWorker::WorkerLoop() {
    while (m_running.load()) {
        NavPathRequest req;
        {
            std::unique_lock<std::mutex> lk(m_mutex);
            m_cv.wait_for(lk, std::chrono::milliseconds(50),
                [this]{ return !m_requests.empty() || !m_running.load(); });
            if (!m_running.load()) break;
            if (m_requests.empty()) continue;
            req = std::move(m_requests.front());
            m_requests.pop();
        }

        NavPathResult res;
        res.id = req.id;
        if (m_builder && m_builder->IsValid()) {
            res.path    = m_builder->FindPath(req.sx, req.sy, req.sz,
                                               req.ex, req.ey, req.ez);
            res.success = !res.path.empty();
        }
        { std::lock_guard<std::mutex> lk(m_mutex); m_results.push(std::move(res)); }
    }
}
