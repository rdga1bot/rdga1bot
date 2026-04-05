#pragma once
#include <optional>
#include <vector>
#include <chrono>
#include <string>
#include <functional>
#include "Eyes.h"
#include "Hands.h"
#include "Config.h"
#include "Stats.h"
#include "MemReader.h"
#include "l2_objects.h"
#include "RandomDelay.h"

class Geodata;

// ── GameState ─────────────────────────────────────────────────────────────────
// Повний знімок стану гри + доступ до інструментів.
// Оновлюється Brain::updateGameState() на початку кожного тіку.
// Передається в Objective::execute() — єдина точка доступу до всього.
//
// Принципи:
//   - Дані (hp, target, mobs) — копії/значення, безпечні для читання
//   - Інструменти (eyes, hands, cfg, stats) — references, живуть довше GameState
//   - Не зберігати GameState між тіками (дані застаріють)
struct GameState {
    using Clock = std::chrono::steady_clock;
    using TP    = Clock::time_point;

    // ── Інструменти (references — живуть весь час роботи бота) ──────────────
    Eyes&         eyes;
    Hands&        hands;
    const Config& cfg;
    Stats&        stats;

    // ── Стан персонажа ────────────────────────────────────────────────────────
    int  hp = 0, mp = 0, cp = 0;
    bool hp_valid    = false;
    float player_x   = 0.f, player_y = 0.f, player_z = 0.f;
    float player_heading = 0.f;
    bool  coords_valid = false;

    // ── Таргет ────────────────────────────────────────────────────────────────
    std::optional<Eyes::Target> target;
    bool has_target = false;
    int  target_hp  = 0;

    // ── Мінімапа ──────────────────────────────────────────────────────────────
    std::vector<Eyes::MinimapDot> minimap_dots;

    // ── KnownList ─────────────────────────────────────────────────────────────
    std::vector<L2Character> kl_mobs;
    int  kl_alive_count = 0;
    bool kl_mob_died    = false;

    // ── Бафи ──────────────────────────────────────────────────────────────────
    double secs_since_last_buff = 9999.0;
    bool   buff_needed() const {
        return secs_since_last_buff >= (double)cfg.buff_interval;
    }

    // ── Стан ──────────────────────────────────────────────────────────────────
    bool is_dead     = false;
    bool in_grace    = false;
    bool hands_ready = false;

    // ── Shared mutable Brain state (pointers to Brain-level fields) ───────────
    TP* last_kill_time = nullptr;   // Brain::m_last_kill_time — LootObjective writes
    TP* last_buff      = nullptr;   // Brain::m_last_buff — BuffObjective writes
    TP* respawn_until  = nullptr;   // Brain::m_respawn_until — DeadObjective writes

    // ── Shared Objective fields (TargetObjective public fields) ──────────────
    bool* attack_was_unreachable = nullptr; // TargetObjective::m_attack_was_unreachable
    int*  macro_idx              = nullptr; // TargetObjective::m_macro_idx

    // ── RandomDelay (owned by Brain, exposed as raw ptrs) ────────────────────
    RandomDelay* rd_attack = nullptr;
    RandomDelay* rd_rotate = nullptr;
    RandomDelay* rd_walk   = nullptr;

    // ── Geodata ───────────────────────────────────────────────────────────────
    Geodata* geodata = nullptr;

    // ── Callbacks (Brain methods exposed for Objectives) ─────────────────────
    std::function<bool(const L2Character&)>    navigate_to_mob;
    std::function<bool(int)>                   is_blacklisted;
    std::function<void(int, float)>            blacklist_mob;
    std::function<std::optional<L2Character>(
        const std::vector<L2Character>&, float, float)> select_target;
    std::function<std::optional<L2Character>(
        const std::vector<L2Character>&, float, float, float)> find_nearest_mob;

    // ── Логування ────────────────────────────────────────────────────────────
    std::function<void(const std::string&)> log_fn;
    void log(const std::string& msg) const {
        if (log_fn) log_fn(msg);
    }

    // ── Утиліти ───────────────────────────────────────────────────────────────
    bool hasLiveMobs() const { return kl_alive_count > 0 || !minimap_dots.empty(); }
};
