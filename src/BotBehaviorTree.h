#pragma once
#include "BehaviorTree.h"
#include "game_state.h"
#include "Config.h"
#include "RandomDelay.h"
#include "l2_objects.h"
#include "geodata_worker.h"
#include <chrono>
#include <memory>
#include <deque>
#include <optional>
#include <string>
#include <cmath>
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
    using Clock = std::chrono::steady_clock;
    using TP    = Clock::time_point;

    BotBehaviorTree();

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

    // ── Helpers ───────────────────────────────────────────────────────────────
    static double secsSince(TP t) {
        return std::chrono::duration<double>(Clock::now() - t).count();
    }
    static TP now() { return Clock::now(); }
    static TP futureBy(double s) {
        return Clock::now() + std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(s));
    }

    // thread_local ptr для доступу з static Action/Condition функцій
    // (single-threaded Brain — безпечно)
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
    static BTStatus actTarget  (GameState& gs);

    // Helpers
    void resetAttackState(GameState& gs);
    void resetTargetState(GameState& gs);
    void blacklistCurrentTarget(GameState& gs);

    // Breadcrumbs helpers
    void addCrumb(float x, float y, float z, const Config::BreadcrumbConfig& cfg);
    std::optional<Crumb> findBacktrackCrumb(float px, float py, float range) const;

    // RandMs helper
    static int RandMs(RandomDelay* rd, const GameState& gs, int fixed_ms) {
        if (gs.cfg.delays.enabled && rd) return rd->Get();
        return fixed_ms;
    }
};
