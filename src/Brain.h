#pragma once
#include <optional>
#include <chrono>
#include <functional>
#include <string>
#include <memory>
#include <vector>
#include "Eyes.h"
#include "Hands.h"
#include "Config.h"
#include "Stats.h"
#include "Notify.h"
#include "MemReader.h"
#include "world_state.h"
#include "Geodata.h"
#include "RandomDelay.h"
#include "vision_worker.h"
#include "geodata_worker.h"

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

    // Навігація до моба через memory (heading + координати).
    // Викликається з HandleTargeting якщо navigation.enabled=true.
    // Повертає true якщо виконано поворот або рух.
    bool NavigateToMob(const L2Character& mob);

    // Memory Reading: оновлення стану гравця з пам'яті (викликається з main loop)
    void SetMemPlayerState(const MemReader::PlayerState& s) { m_mem_player = s; }
    const MemReader::PlayerState& GetMemPlayerState() const { return m_mem_player; }

    // KnownList: встановити WorldState (main.cpp передає ownership)
    void SetWorldState(std::unique_ptr<WorldState> world) { m_world = std::move(world); }
    void SetPlayerBase(uintptr_t base) { m_player_base = base; }
    uintptr_t GetPlayerBase() const { return m_player_base; }
    bool HasPlayerBase() const { return m_player_base != 0; }
    WorldState* GetWorldState() const { return m_world.get(); }

    // Geodata: навігація по геодаті (необов'язково — якщо null, стандартна навігація)
    void SetGeodata(std::shared_ptr<Geodata> geo) { m_geodata = std::move(geo); }
    Geodata* GetGeodata() const { return m_geodata.get(); }

    // VisionWorker інтеграція:
    // Встановити async результат DetectNPCs (викликає main.cpp)
    // При наступному тіку Brain використає ці NPCs замість sync DetectNPCs
    void SetAsyncNPCs(const std::vector<Eyes::NPC>& npcs,
                      const std::vector<Eyes::MinimapDot>& minimap);

    // GeodataWorker інтеграція:
    // Встановити знайдений шлях (викликає main.cpp)
    void SetGeoPath(const std::vector<std::pair<float,float>>& path,
                    uint64_t path_id);

    // Повернути pending path request або nullopt якщо не потрібен
    std::optional<PathRequest> GetPendingPathRequest();

    // Blacklist: занести моба в чорний список на seconds секунд
    // (викликається автоматично при HP-stable; доступно ззовні для тестів)
    void BlacklistMob(int objectID, float seconds = 60.f);
    bool IsBlacklisted(int objectID) const;

    // RandomDelay helper: повертає затримку з RandomDelay якщо enabled, інакше fixed_ms
    int RandMs(std::unique_ptr<RandomDelay>& rd, int fixed_ms) {
        if (m_cfg.delays.enabled && rd) return rd->Get();
        return fixed_ms;
    }

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
    bool m_running_to_mob = false;   // RunTick() активний — персонаж біжить
    TP   m_run_started{};            // коли почали бігти (для time-based escape)

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

    // KnownList memory read: чи HP/isDead цілі береться з пам'яті (для діагностики)
    bool m_mem_target_hp_valid = false;

    // RandomDelay генератори — ініціалізуються з Config.delays
    std::unique_ptr<RandomDelay> m_rd_attack;
    std::unique_ptr<RandomDelay> m_rd_rotate;
    std::unique_ptr<RandomDelay> m_rd_walk;

    // Memory-based навігація
    struct NavigationState {
        float targetX    = 0.f, targetY    = 0.f, targetZ    = 0.f;
        float playerX    = 0.f, playerY    = 0.f;
        float playerHeading = 0.f;
        float distance   = 0.f;
        float angleDiff  = 0.f;
    } m_nav_state;

    // KnownList: WorldState (null якщо KnownList вимкнено)
    std::unique_ptr<WorldState> m_world;
    uintptr_t m_player_base = 0;

    // Geodata (null = вимкнено)
    std::shared_ptr<Geodata> m_geodata;

    // Async vision результат (від VisionWorker)
    std::optional<std::vector<Eyes::NPC>>         m_async_npcs;
    std::optional<std::vector<Eyes::MinimapDot>>  m_async_minimap;
    bool m_has_async_vision = false;

    // Geo path (від GeodataWorker)
    std::vector<std::pair<float,float>> m_geo_path;
    size_t   m_geo_path_idx  = 0;
    uint64_t m_geo_path_id   = 0;
    bool     m_geo_path_ready = false;

    // Path request для GeodataWorker
    std::optional<PathRequest> m_pending_path_req;
    uint64_t m_path_req_id = 0;

    // ── Blacklist — недосяжні моби (HP-stable → blacklist на 60с) ────────────
    struct BlacklistedMob {
        int  objectID = 0;
        TP   until{};   // час закінчення блокування
    };
    std::vector<BlacklistedMob> m_blacklist;
    void CleanBlacklist(); // видалити прострочені записи
    void InitRandomDelays(); // ініціалізувати/скинути RandomDelay генератори з cfg.delays
    std::optional<L2Character> SelectWeightedTarget(
        const std::vector<L2Character>& mobs,
        float playerX, float playerY);

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

    static float NormalizeAngle(float angle);  // нормалізує до [-pi..pi]

    static double SecsSince(TP t) {
        return std::chrono::duration<double>(Clock::now() - t).count();
    }
    static TP FutureBy(double s) {
        return Clock::now() + std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(s));
    }
    static TP Now() { return Clock::now(); }
};
