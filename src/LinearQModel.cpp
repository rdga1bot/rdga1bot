#include "LinearQModel.h"
#include <fstream>
#include <iostream>
#include <sstream>
#include <cmath>
#include <algorithm>

LinearQModel::LinearQModel(float huber_delta)
    : m_huber_delta(huber_delta) {
    m_weights = Eigen::MatrixXf::Random(NUM_FEATURES, NUM_ACTIONS)
                * (1.f / std::sqrt((float)NUM_FEATURES));
}

Eigen::VectorXf LinearQModel::getQValues(const Eigen::VectorXf& features) const {
    return m_weights.transpose() * features;
}

LinearQModel::Action LinearQModel::selectAction(
        const Eigen::VectorXf& features,
        float epsilon,
        std::mt19937& rng) const {
    std::uniform_real_distribution<float> uni(0.f, 1.f);
    if (uni(rng) < epsilon) {
        std::uniform_int_distribution<int> act(0, NUM_ACTIONS - 1);
        return static_cast<Action>(act(rng));
    }
    Eigen::VectorXf q = getQValues(features);
    int best = 0;
    q.maxCoeff(&best);
    return static_cast<Action>(best);
}

float LinearQModel::huberWeight(float residual) const {
    float abs_r = std::abs(residual);
    if (abs_r <= m_huber_delta)
        return 1.f;
    return m_huber_delta / abs_r;
}

void LinearQModel::updateBatch(
        const std::vector<const Experience*>& batch,
        float learning_rate,
        float discount_factor) {
    if (batch.empty()) return;
    const int N = (int)batch.size();

    for (int a = 0; a < NUM_ACTIONS; a++) {
        std::vector<int> idx;
        for (int i = 0; i < N; i++)
            if (batch[i]->action == a) idx.push_back(i);
        if (idx.empty()) continue;

        const int M = (int)idx.size();
        Eigen::MatrixXf Phi(M, NUM_FEATURES);
        Eigen::VectorXf targets(M);

        for (int k = 0; k < M; k++) {
            const Experience* e = batch[idx[k]];
            Phi.row(k) = e->state.transpose();

            float td_target;
            if (e->done) {
                td_target = e->reward;
            } else {
                Eigen::VectorXf q_next = getQValues(e->next_state);
                td_target = e->reward + discount_factor * q_next.maxCoeff();
            }
            targets(k) = td_target;
        }

        Eigen::VectorXf w_col = m_weights.col(a);
        static constexpr int MAX_IRLS_ITER = 5;

        for (int iter = 0; iter < MAX_IRLS_ITER; iter++) {
            Eigen::VectorXf residuals = targets - Phi * w_col;

            Eigen::VectorXf W_diag(M);
            for (int k = 0; k < M; k++)
                W_diag(k) = huberWeight(residuals(k));

            Eigen::MatrixXf WPhi = W_diag.asDiagonal() * Phi;
            Eigen::MatrixXf A    = WPhi.transpose() * WPhi;
            Eigen::VectorXf b    = WPhi.transpose()
                                   * (W_diag.asDiagonal() * targets);

            A.diagonal().array() += 1e-4f;
            Eigen::VectorXf w_new = A.ldlt().solve(b);
            w_col = w_col + learning_rate * (w_new - w_col);
        }
        m_weights.col(a) = w_col;
    }

    float total_loss = 0.f;
    for (const auto* e : batch) {
        float pred = getQValues(e->state)(e->action);
        float target_val;
        if (e->done) {
            target_val = e->reward;
        } else {
            target_val = e->reward
                + discount_factor * getQValues(e->next_state).maxCoeff();
        }
        float diff = target_val - pred;
        total_loss += diff * diff;
    }
    m_last_loss    = total_loss / (float)N;
    m_update_count++;
}

bool LinearQModel::saveWeights(const std::string& path) const {
    std::ofstream f(path);
    if (!f) return false;
    f << "{\n";
    f << "  \"num_features\": " << NUM_FEATURES << ",\n";
    f << "  \"num_actions\": "  << NUM_ACTIONS  << ",\n";
    f << "  \"update_count\": " << m_update_count << ",\n";
    f << "  \"last_loss\": "    << m_last_loss  << ",\n";
    f << "  \"weights\": [\n";
    for (int r = 0; r < NUM_FEATURES; r++) {
        f << "    [";
        for (int c = 0; c < NUM_ACTIONS; c++) {
            f << m_weights(r, c);
            if (c < NUM_ACTIONS - 1) f << ", ";
        }
        f << "]";
        if (r < NUM_FEATURES - 1) f << ",";
        f << "\n";
    }
    f << "  ]\n}\n";
    std::cerr << "[RL] Ваги збережено: " << path
              << " (loss=" << m_last_loss
              << " updates=" << m_update_count << ")\n";
    return true;
}

bool LinearQModel::loadWeights(const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        std::cerr << "[RL] weights.json не знайдено — старт з нуля\n";
        return false;
    }
    std::string line;
    int row = 0;
    bool in_weights = false;
    while (std::getline(f, line)) {
        if (line.find("\"weights\"") != std::string::npos) {
            in_weights = true; continue;
        }
        if (!in_weights || row >= NUM_FEATURES) continue;
        auto lb = line.find('[');
        auto rb = line.find(']');
        if (lb == std::string::npos || rb == std::string::npos) continue;
        std::string inner = line.substr(lb + 1, rb - lb - 1);
        std::istringstream ss(inner);
        std::string tok;
        int col = 0;
        while (std::getline(ss, tok, ',') && col < NUM_ACTIONS) {
            try { m_weights(row, col) = std::stof(tok); } catch (...) {}
            col++;
        }
        row++;
    }
    std::cerr << "[RL] Ваги завантажено з " << path << "\n";
    return true;
}
