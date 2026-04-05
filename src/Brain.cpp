#include "Brain.h"
#include "Utils.h"
#include <iostream>
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

    // Objectives: логування через Brain::Log
    m_obj_manager.setLogCallback([this](const std::string& msg) {
        Log(msg, LogLevel::Info);
    });

    // Реєстрація Objectives (порядок = пріоритет)
    m_obj_manager.add(std::make_unique<DeadObjective>());
    // RestObjective: активна якщо HP або MP нижче порогу
    if (m_cfg.mp_threshold > 0)
        m_obj_manager.add(std::make_unique<RestObjective>());
    // ZoneObjective: активна якщо персонаж вийшов з зони
    if (m_cfg.zone_enabled)
        m_obj_manager.add(std::make_unique<ZoneObjective>(
            m_cfg.zone_x, m_cfg.zone_y, m_cfg.zone_radius));
    m_obj_manager.add(std::make_unique<BuffObjective>());
    m_obj_manager.add(std::make_unique<TargetObjective>());
    m_obj_manager.add(std::make_unique<AttackObjective>());
    // LootObjective активується тільки через Switch з AttackObjective
    m_obj_manager.add(std::make_unique<LootObjective>());
}

void Brain::ReloadConfig(const Config& new_cfg) {
    m_cfg = new_cfg;
    InitRandomDelays();
    Log("[CONFIG] Конфігурацію перезавантажено");
}

void Brain::Init() {
    Log("[Brain] Ініціалізація...\n");
    // ObjectiveManager вибере перший canRun=true objective автоматично
}

// ── Process ───────────────────────────────────────────────────────────────────
void Brain::Process(bool debug) {
    if (m_paused) return;
    const auto tick_start = Now();

    // Детекція стану персонажа
    m_me     = m_eyes.DetectMe();
    m_target = m_eyes.DetectTarget();

    // Memory Reading: якщо дані валідні — замінюємо OpenCV детекцію
    if (m_mem_player.valid) {
        Eyes::Me mem_me;
        mem_me.hp = (m_mem_player.max_hp > 0)
            ? (m_mem_player.hp * 100 / m_mem_player.max_hp) : (m_me ? m_me->hp : 0);
        mem_me.mp = (m_mem_player.max_mp > 0)
            ? (m_mem_player.mp * 100 / m_mem_player.max_mp) : (m_me ? m_me->mp : 0);
        mem_me.cp = (m_mem_player.max_cp > 0)
            ? (m_mem_player.cp * 100 / m_mem_player.max_cp) : (m_me ? m_me->cp : 0);
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

    // Перевірка смерті (HP=0 протягом 10 тіків = ~1с)
    // Не тригеримо в Buff стані: HP bar може тимчасово не детектуватись.
    const std::string cur_obj = m_obj_manager.currentName();
    bool is_dead_now = false;
    if (cur_obj != "Dead" && cur_obj != "Buff") {
        if (me.hp == 0 && !InRespawnGrace()) {
            m_hp_zero_count++;
            if (m_hp_zero_count >= 10) is_dead_now = true;
        } else {
            m_hp_zero_count = 0;
        }
    }

    // Переходи стану — обробляємо тут щоб RecordDeath/NotifyDeath не дублювались
    if (is_dead_now && m_prev_obj_name != "Dead") {
        m_stats.RecordDeath();
        m_notify.NotifyDeath();
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
    gs.log_fn = [this](const std::string& msg) { Log(msg); };
    updateGameState(gs);
    gs.is_dead = is_dead_now;  // передаємо поточний стан смерті
    m_obj_manager.tick(gs);

    // Детекція переходу стану
    const std::string new_obj = m_obj_manager.currentName();
    if (new_obj != m_prev_obj_name) {
        m_prev_obj_name = new_obj;
    }

    // Після Done DeadObjective: reset hp_zero_count
    if (m_prev_obj_name != "Dead") {
        if (cur_obj == "Dead") m_hp_zero_count = 0;
    }

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

    if (m_world) {
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

    // Callbacks
    gs.navigate_to_mob = [this](const L2Character& mob) {
        return NavigateToMob(mob);
    };
    gs.is_blacklisted = [this](int id) {
        return IsBlacklisted(id);
    };
    gs.blacklist_mob = [this](int id, float secs) {
        BlacklistMob(id, secs);
    };
    gs.select_target = [this](const std::vector<L2Character>& mobs,
                               float px, float py) -> std::optional<L2Character> {
        return SelectWeightedTarget(mobs, px, py);
    };
    gs.find_nearest_mob = [this](const std::vector<L2Character>& mobs,
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
    m_obj_manager.deliverGeoPath(path, path_id);
    if (!path.empty())
        Log("[GEO-W] Шлях отримано: " + std::to_string(path.size()) + " точок",
            LogLevel::Debug);
}

std::optional<PathRequest> Brain::GetPendingPathRequest() {
    return m_obj_manager.takePendingPathRequest();
}
