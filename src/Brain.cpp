// SPDX-License-Identifier: GPL-3.0-only
#include "Brain.h"
#include "Utils.h"
#include <iostream>
#include <fstream>
#include <ctime>
#include <cmath>
#include <opencv2/opencv.hpp>

using namespace std::chrono_literals;

// ── Log ───────────────────────────────────────────────────────────────────────
void Brain::Log(const std::string& msg, LogLevel level) {
    if (level < m_min_log_level) return;
    std::time_t t = std::time(nullptr);
    char ts[10];
    std::strftime(ts, sizeof(ts), "%H:%M:%S", std::localtime(&t));
    std::string line = std::string("[") + ts + "] " + msg;
    std::cout << line << "\n";
    if (m_log_callback) m_log_callback(std::move(line));
}

// ── Constructor ───────────────────────────────────────────────────────────────
Brain::Brain(Eyes& eyes, Hands& hands, const Config& cfg)
    : m_eyes(eyes)
    , m_hands(hands)
    , m_cfg(cfg)
    , m_notify(cfg.tg_token, cfg.tg_chat_id, cfg.tg_on_death, cfg.tg_stats_interval)
{
    m_last_hp_pot    = Now();
    m_last_mp_pot    = Now();
    m_last_cp_pot    = Now();
    m_session_start  = Now();

    InitRandomDelays();

    // BotBehaviorTree: єдиний планувальник
    m_bot_bt.setLogFn([this](const std::string& msg){ Log(msg); });
    m_bot_bt.init(m_cfg);
    Log("[BT] BotBehaviorTree: " + std::to_string(m_bot_bt.nodeCount()) + " вузлів");

    // Shadow Logger: створюємо якщо ShadowMode увімкнено (MR26)
    if (m_cfg.mem_shadow_mode && m_cfg.knownlist_enabled) {
        m_shadow_logger      = std::make_unique<ShadowLogger>();
        m_shadow_mode_active = true;
        Log("[Shadow] ShadowLogger активовано. Логи: memory/shadow_logs/");
    }
}

void Brain::ReloadConfig(const Config& new_cfg) {
    m_cfg = new_cfg;
    InitRandomDelays();
    Log("[CONFIG] Конфігурацію перезавантажено");
}

void Brain::Init() {
    Log("[Brain] Ініціалізація...\n");
    // Скидаємо stats для нової сесії — session_start оновлюється до поточного часу.
    // БЕЗ цього: після exception-restart session_start залишається від першого запуску
    // процесу → uptime_sec:246760+ і garbage kills при збереженні.
    m_stats = Stats{};
    // Очищаємо live stats з попередньої сесії щоб QA Monitor не читав старі дані
    {
        std::ofstream clear("/tmp/rdga1bot_stats.json");
        if (clear) clear << "{\"ts\":0,\"kills\":0,\"deaths\":0,\"attacks\":0,"
                            "\"hp_potions\":0,\"mp_potions\":0,"
                            "\"targeting_failures\":0,\"uptime_sec\":0}\n";
    }
}

