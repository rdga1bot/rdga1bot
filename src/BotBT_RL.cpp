// BotBT_RL.cpp
// Reinforcement Learning integration: initRL, shutdownRL,
// rlPreTick, rlPostTick.
//
// initRL/shutdownRL — lifecycle: LinearQModel + LearningWorker.
// rlPreTick  — ДО BT tick: epsilon-greedy вибір дії → m_rl_suggested_action.
// rlPostTick — ПІСЛЯ BT tick: reward → Experience → requestUpdate (async IRLS).
//
// Сигнальні поля (встановлюються в actAttack/actDead/actBuff):
//   m_rl_sig_kill, m_rl_sig_death, m_rl_sig_targeting_failed, m_rl_sig_buff_done
//
// ВАЖЛИВО: RL НЕ переставляє гілки BT — тільки hints через m_rl_suggested_action.
// Активні overrides: condNeedsRest (RestNow) і condNeedsBuff (BuffNow) при
// confidence > 0.5f (softmax fix від'ємних Q через maxCoeff).
// НЕ змінювати поріг confidence без тестування (MR34).
#include "BotBehaviorTree.h"
#include "FeatureExtractor.h"
#include "RewardCalculator.h"
#include "Blackboard.h"
#include "BotMood.h"

void BotBehaviorTree::initRL(const Config& cfg) {
    const auto& lc = cfg.learning;
    if (!lc.enabled) return;

    m_rl_weights_file = lc.weights_file;

    m_rl_model  = std::make_shared<LinearQModel>(lc.huber_delta);
    m_rl_buffer = std::make_shared<ExperienceBuffer>(lc.buffer_size);
    m_rl_worker = std::make_unique<LearningWorker>(
        m_rl_model, m_rl_buffer,
        lc.batch_size, lc.learning_rate, lc.discount_factor);

    if (m_log_fn) m_rl_worker->setLogFn(m_log_fn);

    m_rl_model->loadWeights(lc.weights_file);

    m_rl_epsilon       = lc.epsilon_start;
    m_rl_has_prev      = false;
    m_rl_last_features = Eigen::VectorXf::Zero(LinearQModel::NUM_FEATURES);

    m_rl_worker->start(/*core_id=*/-1);
    if (m_log_fn) m_log_fn("[RL] LearningWorker запущено. epsilon=" + std::to_string(m_rl_epsilon));
    else std::cerr << "[RL] LearningWorker запущено. epsilon=" << m_rl_epsilon << "\n";
}

void BotBehaviorTree::shutdownRL() {
    if (m_rl_worker) {
        m_rl_worker->stop();
        m_rl_worker.reset();
    }
    if (m_rl_model) {
        const std::string path = m_rl_weights_file.empty() ? "./weights.json" : m_rl_weights_file;
        m_rl_model->saveWeights(path);
        if (m_log_fn) m_log_fn("[RL] Ваги збережено: " + path);
        m_rl_model.reset();
    }
    m_rl_buffer.reset();
}

