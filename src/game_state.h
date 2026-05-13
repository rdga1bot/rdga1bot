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
class NavMeshBuilder;
class Blackboard;

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

    // ── NPC (OpenCV) — кешується Brain::updateGameState() раз на тік ───────────
    // Async-результат VisionWorker або sync DetectNPCs() (якщо VisionWorker вимкнено).
    // BT вузли читають звідси — НЕ викликають DetectNPCs() напряму.
    std::vector<Eyes::NPC> npcs;

    // ── Мінімапа ──────────────────────────────────────────────────────────────
    std::vector<Eyes::MinimapDot> minimap_dots;
    // true якщо є хоч одна точка ближча за ~70px від центру мінімапи (~1200 юнітів).
    // 70/78 ≈ 90% радіусу — відсікає тільки мобів на самому краю мінімапи (/target "Name" зона).
    // Поріг 35px був надто малим: моб на dist≈67px не детектувався як загроза (TH смерть).
    bool minimap_close_threat = false;

    // ── KnownList ─────────────────────────────────────────────────────────────
    std::vector<L2Character> kl_mobs;
    int  kl_alive_count = 0;
    bool kl_mob_died    = false;

    // ── Бафи ──────────────────────────────────────────────────────────────────
    double secs_since_last_buff = 9999.0;
    double secs_since_last_kill = 9999.0; // read-only snapshot для canRun перевірок
    bool   buff_needed() const {
        return secs_since_last_buff >= (double)cfg.buff_interval;
    }

    // ── Стан ──────────────────────────────────────────────────────────────────
    bool is_dead     = false;
    bool in_grace    = false;
    bool hands_ready = false;
    // true якщо HP зменшився порівняно з попереднім тіком (активно атакують).
    // Встановлюється в Brain::updateGameState() на основі m_hp_prev.
    bool hp_falling  = false;

    // ── RandomDelay (owned by Brain, exposed as raw ptrs) ────────────────────
    RandomDelay* rd_attack = nullptr;
    RandomDelay* rd_rotate = nullptr;
    RandomDelay* rd_walk   = nullptr;

    // ── Geodata ───────────────────────────────────────────────────────────────
    Geodata* geodata = nullptr;

    // ── NavMesh ───────────────────────────────────────────────────────────────
    NavMeshBuilder* navmesh = nullptr;

    // ── Callbacks (Brain methods exposed for Objectives) ─────────────────────
    // Згруповано для читабельності. Використання: gs.cb.navigate_to_mob(...)
    struct Callbacks {
        std::function<bool(const L2Character&)>    navigate_to_mob;
        std::function<bool(int)>                   is_blacklisted;
        std::function<void(int, float)>            blacklist_mob;
        std::function<std::optional<L2Character>(
            const std::vector<L2Character>&, float, float)> select_target;
        std::function<std::optional<L2Character>(
            const std::vector<L2Character>&, float, float, float)> find_nearest_mob;
        std::function<void(const std::string&)>    log_fn;
        std::function<void()>                      notify_death_fn;
    } cb;

    // log() — зручний helper (НЕ чіпати)
    void log(const std::string& msg) const {
        if (cb.log_fn) cb.log_fn(msg);
    }

    // ── Утиліти ───────────────────────────────────────────────────────────────
    bool hasLiveMobs() const { return kl_alive_count > 0 || !minimap_dots.empty(); }

    // Director/Agent Blackboard — nullable. Set by Brain::updateGameState().
    // BT nodes read strategic hints: mood, directive, flee_active, zone.
    Blackboard* blackboard = nullptr;
};
