#pragma once
#include <optional>
#include <chrono>
#include <functional>
#include <string>
#include "Eyes.h"
#include "Hands.h"
#include "Config.h"
#include "Stats.h"
#include "Notify.h"
#include "MemReader.h"

class Brain {
public:
    enum class State    { Idle, Targeting, Attacking, Looting, Dead, Buffing };
    enum class LogLevel { Debug = 0, Info = 1, Warning = 2, Error = 3, None = 4 };

    Brain(Eyes& eyes, Hands& hands, const Config& cfg);
    void Init();
    void Process(bool debug = false);
    State GetState() const { return m_state; }
    const Stats& GetStats() const { return m_stats; }
    const std::optional<Eyes::Me>& Me() const { return m_me; }
    const std::optional<Eyes::Target>& Target() const { return m_target; }

    static const char* StateName(State s);

    // Пауза
    void TogglePause() { m_paused = !m_paused; }
    bool IsPaused() const { return m_paused; }

    // Hot-reload конфігурації без перезапуску
    void ReloadConfig(const Config& new_cfg);

    // Memory Reading: оновлення стану гравця з пам'яті (викликається з main loop)
    void SetMemPlayerState(const MemReader::PlayerState& s) { m_mem_player = s; }
    const MemReader::PlayerState& GetMemPlayerState() const { return m_mem_player; }

    // Рівень логування
    void SetLogLevel(LogLevel level) { m_min_log_level = level; }

    // Встановити callback для логу (викликається з кожного Log())
    void SetLogCallback(std::function<void(const std::string&)> cb) {
        m_log_callback = std::move(cb);
    }

private:
    using Clock = std::chrono::steady_clock;
    using TP = Clock::time_point;

    State m_state = State::Idle;
    bool m_paused = false;
    Eyes& m_eyes;
    Hands& m_hands;
    Config m_cfg; // зберігається за значенням — дозволяє ReloadConfig()
    Stats m_stats;
    Notify m_notify;

    // Сприйняття (заповнюється кожен тік)
    std::optional<Eyes::Me> m_me;
    std::optional<Eyes::Target> m_target;

    // Підстан таргетингу
    int m_macro_idx = 0;
    int m_macro_attempts = 0;
    int m_step_count = 0;
    int m_minimap_rotate_count = 0; // скільки разів поспіль повернули за мінімапою без результату
    int m_far_target_rejects = 0;   // скільки разів відхилили далекий таргет (макс 5 на сесію)
    bool m_pokemon_targeted = false;    // поточний таргет — Pokemon (потрібен sweep після смерті)
    bool m_pokemon_macro_fired = false; // Pokemon macro вже викликано в цьому TARGETING циклі
    int m_dead_target_esc_count = 0;    // скільки разів поспіль ESC для hp=0 таргету (debounce)
    int m_dead_cycles_total = 0;        // скільки ESC-fallthrough циклів поспіль → switch to macro

    // Атака: HP моба на попередньому тіку (для миттєвої детекції смерті)
    int m_prev_target_hp = -1;

    // Бій
    int m_no_target_count = 0;
    int m_target_hp_zero_count = 0; // debounce: потрібно N тіків hp=0 перед LOOTING
    TP m_last_target_redetect; // для re-detect target bar кожні 200мс в ATTACKING
    int m_attack_idx = 0;
    bool m_first_attack = true;
    TP m_last_attack;
    TP m_combat_watchdog_start;
    // Підхід до моба: re-target на найближчого поки не атакували
    int m_approach_retarget_count = 0; // скільки разів перемкнули під час підходу (макс 3)
    TP  m_approach_last_retarget;      // час останнього re-target під час підходу
    int m_approach_entry_hp = 100;     // HP моба при вході в ATTACKING (для перевірки чи удар дійшов)
    // Auto-approach: відстеження HP цілі для виявлення out-of-range
    int m_attack_last_target_hp = -1;
    TP m_attack_hp_stable_since;

    // Лут
    bool m_looting_issued = false;
    TP m_loot_start;

    // Баф
    int m_buff_idx = 0;
    TP m_last_buff;
    int m_buff_stage = 0;   // 0=not started, 1=wait tab, 2=wait profile, 3=wait close, 4=done
    int m_buff_open_retries = 0; // скільки разів повторно відкривали ALT+B (макс 3)
    cv::Point m_buff_tab_click_pos{0, 0}; // куди кликнули "Баффер" (для відносних координат profile)
    bool m_buff_tab_fallback = false; // true = таб знайдено тільки через fallback (без шаблону)

    // Шаблони для визначення кнопок ALT+B вікна (завантажуються один раз)
    cv::Mat m_buff_tab_templ;     // buff_tab.png — вкладка "Баффер"
    cv::Mat m_buff_profile_templ; // buff_profile.png — профіль "tty"

    // Кулдауни потіонів
    TP m_last_hp_pot;
    TP m_last_mp_pot;
    TP m_last_cp_pot;

    // Смерть/відроджування
    int m_hp_zero_count = 0;
    TP m_session_start;
    TP m_respawn_until;
    TP m_last_kill_time; // для post-combat cooldown перед бафом
    bool m_in_death = false;
    int m_dead_phase = 0;

    // ── Navigation: obstacle detection ───────────────────────────────────────
    int  m_walk_stuck_count  = 0;    // скільки тіків поспіль не рухались після WalkForward
    bool m_nav_prev_was_walk = false; // попередній тік виконав WalkForward → перевіряємо рух
    int  m_nav_stuck_recoveries = 0; // загальний лічильник відновлень → чергує L/R незалежно від macro_attempts

    // Minimap optical flow stuck detection
    TP   m_minimap_low_flow_since{};  // коли почався period низького flow (0 = не активний)
    bool m_minimap_flow_stuck = false; // чи вже тригернули escape по flow цього разу

    // Unreachable mob detection: якщо HP-stable спрацював (моб недосяжний) →
    // у наступному TARGETING циклі навігація отримує більше часу до першого макросу
    bool m_attack_was_unreachable = false;

    // Patrol
    int  m_patrol_step_idx  = 0;    // поточний крок патрулю
    bool m_patrol_active    = false; // зараз виконується крок патрулю

    // DetectMe failure counter (для WARNING при зависанні)
    int m_detect_me_fail_count = 0;
    // IsReady=false counter в TARGETING (для WARNING при зависанні Input thread)
    int m_not_ready_count = 0;
    // Heartbeat tick counter
    int m_heartbeat_tick = 0;

    // Memory Reading: стан гравця з пам'яті (оновлюється з main loop кожен тік)
    MemReader::PlayerState m_mem_player;

    // Лог з рівнем (рівень за замовчуванням — Info)
    void Log(const std::string& msg, LogLevel level = LogLevel::Info);

    LogLevel m_min_log_level = LogLevel::Info;
    std::function<void(const std::string&)> m_log_callback;

    void EnterState(State s);
    void HandleIdle();
    void HandleTargeting();
    void HandleAttacking();
    void HandleLooting();
    void HandleDead();
    void HandleBuffing();
    void CheckPotions(const Eyes::Me& me);

    bool HasTarget() const { return m_target.has_value() && m_target->hp > 0; }
    bool InRespawnGrace() const { return Clock::now() < m_respawn_until; }

    static double SecsSince(TP t) {
        return std::chrono::duration<double>(Clock::now() - t).count();
    }
    static TP FutureBy(double s) {
        return Clock::now() + std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(s));
    }
    static TP Now() { return Clock::now(); }
};
