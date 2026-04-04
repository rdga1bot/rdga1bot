#include "geodata_worker.h"
#include "Geodata.h"
#include <iostream>
#include <cmath>

#ifdef __linux__
#include <sched.h>
#include <pthread.h>
#endif

GeodataWorker::GeodataWorker() = default;
GeodataWorker::~GeodataWorker() { Stop(); }

void GeodataWorker::Start(const Geodata* geo, int core_id) {
    if (!geo || m_running.load()) return;
    m_geo     = geo;
    m_core_id = core_id;
    m_running = true;
    m_thread  = std::thread(&GeodataWorker::WorkerLoop, this);

#ifdef __linux__
    if (core_id >= 0) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET((size_t)core_id, &cpuset);
        pthread_setaffinity_np(m_thread.native_handle(),
                               sizeof(cpu_set_t), &cpuset);
        std::cerr << "[GEO-W] Started on Core " << core_id << "\n";
    } else {
        std::cerr << "[GEO-W] Started (no affinity)\n";
    }
#endif
}

void GeodataWorker::Stop() {
    if (!m_running.load()) return;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_running = false;
    }
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
}

void GeodataWorker::RequestPath(const PathRequest& req) {
    if (!m_running.load()) return;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_pending = req; // новий запит замінює старий
    }
    m_cv.notify_one();
}

std::optional<PathResult> GeodataWorker::TryGetResult() {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (!m_result.has_value()) return std::nullopt;
    auto r = std::move(m_result);
    m_result = std::nullopt;
    return r;
}

void GeodataWorker::WorkerLoop() {
    while (m_running.load()) {
        PathRequest req;
        {
            std::unique_lock<std::mutex> lk(m_mutex);
            m_cv.wait_for(lk, std::chrono::milliseconds(50),
                [this]{ return m_pending.has_value() || !m_running.load(); });
            if (!m_running.load()) break;
            if (!m_pending.has_value()) continue;
            req = std::move(*m_pending);
            m_pending = std::nullopt;
        }

        auto t0 = std::chrono::steady_clock::now();

        PathResult res;
        res.id   = req.id;
        res.path = m_geo->FindPath(req.sx, req.sy, req.sz,
                                   req.ex, req.ey, req.ez,
                                   req.maxRange);
        res.success  = !res.path.empty();
        float dx = req.ex - req.sx, dy = req.ey - req.sy;
        res.distance = std::sqrt(dx*dx + dy*dy);
        res.time_ms  = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();

        if (res.time_ms > 40)
            std::cerr << "[GEO-W] Path #" << req.id
                      << " in " << res.time_ms << "ms"
                      << " nodes=" << res.path.size() << "\n";

        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_result = std::move(res);
        }
    }
}
