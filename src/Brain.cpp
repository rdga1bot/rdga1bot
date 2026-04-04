#include "Brain.h"
#include <iostream>
#include <ctime>
#include <cmath>
#include <opencv2/opencv.hpp>

using namespace std::chrono_literals;

// Лог: фільтр за рівнем, виводить у stdout і Dashboard callback
void Brain::Log(const std::string& msg, LogLevel level) {
    if (level < m_min_log_level) return;
    // Префікс з часом HH:MM:SS
    std::time_t t = std::time(nullptr);
    char ts[10];
    std::strftime(ts, sizeof(ts), "%H:%M:%S", std::localtime(&t));
    std::string line = std::string("[") + ts + "] " + msg;
    std::cout << line << "\n";
    if (m_log_callback) m_log_callback(std::move(line));
}

Brain::Brain(Eyes& eyes, Hands& hands, const Config& cfg)
    : m_eyes(eyes)
    , m_hands(hands)
    , m_cfg(cfg)
    , m_notify(cfg.tg_token, cfg.tg_chat_id, cfg.tg_on_death, cfg.tg_stats_interval)
{
    m_last_attack = Now();
    m_combat_watchdog_start = Now();
    m_last_buff = Clock::now() - std::chrono::hours(1); // перший баф одразу при старті
    m_respawn_until = FutureBy(10.0); // 10с grace при старті
    m_last_hp_pot = Now();
    m_last_mp_pot = Now();
    m_last_cp_pot = Now();
    m_session_start = Now();
    m_last_kill_time = Now() - std::chrono::hours(1); // далеке минуле → cooldown вже пройшов

    // Ініціалізуємо RandomDelay генератори
    InitRandomDelays();

    // Завантажуємо шаблони кнопок ALT+B (один раз при старті)
    m_buff_tab_templ     = cv::imread("template/buff_tab.png");
    m_buff_profile_templ = cv::imread("template/buff_profile.png");
    if (m_buff_tab_templ.empty())
        Log("[Buffs] template/buff_tab.png не знайдено — використовуємо координати");
    if (m_buff_profile_templ.empty())
        Log("[Buffs] template/buff_profile.png не знайдено — використовуємо координати");
    if (!m_buff_tab_templ.empty() && !m_buff_profile_templ.empty())
        Log("[Buffs] Шаблони ALT+B завантажено ✓");
}

void Brain::ReloadConfig(const Config& new_cfg) {
    m_cfg = new_cfg;
    InitRandomDelays();
    Log("[CONFIG] Конфігурацію перезавантажено");
}

void Brain::Init() {
    Log("[Brain] Ініціалізація...\n");
    EnterState(State::Idle);
}

const char* Brain::StateName(State s) {
    switch (s) {
        case State::Idle:      return "IDLE";
        case State::Targeting: return "TARGETING";
        case State::Attacking: return "ATTACKING";
        case State::Looting:   return "LOOTING";
        case State::Dead:      return "DEAD";
        case State::Buffing:   return "BUFFING";
        default:               return "UNKNOWN";
    }
}

void Brain::EnterState(State s) {
    if (m_state != s) {
        Log("[STATE] " + std::string(StateName(m_state)) + " -> " + std::string(StateName(s)));
    }
    m_state = s;

    switch (s) {
        case State::Idle:
            break;

        case State::Targeting:
            // m_macro_idx НЕ скидаємо — він зберігається між циклами для ротації F7→F8→F9→...
            m_macro_attempts = 0;
            m_prev_target_hp = -1;
            m_minimap_rotate_count = 0;
            m_far_target_rejects = 0;
            m_pokemon_targeted = false;
            m_pokemon_macro_fired = false;
            m_dead_target_esc_count = 0;
            m_dead_cycles_total = 0;
            m_walk_stuck_count = 0;
            m_nav_prev_was_walk = false;
            m_nav_stuck_recoveries = 0;
            m_patrol_step_idx = 0;
            m_minimap_low_flow_since = TP{};
            m_minimap_flow_stuck = false;
            m_running_to_mob = false;
            m_run_started = TP{};
            // m_attack_was_unreachable НЕ скидаємо тут — скидається в HandleTargeting()
            // тільки коли мінімапа реально показала мобів (знайшли вихід з кімнати)
            break;

        case State::Attacking:
            m_running_to_mob = false;
            m_no_target_count = 0;
            m_target_hp_zero_count = 0;
            m_first_attack = true;
            m_attack_idx = 0;
            m_prev_target_hp = -1;
            m_combat_watchdog_start = Now();
            m_last_target_redetect = Now();
            m_last_attack = Now() - std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<double>(m_cfg.attack_wait + 0.1));
            m_attack_last_target_hp = -1;
            m_attack_hp_stable_since = Now();
            m_approach_retarget_count = 0;
            m_approach_last_retarget = Now();
            m_approach_entry_hp = m_target.has_value() ? m_target->hp : 100;
            break;

        case State::Looting:
            m_looting_issued = false;
            m_loot_start = Now();
            m_eyes.ResetTarget(); // кеш бару скидаємо — наступний моб буде детектуватись заново
            break;

        case State::Dead:
            m_dead_phase = 0;
            m_in_death = true;
            m_hp_zero_count = 0;
            m_stats.RecordDeath();
            m_notify.NotifyDeath();
            Log("[DEAD] Персонаж загинув. Спроба відродження...", LogLevel::Error);
            break;

        case State::Buffing:
            m_buff_idx = 0;
            m_buff_stage = 0;
            m_buff_open_retries = 0;
            m_buff_tab_fallback = false;
            break;
    }
}

