#pragma once
#include <random>
#include <algorithm>
#include <cmath>

// ── RandomDelay ───────────────────────────────────────────────────────────────
// Варіативні затримки з нормальним розподілом для антидетекту.
// Get() повертає цілочисельну затримку в мілісекундах.
// Значення обрізаються до [mean/2 .. mean*2] щоб уникнути нереальних затримок.
//
// Використання:
//   RandomDelay delay(500, 75);   // mean=500мс, σ=75мс
//   int ms = delay.Get();         // ~500мс з σ=75
//
// Thread safety: NOT thread-safe — один екземпляр на потік.
class RandomDelay {
public:
    // mean_ms   — середня затримка (мілісекунди)
    // stddev_ms — стандартне відхилення; -1 = mean*0.15 (15%)
    RandomDelay(float mean_ms, float stddev_ms = -1.f)
        : m_mean(mean_ms)
        , m_stddev(stddev_ms < 0.f ? mean_ms * 0.15f : stddev_ms)
        , m_dist(mean_ms, m_stddev)
        , m_rng(std::random_device{}())
    {}

    // Повертає варіативну затримку в мілісекундах.
    // Гарантовано: [mean/2 .. mean*2]
    int Get() {
        if (m_mean <= 0.f) return 0;
        float v = m_dist(m_rng);
        v = std::max(m_mean * 0.5f, std::min(m_mean * 2.0f, v));
        return static_cast<int>(std::round(v));
    }

    // Оновити параметри (при hot-reload конфігурації)
    void SetParams(float mean_ms, float stddev_ms = -1.f) {
        m_mean   = mean_ms;
        m_stddev = (stddev_ms < 0.f) ? mean_ms * 0.15f : stddev_ms;
        m_dist   = std::normal_distribution<float>(m_mean, m_stddev);
    }

    float GetMean()   const { return m_mean; }
    float GetStdDev() const { return m_stddev; }

private:
    float m_mean;
    float m_stddev;
    std::normal_distribution<float> m_dist;
    std::mt19937                    m_rng;
};