// ── Process ───────────────────────────────────────────────────────────────────
void Brain::Process(bool debug) {
    if (m_paused) return;
    const auto tick_start = Now();

    // Детекція стану персонажа
    m_me     = m_eyes.DetectMe();
    m_target = m_eyes.DetectTarget();

    // Memory Reading: якщо дані валідні — замінюємо OpenCV детекцію.
    // Guardrail: якщо pct поза [0,100] — хибний offset, залишаємо OCR значення.
    if (m_mem_player.valid) {
        auto safePct = [&](float cur, float max, int ocr_fallback) -> int {
            if (max <= 0.f) return ocr_fallback;
            int pct = (int)(cur * 100.f / max);
            return (pct >= 0 && pct <= 100) ? pct : ocr_fallback;
        };
        int ocr_hp = m_me ? m_me->hp : 0;
        int ocr_mp = m_me ? m_me->mp : 0;
        int ocr_cp = m_me ? m_me->cp : 0;
        Eyes::Me mem_me;
        mem_me.hp = safePct(m_mem_player.hp,    m_mem_player.max_hp, ocr_hp);
        mem_me.mp = safePct(m_mem_player.mp,    m_mem_player.max_mp, ocr_mp);
        mem_me.cp = safePct(m_mem_player.cp,    m_mem_player.max_cp, ocr_cp);
        m_me = mem_me;
        m_detect_me_fail_count = 0;
    }

    // KnownList: оновлюємо WorldState кожен тік
    if (m_world && m_player_base) {
        if (m_mem_player.valid) {
            m_world->playerX = m_mem_player.x;
            m_world->playerY = m_mem_player.y;
            m_world->playerZ = m_mem_player.z;
        }
        m_world->update(m_player_base, m_cfg.knownlist_max_range);
    }

    // Авто-калібрування TargetStatusWnd
    if (m_eyes.GetAutoCalX() >= 0) {
        Log("[Eyes] TargetWnd авто-калібрування: x=" +
            std::to_string(m_eyes.GetAutoCalX()) + " → " +
            std::to_string(m_eyes.GetTargetWndX()), LogLevel::Warning);
        m_eyes.ClearAutoCalX();
    }

    // Якщо не вдалося детектувати HP — чекаємо
    if (!m_me.has_value()) {
        m_detect_me_fail_count++;
        if (debug)
            Log("[DEBUG] DetectMe() failed.", LogLevel::Debug);
        if (m_detect_me_fail_count == 30 ||
            (m_detect_me_fail_count > 30 && m_detect_me_fail_count % 300 == 0)) {
            Log("[WARNING] DetectMe() не знаходить HP бар вже " +
                std::to_string(m_detect_me_fail_count / 10) + "с! "
                "Перевір калібрування (F12) або положення вікна.", LogLevel::Warning);
        }
        return;
    }
    m_detect_me_fail_count = 0;

    const auto& me = m_me.value();

    // Перевірка потіонів (не залежать від стану)
    CheckPotions(me);

    // Перевірка смерті (HP≤1% протягом 10 тіків = ~1с)
    // ≤1 замість ==0: мертвий персонаж може читатись як 1% (1px бару видимий)
    // Не тригеримо в Buff стані: HP bar може тимчасово не детектуватись.
    const std::string cur_obj = m_bot_bt.currentBranch();
    bool is_dead_now = false;
    if (cur_obj != "Dead" && cur_obj != "Buff") {
        if (me.hp <= 1 && !InRespawnGrace()) {
            m_hp_zero_count++;
            if (m_hp_zero_count >= 10) is_dead_now = true;
        } else {
            m_hp_zero_count = 0;
        }
    }

    // Тригер смерті: тільки логуємо для діагностики.
    // RecordDeath() і NotifyDeath() перенесено в actDead Фаза 0 (BT) —
    // виконуються рівно 1 раз на реальну смерть.
    // OCR-based is_dead_now може хибно спрацьовувати при HP=1% (TH Vampiric Rage).
    if (is_dead_now && m_prev_obj_name != "Dead") {
        Log("[DEAD] Персонаж загинув. Спроба відродження...", LogLevel::Error);
    }

    // Heartbeat: кожні 5с (~50 тіків × 100мс)
    m_heartbeat_tick++;
    if (m_heartbeat_tick % 50 == 1) {
        std::string buff_info = !m_cfg.buff_enabled
            ? "buff=ВИМК"
            : "buff_in=" + std::to_string((int)(m_cfg.buff_interval - SecsSinceLastBuff())) + "с";
        Log("[HB] OBJ=" + cur_obj +
            " HP=" + std::to_string(me.hp) +
            " MP=" + std::to_string(me.mp) +
            " CP=" + std::to_string(me.cp) +
            " ready=" + std::string(m_hands.IsReady() ? "Y" : "N") +
            " " + buff_info);
        if (m_heartbeat_tick == 1 && m_eyes.MyBars().has_value()) {
            const auto& bars = m_eyes.MyBars().value();
            auto r2s = [](const cv::Rect& r) {
                return "(" + std::to_string(r.x) + "," + std::to_string(r.y) +
                       " " + std::to_string(r.width) + "x" + std::to_string(r.height) + ")";
            };
            Log("[HB] BarRects HP=" + r2s(bars.hp_bar) +
                " MP=" + r2s(bars.mp_bar) +
                " CP=" + r2s(bars.cp_bar), LogLevel::Debug);
        }
    }

    // Telegram статистика
    m_notify.CheckStatsInterval(m_stats);

    // Скидаємо async vision флаг
    m_has_async_vision = false;

    // Objectives tick — вся решта логіки
    GameState gs{ m_eyes, m_hands, m_cfg, m_stats };
    gs.cb.log_fn          = [this](const std::string& msg) { Log(msg); };
    gs.cb.notify_death_fn = [this]() { m_notify.NotifyDeath(); };
    updateGameState(gs);
    gs.is_dead = is_dead_now;  // передаємо поточний стан смерті

    std::string branch = m_bot_bt.tick(gs);
    if (m_heartbeat_tick % 50 == 1)
        Log("[BT] " + branch + " avg=" + std::to_string(m_bot_bt.avgTimeUs()) + "µs");
    if (branch != m_prev_obj_name)
        m_prev_obj_name = branch;

    // Після Done DeadObjective: reset hp_zero_count
    if (m_prev_obj_name != "Dead") {
        if (cur_obj == "Dead") m_hp_zero_count = 0;
    }

    // Shadow Mode: паралельне читання Memory для порівняння з OCR (MR26)
    // НЕ впливає на рішення бота — OCR завжди primary
    if (m_shadow_mode_active && m_shadow_logger)
        readShadowMemoryState(gs);

    // Performance
    double tick_ms = std::chrono::duration<double, std::milli>(Now() - tick_start).count();
    if (tick_ms > 50.0)
        Log("[PERF] Повільний тік: " + std::to_string((int)tick_ms) + "мс",
            LogLevel::Warning);
}

