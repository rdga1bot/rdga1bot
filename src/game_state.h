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
    // ── Інструменти (references — живуть весь час роботи бота) ──────────────
    Eyes&         eyes;
    Hands&        hands;
    const Config& cfg;
    Stats&        stats;

    // ── Стан персонажа ────────────────────────────────────────────────────────
    int  hp = 0, mp = 0, cp = 0;
    bool hp_valid    = false;
    float player_x   = 0.f, player_y = 0.f, player_z = 0.f;
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

    // ── Логування ────────────────────────────────────────────────────────────
    std::function<void(const std::string&)> log_fn;
    void log(const std::string& msg) const {
        if (log_fn) log_fn(msg);
    }

    // ── Утиліти ───────────────────────────────────────────────────────────────
    bool hasLiveMobs() const { return kl_alive_count > 0 || !minimap_dots.empty(); }
};
