#include "vision_worker.h"
#include <iostream>
#include <cstring>

#ifdef __linux__
#include <sched.h>
#include <pthread.h>
#endif

VisionWorker::VisionWorker() = default;

VisionWorker::~VisionWorker() {
    Stop();
}

void VisionWorker::Start(int core_id) {
    if (m_running.load()) return;
    m_core_id   = core_id;
    m_running   = true;
    m_thread    = std::thread(&VisionWorker::WorkerLoop, this);

#ifdef __linux__
    if (core_id >= 0) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET((size_t)core_id, &cpuset);
        pthread_setaffinity_np(m_thread.native_handle(),
                               sizeof(cpu_set_t), &cpuset);
        std::cerr << "[VIS-W] Started on Core " << core_id << "\n";
    } else {
        std::cerr << "[VIS-W] Started (no affinity)\n";
    }
#endif
}

void VisionWorker::Stop() {
    if (!m_running.load()) return;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_running = false;
    }
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
}

void VisionWorker::SubmitFrame(const cv::Mat& frame, uint64_t frame_id,
                                const Eyes& e) {
    if (!m_running.load()) return;

    Task task;
    task.frame_id            = frame_id;
    task.frame               = frame.clone();
    // Копіюємо тільки потрібні параметри конфігурації (без race condition)
    task.npc_from_hsv        = e.m_npc_name_color_from_hsv;
    task.npc_to_hsv          = e.m_npc_name_color_to_hsv;
    task.npc_color_threshold = e.m_npc_name_color_threshold;
    task.npc_min_h           = e.m_npc_name_min_height;
    task.npc_max_h           = e.m_npc_name_max_height;
    task.npc_min_w           = e.m_npc_name_min_width;
    task.npc_max_w           = e.m_npc_name_max_width;
    task.npc_center_offset   = e.m_npc_name_center_offset;
    task.minimap_cx          = e.m_minimap_cx_in_roi;
    task.minimap_cy          = e.m_minimap_cy_in_roi;
    task.minimap_radius      = e.m_minimap_radius;
    task.minimap_player_excl_r = e.m_minimap_player_excl_r;
    task.minimap_dot_area_min  = e.m_minimap_dot_area_min;

    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_pending = std::move(task); // новий кадр замінює старий
    }
    m_cv.notify_one();
}

std::optional<VisionWorker::Result> VisionWorker::TryGetResult() {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (!m_result.has_value()) return std::nullopt;
    auto r = std::move(m_result);
    m_result = std::nullopt;
    return r;
}

void VisionWorker::WorkerLoop() {
    while (m_running.load()) {
        Task task;
        {
            std::unique_lock<std::mutex> lk(m_mutex);
            m_cv.wait_for(lk, std::chrono::milliseconds(20),
                [this]{ return m_pending.has_value() || !m_running.load(); });
            if (!m_running.load()) break;
            if (!m_pending.has_value()) continue;
            task = std::move(*m_pending);
            m_pending = std::nullopt;
        }

        auto t0 = std::chrono::steady_clock::now();
        auto result = ProcessTask(task);
        auto ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        m_last_process_ms.store(ms);

        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_result = std::move(result);
        }
    }
}

// ProcessTask: stateless обробка кадру без звернення до Eyes об'єкту.
// Повторює логіку DetectNPCs() і DetectMinimap() використовуючи
// скопійовані параметри конфігурації.
VisionWorker::Result VisionWorker::ProcessTask(const Task& t) {
    Result res;
    res.frame_id = t.frame_id;

    if (t.frame.empty()) return res;

    // ── NPC Detection (спрощена версія DetectNPCs) ────────────────────────────
    // Конвертуємо в HSV для пошуку NPC назв
    cv::Mat hsv;
    cv::cvtColor(t.frame, hsv, cv::COLOR_BGR2HSV);

    cv::Mat mask;
    cv::inRange(hsv, t.npc_from_hsv, t.npc_to_hsv, mask);

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(mask, contours, cv::RETR_EXTERNAL,
                     cv::CHAIN_APPROX_SIMPLE);

    for (const auto& c : contours) {
        cv::Rect r = cv::boundingRect(c);
        if (r.height < t.npc_min_h || r.height > t.npc_max_h) continue;
        if (r.width  < t.npc_min_w || r.width  > t.npc_max_w) continue;

        // Перевірка порогу кольору
        cv::Mat roi = mask(r);
        double ratio = (double)cv::countNonZero(roi) / (r.width * r.height);
        if (ratio < t.npc_color_threshold) continue;

        Eyes::NPC npc;
        npc.rect   = r;
        npc.center = { r.x + r.width / 2,
                       r.y + r.height + t.npc_center_offset };
        npc.state  = Eyes::NPC::State::Default;
        npc.name_id = 0;
        npc.tracking_id = 0;
        res.npcs.push_back(npc);
    }

    // ── Minimap Detection ─────────────────────────────────────────────────────
    int roi_w = t.frame.cols < 185 ? t.frame.cols : 185;
    cv::Rect minimap_roi(t.frame.cols - roi_w, 0, roi_w, 165);
    if (minimap_roi.y + minimap_roi.height > t.frame.rows)
        minimap_roi.height = t.frame.rows;
    if (minimap_roi.width <= 0 || minimap_roi.height <= 0) return res;

    cv::Mat mm_bgr = t.frame(minimap_roi);
    cv::Mat mm_hsv;
    cv::cvtColor(mm_bgr, mm_hsv, cv::COLOR_BGR2HSV);

    cv::Mat mm_mask1, mm_mask2, mm_mask;
    cv::inRange(mm_hsv, cv::Scalar(0,  100, 80),
                        cv::Scalar(20, 255,255), mm_mask1);
    cv::inRange(mm_hsv, cv::Scalar(165,100, 80),
                        cv::Scalar(180,255,255), mm_mask2);
    cv::bitwise_or(mm_mask1, mm_mask2, mm_mask);

    std::vector<std::vector<cv::Point>> mm_contours;
    cv::findContours(mm_mask, mm_contours, cv::RETR_EXTERNAL,
                     cv::CHAIN_APPROX_SIMPLE);

    cv::Point center(t.minimap_cx, t.minimap_cy);
    for (const auto& mc : mm_contours) {
        double area = cv::contourArea(mc);
        if (area < (double)t.minimap_dot_area_min) continue;
        cv::Moments M = cv::moments(mc);
        if (M.m00 == 0) continue;
        int cx = (int)(M.m10 / M.m00);
        int cy = (int)(M.m01 / M.m00);
        float dist = std::sqrt((float)((cx-center.x)*(cx-center.x) +
                                       (cy-center.y)*(cy-center.y)));
        if (dist < (float)t.minimap_player_excl_r) continue;
        if (dist > (float)t.minimap_radius) continue;

        Eyes::MinimapDot dot;
        dot.dx       = cx - center.x;
        dot.dy       = cy - center.y;
        dot.dist     = dist;
        dot.selected = false;
        res.minimap.push_back(dot);
    }

    return res;
}