// ── updateGameState ───────────────────────────────────────────────────────────
void Brain::updateGameState(GameState& gs) {
    if (m_me.has_value()) {
        gs.hp = m_me->hp; gs.mp = m_me->mp; gs.cp = m_me->cp;
        gs.hp_valid = true;
    } else {
        gs.hp_valid = false;
    }
    gs.player_x       = m_mem_player.x;
    gs.player_y       = m_mem_player.y;
    gs.player_z       = m_mem_player.z;
    gs.player_heading = m_mem_player.heading;
    gs.coords_valid   = m_mem_player.valid;

    // Fallback: якщо MemReader вимкнений/невалідний, але PlayerBase є —
    // читаємо XYZ гравця напряму з playerBase (той самий offset що й KnownList).
    // Потрібно для KL-HP distanceTo() — без coords KL-HP ніколи не запускається.
    if (!gs.coords_valid && m_world && m_player_base) {
        if (m_world->refreshPlayerXYZ()) {
            gs.player_x   = m_world->playerX;
            gs.player_y   = m_world->playerY;
            gs.player_z   = m_world->playerZ;
            gs.coords_valid = true;
        }
    }

    gs.target     = m_target;
    gs.has_target = HasTarget();
    gs.target_hp  = m_target.has_value() ? m_target->hp : 0;

    // Minimap throttle: оновлюємо не частіше ніж MINIMAP_UPDATE_MS (10 FPS)
    {
        auto minimap_now = Clock::now();
        auto minimap_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            minimap_now - m_minimap_last_update).count();
        if (minimap_elapsed_ms >= MINIMAP_UPDATE_MS) {
            if (m_has_async_vision && m_async_minimap.has_value())
                m_minimap_cache = *m_async_minimap;
            else
                m_minimap_cache = m_eyes.DetectMinimap();
            m_minimap_last_update = minimap_now;
        }
    }
    gs.minimap_dots = m_minimap_cache;

    // hp_falling: будь-яке зменшення HP порівняно з попереднім тіком.
    // Порогу нема — навіть -1 одиниця означає активну атаку (Vampiric Rage: зупинка = смерть).
    if (gs.hp_valid && m_hp_prev >= 0) {
        gs.hp_falling = (gs.hp < m_hp_prev);
    } else {
        gs.hp_falling = false;
    }
    m_hp_prev = gs.hp_valid ? gs.hp : -1;

    // minimap_close_threat: є точка мінімапи ближча за ~70px від центру.
    // 70px з 78px радіусу ≈ 90% радіусу мінімапи (~1200 юнітів).
    // Далекі моби (/target "Name" зона) видно тільки біля самого краю (dist >70px).
    // Поріг 35px був надто малим: моб на dist≈67px не детектувався, хоча вже в зоні атаки.
    {
        constexpr float kClosePx = 70.f;
        gs.minimap_close_threat = false;
        for (const auto& d : gs.minimap_dots) {
            if (d.dist < kClosePx) { gs.minimap_close_threat = true; break; }
        }
    }

    // Читаємо KnownList тільки якщо PlayerBase валідний.
    // Без цієї умови WorldState bg thread може повертати stale garbage-дані
    // після reset PlayerBase (кожні 30с validity check).
    if (m_world && m_player_base) {
        gs.kl_mobs        = m_world->mobs();
        gs.kl_alive_count = m_world->aliveCount();
        gs.kl_mob_died    = m_world->anyMobDiedThisTick();
    } else {
        gs.kl_mobs.clear();
        gs.kl_alive_count = 0;
        gs.kl_mob_died    = false;
    }

    gs.secs_since_last_buff = SecsSinceLastBuff();
    gs.secs_since_last_kill = SecsSinceLastKill();
    gs.in_grace    = InRespawnGrace();
    gs.hands_ready = m_hands.IsReady();
    // is_dead буде встановлено в Process() після updateGameState

    // RandomDelay (Brain-owned)
    gs.rd_attack = m_rd_attack.get();
    gs.rd_rotate = m_rd_rotate.get();
    gs.rd_walk   = m_rd_walk.get();

    // Geodata
    gs.geodata = m_geodata.get();

    // NavMesh
    gs.navmesh = m_navmesh_builder.get();

    // NavMesh: збір точок перенесено в TryRecordNavPoint() — викликається з main loop
    // кожну ітерацію (навіть hands busy), щоб не пропускати рух під час дій

    // Callbacks
    gs.cb.navigate_to_mob = [this](const L2Character& mob) {
        return NavigateToMob(mob);
    };
    gs.cb.is_blacklisted   = [this](int id) {
        return IsBlacklisted(id);
    };
    gs.cb.blacklist_mob     = [this](int id, float secs) {
        BlacklistMob(id, secs);
    };
    gs.cb.select_target     = [this](const std::vector<L2Character>& mobs,
                               float px, float py) -> std::optional<L2Character> {
        return SelectWeightedTarget(mobs, px, py);
    };
    gs.cb.find_nearest_mob = [this](const std::vector<L2Character>& mobs,
                                  float px, float py, float range) -> std::optional<L2Character> {
        if (!m_world) return std::nullopt;
        return m_world->findNearestMob(mobs, px, py, range);
    };
}

