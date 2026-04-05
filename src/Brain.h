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
#include "game_state.h"
#include "objective_manager.h"
#include "farm_objectives.h"

class Brain {
public:
    enum class LogLevel { Debug = 0, Info = 1, Warning = 2, Error = 3, None = 4 };

    Brain(Eyes& eyes, Hands& hands, const Config& cfg);
    void Init();
    void Process(bool debug = false);

    // Поточна активна ціль (замінює State enum)
    std::string GetState() const { return m_obj_manager.currentName(); }

    const Stats& GetStats() const { return m_stats; }
    const std::optional<Eyes::Me>& Me() const { return m_me; }
    const std::optional<Eyes::Target>& Target() const { return m_target; }

    // Пауза
    void TogglePause() { m_paused = !m_paused; }
    bool IsPaused() const { return m_paused; }

    // Hot-reload конфігурації без перезапуску
    void ReloadConfig(const Config& new_cfg);

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

    // VisionWorker інтеграція
    void SetAsyncNPCs(const std::vector<Eyes::NPC>& npcs,
                      const std::vector<Eyes::MinimapDot>& minimap);

    // GeodataWorker інтеграція (proxies до TargetObjective)
    void SetGeoPath(const std::vector<std::pair<float,float>>& path, uint64_t path_id);
    std::optional<PathRequest> GetPendingPathRequest();

    // Blacklist
    void BlacklistMob(int objectID, float seconds = 60.f);
    bool IsBlacklisted(int objectID) const;

    // Рівень логування
    void SetLogLevel(LogLevel level) { m_min_log_level = level; }

    // Встановити callback для логу (викликається з кожного Log())
    void SetLogCallback(std::function<void(const std::string&)> cb) {
        m_log_callback = std::move(cb);
    }

    // Objectives: ім'я активної цілі (для Dashboard і логу)
    std::string GetCurrentObjective() const {
        return m_obj_manager.currentName();
    }

    // NavigateToMob: залишається в Brain (використовує m_mem_player, m_geodata)
    bool NavigateToMob(const L2Character& mob);

private:
    using Clock = std::chrono::steady_clock;
    using TP = Clock::time_point;

    bool m_paused = false;
    Eyes& m_eyes;
    Hands& m_hands;
    Config m_cfg;
    Stats m_stats;
    Notify m_notify;

    // Сприйняття (заповнюється кожен тік)
    std::optional<Eyes::Me> m_me;
    std::optional<Eyes::Target> m_target;

    // Смерть / grace period
    int  m_hp_zero_count   = 0;
    TP   m_session_start;
    TP   m_respawn_until;
    TP   m_last_kill_time;
    TP   m_last_buff;

    // Потіони (не залежать від стану)
    TP m_last_hp_pot;
    TP m_last_mp_pot;
    TP m_last_cp_pot;

    // Memory Reading
    MemReader::PlayerState m_mem_player;

    // KnownList
    std::unique_ptr<WorldState> m_world;
    uintptr_t m_player_base = 0;

    // Geodata
    std::shared_ptr<Geodata> m_geodata;

    // Async vision результат (від VisionWorker)
    std::optional<std::vector<Eyes::NPC>>        m_async_npcs;
    std::optional<std::vector<Eyes::MinimapDot>> m_async_minimap;
    bool m_has_async_vision = false;

    // RandomDelay генератори (ініціалізуються з Config.delays; живуть у Brain для ReloadConfig)
    std::unique_ptr<RandomDelay> m_rd_attack;
    std::unique_ptr<RandomDelay> m_rd_rotate;
    std::unique_ptr<RandomDelay> m_rd_walk;

    // Blacklist — недосяжні моби
    struct BlacklistedMob {
        int  objectID = 0;
        TP   until{};
    };
    std::vector<BlacklistedMob> m_blacklist;

    // Objectives
    ObjectiveManager m_obj_manager;
    std::string m_prev_obj_name; // для детекції переходів (RecordDeath тощо)

    // Diagnostics
    int m_detect_me_fail_count = 0;
    int m_heartbeat_tick       = 0;
    int m_not_ready_count      = 0;

    // Navigation state (використовується NavigateToMob)
    struct NavigationState {
        float targetX    = 0.f, targetY    = 0.f, targetZ    = 0.f;
        float playerX    = 0.f, playerY    = 0.f;
        float playerHeading = 0.f;
        float distance   = 0.f;
        float angleDiff  = 0.f;
    } m_nav_state;

    // Логування
    void Log(const std::string& msg, LogLevel level = LogLevel::Info);
    LogLevel m_min_log_level = LogLevel::Info;
    std::function<void(const std::string&)> m_log_callback;

    // Helpers
    void updateGameState(GameState& gs);
    void CheckPotions(const Eyes::Me& me);
    void CleanBlacklist();
    void InitRandomDelays();
    std::optional<L2Character> SelectWeightedTarget(
        const std::vector<L2Character>& mobs, float playerX, float playerY);

    bool HasTarget() const { return m_target.has_value() && m_target->hp > 0; }
    bool InRespawnGrace() const { return Clock::now() < m_respawn_until; }
    static float NormalizeAngle(float angle);

    static double SecsSince(TP t) {
        return std::chrono::duration<double>(Clock::now() - t).count();
    }
    static TP FutureBy(double s) {
        return Clock::now() + std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(s));
    }
    static TP Now() { return Clock::now(); }
};
