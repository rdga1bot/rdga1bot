#pragma once
#include <Eigen/Dense>
#include <string>
#include <vector>
#include <random>
#include "FeatureExtractor.h"
#include "ExperienceBuffer.h"

// Лінійна Q-функція: Q(s,a) = W[:,a]^T * phi(s)
// W: матриця NUM_FEATURES x NUM_ACTIONS
// Навчання: IRLS з Huber-вагами (стійко до викидів від смертей)
class LinearQModel {
public:
    enum class Action : int {
        TargetNearest   = 0,
        TargetWeighted  = 1,
        NavigateMemory  = 2,
        Patrol          = 3,
        RestNow         = 4,
        BuffNow         = 5,
        NUM_ACTIONS     = 6
    };

    static constexpr int NUM_ACTIONS  = (int)Action::NUM_ACTIONS;
    static constexpr int NUM_FEATURES = FeatureExtractor::NUM_FEATURES;

    explicit LinearQModel(float huber_delta = 1.345f);

    Eigen::VectorXf getQValues(const Eigen::VectorXf& features) const;

    Action selectAction(const Eigen::VectorXf& features,
                        float epsilon, std::mt19937& rng) const;

    // IRLS batch update з Huber-вагами (виклик з LearningWorker)
    void updateBatch(const std::vector<const Experience*>& batch,
                     float learning_rate,
                     float discount_factor);

    bool saveWeights(const std::string& path) const;
    bool loadWeights(const std::string& path);

    float lastLoss()    const { return m_last_loss; }
    int   updateCount() const { return m_update_count; }

private:
    Eigen::MatrixXf m_weights;   // NUM_FEATURES x NUM_ACTIONS
    float           m_huber_delta;
    float           m_last_loss    = 0.f;
    int             m_update_count = 0;

    float huberWeight(float residual) const;
};