// ── CheckPotions ──────────────────────────────────────────────────────────────
void Brain::CheckPotions(const Eyes::Me& me) {
    if (me.hp > 0 && me.hp < m_cfg.hp_threshold && SecsSince(m_last_hp_pot) > 5.0) {
        Log("[POTION] HP " + std::to_string(me.hp) + "% < " +
            std::to_string(m_cfg.hp_threshold) + "% → вживаємо HP потіон\n");
        m_hands.PressKeyboardKey(m_cfg.hp_key);
        m_hands.Send(50);
        m_last_hp_pot = Now();
        m_stats.RecordHPPotion();
    }
    if (me.mp > 0 && me.mp < m_cfg.mp_threshold && SecsSince(m_last_mp_pot) > 5.0) {
        Log("[POTION] MP " + std::to_string(me.mp) + "% < " +
            std::to_string(m_cfg.mp_threshold) + "% → вживаємо MP потіон\n");
        m_hands.PressKeyboardKey(m_cfg.mp_key);
        m_hands.Send(50);
        m_last_mp_pot = Now();
        m_stats.RecordMPPotion();
    }
    if (me.cp > 0 && me.cp < m_cfg.cp_threshold && SecsSince(m_last_cp_pot) > 5.0) {
        Log("[POTION] CP " + std::to_string(me.cp) + "% < " +
            std::to_string(m_cfg.cp_threshold) + "% → вживаємо CP потіон");
        m_hands.PressKeyboardKey(m_cfg.cp_key);
        m_hands.Send(50);
        m_last_cp_pot = Now();
    }
}