void Brain::Process(bool debug) {
    if (m_paused) return;

    const auto tick_start = Now();

    // Детекція стану персонажа
    m_me = m_eyes.DetectMe();
    m_target = m_eyes.DetectTarget();

    // Memory Reading: якщо дані валідні — замінюємо/доповнюємо OpenCV детекцію
    if (m_mem_player.valid) {
        Eyes::Me mem_me;
        // HP/MP/CP як % (0-100)
        mem_me.hp = (m_mem_player.max_hp > 0)
            ? (m_mem_player.hp * 100 / m_mem_player.max_hp) : (m_me ? m_me->hp : 0);
        mem_me.mp = (m_mem_player.max_mp > 0)
            ? (m_mem_player.mp * 100 / m_mem_player.max_mp) : (m_me ? m_me->mp : 0);
        mem_me.cp = (m_mem_player.max_cp > 0)
            ? (m_mem_player.cp * 100 / m_mem_player.max_cp) : (m_me ? m_me->cp : 0);
        m_me = mem_me;
        m_detect_me_fail_count = 0;
    }

    // KnownList: оновлюємо WorldState кожен тік (якщо увімкнено)
    if (m_world && m_player_base) {
        if (m_mem_player.valid) {
            m_world->playerX = m_mem_player.x;
            m_world->playerY = m_mem_player.y;
            m_world->playerZ = m_mem_player.z;
        }
        m_world->update(m_player_base, m_cfg.knownlist_max_range);
    }

    // Авто-калібрування TargetStatusWnd: логуємо якщо x змінився
    if (m_eyes.GetAutoCalX() >= 0) {
        Log("[Eyes] TargetWnd авто-калібрування: x=" +
            std::to_string(m_eyes.GetAutoCalX()) + " → " +
            std::to_string(m_eyes.GetTargetWndX()),  // old → new
            LogLevel::Warning);
        m_eyes.ClearAutoCalX();
    }

    // Якщо не вдалося детектувати HP — чекаємо
    if (!m_me.has_value()) {
        m_detect_me_fail_count++;
        if (debug) {
            Log("[DEBUG] DetectMe() failed. Можливо HP/MP бар кольори не відповідають.", LogLevel::Debug);
        }
        // WARNING після 3с мовчання, потім кожні 30с
        if (m_detect_me_fail_count == 30 || (m_detect_me_fail_count > 30 && m_detect_me_fail_count % 300 == 0)) {
            Log("[WARNING] DetectMe() не знаходить HP бар вже " +
                std::to_string(m_detect_me_fail_count / 10) + "с! "
                "Перевір калібрування (F12) або положення вікна.", LogLevel::Warning);
        }
        return;
    }
    m_detect_me_fail_count = 0;

    const auto& me = m_me.value();

    // Перевірка смерті (HP = 0 протягом 10 тіків поспіль = 1с)
    // Debounce 10 тіків: справжня смерть тримає HP=0 нескінченно; false positive (текстура/флікер) — 1-3 тіки.
    // Не тригеримо в BUFFING: під час бафу персонаж стоїть без атак → HP bar може тимчасово не детектуватись.
    if (m_state != State::Dead && m_state != State::Buffing) {
        if (me.hp == 0 && !InRespawnGrace()) {
            m_hp_zero_count++;
            if (m_hp_zero_count >= 10) {
                EnterState(State::Dead);
                return;
            }
        } else {
            m_hp_zero_count = 0;
        }
    }

    // Перевірка потіонів (не в стані смерті)
    if (m_state != State::Dead) {
        CheckPotions(me);
    }

    // Перевірка часу бафів: тільки в IDLE/TARGETING + інтервал минув + cooldown пройшов.
    // Cooldown: >= 2с після останнього вбивства (LOOTING займає ~300мс → 2с завжди в TARGETING).
    // При старті: m_last_kill_time = Now()-1год → SecsSince >= 2 одразу.
    // На активному споті: kills кожні 3-5с → через 2с після kill ми вже в TARGETING → баф.
    const bool buff_cooldown_ok = SecsSince(m_last_kill_time) >= 2.0;
    if (m_cfg.buff_enabled &&
        (m_state == State::Idle || m_state == State::Targeting) &&
        (m_cfg.buff_use_altb || !m_cfg.buff_keys.empty()) &&
        SecsSince(m_last_buff) >= (double)m_cfg.buff_interval &&
        buff_cooldown_ok) {
        EnterState(State::Buffing);
        return;
    }

    // Перевірка Telegram статистики
    m_notify.CheckStatsInterval(m_stats);

    // Heartbeat: кожні 5с виводимо стан для діагностики зависань
    m_heartbeat_tick++;
    if (m_heartbeat_tick % 50 == 1) {
        std::string buff_info = !m_cfg.buff_enabled
            ? "buff=ВИМК"
            : "buff_in=" + std::to_string((int)(m_cfg.buff_interval - SecsSince(m_last_buff))) + "с";
        Log("[HB] State=" + std::string(StateName(m_state)) +
            " HP=" + std::to_string(me.hp) +
            " MP=" + std::to_string(me.mp) +
            " CP=" + std::to_string(me.cp) +
            " ready=" + std::string(m_hands.IsReady() ? "Y" : "N") +
            " " + buff_info);
        // Один раз: логуємо bar rects для діагностики
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

    // Диспетч до обробника стану
    switch (m_state) {
        case State::Idle:      HandleIdle();      break;
        case State::Targeting: HandleTargeting(); break;
        case State::Attacking: HandleAttacking(); break;
        case State::Looting:   HandleLooting();   break;
        case State::Dead:      HandleDead();      break;
        case State::Buffing:   HandleBuffing();   break;
    }

    // Performance: попередження якщо тік > 50мс
    double tick_ms = std::chrono::duration<double, std::milli>(Now() - tick_start).count();
    if (tick_ms > 50.0) {
        Log("[PERF] Повільний тік: " + std::to_string((int)tick_ms) + "мс", LogLevel::Warning);
    }

    // Скидаємо async vision флаг — результат вже використано (або не прийшов)
    m_has_async_vision = false;
}

void Brain::HandleIdle() {
    if (HasTarget()) {
        EnterState(State::Attacking);
    } else {
        EnterState(State::Targeting);
    }
}

void Brain::HandleTargeting() {
    if (!m_hands.IsReady()) {
        m_not_ready_count++;
        if (m_not_ready_count == 20 || m_not_ready_count % 100 == 0) {
            Log("[WARNING] TARGETING: IsReady=false вже " +
                std::to_string(m_not_ready_count) + " тіків — Input thread завис?",
                LogLevel::Warning);
        }
        return;
    }
    m_not_ready_count = 0;

    // Є ціль — перевіряємо чи вона близько (screen Y), потім атакуємо
    if (HasTarget()) {
        // Якщо є /target макроси — відфільтровуємо далеких мобів за Y позицією на екрані.
        // Висока Y (низ екрану) = близько, низька Y (верх) = далеко.
        // Поріг 300px для вікна ~768px (верхні ~39% = "дійсно далеко").
        // Screen-Y фільтр: cy < порогу → таргет "далеко" → ESC, шукаємо ближчого.
        // Конфігурується в .ini: NearbyYThreshold (0 = вимкнено), MaxFarRejects.
        // Дефолт 200px / 5 відхилень — обережно для підземель/коридорів.
        const int kNearbyYThreshold = m_cfg.nearby_y_threshold;
        const int kMaxFarRejects    = m_cfg.max_far_rejects;

        if (kNearbyYThreshold > 0 && !m_cfg.target_macro_keys.empty()
            && m_far_target_rejects < kMaxFarRejects) {
            std::vector<Eyes::NPC> npcs;
            if (m_has_async_vision && m_async_npcs.has_value()) {
                npcs = *m_async_npcs;
            } else {
                npcs = m_eyes.DetectNPCs();
            }
            for (const auto& npc : npcs) {
                if (npc.Selected() && npc.center.y < kNearbyYThreshold) {
                    Log("[TARGETING] Моб далеко (cy=" + std::to_string(npc.center.y)
                        + ", reject=" + std::to_string(m_far_target_rejects + 1)
                        + "/" + std::to_string(kMaxFarRejects) + ") → ESC");
                    m_hands.PressKeyboardKey(Input::KeyboardKey::Escape);
                    m_far_target_rejects++;
                    m_hands.Send(150);
                    return;
                }
            }
        }

        EnterState(State::Attacking);
        return;
    }

    // ── Мертвий таргет (hp=0) ────────────────────────────────────────────────
    if (m_target.has_value() && m_target->hp == 0) {
        if (m_pokemon_targeted) {
            // Pokemon: F3 вже відправив sweep через /useshortcut 2 5 разом з /target Pokemon.
            // Чекаємо анімацію sweep, потім знімаємо таргет.
            Log("[Pokemon] sweep (чекаємо анімацію)...");
            m_hands.Delay(1500);
            m_hands.PressKeyboardKey(Input::KeyboardKey::Escape);
            m_pokemon_targeted = false;
            m_dead_target_esc_count = 0;
            m_hands.Send(100);
            return;
        }
        m_dead_target_esc_count++;
        if (m_dead_target_esc_count == 1) {
            // Перший тік hp=0: чекаємо підтвердження (гра рендерить UI ~100-200мс після F2/macro).
            // Якщо наступний тік теж hp=0 → справжній мертвий моб → ESC.
            // Якщо наступний тік hp>0 → фалш-негатив детекції → продовжуємо до ATTACKING.
            // 250мс (замість 100): /target Name макроси обробляються ~200мс → дає достатньо часу.
            Log("[TARGETING] Мертвий таргет hp=0 ×1 → чекаємо підтвердження...", LogLevel::Debug);
            m_hands.Send(250);
            return;
        }
        Log("[TARGETING] Мертвий таргет hp=0 → ESC ×" + std::to_string(m_dead_target_esc_count - 1));
        if (m_dead_target_esc_count <= 6) {
            // Підтверджений мертвий моб (2+ тіки hp=0) — знімаємо таргет
            m_hands.PressKeyboardKey(Input::KeyboardKey::Escape);
            m_hands.Send(100);
            return;
        }
        // Після 5 спроб ESC не допомагає — fallthrough до F2/macro (не return).
        // Скидаємо лічильник щоб наступний цикл почався заново (не спамимо F2 кожен тік).
        m_dead_target_esc_count = 0;
        m_dead_cycles_total++;
        // Після 3 fallthrough циклів поспіль: F2 постійно повертає мертвих мобів →
        // одразу переходимо на /target макроси (вони не вибирають мертвих).
        static constexpr int kDeadCyclesMacroSwitch = 3;
        if (m_dead_cycles_total >= kDeadCyclesMacroSwitch && !m_cfg.target_macro_keys.empty()) {
            Log("[TARGETING] Dead-target loop ×" + std::to_string(m_dead_cycles_total) +
                " → /target макрос", LogLevel::Warning);
            m_macro_attempts++;
            m_hands.TargetMacro(m_macro_idx);
            m_macro_idx = (m_macro_idx + 1) % (int)m_cfg.target_macro_keys.size();
            m_hands.Send(300); // 300мс: /target Name потребує ~200мс обробки в грі
            return;
        }
        Log("[TARGETING] Мертвий таргет не зникає після 5 ESC → пробуємо F2", LogLevel::Warning);
    } else {
        m_dead_target_esc_count = 0; // живий або відсутній таргет — скидаємо ESC лічильник
        // dead_cycles скидаємо тільки при ЖИВОМУ таргеті (hp>0).
        // При відсутності таргету (після ESC) — НЕ скидаємо: лічильник має накопичуватись
        // через цикли щоб після 3 fallthrough перейти на /target макрос.
        if (m_target.has_value() && m_target->hp > 0) {
            m_dead_cycles_total = 0;
        }
    }

    m_macro_attempts++;

    // ── Мінімапа: повернутись до найближчого моба перед F2 ──────────────────
    // Поріг dx=20: потрібно щоб моб був помітно збоку (не плутати з шумом/рамкою)
    // Ліміт rotate_count=4: якщо після 4 ротацій моб не знайдено → вважаємо точку фейком
    static constexpr int kMinimapDxThreshold = 20;
    static constexpr int kMinimapRotateLimit = 4;

    // Якщо є async результат від VisionWorker — використовуємо його,
    // інакше sync DetectMinimap() (fallback)
    std::vector<Eyes::MinimapDot> minimap_dots;
    if (m_has_async_vision && m_async_minimap.has_value()) {
        minimap_dots = *m_async_minimap;
    } else {
        minimap_dots = m_eyes.DetectMinimap();
    }

    // Якщо є вибраний моб (фіолетовий ореол) — ротуємось до нього; інакше до найближчого.
    const Eyes::MinimapDot* map_ref = nullptr;
    bool map_ref_selected = false;
    for (const auto& d : minimap_dots) {
        if (d.selected) { map_ref = &d; map_ref_selected = true; break; }
    }
    if (!map_ref && !minimap_dots.empty()) map_ref = &minimap_dots[0];

    if (map_ref && m_minimap_rotate_count < kMinimapRotateLimit) {
        const int dx = map_ref->dx;
        const int dy = map_ref->dy;
        const char* who = map_ref_selected ? "Вибраний" : "Найближчий";
        if (dx < -kMinimapDxThreshold) {
            m_hands.RotateLeft(RandMs(m_rd_rotate, 120));
            m_minimap_rotate_count++;
            Log(std::string("[MAP] ") + who + " моб ліворуч (dx=" + std::to_string(dx) +
                ", rot=" + std::to_string(m_minimap_rotate_count) + ") → RotateLeft",
                LogLevel::Debug);
        } else if (dx > kMinimapDxThreshold) {
            m_hands.RotateRight(RandMs(m_rd_rotate, 120));
            m_minimap_rotate_count++;
            Log(std::string("[MAP] ") + who + " моб праворуч (dx=" + std::to_string(dx) +
                ", rot=" + std::to_string(m_minimap_rotate_count) + ") → RotateRight",
                LogLevel::Debug);
        } else {
            // Моб по центру горизонтально — скидаємо лічильник
            m_minimap_rotate_count = 0;
            // Якщо моб позаду (dy > 30) — розворот на 180°
            if (dy > 30) {
                m_hands.RotateRight(RandMs(m_rd_rotate, 700));
                Log(std::string("[MAP] ") + who + " моб позаду (dy=" + std::to_string(dy) +
                    ") → розворот 180°", LogLevel::Debug);
            }
        }
    } else if (minimap_dots.empty()) {
        m_minimap_rotate_count = 0; // мінімапа порожня — скидаємо
    }

    // Основний метод: F2 /nexttarget
    m_hands.NextTarget();

    // Якщо мінімапа має моби — значить знайшли вихід з кімнати, скидаємо флаг
    if (!minimap_dots.empty() && m_attack_was_unreachable) {
        m_attack_was_unreachable = false;
        Log("[TARGETING] Мінімапа: моби знайдені → скидаємо unreachable flag", LogLevel::Debug);
    }

    // /target макроси (F7-F11) — ротація по черзі.
    // m_macro_idx НЕ скидається між циклами → природня ротація F7→F8→F9→...
    //
    // Режим 1 (attempt==1, нормальний): макрос ЗАМІСТЬ F2 на першій спробі кожного циклу.
    //   → бот таргетує різних мобів по іменах, а не завжди найближчого (F2).
    //   → якщо макрос не знайшов → attempt 2+ → F2 як fallback.
    //
    // Режим 2 (attempt > threshold, парна): fallback якщо F2 теж не знайшов.
    //
    // Unreachable: при m_attack_was_unreachable=true — відкладаємо до attempt 15
    //   (навігація повинна вийти з кімнати перш ніж макроси знову спрацюють).
    static constexpr int kMacroFallbackAfterUnreach = 15;
    const bool macro_at_start = !m_cfg.target_macro_keys.empty()
                                && !m_attack_was_unreachable
                                && m_macro_attempts == 1;
    const bool macro_fallback = !m_cfg.target_macro_keys.empty()
                                && m_macro_attempts > (m_attack_was_unreachable ? kMacroFallbackAfterUnreach : 2)
                                && m_macro_attempts % 2 == 0;
    if (macro_at_start || macro_fallback) {
        m_hands.Delay(80);
        m_hands.TargetMacro(m_macro_idx);
        m_macro_idx = (m_macro_idx + 1) % (int)m_cfg.target_macro_keys.size();
    }

    // Pokemon макрос: ОДИН РАЗ на початку TARGETING циклу (attempt=1).
    // Раніше: кожні 10 спроб (5,15,25...) → 2+ sweeps по 1500мс = +3с/цикл.
    // Тепер: тільки attempt==1, flag скидається в EnterState(Targeting).
    if (m_cfg.has_pokemon_key && !m_pokemon_macro_fired && m_macro_attempts == 1) {
        m_pokemon_macro_fired = true;
        m_hands.Delay(50);
        m_hands.PressKeyboardKey(m_cfg.pokemon_key);
        m_pokemon_targeted = true;
        Log("[Pokemon] макрос", LogLevel::Debug);
    }

    // ── Navigation: stuck detection ──────────────────────────────────────────
    // Перевіряємо рух ПІСЛЯ попереднього WalkForward.
    // IsCharacterMoving() завжди викликаємо для оновлення prev_frame.
    // GetMinimapFlow() — тільки якщо FlowDetection=true, і тільки після WalkForward
    // (flow=0 при стоячому таргетингу = норма, не ознака застрягання!).
    if (m_cfg.nav_stuck_detection) {
        const bool is_moving = m_eyes.IsCharacterMoving();

        if (m_nav_prev_was_walk) {
            m_nav_prev_was_walk = false;

            if (m_running_to_mob) {
                // Під час безперервного бігу flow ненадійний (підземелля: uniform walls,
                // rotating minimap → flow завжди ~0.01-0.3 навіть при реальному русі).
                // Використовуємо time-based escape: якщо біжимо >15с без таргету → поворот.
                const double run_secs = SecsSince(m_run_started);
                if (run_secs >= 15.0) {
                    const int rotation_ms = std::min(900 + (m_nav_stuck_recoveries / 2) * 450, 1800);
                    m_hands.WalkBack(300);
                    if (m_nav_stuck_recoveries % 2 == 0) {
                        m_hands.RotateRight(rotation_ms);
                        Log("[NAV] Біг " + std::to_string((int)run_secs) + "с без таргету"
                            " → WalkBack + RotateRight(" + std::to_string(rotation_ms) + "мс)");
                    } else {
                        m_hands.RotateLeft(rotation_ms);
                        Log("[NAV] Біг " + std::to_string((int)run_secs) + "с без таргету"
                            " → WalkBack + RotateLeft(" + std::to_string(rotation_ms) + "мс)");
                    }
                    m_nav_stuck_recoveries++;
                    m_run_started = Now(); // скидаємо таймер після повороту
                    m_nav_prev_was_walk = true; // перевіримо рух наступного тіку
                }
                // Якщо < 15с — продовжуємо бігти, ігноруємо flow
            } else {
                // Звичайний WalkForward (не RunTick): flow-based stuck detection
                const float flow    = m_cfg.nav_flow_detection ? m_eyes.GetMovementFlow()   : -1.0f;
                const float mm_flow = m_cfg.nav_flow_detection ? m_eyes.GetMinimapFlow()    : -1.0f;
                const bool actually_moved = is_moving || (flow > 1.5f) || (mm_flow > 0.3f);
                if (!actually_moved) {
                    m_walk_stuck_count++;
                    Log("[NAV] Не рухаємось після WalkForward ×" + std::to_string(m_walk_stuck_count)
                        + (flow >= 0 ? " flow=" + std::to_string(flow).substr(0,4) : ""),
                        LogLevel::Debug);
                    if (m_walk_stuck_count >= m_cfg.nav_stuck_threshold) {
                        m_walk_stuck_count = 0;
                        const int rotation_ms = std::min(900 + (m_nav_stuck_recoveries / 2) * 450, 1800);
                        m_hands.WalkBack(300);
                        if (m_nav_stuck_recoveries % 2 == 0) {
                            m_hands.RotateRight(rotation_ms);
                            Log("[NAV] Застряг ×" + std::to_string(m_nav_stuck_recoveries + 1) +
                                " → WalkBack + RotateRight(" + std::to_string(rotation_ms) + "мс)");
                        } else {
                            m_hands.RotateLeft(rotation_ms);
                            Log("[NAV] Застряг ×" + std::to_string(m_nav_stuck_recoveries + 1) +
                                " → WalkBack + RotateLeft(" + std::to_string(rotation_ms) + "мс)");
                        }
                        m_hands.WalkForward(500);
                        m_nav_prev_was_walk = true;
                        m_nav_stuck_recoveries++;
                    }
                } else {
                    m_walk_stuck_count = 0;
                    m_nav_stuck_recoveries = 0;
                    if (flow > 0) Log("[NAV] Рух ок, flow=" + std::to_string(flow).substr(0,4),
                                      LogLevel::Debug);
                }
            }
        }
    }

    // ── Memory навігація (пріоритет над мінімапою якщо увімкнено) ────────────
    if (m_cfg.navigation.enabled && m_world && m_player_base) {
        auto kl_mobs = m_world->mobs(); // snapshot copy (thread-safe)
        // Фільтрація blacklisted мобів
        CleanBlacklist();
        kl_mobs.erase(std::remove_if(kl_mobs.begin(), kl_mobs.end(),
            [this](const L2Character& mob){ return IsBlacklisted(mob.objectID); }),
            kl_mobs.end());
        if (!kl_mobs.empty()) {
            auto nearest = m_cfg.weighted_target.enabled
                ? SelectWeightedTarget(kl_mobs, m_mem_player.x, m_mem_player.y)
                : m_world->findNearestMob(kl_mobs, m_mem_player.x, m_mem_player.y,
                                          m_cfg.weighted_target.max_range);
            if (nearest.has_value()) {
                bool navigated = NavigateToMob(*nearest);
                if (navigated) {
                    std::string who = nearest->name.empty()
                        ? ("ID=" + std::to_string(nearest->objectID))
                        : nearest->name;
                    Log("[NAV-MEM] → " + who +
                        " dist=" + std::to_string((int)m_nav_state.distance) +
                        " hp=" + std::to_string((int)nearest->hpPercent()) + "%",
                        LogLevel::Debug);
                    m_hands.Send(150);
                    return;
                }
            }
        }
    }

    // ── Біг до моба: RunTick() кожен тік поки є моби ────────────────────────
    // RunTick() = HoldKeyboardKey(UP, 150мс) — персонаж біжить один тік.
    // Наступний тік знову RunTick() → безперервний рух без стану.
    // Ротація LEFT/RIGHT можлива одночасно з UP (L2 підтримує).
    // Рух вимкнено: RotateRight + RunTick накопичує кут → персонаж тікає від мобів.
    // В підземеллях достатньо ротації + F2; моби з aggro самі підходять після першого удару.
    const bool should_run  = false;

    if (should_run) {
        if (!m_running_to_mob) {
            m_running_to_mob = true;
            m_run_started    = Now();
            m_walk_stuck_count = 0;
            Log("[TARGETING] Спроба " + std::to_string(m_macro_attempts) +
                " — біжимо до моба");
        }
        if (!(m_cfg.nav_wall_detection && m_eyes.IsWallAhead())) {
            m_hands.RunTick(800);
            m_nav_prev_was_walk = true; // stuck detection активний між ранами
        }
    } else if (m_running_to_mob) {
        m_running_to_mob = false;
        Log("[TARGETING] Зупиняємо біг — мобів не видно", LogLevel::Debug);
    }

    // ── Fallback ротація / Patrol / Розвідка ─────────────────────────────────
    if (m_macro_attempts % 5 == 0 && minimap_dots.empty()) {
        m_hands.Delay(50);
        m_step_count++;
        m_stats.RecordTargetingFailure();

        // Patrol: якщо увімкнено і шукаємо довго → виконуємо крок патрулю
        const bool patrol_ready = m_cfg.patrol_enabled
            && !m_cfg.patrol_path.empty()
            && m_macro_attempts >= m_cfg.patrol_trigger_attempts
            && m_macro_attempts % 5 == 0;

        if (patrol_ready) {
            const auto& step = m_cfg.patrol_path[m_patrol_step_idx % (int)m_cfg.patrol_path.size()];
            switch (step.dir) {
                case Config::PatrolStep::Dir::Forward:
                    m_hands.WalkForward(step.ms);
                    m_nav_prev_was_walk = true;
                    Log("[PATROL] Крок " + std::to_string(m_patrol_step_idx % (int)m_cfg.patrol_path.size() + 1) +
                        "/" + std::to_string(m_cfg.patrol_path.size()) +
                        " → Forward(" + std::to_string(step.ms) + "мс)");
                    break;
                case Config::PatrolStep::Dir::Back:
                    m_hands.WalkBack(step.ms);
                    Log("[PATROL] Крок " + std::to_string(m_patrol_step_idx % (int)m_cfg.patrol_path.size() + 1) +
                        "/" + std::to_string(m_cfg.patrol_path.size()) +
                        " → Back(" + std::to_string(step.ms) + "мс)");
                    break;
                case Config::PatrolStep::Dir::RotateLeft:
                    m_hands.RotateLeft(step.ms);
                    Log("[PATROL] Крок " + std::to_string(m_patrol_step_idx % (int)m_cfg.patrol_path.size() + 1) +
                        "/" + std::to_string(m_cfg.patrol_path.size()) +
                        " → RotateLeft(" + std::to_string(step.ms) + "мс)");
                    break;
                case Config::PatrolStep::Dir::RotateRight:
                    m_hands.RotateRight(step.ms);
                    Log("[PATROL] Крок " + std::to_string(m_patrol_step_idx % (int)m_cfg.patrol_path.size() + 1) +
                        "/" + std::to_string(m_cfg.patrol_path.size()) +
                        " → RotateRight(" + std::to_string(step.ms) + "мс)");
                    break;
            }
            m_patrol_step_idx++;
        } else {
            // Звичайна fallback ротація: чергуємо ліво/право
            if ((m_macro_attempts / 5) % 2 == 0)
                m_hands.RotateRight(RandMs(m_rd_rotate, 350));
            else
                m_hands.RotateLeft(RandMs(m_rd_rotate, 350));
            // Розвідка вперед кожні 15 спроб коли немає мобів і не patrol
            if (!patrol_ready && m_macro_attempts % 15 == 0
                && !m_nav_prev_was_walk && m_eyes.IsGroundAhead()) {
                m_hands.WalkForward(1200);
                m_nav_prev_was_walk = true;
                Log("[TARGETING] Спроба " + std::to_string(m_macro_attempts) +
                    " — розвідка вперед (мінімапа порожня)");
            } else {
                Log("[TARGETING] Спроба " + std::to_string(m_macro_attempts) +
                    " — ротація (мінімапа порожня)");
            }
        }
    } else if (m_macro_attempts % 5 == 0) {
        m_stats.RecordTargetingFailure();
        Log("[TARGETING] Спроба " + std::to_string(m_macro_attempts) + " — шукаємо...");
    }

    m_hands.Send(150); // зменшено 250→150мс (гра відповідає на F2 за ~80мс)
}

void Brain::HandleAttacking() {
    if (!m_hands.IsReady()) return;

    // Re-detect target bar кожні 200мс — щоб помітити зникнення бару (моб загинув)
    if (SecsSince(m_last_target_redetect) >= 0.2) {
        m_eyes.ResetTarget();
        m_target = m_eyes.DetectTarget();
        m_last_target_redetect = Now();
    }

    // KnownList memory read: instant kill detection + HP update
    // Якщо feature flag вимкнено → логіка як раніше (OpenCV + debounce).
    if (m_world && !m_first_attack) {
        // Fast re-read таргету між bgLoop сканами
        if (m_player_base) m_world->update(m_player_base, m_cfg.knownlist_max_range);
        auto mem_mobs = m_world->mobs(); // snapshot copy (thread-safe)

        // Instant kill detection (mem_use_for_kill_detect або anyMobDiedThisTick)
        if (m_cfg.mem_use_for_kill_detect && !mem_mobs.empty()) {
            // Шукаємо мертвого моба в KnownList → instant LOOTING без debounce
            for (const auto& mob : mem_mobs) {
                if (mob.isDead || mob.hp <= 0.f) {
                    m_mem_target_hp_valid = true;
                    Log("[ATTACKING] [MEM] Instant kill (HP=" +
                        std::to_string((int)mob.hp) + ") → LOOTING");
                    EnterState(State::Looting);
                    return;
                }
            }
        } else if (m_world->anyMobDiedThisTick()) {
            // Fallback: anyMobDiedThisTick() (працює навіть без mem_use_for_kill_detect)
            Log("[ATTACKING] [KnownList] Таргет мертвий → LOOTING");
            EnterState(State::Looting);
            return;
        }

        // HP моба з пам'яті (якщо увімкнено) → оновлюємо m_target->hp для OpenCV pipeline
        // UseForTargetHP: беремо моба з МІНІМАЛЬНИМ HP% (той що атакується = найслабший)
        // Перший живий — хибний, бо KnownList не впорядкований за таргетом.
        if (m_cfg.mem_use_for_target_hp && !mem_mobs.empty() && m_target.has_value()) {
            float min_pct = 101.f;
            for (const auto& mob : mem_mobs) {
                if (!mob.isDead && mob.hp > 0.f && mob.hpMax > 0.f) {
                    float pct = mob.hpPercent();
                    if (pct < min_pct) min_pct = pct;
                }
            }
            if (min_pct <= 100.f) {
                m_mem_target_hp_valid = true;
                m_target->hp = (int)min_pct;
            }
        }
    }

    // Детекція смерті моба: HP ≤2% — потрібно 3 тіки поспіль (debounce false positives)
    // ⚠ Порогу 5% → 2%: бар ElmoreLab = 152px, мінімум 1px = 0.66% → округл. до 1%.
    // Мертвий моб (0px) = hp=0. Моб з 0.5 HP залишку = 1-2px = hp=1-2%.
    // Поріг 5% викликав "смикання": моб при 3-5% ще живий, бот знімав таргет → ESC → ре-таргет.
    if (m_target.has_value() && m_target->hp <= 2) {
        m_target_hp_zero_count++;
        if (m_target_hp_zero_count >= 3) {
            if (m_first_attack) {
                Log("[ATTACKING] Kill(hp=" + std::to_string(m_target->hp) + "%) вже мертвий до першої атаки → TARGETING");
                EnterState(State::Targeting);
            } else {
                Log("[ATTACKING] Kill(hp=" + std::to_string(m_target->hp) + "%) → LOOTING");
                EnterState(State::Looting);
            }
            return;
        }
    } else {
        m_target_hp_zero_count = 0;
    }

    // Таргет зник: потрібно 8 тіків поспіль ~800мс (debounce тимчасових провалів HasTargetName)
    if (!HasTarget()) {
        m_no_target_count++;
        if (m_no_target_count == 1) {
            std::string hp_str = m_target.has_value() ? std::to_string(m_target->hp) + "%" : "?";
            Log("[ATTACKING] Таргет зник (hp=" + hp_str + ", no_target ×1..8)");
        }
        if (m_no_target_count >= 8) {
            if (m_first_attack) {
                Log("[ATTACKING] NoTarget ×8 (first_attack) → TARGETING");
                EnterState(State::Targeting);
            } else {
                Log("[ATTACKING] NoTarget ×8 → LOOTING");
                EnterState(State::Looting);
            }
            return;
        }
        m_hands.Send(100);
        return;
    }
    m_no_target_count = 0;

    // Підхід до моба: ESC + /nexttarget кілька разів після першої атаки —
    // щоб переключитись на ближчого моба поки персонаж біжить до поточного таргету.
    // Гейт !m_first_attack: чекаємо першої атаки (персонаж рушив), тоді ESC+F2 як side-effect.
    // Не робимо return — атака продовжується на тому ж тіку; Eyes перевіряє нову ціль наступного тіку.
    // Підхід до моба: re-target тільки якщо моб ще не отримав шкоди (HP >= 90%).
    // Якщо вже б'ємо — добиваємо до кінця, не перемикаємось (інакше в густих зонах
    // бот крутиться між мобами і ніхто не вмирає).
    // Ре-таргет тільки якщо HP моба не змінився від початку бою —
    // тобто удар ще не дійшов (персонаж біжить). Якщо hp вже менший — добиваємо.
    // Re-target тільки для свіжих мобів (entry_hp >= 90%) і тільки якщо ще не дістали
    // (поточний hp близько до початкового). Якщо моб вже отримав удари — добиваємо.
    // Перевірка результату попереднього approach re-target:
    // якщо новий моб майже мертвий (HP < 15%) — скасовуємо переключення,
    // ESC і шукаємо кращий таргет. Не рахуємо цей re-target.
    // 15% (не 30%): на активних спотах моби на 15-29% ще варто атакувати.
    static constexpr int kMinApproachTargetHP = 15;
    if (m_approach_retarget_count > 0 &&
        m_target.has_value() && m_target->hp > 0 && m_target->hp < kMinApproachTargetHP &&
        SecsSince(m_approach_last_retarget) < 0.5)
    {
        Log("[ATTACKING] Підхід: новий таргет hp=" + std::to_string(m_target->hp)
            + "% < 15% (майже мертвий) → скасовуємо, шукаємо кращий");
        m_approach_retarget_count--;
        m_hands.PressKeyboardKey(Input::KeyboardKey::Escape);
        m_hands.Send(100);
        return;
    }

    // Approach re-target: тільки якщо мінімапа показує інших мобів.
    // Без цієї перевірки ESC+F2 в порожній зоні = втрата таргету → фейкове вбивство.
    // Opt: DetectMinimap() (~2-5мс) тільки якщо всі дешеві умови виконані.
    {
        const bool approach_possible = (false && m_approach_retarget_count < 3 &&
            !m_first_attack &&
            m_approach_entry_hp >= 90 &&
            m_target.has_value() && m_target->hp >= m_approach_entry_hp - 20 &&
            SecsSince(m_combat_watchdog_start) < 2.0 &&
            SecsSince(m_approach_last_retarget) >= 1.0);
        auto approach_dots = approach_possible ? m_eyes.DetectMinimap()
                                               : std::vector<Eyes::MinimapDot>{};
        if (approach_possible &&
            !approach_dots.empty()) // є куди переключатись
        {
            // ESC скидає живу ціль — інакше /nexttarget (F2) може ігноруватись грою
            m_hands.PressKeyboardKey(Input::KeyboardKey::Escape);
            m_hands.Delay(100);
            m_hands.PressKeyboardKey(m_cfg.next_target_key);
            m_hands.Send(150);
            m_eyes.ResetTarget(); // примусово перевіряємо нову ціль на наступному тіку
            m_approach_retarget_count++;
            m_approach_last_retarget = Now();
            Log("[ATTACKING] Re-target підхід #" + std::to_string(m_approach_retarget_count)
                + " (entry_hp=" + std::to_string(m_approach_entry_hp)
                + "% cur=" + std::to_string(m_target->hp) + "%, map="
                + std::to_string(approach_dots.size()) + ")");
            // не return — продовжуємо тік (атака/детекція таргету нижче)
        }
    }

    // HP-stable detection: якщо HP моба не змінився 5с після першої атаки —
    // моб недосяжний (застряг у текстурі / за стіною / вийшов з радіусу).
    // Залишаємо таргет + просуваємо macro_idx щоб наступний цикл спробував інший макрос.
    if (!m_first_attack && m_target.has_value() && m_target->hp > 0) {
        if (m_target->hp != m_attack_last_target_hp) {
            m_attack_last_target_hp = m_target->hp;
            m_attack_hp_stable_since = Now();
        } else if (SecsSince(m_attack_hp_stable_since) > 5.0) {
            Log("[ATTACKING] HP стабільний 5с (моб недосяжний) → TARGETING + інший макрос",
                LogLevel::Warning);
            if (!m_cfg.target_macro_keys.empty())
                m_macro_idx = (m_macro_idx + 1) % (int)m_cfg.target_macro_keys.size();
            m_attack_was_unreachable = true; // наступний TARGETING дасть більше часу навігації
            // Blacklist: знаходимо objectID недосяжного моба через KnownList (моб з мін HP%)
            if (m_world) {
                auto bl_mobs = m_world->mobs();
                if (!bl_mobs.empty()) {
                    // Беремо живого моба з мінімальним HP% (той що атакується)
                    int   bl_id  = 0;
                    float bl_pct = 101.f;
                    for (const auto& mob : bl_mobs) {
                        if (!mob.isDead && mob.hp > 0.f && mob.hpMax > 0.f) {
                            float pct = mob.hpPercent();
                            if (pct < bl_pct) { bl_pct = pct; bl_id = mob.objectID; }
                        }
                    }
                    if (bl_id != 0) BlacklistMob(bl_id, 60.f);
                }
            }
            m_target = std::nullopt; // скидаємо щоб HandleTargeting не бачив старий таргет
            m_hands.PressKeyboardKey(Input::KeyboardKey::Escape);
            m_hands.Send(200);
            EnterState(State::Targeting);
            return;
        }
    }

    // Watchdog: >attack_watchdog секунд в атаці → примусово переходимо до лутингу
    if (SecsSince(m_combat_watchdog_start) > m_cfg.attack_watchdog) {
        Log("[ATTACKING] Watchdog: таймаут → LOOTING", LogLevel::Warning);
        EnterState(State::Looting);
        return;
    }

    // Атака з кулдауном (per-skill delay або загальний attack_wait)
    double eff_delay = (m_cfg.delays.enabled && m_rd_attack)
        ? (double)m_rd_attack->Get() / 1000.0
        : m_cfg.GetAttackDelay(m_attack_idx);
    if (SecsSince(m_last_attack) >= eff_delay) {
        // Перша атака: Spoil для Spoiler класу
        if (m_first_attack && m_cfg.IsSpoiler()) {
            m_hands.PressKeyboardKey(m_cfg.spoil_key);
            m_hands.Delay(200);
        }
        m_hands.AttackSkill(m_attack_idx++);
        m_hands.Send(50);
        m_last_attack = Now();
        m_first_attack = false;
        m_stats.RecordAttack();
    }
}

void Brain::HandleLooting() {
    if (!m_hands.IsReady()) return;

    if (!m_looting_issued) {
        Log("[LOOTING] Вбивство #" + std::to_string(m_stats.kills + 1));
        m_stats.RecordKill();
        m_last_kill_time = Now(); // скидаємо post-combat cooldown
        // Авто-збереження stats
        if (m_cfg.auto_save_kills > 0 && m_stats.kills % m_cfg.auto_save_kills == 0) {
            m_stats.SaveToFile();
            Log("[STATS] Авто-збереження (" + std::to_string(m_stats.kills) + " kills)");
        }

        if (m_cfg.loot_enabled) {
            // ESC — знімаємо мертвий таргет; гра auto-loot за 300ms
            m_hands.PressKeyboardKey(Input::KeyboardKey::Escape);
            m_hands.Delay(300);
            m_hands.Send();
            m_looting_issued = true;
            return;
        }
        // loot_enabled=false: одразу до таргетингу без очікування авто-лута
    }

    EnterState(State::Targeting);
}

void Brain::HandleDead() {
    if (!m_hands.IsReady()) return;

    switch (m_dead_phase) {
        case 0:
            // Фаза 0: натискаємо Enter для відродження, чекаємо 5с
            Log("[DEAD] Фаза 0: Enter (відродження)\n");
            m_hands.PressKeyboardKey(Input::KeyboardKey::Enter);
            m_hands.Send(5000);
            m_dead_phase = 1;
            break;

        case 1:
            // Фаза 1: підтверджуємо відродження, чекаємо 20с
            Log("[DEAD] Фаза 1: підтвердження відродження, чекаємо 20с\n");
            m_hands.PressKeyboardKey(Input::KeyboardKey::Enter);
            m_hands.Send(20000);
            m_dead_phase = 2;
            break;

        case 2:
            // Фаза 2: у Village, встановлюємо grace period 30с
            Log("[DEAD] Фаза 2: відроджено, grace period 30с\n");
            m_respawn_until = FutureBy(30.0);
            m_in_death = false;
            EnterState(State::Idle);
            break;
    }
}

void Brain::HandleBuffing() {
    if (!m_hands.IsReady()) return;

    // Safety: якщо вбивство сталося після входу в BUFFING (edge case) — виходимо.
    if (SecsSince(m_last_kill_time) < 2.0) {
        Log("[Buffs] Kill щойно → скасовуємо баф", LogLevel::Debug);
        m_last_buff = Now() - std::chrono::seconds(m_cfg.buff_interval - 30);
        EnterState(State::Idle);
        return;
    }

    // Перевірка: якщо HP впало — переривати бафи, відкласти на 60с
    if (m_me.has_value() && m_me->hp > 0 && m_me->hp < m_cfg.hp_threshold) {
        Log("[Buffs] HP " + std::to_string(m_me->hp) + "% → перериваємо бафи!\n");
        // Відкладаємо повторну спробу на 60с (не скидаємо на повний інтервал)
        m_last_buff = Now() - std::chrono::seconds(m_cfg.buff_interval - 60);
        EnterState(State::Idle);
        return;
    }
    // Хелпер: надіслати ALT+B (відкрити/закрити вікно баффера)
    auto sendAltB = [&]() {
        m_hands.KeyboardKeyDown(Input::KeyboardKey::LeftAlt);
        m_hands.Delay(50);
        m_hands.KeyboardKeyDown(Input::KeyboardKey::B);
        m_hands.Delay(50);
        m_hands.KeyboardKeyUp(Input::KeyboardKey::B);
        m_hands.Delay(50);
        m_hands.KeyboardKeyUp(Input::KeyboardKey::LeftAlt);
    };

    if (HasTarget()) {
        // Якщо ALT+B вікно вже відкрито (stage >= 1) — закриваємо перед виходом,
        // інакше наступна спроба надішле ALT+B і закриє вже відкрите вікно.
        if (m_buff_stage >= 1) {
            sendAltB();
            m_hands.Send();
        }
        m_last_buff = Now() - std::chrono::seconds(m_cfg.buff_interval - 10);
        Log("[Buffs] Є таргет → перериваємо бафи, retry через 10с\n");
        EnterState(State::Attacking);
        return;
    }

    // Хелпер: визначити координати кліку через шаблон або fallback
    auto resolveClick = [&](const cv::Mat& templ, int fallback_x, int fallback_y,
                            const char* label) -> std::pair<int,int> {
        if (!templ.empty()) {
            float score = 0.0f;
            auto pt = m_eyes.FindTemplate(templ, 0.60f, &score);
            if (pt.has_value()) {
                Log(std::string("[Buffs] ") + label + " знайдено (score="
                    + std::to_string((int)(score * 100)) + "%): "
                    + std::to_string(pt->x) + "," + std::to_string(pt->y));
                return {pt->x, pt->y};
            }
            // Зберегти кадр для аналізу (щоб перезняти шаблон)
            m_eyes.SaveFrame(std::string("tmp/buff_debug_") + label + ".png");
            Log(std::string("[Buffs] ") + label + " шаблон не знайдено (score="
                + std::to_string((int)(score * 100)) + "%) → збережено tmp/buff_debug_"
                + label + ".png, координати ("
                + std::to_string(fallback_x) + "," + std::to_string(fallback_y) + ")",
                LogLevel::Warning);
        }
        return {fallback_x, fallback_y};
    };

    if (!m_cfg.buff_use_altb) {
        // Режим buff_keys (без ALT+B)
        if (m_buff_stage == 0) {
            Log("[Buffs] Застосовуємо бафи (" + std::to_string(m_cfg.buff_keys.size()) + ")\n");
            for (size_t i = 0; i < m_cfg.buff_keys.size(); i++) {
                m_hands.PressKeyboardKey(m_cfg.buff_keys[i]);
                m_hands.Delay(800);
            }
            m_hands.Send();
            m_buff_stage = 4; // → done
        } else {
            m_buff_stage = 0;
            m_last_buff = Now();
            Log("[Buffs] Завершено, наступний баф через " + std::to_string(m_cfg.buff_interval) + "с\n");
            EnterState(State::Idle);
        }
        return;
    }

    // ALT+B режим — багатостадійний FSM з template matching
    switch (m_buff_stage) {

    case 0: { // Чекаємо скидання бойового стану L2 → ESC + ALT+B
        // L2 combat/peace status скидається ~15с після останньої атаки.
        // До цього ALT+B не відкриє вікно ком'юніті — гра блокує.
        const double kCombatExpire = 15.0;
        const double secs_since_kill = SecsSince(m_last_kill_time);
        if (secs_since_kill < kCombatExpire) {
            // Log кожні ~5с (50 тіків × 100мс)
            if (m_buff_open_retries % 50 == 0) {
                Log("[Buffs] Чекаємо скидання бойового стану ще " +
                    std::to_string((int)(kCombatExpire - secs_since_kill)) + "с...",
                    LogLevel::Debug);
            }
            ++m_buff_open_retries;
            return; // m_buff_stage залишається 0 → наступний тік перевіряємо знову
        }
        m_buff_open_retries = 0; // скидаємо — використовується в stage 1 для retries
        Log("[Buffs] ESC + ALT+B → знімаємо таргет і відкриваємо вікно...");
        m_hands.PressKeyboardKey(Input::KeyboardKey::Escape);
        m_hands.Delay(300); // L2 обробляє ESC, таргет знятий
        sendAltB();
        m_hands.Delay(2000); // ALT+B відкривається ~1с + запас
        m_hands.Send();
        m_buff_stage = 1;
        break;
    }

    case 1: { // Знайти і натиснути вкладку "Баффер"
        // Зберігаємо кожну спробу: buff_stage1_check0.png, _check1.png, _check2.png
        m_eyes.SaveFrame("tmp/buff_stage1_check"
            + std::to_string(m_buff_open_retries) + ".png");
        float tab_score = 0.0f;
        auto tab_pt = m_buff_tab_templ.empty()
            ? std::optional<cv::Point>{}
            : m_eyes.FindTemplate(m_buff_tab_templ, 0.50f, &tab_score); // знижено 0.60→0.50

        if (!tab_pt.has_value() && m_buff_open_retries < 3) {
            // BBS не відкрилась — один ALT+B щоб відкрити (або закрити якщо на
            // неправильній сторінці, тоді наступний retry відкриє).
            m_buff_open_retries++;
            Log("[Buffs] Баффер не знайдено (score="
                + std::to_string((int)(tab_score * 100)) + "%) — ALT+B retry "
                + std::to_string(m_buff_open_retries) + "/3", LogLevel::Warning);
            sendAltB();
            m_hands.Delay(2500); // чекаємо завантаження BBS
            m_hands.Send();
            // залишаємось в stage 1
            break;
        }

        int cx, cy;
        if (tab_pt.has_value()) {
            Log("[Buffs] Баффер знайдено (score=" + std::to_string((int)(tab_score * 100))
                + "%): " + std::to_string(tab_pt->x) + "," + std::to_string(tab_pt->y));
            cx = tab_pt->x; cy = tab_pt->y;
        } else {
            // Всі retries вичерпано — fallback координати
            m_eyes.SaveFrame("tmp/buff_debug_Баффер.png");
            Log("[Buffs] Баффер не знайдено (score=" + std::to_string((int)(tab_score * 100))
                + "%) → fallback (" + std::to_string(m_cfg.buff_tab_x) + ","
                + std::to_string(m_cfg.buff_tab_y) + ")", LogLevel::Warning);
            cx = m_cfg.buff_tab_x; cy = m_cfg.buff_tab_y;
            m_buff_tab_fallback = true; // позначаємо: шаблон не знайдено, retry скоро
        }

        m_buff_tab_click_pos = {cx, cy}; // зберігаємо позицію для відносних координат
        m_eyes.SaveFrame("tmp/buff_stage1_before_click.png"); // кадр ДО кліку на Баффер
        m_hands.MoveMouseTo({cx, cy}); // WindowPoint() додає offset вікна → правильні screen coords
        m_hands.Delay(200);
        m_hands.LeftMouseButtonClick();
        m_hands.Delay(4000); // чекаємо поки сервер завантажить профілі
        m_hands.Send();
        m_buff_stage = 2;
        break;
    }

    case 2: { // Знайти і натиснути профіль "tty"
        m_eyes.SaveFrame("tmp/buff_stage2_after_tab.png"); // кадр ПІСЛЯ кліку на Баффер
        int fb_x = m_cfg.buff_profile_x;
        int fb_y = m_cfg.buff_profile_y;
        if (m_buff_tab_click_pos.x > 0 && m_cfg.buff_tab_x > 0) {
            int dx = m_buff_tab_click_pos.x - m_cfg.buff_tab_x;
            int dy = m_buff_tab_click_pos.y - m_cfg.buff_tab_y;
            fb_x = m_cfg.buff_profile_x + dx;
            fb_y = m_cfg.buff_profile_y + dy;
        }
        float prof_score = 0.f;
        auto prof_pt = m_buff_profile_templ.empty()
            ? std::optional<cv::Point>{}
            : m_eyes.FindTemplate(m_buff_profile_templ, 0.60f, &prof_score);

        // Не ретруємо — профілі вже завантажені після Delay(4000) в Stage 1.
        // Retries тут тільки з'їдали б 10с і дозволяли BBS авто-закритись.
        int cx, cy;
        if (prof_pt.has_value()) {
            Log("[Buffs] tty знайдено (score=" + std::to_string((int)(prof_score*100))
                + "%): " + std::to_string(prof_pt->x) + "," + std::to_string(prof_pt->y));
            cx = prof_pt->x; cy = prof_pt->y;
        } else {
            m_eyes.SaveFrame("tmp/buff_debug_tty.png"); // зберігаємо що бачить бот
            Log("[Buffs] tty не знайдено (score=" + std::to_string((int)(prof_score*100))
                + "%) → fallback (" + std::to_string(fb_x)
                + "," + std::to_string(fb_y) + ")", LogLevel::Warning);
            cx = fb_x; cy = fb_y;
        }
        m_hands.MoveMouseTo({cx, cy});
        m_hands.Delay(200);
        m_hands.LeftMouseButtonClick();
        m_hands.Delay(1000);
        m_hands.Send();
        m_buff_stage = 3;
        break;
    }

    case 3: // Закрити ALT+B
        sendAltB();
        m_hands.Delay(300);
        m_hands.Send();
        m_buff_stage = 4;
        break;

    case 4: // Готово
    default:
        m_buff_stage = 0;
        if (m_buff_tab_fallback) {
            // Шаблон "Баффер" не знайдено → можливо вікно не відкрилось.
            // Retry через 120с замість buff_interval.
            m_last_buff = Now() - std::chrono::seconds(m_cfg.buff_interval - 120);
            Log("[Buffs] Завершено (fallback), retry через 120с\n", LogLevel::Warning);
        } else {
            m_last_buff = Now();
            Log("[Buffs] Завершено, наступний баф через " + std::to_string(m_cfg.buff_interval) + "с\n");
        }
        EnterState(State::Idle);
        break;
    }
}

void Brain::CheckPotions(const Eyes::Me& me) {
    // HP потіон
    if (me.hp > 0 && me.hp < m_cfg.hp_threshold && SecsSince(m_last_hp_pot) > 5.0) {
        Log("[POTION] HP " + std::to_string(me.hp) + "% < " + std::to_string(m_cfg.hp_threshold) + "% → вживаємо HP потіон\n");
        m_hands.PressKeyboardKey(m_cfg.hp_key);
        m_hands.Send(50);
        m_last_hp_pot = Now();
        m_stats.RecordHPPotion();
    }

    // MP потіон
    if (me.mp > 0 && me.mp < m_cfg.mp_threshold && SecsSince(m_last_mp_pot) > 5.0) {
        Log("[POTION] MP " + std::to_string(me.mp) + "% < " + std::to_string(m_cfg.mp_threshold) + "% → вживаємо MP потіон\n");
        m_hands.PressKeyboardKey(m_cfg.mp_key);
        m_hands.Send(50);
        m_last_mp_pot = Now();
        m_stats.RecordMPPotion();
    }

    // CP потіон
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
    // Оновлюємо якщо вже є
    for (auto& b : m_blacklist) {
        if (b.objectID == objectID) {
            b.until = FutureBy(seconds);
            return;
        }
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
        if (b.objectID == objectID && now < b.until)
            return true;
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

    // Якщо heading не відкалібровано або navigation.use_heading=false →
    // тільки рух вперед без обчислення кута
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

    // Повний режим: heading відомий → обчислюємо кут і повертаємось
    // L2: X=East, Y=North. targetAngle = atan2(dx, dy) (не atan2(dy,dx) як у math)
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

    // Правильно повернуті → рухаємось
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


void Brain::SetAsyncNPCs(const std::vector<Eyes::NPC>& npcs,
                          const std::vector<Eyes::MinimapDot>& minimap) {
    m_async_npcs    = npcs;
    m_async_minimap = minimap;
    m_has_async_vision = true;
}

void Brain::SetGeoPath(const std::vector<std::pair<float,float>>& path,
                        uint64_t path_id) {
    if (path_id <= m_geo_path_id) return; // застарілий результат
    m_geo_path       = path;
    m_geo_path_idx   = 0;
    m_geo_path_id    = path_id;
    m_geo_path_ready = !path.empty();
    if (m_geo_path_ready)
        Log("[GEO-W] Шлях отримано: " +
            std::to_string(path.size()) + " точок", LogLevel::Debug);
}

std::optional<PathRequest> Brain::GetPendingPathRequest() {
    if (!m_pending_path_req.has_value()) return std::nullopt;
    auto req = std::move(m_pending_path_req);
    m_pending_path_req = std::nullopt;
    return req;
}
