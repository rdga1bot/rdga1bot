#pragma once
#include "LinearQModel.h"
#include "ExperienceBuffer.h"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <functional>
#include <memory>
#include <string>

// LearningWorker: виконує LinearQModel::updateBatch() в окремому потоці.
// Аналог GeodataWorker — не блокує main loop.
//
// Thread safety:
//   pushExperience() — викликається тільки з main thread
//   requestUpdate()  — thread-safe
//   getLastQValues() — thread-safe
class LearningWorker {
public:
    explicit LearningWorker(std::shared_ptr<LinearQModel> model,
                            std::shared_ptr<ExperienceBuffer> buffer,
                            int batch_size,
                            float learning_rate,
                            float discount_factor);
    ~LearningWorker();

    void start(int core_id = -1);
    void stop();
    bool isRunning() const { return m_running.load(); }

    // Встановити лог-функцію для повідомлень з worker thread.
    // Thread-safe: викликати до start() або після stop().
    void setLogFn(std::function<void(const std::string&)> fn);

    void pushExperience(Experience exp);
    void requestUpdate();
    Eigen::VectorXf getLastQValues() const;

    float lastLoss()    const { return m_model->lastLoss(); }
    int   updateCount() const { return m_model->updateCount(); }

private:
    void workerLoop();

    std::shared_ptr<LinearQModel>     m_model;
    std::shared_ptr<ExperienceBuffer> m_buffer;
    int    m_batch_size;
    float  m_learning_rate;
    float  m_discount_factor;

    std::thread             m_thread;
    std::mutex              m_mutex;
    std::condition_variable m_cv;
    std::atomic<bool>       m_running{false};
    std::atomic<bool>       m_update_requested{false};

    mutable std::mutex      m_qval_mutex;
    Eigen::VectorXf         m_last_q_values;

    mutable std::mutex                       m_log_mutex;
    std::function<void(const std::string&)>  m_log_fn;

    std::mt19937 m_rng{std::random_device{}()};
};