// ── NormalizeAngle ────────────────────────────────────────────────────────────
float Brain::NormalizeAngle(float angle) {
    while (angle >  (float)M_PI) angle -= 2.f * (float)M_PI;
    while (angle < -(float)M_PI) angle += 2.f * (float)M_PI;
    return angle;
}

// ── Blacklist ─────────────────────────────────────────────────────────────────
void Brain::CleanBlacklist() {
    TP now = Now();
    m_blacklist.erase(
        std::remove_if(m_blacklist.begin(), m_blacklist.end(),
            [&now](const BlacklistedMob& b){ return now >= b.until; }),
        m_blacklist.end());
}

void Brain::BlacklistMob(int objectID, float seconds) {
    if (objectID == 0) return;
    CleanBlacklist();
    for (auto& b : m_blacklist) {
        if (b.objectID == objectID) { b.until = FutureBy(seconds); return; }
    }
    BlacklistedMob entry;
    entry.objectID = objectID;
    entry.until    = FutureBy(seconds);
    m_blacklist.push_back(entry);
    Log("[BLACKLIST] Моб ID=" + std::to_string(objectID) +
        " заблокований на " + std::to_string((int)seconds) + "с", LogLevel::Warning);
}

bool Brain::IsBlacklisted(int objectID) const {
    if (objectID == 0) return false;
    TP now = Now();
    for (const auto& b : m_blacklist) {
        if (b.objectID == objectID && now < b.until) return true;
    }
    return false;
}

// ── LoadNavMeshPoints ─────────────────────────────────────────────────────────
void Brain::LoadNavMeshPoints() {
    if (!m_cfg.navmesh_cfg.collect_points) return;
    const std::string& path = m_cfg.navmesh_cfg.points_file;
    std::ifstream f(path, std::ios::binary);
    if (!f) return;
    uint32_t n = 0;
    f.read((char*)&n, 4);
    if (n == 0 || n > 100000) return;
    m_nav_points.resize(n);
    f.read((char*)m_nav_points.data(), n * 12);
    // Заповнюємо grid щоб нові точки не дублювали наявні
    const float cell = m_cfg.navmesh_cfg.collect_dist > 0.f
                     ? m_cfg.navmesh_cfg.collect_dist : 30.f;
    for (const auto& p : m_nav_points) {
        int64_t ix = (int64_t)std::floor(p.x / cell);
        int64_t iy = (int64_t)std::floor(p.y / cell);
        uint64_t key = (uint64_t)(ix + 0x80000) | ((uint64_t)(iy + 0x80000) << 32);
        m_nav_grid_cells.insert(key);
    }
    std::cerr << "[NAVMESH] Завантажено " << n << " точок з " << path << "\n";
}

// ── SaveNavMeshPoints ─────────────────────────────────────────────────────────
void Brain::SaveNavMeshPoints() const {
    if (!m_cfg.navmesh_cfg.collect_points || m_nav_points.empty()) return;
    std::ofstream f(m_cfg.navmesh_cfg.points_file, std::ios::binary);
    if (!f) {
        std::cerr << "[NAVMESH] Не вдалося зберегти точки: "
                  << m_cfg.navmesh_cfg.points_file << "\n";
        return;
    }
    uint32_t n = (uint32_t)m_nav_points.size();
    f.write((const char*)&n, 4);
    f.write((const char*)m_nav_points.data(), n * 12);
    std::cerr << "[NAVMESH] Збережено " << n << " точок у "
              << m_cfg.navmesh_cfg.points_file << "\n";
}

