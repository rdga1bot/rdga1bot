// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "BehaviorTree.h"
#include "game_state.h"
#include "Config.h"
#include "RandomDelay.h"
#include "l2_objects.h"
#include "geodata_worker.h"
#include "LinearQModel.h"
#include "ExperienceBuffer.h"
#include "LearningWorker.h"
#include "FeatureExtractor.h"
#include "RewardCalculator.h"
#include <chrono>
#include <functional>
#include <memory>
#include <deque>
#include <optional>
#include <string>
#include <cmath>
#include <random>
#include <opencv2/opencv.hpp>

// ── BotBehaviorTree ───────────────────────────────────────────────────────────
// Farm Behavior Tree для rdga1bot.
// Повна міграція: весь стан і логіка Objectives — тут, без делегування.
//
// BT дерево (Selector root):
//   Dead branch     — відродження (3 фази)
//   Rest branch     — пауза при низькому HP/MP
//   Zone branch     — повернення в зону фарму
//   Buff branch     — бафи
//   Loot branch     — лутинг (після kill сигналу)
//   Attack branch   — атака поточного таргету
//   Target branch   — пошук нової цілі (MR20c)
//
// Стан між тіками — поля цього класу (замість Objective приватних полів).

class BotBehaviorTree {
public:
    using Clock  = std::chrono::steady_clock;
    using TP     = Clock::time_point;
    using LogFn  = std::function<void(const std::string&)>;

    BotBehaviorTree();
    ~BotBehaviorTree();

    // Встановити лог-функцію (перенаправляє [RL] повідомлення у Brain::Log).
    // Викликати ДО init().
    void setLogFn(LogFn fn) { m_log_fn = std::move(fn); }

    // Ініціалізація дерева. Викликати один раз при старті.
    void init(const Config& cfg);

    // Один тік. gs — вже заповнений Brain::updateGameState().
    // Повертає ім'я активної гілки для Dashboard/логу.
    std::string tick(GameState& gs);

    // Скинути стан (при смерті або рестарті)
    void reset();

    // Поточна активна гілка
    const std::string& currentBranch() const { return m_active_branch; }

    // Metrics
    uint32_t tickCount()   const { return m_bt.tickCount(); }
    uint64_t totalTimeUs() const { return m_bt.totalTimeUs(); }
    uint32_t avgTimeUs()   const { return m_bt.avgTimeUs(); }
    uint16_t nodeCount()   const { return m_bt.nodeCount(); }

    // Читання стану для Brain (аналог ObjectiveManager virtual getters)
    TP   lastKillTime()  const { return m_last_kill_time; }
    TP   lastBuff()      const { return m_last_buff; }
    TP   respawnUntil()  const { return m_respawn_until; }
    bool inGrace()       const { return Clock::now() < m_respawn_until; }

    // Сигнал від Attack гілки що треба Loot
    void notifyKill()  { m_loot_pending = true; m_last_kill_time = Clock::now(); }

    // GeoPath delivery (для Brain dispatch)
    void deliverGeoPath(const std::vector<std::pair<float,float>>& path, uint64_t id);
    std::optional<PathRequest> takePendingPathRequest();

    // ── RL публічний API ─────────────────────────────────────────────────────
    bool isLearningEnabled()  const { return m_rl_model != nullptr; }
    float getRLEpsilon()      const { return m_rl_epsilon; }
    float getRLLastLoss()     const {
        return m_rl_worker ? m_rl_worker->lastLoss() : 0.f;
    }
    int   getRLUpdateCount()  const {
        return m_rl_worker ? m_rl_worker->updateCount() : 0;
    }
    LinearQModel::Action getRLSuggestedAction() const {
        return m_rl_suggested_action;
    }
    Eigen::VectorXf getRLQValues(const Eigen::VectorXf& f) const {
        return m_rl_model ? m_rl_model->getQValues(f)
                          : Eigen::VectorXf::Zero(LinearQModel::NUM_ACTIONS);
    }

    // Сигнальні методи — викликати з actLoot, actDead, actBuff, actTarget
    void notifyKillRL()   { m_rl_sig_kill   = true; m_rl_kills_since_save++; }
    void notifyDeathRL()  { m_rl_sig_death  = true; }
    void notifyBuffRL()   { m_rl_sig_buff_done = true; }

    void initRL(const Config& cfg);
    void shutdownRL();

private:
    BehaviorTree m_bt;
    std::string  m_active_branch = "None";

    // ── Стан Dead гілки ───────────────────────────────────────────────────────
    int m_dead_phase    = 0;
    TP  m_respawn_until{};

