#pragma once
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <optional>
#include <vector>
#include <functional>
#include <opencv2/opencv.hpp>
#include "Eyes.h"

// ── VisionWorker ──────────────────────────────────────────────────────────────
// Виконує важкі stateless OpenCV операції (DetectNPCs) у окремому потоці.
// Eyes залишається в main thread — VisionWorker отримує готовий BGR кадр
// і повертає результат DetectNPCs наступного тіку.
//
// Thread safety:
//   - SubmitFrame() / TryGetResult() — thread-safe (mutex)
//   - Eyes* передається ззовні — НЕ thread-safe, тільки читання config
//
// Feature flag: якщо disabled → Brain використовує sync DetectNPCs()
class VisionWorker {
public:
    // Результат async детекції NPC
    struct Result {
        uint64_t          frame_id  = 0;
        std::vector<Eyes::NPC> npcs;
        std::vector<Eyes::MinimapDot> minimap;
    };

    VisionWorker();
    ~VisionWorker();

    // Запустити воркер (один раз при старті).
    // core_id=-1 → без CPU affinity.
    void Start(int core_id = -1);
    void Stop();
    bool IsRunning() const { return m_running.load(); }

    // Відправити кадр на обробку (non-blocking).
    // frame робить clone — caller може відразу використовувати оригінал.
    // Якщо воркер зайнятий (черга повна) — кадр пропускається.
    void SubmitFrame(const cv::Mat& frame, uint64_t frame_id,
                     const Eyes& eyes_cfg);

    // Отримати готовий результат (non-blocking).
    // Повертає nullopt якщо результату ще немає.
    std::optional<Result> TryGetResult();

    // Діагностика: час обробки останнього кадру (мс)
    int LastProcessTimeMs() const { return m_last_process_ms.load(); }

private:
    struct Task {
        uint64_t   frame_id;
        cv::Mat    frame;    // cloned кадр
        // Мінімальний знімок конфігурації Eyes що потрібна для DetectNPCs
        // (копіюється з Eyes — уникаємо race condition)
        cv::Scalar npc_from_hsv;
        cv::Scalar npc_to_hsv;
        double     npc_color_threshold;
        int        npc_min_h, npc_max_h;
        int        npc_min_w, npc_max_w;
        int        npc_center_offset;
        int        minimap_cx, minimap_cy, minimap_radius;
        int        minimap_player_excl_r;
        float      minimap_dot_area_min;
    };

    void WorkerLoop();

    // Обробка: виконується в окремому потоці
    // Повертає результат без звернення до Eyes об'єкту
    Result ProcessTask(const Task& task);

    std::thread             m_thread;
    std::mutex              m_mutex;
    std::condition_variable m_cv;
    std::atomic<bool>       m_running{false};

    // Черга задач: максимум 2 кадри (старіші пропускаємо)
    static constexpr int    kMaxQueue = 2;
    std::optional<Task>     m_pending;  // єдина задача в черзі
    std::optional<Result>   m_result;   // готовий результат

    std::atomic<int>        m_last_process_ms{0};
    int                     m_core_id = -1;
};
