#pragma once
#include <Eigen/Dense>
#include <vector>
#include <random>
#include <cstdint>

// Один перехід (s, a, r, s', done)
struct Experience {
    Eigen::VectorXf state;
    int             action;
    float           reward;
    Eigen::VectorXf next_state;
    bool            done;
};

// Циклічний буфер досвіду (без dynamic resize після init)
class ExperienceBuffer {
public:
    explicit ExperienceBuffer(int capacity)
        : m_capacity(capacity), m_size(0), m_head(0) {
        m_data.reserve(capacity);
    }

    void push(Experience exp) {
        if ((int)m_data.size() < m_capacity) {
            m_data.push_back(std::move(exp));
        } else {
            m_data[m_head] = std::move(exp);
        }
        m_head = (m_head + 1) % m_capacity;
        m_size = std::min(m_size + 1, m_capacity);
    }

    std::vector<const Experience*> sample(int batch_size,
                                          std::mt19937& rng) const {
        std::vector<const Experience*> batch;
        if (m_size == 0) return batch;
        batch.reserve(batch_size);
        std::uniform_int_distribution<int> dist(0, m_size - 1);
        for (int i = 0; i < batch_size; i++)
            batch.push_back(&m_data[dist(rng)]);
        return batch;
    }

    int  size()     const { return m_size; }
    int  capacity() const { return m_capacity; }
    bool ready(int min_size) const { return m_size >= min_size; }

private:
    int                     m_capacity;
    int                     m_size;
    int                     m_head;
    std::vector<Experience> m_data;
};