    // ── Стан Rest гілки ───────────────────────────────────────────────────────
    TP  m_rest_start{};

    // ── Стан Buff гілки ───────────────────────────────────────────────────────
    TP  m_last_buff{};       // коли востаннє бафались
    int m_buff_stage    = 0;
    int m_buff_retries  = 0;
    bool m_buff_tab_fallback = false;
    cv::Point m_buff_tab_click_pos{0, 0};
    cv::Mat m_buff_tab_templ;
    cv::Mat m_buff_profile_templ;

    // ── Стан Loot гілки ───────────────────────────────────────────────────────
    bool m_loot_pending = false;
    bool m_loot_issued  = false;
    TP   m_last_kill_time{};

    // ── Стан Attack гілки ─────────────────────────────────────────────────────
    bool  m_atk_first_attack         = true;
    int   m_atk_attack_idx           = 0;
    int   m_atk_no_target_count      = 0;
    int   m_atk_hp_zero_count        = 0;
    int   m_atk_last_hp              = -1;
    TP    m_atk_hp_stable_since{};
    TP    m_atk_watchdog_start{};
    TP    m_atk_last_attack{};
    TP    m_atk_last_redetect{};
    bool  m_atk_mem_hp_valid         = false;
    float m_atk_mem_hp_abs           = -1.f;
    bool  m_atk_low_hp_timer_active  = false;
    TP    m_atk_low_hp_since{};
    std::unique_ptr<RandomDelay> m_atk_rd;

    // ── Zone стан ─────────────────────────────────────────────────────────────
    float m_zone_cx = 0.f, m_zone_cy = 0.f, m_zone_r = 0.f;
    bool  m_zone_enabled = false;

    // ── unreachable прапор (з Attack → Target) ───────────────────────────────
    bool m_unreachable_flag = false;

    // ── Стан Target гілки ─────────────────────────────────────────────────────
    bool  m_tgt_active             = false; // false = потрібна ініціалізація
    int   m_tgt_macro_attempts     = 0;
    int   m_tgt_step_count         = 0;
    int   m_tgt_macro_idx          = 0;     // НЕ скидається між циклами
    int   m_tgt_minimap_rotate_count = 0;
    int   m_tgt_far_rejects        = 0;
    bool  m_tgt_pokemon_fired      = false;
    bool  m_tgt_pokemon_targeted   = false;
    int   m_tgt_dead_esc_count     = 0;
    int   m_tgt_dead_cycles_total  = 0;
    int   m_tgt_walk_stuck_count   = 0;
    bool  m_tgt_nav_prev_was_walk  = false;
    int   m_tgt_nav_stuck_recoveries = 0;
    int   m_tgt_patrol_step_idx    = 0;
    bool  m_tgt_running_to_mob     = false;
    TP    m_tgt_run_started{};
    int   m_tgt_not_ready_count    = 0;
    bool  m_attack_was_unreachable = false; // зберігається між циклами

    // Спільний стан між вузлами піддерева Target (обчислюється в actTgtMinimap)
    const Eyes::MinimapDot* m_tgt_map_ref         = nullptr;
    bool                    m_tgt_map_ref_selected = false;

    // Geo path
    std::vector<std::pair<float,float>> m_tgt_geo_path;
    size_t   m_tgt_geo_path_idx   = 0;
    uint64_t m_tgt_geo_path_id    = 0;
    bool     m_tgt_geo_path_ready = false;
    std::optional<PathRequest> m_tgt_pending_path;
    uint64_t m_tgt_path_req_id    = 0;

    std::unique_ptr<RandomDelay> m_tgt_rd_rotate;
    std::unique_ptr<RandomDelay> m_tgt_rd_walk;

    // Breadcrumbs
    struct Crumb { float x, y, z; };
    std::deque<Crumb> m_breadcrumbs;
    bool              m_backtracking = false;

    // ── Лог-функція (перенаправляє у Brain::Log) ─────────────────────────────
    LogFn m_log_fn;

    // ── RL компоненти (null якщо [Learning] Enabled=false) ───────────────────
    std::shared_ptr<LinearQModel>     m_rl_model;
    std::shared_ptr<ExperienceBuffer> m_rl_buffer;
    std::unique_ptr<LearningWorker>   m_rl_worker;
    std::string                       m_rl_weights_file;

