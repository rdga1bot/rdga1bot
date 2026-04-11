#include "LearningWorker.h"
#include <iostream>
#ifdef __linux__
#include <sched.h>
#include <pthread.h>
#endif

LearningWorker::LearningWorker(
        std::shared_ptr<LinearQModel>     model,
        std::shared_ptr<ExperienceBuffer> buffer,
        int   batch_size,
        float learning_rate,
        float discount_factor)
    : m_model(std::move(model))
    , m_buffer(std::move(buffer))
    , m_batch_size(batch_size)
    , m_learning_rate(learning_rate)
    , m_discount_factor(discount_factor)
    , m_last_q_values(Eigen::VectorXf::Zero(LinearQModel::NUM_ACTIONS))
{}

LearningWorker::~LearningWorker() { stop(); }

void LearningWorker::start(int core_id) {
    if (m_running.load()) return;
    m_running = true;
    m_thread  = std::thread(&LearningWorker::workerLoop, this);
#ifdef __linux__
    if (core_id >= 0) {
        cpu_set_t cs; CPU_ZERO(&cs); CPU_SET((size_t)core_id, &cs);
        pthread_setaffinity_np(m_thread.native_handle(), sizeof(cs), &cs);
        std::cerr << "[RL-W] Started on Core " << core_id << "\n";
    } else {
        std::cerr << "[RL-W] Started (no affinity)\n";
    }
#endif
}

void LearningWorker::stop() {
    if (!m_running.load()) return;
    { std::lock_guard<std::mutex> lk(m_mutex); m_running = false; }
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
}

void LearningWorker::pushExperience(Experience exp) {
    m_buffer->push(std::move(exp));
}

void LearningWorker::requestUpdate() {
    m_update_requested.store(true);
    m_cv.notify_one();
}

Eigen::VectorXf LearningWorker::getLastQValues() const {
    std::lock_guard<std::mutex> lk(m_qval_mutex);
    return m_last_q_values;
}

void LearningWorker::workerLoop() {
    while (m_running.load()) {
        {
            std::unique_lock<std::mutex> lk(m_mutex);
            m_cv.wait_for(lk, std::chrono::milliseconds(100),
                [this]{ return m_update_requested.load()
                             || !m_running.load(); });
        }
        if (!m_running.load()) break;
        if (!m_update_requested.exchange(false)) continue;

        if (!m_buffer->ready(m_batch_size)) continue;

        auto batch = m_buffer->sample(m_batch_size, m_rng);
        m_model->updateBatch(batch, m_learning_rate, m_discount_factor);

        if (m_model->updateCount() % 10 == 0) {
            std::cerr << "[RL-W] update #" << m_model->updateCount()
                      << " loss=" << m_model->lastLoss() << "\n";
        }
    }
}