// ── TryRecordNavPoint ─────────────────────────────────────────────────────────
// Викликати з main loop на КОЖНІЙ ітерації (навіть коли hands busy),
// щоб фіксувати рух під час WalkForward/RotateLeft/AttackSkill тощо.
void Brain::TryRecordNavPoint() {
    if (!m_cfg.navmesh_cfg.collect_points) return;

    const float dist2 = m_cfg.navmesh_cfg.collect_dist * m_cfg.navmesh_cfg.collect_dist;

    // Спочатку — позиція гравця з MemReader (якщо Enabled=true)
    if (m_mem_player.valid) {
        float dx = m_mem_player.x - m_nav_last_x;
        float dy = m_mem_player.y - m_nav_last_y;
        if (dx*dx + dy*dy >= dist2) {
            m_nav_points.push_back({m_mem_player.x, m_mem_player.y, m_mem_player.z});
            m_nav_last_x = m_mem_player.x;
            m_nav_last_y = m_mem_player.y;
            std::cerr << "[NAVMESH] +player #" << m_nav_points.size()
                      << " x=" << (int)m_mem_player.x
                      << " y=" << (int)m_mem_player.y
                      << " z=" << (int)m_mem_player.z << "\n";
        }
        return;
    }

    // Mob positions: основне джерело NavMesh точок під час фарму.
    // Бот рухається до мобів → mob XYZ = реальні досяжні місця в підземеллі.
    // Grid-based дедуплікація (клітина = collect_dist): кожна область записується раз.
    // Запускається завжди — не залежить від доступності позиції гравця.
    if (m_world) {
        const auto mobs = m_world->mobs(); // snapshot під lock
        const float cell = m_cfg.navmesh_cfg.collect_dist > 0.f
                         ? m_cfg.navmesh_cfg.collect_dist : 30.f;
        for (const auto& mob : mobs) {
            if (!mob.isAlive()) continue;
            int64_t ix = (int64_t)std::floor(mob.x / cell);
            int64_t iy = (int64_t)std::floor(mob.y / cell);
            uint64_t key = (uint64_t)(ix + 0x80000) | ((uint64_t)(iy + 0x80000) << 32);
            if (m_nav_grid_cells.count(key)) continue;
            m_nav_grid_cells.insert(key);
            m_nav_points.push_back({mob.x, mob.y, mob.z});
            std::cerr << "[NAVMESH] +mob #" << m_nav_points.size()
                      << " x=" << (int)mob.x
                      << " y=" << (int)mob.y
                      << " z=" << (int)mob.z << "\n";
        }
    }

    // Додатково: клієнтська позиція гравця (pb+OFF_PLAYER_X_CLIENT = 0x78).
    // Оновлюється при click-to-move прибутті. НЕ для kill detection!
    if (m_world && m_world->refreshPlayerXYZClient()) {
        float px = m_world->playerX;
        float py = m_world->playerY;
        float pz = m_world->playerZ;
        if (std::fabsf(px) > 30000.f || std::fabsf(py) > 30000.f) {
            float dx = px - m_nav_last_x;
            float dy = py - m_nav_last_y;
            if (dx*dx + dy*dy >= dist2) {
                int64_t ix = (int64_t)std::floor(px / (m_cfg.navmesh_cfg.collect_dist > 0 ? m_cfg.navmesh_cfg.collect_dist : 30.f));
                int64_t iy = (int64_t)std::floor(py / (m_cfg.navmesh_cfg.collect_dist > 0 ? m_cfg.navmesh_cfg.collect_dist : 30.f));
                uint64_t key = (uint64_t)(ix + 0x80000) | ((uint64_t)(iy + 0x80000) << 32);
                if (!m_nav_grid_cells.count(key)) {
                    m_nav_grid_cells.insert(key);
                    m_nav_points.push_back({px, py, pz});
                    m_nav_last_x = px; m_nav_last_y = py;
                    std::cerr << "[NAVMESH] +player #" << m_nav_points.size()
                              << " x=" << (int)px << " y=" << (int)py << "\n";
                } else {
                    m_nav_last_x = px; m_nav_last_y = py;
                }
            }
        }
    }
}