void BotBehaviorTree::rlPreTick(GameState& gs) {
    if (!m_rl_model) return;

    m_rl_sig_kill             = false;
    m_rl_sig_death            = false;
    m_rl_sig_targeting_failed = false;
    m_rl_sig_buff_done        = false;

    Eigen::VectorXf phi = FeatureExtractor::extract(gs);

    LinearQModel::Action action =
        m_rl_model->selectAction(phi, m_rl_epsilon, m_rl_rng);

    Eigen::VectorXf q_vals    = m_rl_model->getQValues(phi);
    // Softmax confidence: частка обраної дії → завжди в (0,1]
    // Уникає проблеми від'ємних Q-значень (maxCoeff < 0 → override ніколи не спрацьовував)
    {
        Eigen::VectorXf shifted = q_vals.array() - q_vals.maxCoeff();
        Eigen::VectorXf sm = shifted.array().exp();
        sm /= sm.sum();
        m_rl_action_confidence = sm((int)action);
    }
    m_rl_suggested_action     = action;

    m_rl_last_features = phi;
    m_rl_last_action   = action;
    m_rl_has_prev      = true;

    // Periodic feature vector log
    const int fli = gs.cfg.learning.feature_log_interval;
    if (fli > 0 && m_log_fn) {
        m_rl_feature_log_ticks++;
        if (m_rl_feature_log_ticks >= fli) {
            m_rl_feature_log_ticks = 0;
            static const char* names[10] = {
                "hp","mp","has_tgt","tgt_hp",
                "kl_alive","minimap","secs_kill","secs_buff",
                "is_dead","in_grace"
            };
            std::string s = "[RL-F] features:";
            for (int i = 0; i < (int)phi.size(); i++) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), " %s=%.3f", names[i], phi[i]);
                s += buf;
            }
            char buf[64];
            std::snprintf(buf, sizeof(buf), " | eps=%.4f conf=%.3f act=%d",
                          m_rl_epsilon, m_rl_action_confidence, (int)action);
            s += buf;
            m_log_fn(s);
        }
    }
}

void BotBehaviorTree::rlPostTick(GameState& gs) {
    if (!m_rl_model || !m_rl_has_prev) return;

    const auto& lc = gs.cfg.learning;

    RewardCalculator::RewardSignals sig;
    sig.kill_happened      = m_rl_sig_kill;
    sig.death_happened     = m_rl_sig_death;
    sig.targeting_failed   = m_rl_sig_targeting_failed;
    sig.buff_done          = m_rl_sig_buff_done;
    float reward = RewardCalculator::compute(sig, gs.stats);

    // Mood-aware reward shaping: Director context scales kill/death components.
    // E.g. AGGRESSIVE → kill ×1.25, FLEE → death ×2.0 (RL learns danger faster).
    // Applied as a delta on top of base reward — idle penalty stays unchanged.
    if (gs.blackboard) {
        const auto mood  = static_cast<BotMood>(gs.blackboard->getI(BB::Int::CURRENT_MOOD));
        const auto scale = getMoodRewardScale(mood);
        if (sig.kill_happened)
            reward += RewardCalculator::REWARD_KILL  * (scale.kill_scale  - 1.f);
        if (sig.death_happened)
            reward += RewardCalculator::REWARD_DEATH * (scale.death_scale - 1.f);
        if (scale.rest_bonus > 0.f && !sig.kill_happened && !sig.death_happened)
            reward += scale.rest_bonus;
    }

    Eigen::VectorXf phi_next = FeatureExtractor::extract(gs);
    bool done = m_rl_sig_death;

    m_rl_worker->pushExperience(Experience{
        m_rl_last_features,
        (int)m_rl_last_action,
        reward,
        phi_next,
        done
    });

    m_rl_ticks_since_update++;
    if (m_rl_ticks_since_update >= lc.update_frequency) {
        m_rl_ticks_since_update = 0;
        m_rl_worker->requestUpdate();
        // Decay epsilon кожен update step (кожні UpdateFrequency тіків).
        // Раніше decay був тільки на смерть → epsilon не спадав за сесію.
        m_rl_epsilon = std::max(lc.epsilon_min, m_rl_epsilon * lc.epsilon_decay);
    }

    if (done) {
        // Додатковий decay на смерть (кінець епізоду = більший штраф на exploration)
        m_rl_epsilon = std::max(lc.epsilon_min, m_rl_epsilon * lc.epsilon_decay);
        if (m_log_fn) m_log_fn("[RL] Епізод завершено (смерть). epsilon=" + std::to_string(m_rl_epsilon));
        else std::cerr << "[RL] Епізод завершено. epsilon=" << m_rl_epsilon << "\n";
    }

    if (m_rl_kills_since_save >= lc.save_frequency) {
        m_rl_kills_since_save = 0;
        m_rl_model->saveWeights(lc.weights_file);
    }
}