    // ── RL стан між тіками ────────────────────────────────────────────────────
    LinearQModel::Action  m_rl_suggested_action  = LinearQModel::Action::TargetNearest;
    float                 m_rl_action_confidence = 0.f;
    Eigen::VectorXf       m_rl_last_features;
    LinearQModel::Action  m_rl_last_action       = LinearQModel::Action::TargetNearest;
    bool                  m_rl_has_prev          = false;
    float                 m_rl_epsilon           = 1.0f;
    int                   m_rl_ticks_since_update  = 0;
    int                   m_rl_kills_since_save    = 0;
    int                   m_rl_feature_log_ticks   = 0;
    std::mt19937          m_rl_rng{std::random_device{}()};

    // ── RL сигнали за тік (скидаються на початку кожного tick) ───────────────
    bool  m_rl_sig_kill             = false;
    bool  m_rl_sig_death            = false;
    bool  m_rl_sig_targeting_failed = false;
    bool  m_rl_sig_buff_done        = false;

    void rlPreTick (GameState& gs);
    void rlPostTick(GameState& gs);

    // ── Helpers ───────────────────────────────────────────────────────────────
    static double secsSince(TP t) {
        return std::chrono::duration<double>(Clock::now() - t).count();
    }
    static TP now() { return Clock::now(); }
    static TP futureBy(double s) {
        return Clock::now() + std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(s));
    }

    // thread_local ptr для доступу з static Action/Condition функцій.
    // SINGLE-THREAD CONTRACT: tick() викликається виключно з Brain-потоку.
    // Якщо s_self != nullptr і != this — два BotBehaviorTree у одному потоці (помилка).
    // Перевіряється assert(s_self == nullptr || s_self == this) на початку tick().
    static thread_local BotBehaviorTree* s_self;

    // ── Condition functions ───────────────────────────────────────────────────
    static bool condIsDead      (GameState& gs);
    static bool condNeedsRest   (GameState& gs);
    static bool condZoneViolated(GameState& gs);
    static bool condNeedsBuff   (GameState& gs);
    static bool condLootPending (GameState& gs);
    static bool condHasTarget   (GameState& gs);

    // ── Action functions ──────────────────────────────────────────────────────
    static BTStatus actDead    (GameState& gs);
    static BTStatus actRest    (GameState& gs);
    static BTStatus actZone    (GameState& gs);
    static BTStatus actBuff    (GameState& gs);
    static BTStatus actLoot    (GameState& gs);
    static BTStatus actAttack  (GameState& gs);
    static BTStatus actTarget  (GameState& gs); // замінено піддеревом в MR28

    // ── Target піддерево (MR28) ──────────────────────────────────────────────
    static BTStatus actTgtInit       (GameState& gs); // ініціалізація + breadcrumb
    static BTStatus actTgtDeadTarget (GameState& gs); // мертвий таргет hp=0
    static BTStatus actTgtMinimap    (GameState& gs); // ротація мінімапою
    static BTStatus actTgtF2AndMacro (GameState& gs); // F2 + macro + pokemon
    static BTStatus actTgtNavigation (GameState& gs); // stuck + nav + geodata req
    static BTStatus actTgtGeoPath    (GameState& gs); // navmesh + geo waypoints
    static BTStatus actTgtPatrol     (GameState& gs); // patrol + rotate + explore

    // Helpers
    void resetAttackState(GameState& gs);
    void resetTargetState(GameState& gs);
    void blacklistCurrentTarget(GameState& gs);

    // Breadcrumbs helpers
    void addCrumb(float x, float y, float z, const Config::BreadcrumbConfig& cfg);
    std::optional<Crumb> findBacktrackCrumb(float px, float py, float range) const;

    // ── actTarget підфункції ──────────────────────────────────────────────────
    std::optional<BTStatus> tgtHandleDeadTarget  (GameState& gs);
    void                    tgtHandleMinimap      (GameState& gs,
                                const Eyes::MinimapDot* map_ref, bool map_ref_selected);
    void                    tgtSendF2AndMacro     (GameState& gs);
    std::optional<BTStatus> tgtHandleNavigation   (GameState& gs,
                                const Eyes::MinimapDot* map_ref);
    std::optional<BTStatus> tgtHandleGeoPath      (GameState& gs,
                                const Eyes::MinimapDot* map_ref);
    void                    tgtHandlePatrolAndRotate(GameState& gs,
                                const Eyes::MinimapDot* map_ref);

    // RandMs helper
    static int RandMs(RandomDelay* rd, const GameState& gs, int fixed_ms) {
        if (gs.cfg.delays.enabled && rd) return rd->Get();
        return fixed_ms;
    }
};