// ── NavigateToMob ─────────────────────────────────────────────────────────────
bool Brain::NavigateToMob(const L2Character& mob) {
    if (!m_mem_player.valid) return false;

    m_nav_state.playerX       = m_mem_player.x;
    m_nav_state.playerY       = m_mem_player.y;
    m_nav_state.playerHeading = m_mem_player.heading;
    m_nav_state.targetX       = mob.x;
    m_nav_state.targetY       = mob.y;
    m_nav_state.targetZ       = mob.z;

    float dx = mob.x - m_mem_player.x;
    float dy = mob.y - m_mem_player.y;
    m_nav_state.distance = std::sqrt(dx*dx + dy*dy);

    if (!m_cfg.navigation.use_heading || m_mem_player.heading == 0.f) {
        if (m_nav_state.distance > m_cfg.navigation.attack_range) {
            int walk_ms = std::min((int)(m_nav_state.distance * 10.f), 800);
            m_hands.WalkForward(walk_ms);
            Log("[NAV-MEM] WalkForward dist=" +
                std::to_string((int)m_nav_state.distance) +
                " ms=" + std::to_string(walk_ms), LogLevel::Debug);
            return true;
        }
        return false;
    }

    float targetAngle = std::atan2(dx, dy);
    m_nav_state.angleDiff = NormalizeAngle(targetAngle - m_nav_state.playerHeading);

    const float tol = m_cfg.navigation.angle_tolerance;

    if (std::fabsf(m_nav_state.angleDiff) > tol) {
        int rot_ms = (int)(std::fabsf(m_nav_state.angleDiff) / 0.175f * 100.f);
        rot_ms = std::min(rot_ms, 500);
        rot_ms = std::max(rot_ms, 80);
        if (m_nav_state.angleDiff > 0) {
            m_hands.RotateRight(rot_ms);
            Log("[NAV-MEM] RotateRight " +
                std::to_string((int)(m_nav_state.angleDiff * 57.3f)) + "° ms=" +
                std::to_string(rot_ms), LogLevel::Debug);
        } else {
            m_hands.RotateLeft(rot_ms);
            Log("[NAV-MEM] RotateLeft " +
                std::to_string((int)(-m_nav_state.angleDiff * 57.3f)) + "° ms=" +
                std::to_string(rot_ms), LogLevel::Debug);
        }
        return true;
    }

    if (m_nav_state.distance > m_cfg.navigation.attack_range) {
        int walk_ms = std::min((int)(m_nav_state.distance * 10.f), 800);
        m_hands.WalkForward(walk_ms);
        Log("[NAV-MEM] WalkForward dist=" +
            std::to_string((int)m_nav_state.distance) +
            " ms=" + std::to_string(walk_ms), LogLevel::Debug);
        return true;
    }
    return false;
}

// ── InitRandomDelays ──────────────────────────────────────────────────────────
void Brain::InitRandomDelays() {
    if (!m_cfg.delays.enabled) {
        m_rd_attack.reset();
        m_rd_rotate.reset();
        m_rd_walk.reset();
        return;
    }
    m_rd_attack = std::make_unique<RandomDelay>(
        m_cfg.delays.attack_mean_ms, m_cfg.delays.attack_std_ms);
    m_rd_rotate = std::make_unique<RandomDelay>(
        m_cfg.delays.rotate_mean_ms, m_cfg.delays.rotate_std_ms);
    m_rd_walk   = std::make_unique<RandomDelay>(
        m_cfg.delays.walk_mean_ms, m_cfg.delays.walk_std_ms);
}

// ── SelectWeightedTarget ──────────────────────────────────────────────────────
std::optional<L2Character> Brain::SelectWeightedTarget(
        const std::vector<L2Character>& mobs,
        float playerX, float playerY) {
    if (!m_cfg.weighted_target.enabled || mobs.empty()) return std::nullopt;

    const float maxRange = m_cfg.weighted_target.max_range;
    std::optional<L2Character> best;
    float bestScore = -1.f;

    for (const auto& mob : mobs) {
        if (mob.isDead || mob.hp <= 0.f) continue;
        if (IsBlacklisted(mob.objectID)) continue;

        // Fuzzy matching по назві якщо увімкнено і є список дозволених
        if (m_cfg.fuzzy.enabled && !m_cfg.mob_names.empty() && !mob.name.empty()) {
            bool name_ok = false;
            for (const auto& allowed : m_cfg.mob_names)
                if (FuzzyMatch(mob.name, allowed, m_cfg.fuzzy.threshold)) { name_ok = true; break; }
            if (!name_ok) continue;
        }

        float dist = mob.distanceTo(playerX, playerY);
        if (dist > maxRange) continue;

        float s_dist  = 1.f - std::min(dist / maxRange, 1.f);
        float hpPct   = mob.hpMax > 0.f ? mob.hp / mob.hpMax : 1.f;
        float s_hp    = 1.f - std::min(hpPct, 1.f);
        float s_fresh = mob.name.empty() ? 0.5f : 1.f;

        float score = m_cfg.weighted_target.w_distance  * s_dist
                    + m_cfg.weighted_target.w_low_hp     * s_hp
                    + m_cfg.weighted_target.w_freshness  * s_fresh;

        if (score > bestScore) { bestScore = score; best = mob; }
    }
    if (best.has_value())
        Log("[WEIGHT] → " +
            (best->name.empty() ? "ID=" + std::to_string(best->objectID) : best->name) +
            " dist=" + std::to_string((int)best->distanceTo(playerX, playerY)) +
            " hp=" + std::to_string((int)best->hpPercent()) + "%",
            LogLevel::Debug);
    return best;
}

// ── VisionWorker / GeodataWorker ──────────────────────────────────────────────
void Brain::SetAsyncNPCs(const std::vector<Eyes::NPC>& npcs,
                          const std::vector<Eyes::MinimapDot>& minimap) {
    m_async_npcs    = npcs;
    m_async_minimap = minimap;
    m_has_async_vision = true;
}

void Brain::SetGeoPath(const std::vector<std::pair<float,float>>& path,
                        uint64_t path_id) {
    m_bot_bt.deliverGeoPath(path, path_id);
    if (!path.empty())
        Log("[GEO-W] Шлях отримано: " + std::to_string(path.size()) + " точок",
            LogLevel::Debug);
}

std::optional<PathRequest> Brain::GetPendingPathRequest() {
    return m_bot_bt.takePendingPathRequest();
}

// ── readShadowMemoryState (MR26) ──────────────────────────────────────────────
void Brain::readShadowMemoryState(GameState& gs) {
    // ── Валідація PlayerState ─────────────────────────────────────────────────
    if (m_mem_player.valid) {
        auto vr = MemoryValidator::validatePlayer(m_mem_player);
        if (!vr.valid) {
            m_consecutive_mem_fails++;
            m_shadow_logger->logValidationError(vr.error);
            // Логуємо тільки при першому перевищенні порогу, далі замовкаємо.
            // Умова ">= threshold" раніше логувала КОЖЕН тік → 140K рядків спаму.
            if (m_consecutive_mem_fails == m_cfg.mem_max_consecutive_fails) {
                m_shadow_logger->logFailureAlert(m_consecutive_mem_fails);
                Log("[Shadow] MemReader: " + std::to_string(m_consecutive_mem_fails)
                    + " послідовних помилок (далі мовчимо): " + vr.error, LogLevel::Warning);
            }
        } else {
            m_consecutive_mem_fails = 0;
        }
    }

    // ── A/B порівняння HP/MP: Memory vs OCR ──────────────────────────────────
    if (m_mem_player.valid && m_me.has_value())
        m_shadow_logger->logPlayerComparison(m_mem_player, m_me.value());

    // ── A/B порівняння кількості мобів: Memory vs мінімапа ───────────────────
    if (gs.kl_alive_count >= 0 && !gs.minimap_dots.empty())
        m_shadow_logger->logMobComparison(gs.kl_alive_count,
                                          (int)gs.minimap_dots.size());

    // ── Валідація першого живого моба з KnownList (вибірково) ─────────────────
    for (const auto& mob : gs.kl_mobs) {
        if (!mob.isAlive()) continue;
        auto mvr = MemoryValidator::validateMob(mob);
        if (!mvr.valid)
            m_shadow_logger->logValidationError(
                "mob[" + std::to_string(mob.objectID) + "]: " + mvr.error);
        break; // тільки перший живий моб за тік
    }

    // ── Статистика кожні StatsLogInterval секунд ──────────────────────────────
    auto now  = Clock::now();
    double elapsed = std::chrono::duration<double>(now - m_last_shadow_stats_log).count();
    if (elapsed >= (double)m_cfg.mem_stats_log_interval_s) {
        m_last_shadow_stats_log = now;
        m_shadow_logger->flush();
        Log("[Shadow] Stats: "
            + std::to_string(m_shadow_logger->totalComparisons()) + " cmp, "
            + std::to_string(m_shadow_logger->discrepancies()) + " diff, "
            + "avg HP diff=" + std::to_string(m_shadow_logger->avgHpDiffPercent()) + "%");
    }
}

