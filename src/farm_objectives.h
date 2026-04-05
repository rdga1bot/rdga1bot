#pragma once
#include "objective.h"
#include "game_state.h"
#include "Eyes.h"
#include "world_state.h"
#include "Geodata.h"
#include "geodata_worker.h"
#include "RandomDelay.h"
#include "Input.h"
#include <chrono>
#include <vector>
#include <memory>
#include <cmath>
#include <opencv2/opencv.hpp>

// ── Допоміжний базовий клас для цілей що мають внутрішній стан часу ──────────
class TimedObjective : public Objective {
public:
    using Clock = std::chrono::steady_clock;
    using TP    = Clock::time_point;

    explicit TimedObjective(std::string name) : Objective(std::move(name)) {}

protected:
    static double SecsSince(TP t) {
        return std::chrono::duration<double>(Clock::now() - t).count();
    }
    static TP Now() { return Clock::now(); }
    static TP FutureBy(double s) {
        return Now() + std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(s));
    }

    // RandMs helper: якщо delays.enabled і rd не null → rand, інакше fixed
    int RandMs(RandomDelay* rd, const GameState& gs, int fixed_ms) {
        if (gs.cfg.delays.enabled && rd) return rd->Get();
        return fixed_ms;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// DeadObjective — замінює HandleDead()
// ─────────────────────────────────────────────────────────────────────────────
class DeadObjective : public TimedObjective {
public:
    DeadObjective() : TimedObjective("Dead") {}

    bool canRun(const GameState& gs) const override {
        return gs.is_dead;
    }

    void onEnter(GameState& gs) override {
        m_phase = 0;
        // RecordDeath і NotifyDeath виконуються в Brain::Process() на переході
        gs.log("[DEAD] Фаза 0: Enter (відродження)\n");
    }

    ObjectiveResult execute(GameState& gs) override {
        if (!gs.hands_ready) return ObjectiveResult::running();

        switch (m_phase) {
            case 0:
                gs.hands.PressKeyboardKey(Input::KeyboardKey::Enter);
                gs.hands.Send(5000);
                m_phase = 1;
                return ObjectiveResult::running();

            case 1:
                gs.log("[DEAD] Фаза 1: підтвердження відродження, чекаємо 20с\n");
                gs.hands.PressKeyboardKey(Input::KeyboardKey::Enter);
                gs.hands.Send(20000);
                m_phase = 2;
                return ObjectiveResult::running();

            case 2:
                gs.log("[DEAD] Фаза 2: відроджено, grace period 30с\n");
                m_respawn_until = FutureBy(30.0);
                return ObjectiveResult::done("відроджено");

            default:
                return ObjectiveResult::done();
        }
    }

    TP respawnUntil() const override { return m_respawn_until; }
    bool inGrace()    const override { return Clock::now() < m_respawn_until; }

private:
    int m_phase = 0;
    TP  m_respawn_until{};
};

// ─────────────────────────────────────────────────────────────────────────────
// BuffObjective — замінює HandleBuffing()
// ─────────────────────────────────────────────────────────────────────────────
class BuffObjective : public TimedObjective {
public:
    BuffObjective() : TimedObjective("Buff") {}

    bool canRun(const GameState& gs) const override {
        if (!gs.cfg.buff_enabled) return false;
        if (gs.is_dead) return false;
        if (gs.has_target) return false;  // не бафаємось під час бою
        if (!gs.buff_needed()) return false;
        if (!gs.cfg.buff_use_altb && gs.cfg.buff_keys.empty()) return false;
        // Cooldown: >= 2с після останнього вбивства
        return gs.secs_since_last_kill >= 2.0;
    }

    void onEnter(GameState& gs) override {
        m_stage          = 0;
        m_open_retries   = 0;
        m_tab_fallback   = false;
        m_tab_click_pos  = {0, 0};
        if (m_buff_tab_templ.empty())
            m_buff_tab_templ = cv::imread("template/buff_tab.png");
        if (m_buff_profile_templ.empty())
            m_buff_profile_templ = cv::imread("template/buff_profile.png");
        gs.log("[Buffs] Початок бафу");
    }

    ObjectiveResult execute(GameState& gs) override {
        if (!gs.hands_ready) return ObjectiveResult::running();

        // Safety: вбивство під час бафу (edge case)
        if (gs.secs_since_last_kill < 2.0) {
            gs.log("[Buffs] Kill щойно → скасовуємо баф");
            m_last_buff = Now() - std::chrono::seconds(gs.cfg.buff_interval - 30);
            return ObjectiveResult::done("kill під час бафу");
        }

        // HP низький → перериваємо
        if (gs.hp_valid && gs.hp > 0 && gs.hp < gs.cfg.hp_threshold) {
            gs.log("[Buffs] HP " + std::to_string(gs.hp) + "% → перериваємо бафи!\n");
            m_last_buff = Now() - std::chrono::seconds(gs.cfg.buff_interval - 60);
            return ObjectiveResult::done("низький HP");
        }

        auto sendAltB = [&]() {
            gs.hands.KeyboardKeyDown(Input::KeyboardKey::LeftAlt);
            gs.hands.Delay(50);
            gs.hands.KeyboardKeyDown(Input::KeyboardKey::B);
            gs.hands.Delay(50);
            gs.hands.KeyboardKeyUp(Input::KeyboardKey::B);
            gs.hands.Delay(50);
            gs.hands.KeyboardKeyUp(Input::KeyboardKey::LeftAlt);
        };

        // Є таргет → перериваємо
        if (gs.has_target) {
            if (m_stage >= 1) { sendAltB(); gs.hands.Send(); }
            m_last_buff = Now() - std::chrono::seconds(gs.cfg.buff_interval - 10);
            gs.log("[Buffs] Є таргет → перериваємо бафи, retry через 10с\n");
            return ObjectiveResult::switchTo("Attack");
        }

        auto resolveClick = [&](const cv::Mat& templ, int fallback_x, int fallback_y,
                                const char* label) -> std::pair<int,int> {
            if (!templ.empty()) {
                float score = 0.0f;
                auto pt = gs.eyes.FindTemplate(templ, 0.60f, &score);
                if (pt.has_value()) {
                    gs.log(std::string("[Buffs] ") + label + " знайдено (score="
                        + std::to_string((int)(score * 100)) + "%): "
                        + std::to_string(pt->x) + "," + std::to_string(pt->y));
                    return {pt->x, pt->y};
                }
                gs.eyes.SaveFrame(std::string("tmp/buff_debug_") + label + ".png");
                gs.log(std::string("[Buffs] ") + label + " шаблон не знайдено (score="
                    + std::to_string((int)(score * 100)) + "%) → збережено tmp/buff_debug_"
                    + label + ".png, координати ("
                    + std::to_string(fallback_x) + "," + std::to_string(fallback_y) + ")");
            }
            return {fallback_x, fallback_y};
        };

        if (!gs.cfg.buff_use_altb) {
            // Режим buff_keys (без ALT+B)
            if (m_stage == 0) {
                gs.log("[Buffs] Застосовуємо бафи (" +
                    std::to_string(gs.cfg.buff_keys.size()) + ")\n");
                for (size_t i = 0; i < gs.cfg.buff_keys.size(); i++) {
                    gs.hands.PressKeyboardKey(gs.cfg.buff_keys[i]);
                    gs.hands.Delay(800);
                }
                gs.hands.Send();
                m_stage = 4;
            } else {
                m_stage = 0;
                m_last_buff = Now();
                gs.log("[Buffs] Завершено, наступний баф через " +
                    std::to_string(gs.cfg.buff_interval) + "с\n");
                gs.hands.PressKeyboardKey(Input::KeyboardKey::Escape);
                gs.hands.Delay(300);
                gs.hands.Send();
                return ObjectiveResult::done("buff_keys завершено");
            }
            return ObjectiveResult::running();
        }

        // ALT+B режим — багатостадійний FSM з template matching
        switch (m_stage) {

        case 0: { // Чекаємо скидання бойового стану L2 → ESC + ALT+B
            const double kCombatExpire = 15.0;
            if (gs.secs_since_last_kill < kCombatExpire) {
                if (m_open_retries % 50 == 0) {
                    gs.log("[Buffs] Чекаємо скидання бойового стану ще " +
                        std::to_string((int)(kCombatExpire - gs.secs_since_last_kill)) + "с...");
                }
                ++m_open_retries;
                return ObjectiveResult::running();
            }
            m_open_retries = 0;
            gs.log("[Buffs] ESC + ALT+B → знімаємо таргет і відкриваємо вікно...");
            gs.hands.PressKeyboardKey(Input::KeyboardKey::Escape);
            gs.hands.Delay(300);
            sendAltB();
            gs.hands.Delay(2000);
            gs.hands.Send();
            m_stage = 1;
            break;
        }

        case 1: { // Знайти і натиснути вкладку "Баффер"
            gs.eyes.SaveFrame("tmp/buff_stage1_check"
                + std::to_string(m_open_retries) + ".png");
            float tab_score = 0.0f;
            auto tab_pt = m_buff_tab_templ.empty()
                ? std::optional<cv::Point>{}
                : gs.eyes.FindTemplate(m_buff_tab_templ, 0.50f, &tab_score);

            if (!tab_pt.has_value() && m_open_retries < 3) {
                m_open_retries++;
                gs.log("[Buffs] Баффер не знайдено (score="
                    + std::to_string((int)(tab_score * 100)) + "%) — ALT+B retry "
                    + std::to_string(m_open_retries) + "/3");
                sendAltB();
                gs.hands.Delay(2500);
                gs.hands.Send();
                break;
            }

            int cx, cy;
            if (tab_pt.has_value()) {
                gs.log("[Buffs] Баффер знайдено (score=" +
                    std::to_string((int)(tab_score * 100)) + "%): " +
                    std::to_string(tab_pt->x) + "," + std::to_string(tab_pt->y));
                cx = tab_pt->x; cy = tab_pt->y;
            } else {
                gs.eyes.SaveFrame("tmp/buff_debug_Баффер.png");
                gs.log("[Buffs] Баффер не знайдено (score=" +
                    std::to_string((int)(tab_score * 100)) + "%) → fallback (" +
                    std::to_string(gs.cfg.buff_tab_x) + "," +
                    std::to_string(gs.cfg.buff_tab_y) + ")");
                cx = gs.cfg.buff_tab_x; cy = gs.cfg.buff_tab_y;
                m_tab_fallback = true;
            }

            m_tab_click_pos = {cx, cy};
            gs.eyes.SaveFrame("tmp/buff_stage1_before_click.png");
            gs.hands.MoveMouseTo({cx, cy});
            gs.hands.Delay(200);
            gs.hands.LeftMouseButtonClick();
            gs.hands.Delay(4000);
            gs.hands.Send();
            m_stage = 2;
            break;
        }

        case 2: { // Знайти і натиснути профіль "tty"
            gs.eyes.SaveFrame("tmp/buff_stage2_after_tab.png");
            int fb_x = gs.cfg.buff_profile_x;
            int fb_y = gs.cfg.buff_profile_y;
            if (m_tab_click_pos.x > 0 && gs.cfg.buff_tab_x > 0) {
                int dx = m_tab_click_pos.x - gs.cfg.buff_tab_x;
                int dy = m_tab_click_pos.y - gs.cfg.buff_tab_y;
                fb_x = gs.cfg.buff_profile_x + dx;
                fb_y = gs.cfg.buff_profile_y + dy;
            }
            float prof_score = 0.f;
            auto prof_pt = m_buff_profile_templ.empty()
                ? std::optional<cv::Point>{}
                : gs.eyes.FindTemplate(m_buff_profile_templ, 0.60f, &prof_score);

            int cx, cy;
            if (prof_pt.has_value()) {
                gs.log("[Buffs] tty знайдено (score=" +
                    std::to_string((int)(prof_score * 100)) + "%): " +
                    std::to_string(prof_pt->x) + "," + std::to_string(prof_pt->y));
                cx = prof_pt->x; cy = prof_pt->y;
            } else {
                gs.eyes.SaveFrame("tmp/buff_debug_tty.png");
                gs.log("[Buffs] tty не знайдено (score=" +
                    std::to_string((int)(prof_score * 100)) + "%) → fallback (" +
                    std::to_string(fb_x) + "," + std::to_string(fb_y) + ")");
                cx = fb_x; cy = fb_y;
            }
            gs.hands.MoveMouseTo({cx, cy});
            gs.hands.Delay(200);
            gs.hands.LeftMouseButtonClick();
            gs.hands.Delay(1000);
            gs.hands.Send();
            m_stage = 3;
            break;
        }

        case 3: // Закрити ALT+B
            sendAltB();
            gs.hands.Delay(300);
            gs.hands.Send();
            m_stage = 4;
            break;

        case 4: // Готово
        default:
            m_stage = 0;
            if (m_tab_fallback) {
                m_last_buff = Now() - std::chrono::seconds(gs.cfg.buff_interval - 120);
                gs.log("[Buffs] Завершено (fallback), retry через 120с\n");
            } else {
                m_last_buff = Now();
                gs.log("[Buffs] Завершено, наступний баф через " +
                    std::to_string(gs.cfg.buff_interval) + "с\n");
            }
            gs.hands.PressKeyboardKey(Input::KeyboardKey::Escape);
            gs.hands.Delay(300);
            gs.hands.Send();
            return ObjectiveResult::done("buff завершено");
        }

        return ObjectiveResult::running();
    }

    TP lastBuff() const override { return m_last_buff; }

private:
    int       m_stage        = 0;
    int       m_open_retries = 0;
    bool      m_tab_fallback = false;
    cv::Point m_tab_click_pos{0, 0};
    cv::Mat   m_buff_tab_templ;
    cv::Mat   m_buff_profile_templ;
    TP        m_last_buff = Clock::now() - std::chrono::hours(1); // перший баф одразу
};

// ─────────────────────────────────────────────────────────────────────────────
// LootObjective — замінює HandleLooting()
// ─────────────────────────────────────────────────────────────────────────────
class LootObjective : public TimedObjective {
public:
    LootObjective() : TimedObjective("Loot") {}

    bool canRun(const GameState& /*gs*/) const override {
        // Активується тільки через Switch з AttackObjective
        return false;
    }

    void onEnter(GameState& gs) override {
        m_issued = false;
        gs.eyes.ResetTarget(); // скидаємо кеш бару — наступний моб детектується заново
        gs.stats.RecordKill();
        m_last_kill_time = Now();
        // Авто-збереження stats
        if (gs.cfg.auto_save_kills > 0 && gs.stats.kills % gs.cfg.auto_save_kills == 0) {
            gs.stats.SaveToFile();
            gs.log("[STATS] Авто-збереження (" + std::to_string(gs.stats.kills) + " kills)");
        }
        gs.log("[LOOTING] Вбивство #" + std::to_string(gs.stats.kills));
    }

    ObjectiveResult execute(GameState& gs) override {
        if (!gs.hands_ready) return ObjectiveResult::running();

        if (!m_issued) {
            if (gs.cfg.loot_enabled) {
                // ESC — знімаємо мертвий таргет; гра auto-loot за 300мс
                gs.hands.PressKeyboardKey(Input::KeyboardKey::Escape);
                gs.hands.Delay(300);
                gs.hands.Send();
                m_issued = true;
                return ObjectiveResult::running();
            }
            // loot_enabled=false: одразу до таргетингу
        }

        return ObjectiveResult::switchTo("Target");
    }

    TP lastKillTime() const override { return m_last_kill_time; }

private:
    bool m_issued = false;
    TP   m_last_kill_time = Clock::now() - std::chrono::hours(1);
};

// ─────────────────────────────────────────────────────────────────────────────
// AttackObjective — замінює HandleAttacking()
// ─────────────────────────────────────────────────────────────────────────────
class AttackObjective : public TimedObjective {
public:
    AttackObjective() : TimedObjective("Attack") {}

    bool canRun(const GameState& gs) const override {
        return !gs.is_dead && gs.has_target;
    }

    void onEnter(GameState& gs) override {
        m_first_attack         = true;
        m_attack_idx           = 0;
        m_no_target_count      = 0;
        m_target_hp_zero_count = 0;
        m_attack_last_hp       = -1;
        m_hp_stable_since      = Now();
        m_watchdog_start       = Now();
        m_last_attack          = Now() - std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(gs.cfg.attack_wait + 0.1));
        m_last_redetect        = Now();
        m_mem_target_hp_valid  = false;
        // Ініціалізація RandomDelay
        if (gs.cfg.delays.enabled) {
            m_rd_attack = std::make_unique<RandomDelay>(
                gs.cfg.delays.attack_mean_ms, gs.cfg.delays.attack_std_ms);
        } else {
            m_rd_attack.reset();
        }
    }

    ObjectiveResult execute(GameState& gs) override {
        if (!gs.hands_ready) return ObjectiveResult::running();

        // Re-detect target bar кожні 200мс
        if (SecsSince(m_last_redetect) >= 0.2) {
            gs.eyes.ResetTarget();
            const_cast<GameState&>(gs).target = gs.eyes.DetectTarget();
            // Перераховуємо has_target після оновлення
            const_cast<GameState&>(gs).has_target =
                gs.target.has_value() && gs.target->hp > 0;
            m_last_redetect = Now();
        }

        // KnownList memory read: instant kill detection + HP update
        if (!gs.kl_mobs.empty() && !m_first_attack) {
            // Instant kill via anyMobDiedThisTick
            if (gs.kl_mob_died) {
                gs.log("[ATTACKING] [KnownList] Таргет мертвий → LOOTING");
                return ObjectiveResult::switchTo("Loot");
            }

            // HP моба з пам'яті (якщо увімкнено) — моб з мінімальним HP% (той що атакується)
            if (gs.cfg.mem_use_for_target_hp && gs.target.has_value()) {
                float min_pct = 101.f;
                for (const auto& mob : gs.kl_mobs) {
                    if (!mob.isDead && mob.hp > 0.f && mob.hpMax > 0.f) {
                        float pct = mob.hpPercent();
                        if (pct < min_pct) min_pct = pct;
                    }
                }
                if (min_pct <= 100.f) {
                    m_mem_target_hp_valid = true;
                    const_cast<GameState&>(gs).target->hp = (int)min_pct;
                    const_cast<GameState&>(gs).has_target = (min_pct > 2.f);
                }
            }
        }

        // Kill detection: HP ≤ 2% — потрібно 3 тіки (debounce)
        if (gs.target.has_value() && gs.target->hp <= 2) {
            m_target_hp_zero_count++;
            if (m_target_hp_zero_count >= 3) {
                if (m_first_attack) {
                    gs.log("[ATTACKING] Kill(hp=" + std::to_string(gs.target->hp) +
                        "%) вже мертвий до першої атаки → TARGETING");
                    return ObjectiveResult::switchTo("Target");
                } else {
                    gs.log("[ATTACKING] Kill(hp=" + std::to_string(gs.target->hp) +
                        "%) → LOOTING");
                    return ObjectiveResult::switchTo("Loot");
                }
            }
        } else {
            m_target_hp_zero_count = 0;
        }

        // Таргет зник: потрібно 8 тіків поспіль ~800мс
        if (!gs.has_target) {
            m_no_target_count++;
            if (m_no_target_count == 1) {
                std::string hp_str = gs.target.has_value()
                    ? std::to_string(gs.target->hp) + "%" : "?";
                gs.log("[ATTACKING] Таргет зник (hp=" + hp_str + ", no_target ×1..8)");
            }
            if (m_no_target_count >= 8) {
                if (m_first_attack) {
                    gs.log("[ATTACKING] NoTarget ×8 (first_attack) → TARGETING");
                    return ObjectiveResult::switchTo("Target");
                } else {
                    gs.log("[ATTACKING] NoTarget ×8 → LOOTING");
                    return ObjectiveResult::switchTo("Loot");
                }
            }
            gs.hands.Send(100);
            return ObjectiveResult::running();
        }
        m_no_target_count = 0;

        // HP-stable: якщо HP моба не змінився 5с після першої атаки — моб недосяжний
        if (!m_first_attack && gs.target.has_value() && gs.target->hp > 0) {
            if (gs.target->hp != m_attack_last_hp) {
                m_attack_last_hp = gs.target->hp;
                m_hp_stable_since = Now();
            } else if (SecsSince(m_hp_stable_since) > 5.0) {
                gs.log("[ATTACKING] HP стабільний 5с (моб недосяжний) → TARGETING + інший макрос");
                // Сигналізуємо TargetObjective через callback (без raw pointers)
                if (gs.on_mob_unreachable) gs.on_mob_unreachable();
                // Blacklist: моб з мін HP% (той що атакується)
                if (gs.blacklist_mob) {
                    for (const auto& mob : gs.kl_mobs) {
                        if (!mob.isDead && mob.hp > 0.f && mob.hpMax > 0.f) {
                            float pct = mob.hpPercent();
                            float best = 101.f;
                            int   bid  = 0;
                            if (pct < best) { best = pct; bid = mob.objectID; }
                            if (bid != 0) gs.blacklist_mob(bid, 60.f);
                            break;
                        }
                    }
                }
                const_cast<GameState&>(gs).target = std::nullopt;
                gs.hands.PressKeyboardKey(Input::KeyboardKey::Escape);
                gs.hands.Send(200);
                return ObjectiveResult::switchTo("Target");
            }
        }

        // Watchdog: >attack_watchdog секунд → LOOTING
        if (SecsSince(m_watchdog_start) > gs.cfg.attack_watchdog) {
            gs.log("[ATTACKING] Watchdog: таймаут → LOOTING");
            return ObjectiveResult::switchTo("Loot");
        }

        // Атака з кулдауном
        double eff_delay = (gs.cfg.delays.enabled && m_rd_attack)
            ? (double)m_rd_attack->Get() / 1000.0
            : gs.cfg.GetAttackDelay(m_attack_idx);
        if (SecsSince(m_last_attack) >= eff_delay) {
            if (m_first_attack && gs.cfg.IsSpoiler()) {
                gs.hands.PressKeyboardKey(gs.cfg.spoil_key);
                gs.hands.Delay(200);
            }
            gs.hands.AttackSkill(m_attack_idx++);
            gs.hands.Send(50);
            m_last_attack = Now();
            m_first_attack = false;
            gs.stats.RecordAttack();
        }

        return ObjectiveResult::running();
    }

private:
    bool  m_first_attack         = true;
    int   m_attack_idx           = 0;
    int   m_no_target_count      = 0;
    int   m_target_hp_zero_count = 0;
    int   m_attack_last_hp       = -1;
    TP    m_hp_stable_since{};
    TP    m_watchdog_start{};
    TP    m_last_attack{};
    TP    m_last_redetect{};
    bool  m_mem_target_hp_valid  = false;
    std::unique_ptr<RandomDelay> m_rd_attack;
};

// ─────────────────────────────────────────────────────────────────────────────
// TargetObjective — замінює HandleTargeting()
// ─────────────────────────────────────────────────────────────────────────────
class TargetObjective : public TimedObjective {
public:
    TargetObjective() : TimedObjective("Target") {}

    bool canRun(const GameState& gs) const override {
        return !gs.is_dead
            && !gs.has_target
            && !gs.buff_needed();
    }

    // ── Virtual overrides для ObjectiveManager dispatch ────────────────────────
    void deliverGeoPath(const std::vector<std::pair<float,float>>& path,
                        uint64_t id) override {
        if (id <= m_geo_path_id) return; // застарілий результат
        m_geo_path       = path;
        m_geo_path_idx   = 0;
        m_geo_path_id    = id;
        m_geo_path_ready = !path.empty();
    }

    std::optional<PathRequest> takePendingPathRequest() override {
        if (!m_pending_path_req.has_value()) return std::nullopt;
        auto req = std::move(m_pending_path_req);
        m_pending_path_req = std::nullopt;
        return req;
    }

    void setAttackWasUnreachable(bool v) override { m_attack_was_unreachable = v; }
    void advanceMacroIdx(int total) override {
        if (total > 0) m_macro_idx = (m_macro_idx + 1) % total;
    }

    void onEnter(GameState& gs) override {
        (void)gs;
        m_macro_attempts       = 0;
        // m_macro_idx НЕ скидаємо — зберігається між циклами
        m_step_count           = 0;
        m_minimap_rotate_count = 0;
        m_far_rejects          = 0;
        m_pokemon_targeted     = false;
        m_pokemon_macro_fired  = false;
        m_dead_esc_count       = 0;
        m_dead_cycles_total    = 0;
        m_walk_stuck_count     = 0;
        m_nav_prev_was_walk    = false;
        m_nav_stuck_recoveries = 0;
        m_patrol_step_idx      = 0;
        m_running_to_mob       = false;
        m_run_started          = TP{};
        m_geo_path_ready       = false;
        m_geo_path_idx         = 0;
        m_pending_path_req     = std::nullopt;
        m_not_ready_count      = 0;
        // m_attack_was_unreachable НЕ скидаємо — зберігається між Enter/Exit
        // Ініціалізація RandomDelay
        if (gs.cfg.delays.enabled) {
            m_rd_rotate = std::make_unique<RandomDelay>(
                gs.cfg.delays.rotate_mean_ms, gs.cfg.delays.rotate_std_ms);
            m_rd_walk = std::make_unique<RandomDelay>(
                gs.cfg.delays.walk_mean_ms, gs.cfg.delays.walk_std_ms);
        } else {
            m_rd_rotate.reset();
            m_rd_walk.reset();
        }
    }

    ObjectiveResult execute(GameState& gs) override {
        if (!gs.hands_ready) {
            m_not_ready_count++;
            if (m_not_ready_count == 20 || m_not_ready_count % 100 == 0) {
                gs.log("[WARNING] TARGETING: IsReady=false вже " +
                    std::to_string(m_not_ready_count) + " тіків — Input thread завис?");
            }
            return ObjectiveResult::running();
        }
        m_not_ready_count = 0;

        // Є ціль — перевіряємо screen-Y, потім атакуємо
        if (gs.has_target) {
            const int kNearbyYThreshold = gs.cfg.nearby_y_threshold;
            const int kMaxFarRejects    = gs.cfg.max_far_rejects;
            if (kNearbyYThreshold > 0 && !gs.cfg.target_macro_keys.empty()
                && m_far_rejects < kMaxFarRejects) {
                auto npcs = gs.eyes.DetectNPCs();
                for (const auto& npc : npcs) {
                    if (npc.Selected() && npc.center.y < kNearbyYThreshold) {
                        gs.log("[TARGETING] Моб далеко (cy=" + std::to_string(npc.center.y)
                            + ", reject=" + std::to_string(m_far_rejects + 1)
                            + "/" + std::to_string(kMaxFarRejects) + ") → ESC");
                        gs.hands.PressKeyboardKey(Input::KeyboardKey::Escape);
                        m_far_rejects++;
                        gs.hands.Send(150);
                        return ObjectiveResult::running();
                    }
                }
            }
            return ObjectiveResult::switchTo("Attack");
        }

        // ── Мертвий таргет (hp=0) ──────────────────────────────────────────────
        if (gs.target.has_value() && gs.target->hp == 0) {
            if (m_pokemon_targeted) {
                gs.log("[Pokemon] sweep (чекаємо анімацію)...");
                gs.hands.Delay(1500);
                gs.hands.PressKeyboardKey(Input::KeyboardKey::Escape);
                m_pokemon_targeted = false;
                m_dead_esc_count = 0;
                gs.hands.Send(100);
                return ObjectiveResult::running();
            }
            m_dead_esc_count++;
            if (m_dead_esc_count == 1) {
                gs.log("[TARGETING] Мертвий таргет hp=0 ×1 → чекаємо підтвердження...");
                gs.hands.Send(250);
                return ObjectiveResult::running();
            }
            gs.log("[TARGETING] Мертвий таргет hp=0 → ESC ×" +
                std::to_string(m_dead_esc_count - 1));
            if (m_dead_esc_count <= 6) {
                gs.hands.PressKeyboardKey(Input::KeyboardKey::Escape);
                gs.hands.Send(100);
                return ObjectiveResult::running();
            }
            // Після 5 спроб — fallthrough до F2/macro
            m_dead_esc_count = 0;
            m_dead_cycles_total++;
            if (m_dead_cycles_total >= gs.cfg.targeting_tuning.dead_cycles_macro_switch
                && !gs.cfg.target_macro_keys.empty()) {
                gs.log("[TARGETING] Dead-target loop ×" +
                    std::to_string(m_dead_cycles_total) + " → /target макрос");
                m_macro_attempts++;
                gs.hands.TargetMacro(m_macro_idx);
                m_macro_idx = (m_macro_idx + 1) % (int)gs.cfg.target_macro_keys.size();
                gs.hands.Send(300);
                return ObjectiveResult::running();
            }
            gs.log("[TARGETING] Мертвий таргет не зникає після 5 ESC → пробуємо F2");
        } else {
            m_dead_esc_count = 0;
            if (gs.target.has_value() && gs.target->hp > 0)
                m_dead_cycles_total = 0;
        }

        m_macro_attempts++;

        // ── Мінімапа: повернутись до найближчого моба перед F2 ────────────────
        const int kMinimapDxThreshold = gs.cfg.targeting_tuning.minimap_dx_threshold;
        const int kMinimapRotateLimit = gs.cfg.targeting_tuning.minimap_rotate_limit;

        const auto& minimap_dots = gs.minimap_dots;

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
                gs.hands.RotateLeft(RandMs(m_rd_rotate.get(), gs, 120));
                m_minimap_rotate_count++;
                gs.log(std::string("[MAP] ") + who + " моб ліворуч (dx=" +
                    std::to_string(dx) + ", rot=" +
                    std::to_string(m_minimap_rotate_count) + ") → RotateLeft");
            } else if (dx > kMinimapDxThreshold) {
                gs.hands.RotateRight(RandMs(m_rd_rotate.get(), gs, 120));
                m_minimap_rotate_count++;
                gs.log(std::string("[MAP] ") + who + " моб праворуч (dx=" +
                    std::to_string(dx) + ", rot=" +
                    std::to_string(m_minimap_rotate_count) + ") → RotateRight");
            } else {
                m_minimap_rotate_count = 0;
                if (dy > 30) {
                    gs.hands.RotateRight(RandMs(m_rd_rotate.get(), gs, 700));
                    gs.log(std::string("[MAP] ") + who + " моб позаду (dy=" +
                        std::to_string(dy) + ") → розворот 180°");
                }
            }
        } else if (map_ref && m_minimap_rotate_count >= kMinimapRotateLimit) {
            m_minimap_rotate_count = 0;
            gs.log("[MAP] Ліміт ротацій → WalkForward до моба (dx=" +
                std::to_string(map_ref->dx) + ")");
            if (gs.eyes.IsGroundAhead()) {
                gs.hands.WalkForward(RandMs(m_rd_walk.get(), gs, 600));
                m_nav_prev_was_walk = true;
            }
        } else if (minimap_dots.empty()) {
            m_minimap_rotate_count = 0;
        }

        // Основний метод: F2 /nexttarget
        gs.hands.NextTarget();

        // Скидаємо unreachable flag якщо мінімапа показала мобів
        if (!minimap_dots.empty() && m_attack_was_unreachable) {
            m_attack_was_unreachable = false;
            gs.log("[TARGETING] Мінімапа: моби знайдені → скидаємо unreachable flag");
        }

        // /target макроси (F7-F11)
        const int  kMacroFallbackAfterUnreach = gs.cfg.targeting_tuning.macro_fallback_unreach;
        const bool macro_at_start = !gs.cfg.target_macro_keys.empty()
                                    && !m_attack_was_unreachable
                                    && m_macro_attempts == 1;
        const bool macro_fallback = !gs.cfg.target_macro_keys.empty()
                                    && m_macro_attempts > (m_attack_was_unreachable
                                        ? kMacroFallbackAfterUnreach : 2)
                                    && m_macro_attempts % 2 == 0;
        if (macro_at_start || macro_fallback) {
            gs.hands.Delay(80);
            gs.hands.TargetMacro(m_macro_idx);
            m_macro_idx = (m_macro_idx + 1) % (int)gs.cfg.target_macro_keys.size();
        }

        // Pokemon макрос: ОДИН РАЗ на початку TARGETING циклу
        if (gs.cfg.has_pokemon_key && !m_pokemon_macro_fired && m_macro_attempts == 1) {
            m_pokemon_macro_fired = true;
            gs.hands.Delay(50);
            gs.hands.PressKeyboardKey(gs.cfg.pokemon_key);
            m_pokemon_targeted = true;
            gs.log("[Pokemon] макрос");
        }

        // ── Navigation: stuck detection ───────────────────────────────────────
        if (gs.cfg.nav_stuck_detection) {
            const bool is_moving = gs.eyes.IsCharacterMoving();

            if (m_nav_prev_was_walk) {
                m_nav_prev_was_walk = false;

                if (m_running_to_mob) {
                    const double run_secs = SecsSince(m_run_started);
                    if (run_secs >= 15.0) {
                        const int rotation_ms = std::min(900 + (m_nav_stuck_recoveries / 2) * 450, 1800);
                        gs.hands.WalkBack(300);
                        if (m_nav_stuck_recoveries % 2 == 0) {
                            gs.hands.RotateRight(rotation_ms);
                            gs.log("[NAV] Біг " + std::to_string((int)run_secs) +
                                "с без таргету → WalkBack + RotateRight(" +
                                std::to_string(rotation_ms) + "мс)");
                        } else {
                            gs.hands.RotateLeft(rotation_ms);
                            gs.log("[NAV] Біг " + std::to_string((int)run_secs) +
                                "с без таргету → WalkBack + RotateLeft(" +
                                std::to_string(rotation_ms) + "мс)");
                        }
                        m_nav_stuck_recoveries++;
                        m_run_started = Now();
                        m_nav_prev_was_walk = true;
                    }
                } else {
                    const float flow    = gs.cfg.nav_flow_detection
                        ? gs.eyes.GetMovementFlow() : -1.0f;
                    const float mm_flow = gs.cfg.nav_flow_detection
                        ? gs.eyes.GetMinimapFlow() : -1.0f;
                    const bool actually_moved = is_moving || (flow > 1.5f) || (mm_flow > 0.3f);
                    if (!actually_moved) {
                        m_walk_stuck_count++;
                        gs.log("[NAV] Не рухаємось після WalkForward ×" +
                            std::to_string(m_walk_stuck_count) +
                            (flow >= 0 ? " flow=" + std::to_string(flow).substr(0,4) : ""));
                        if (m_walk_stuck_count >= gs.cfg.nav_stuck_threshold) {
                            m_walk_stuck_count = 0;
                            const int rotation_ms = std::min(900 + (m_nav_stuck_recoveries / 2) * 450, 1800);
                            gs.hands.WalkBack(300);
                            if (m_nav_stuck_recoveries % 2 == 0) {
                                gs.hands.RotateRight(rotation_ms);
                                gs.log("[NAV] Застряг ×" +
                                    std::to_string(m_nav_stuck_recoveries + 1) +
                                    " → WalkBack + RotateRight(" +
                                    std::to_string(rotation_ms) + "мс)");
                            } else {
                                gs.hands.RotateLeft(rotation_ms);
                                gs.log("[NAV] Застряг ×" +
                                    std::to_string(m_nav_stuck_recoveries + 1) +
                                    " → WalkBack + RotateLeft(" +
                                    std::to_string(rotation_ms) + "мс)");
                            }
                            gs.hands.WalkForward(500);
                            m_nav_prev_was_walk = true;
                            m_nav_stuck_recoveries++;
                        }
                    } else {
                        m_walk_stuck_count = 0;
                        m_nav_stuck_recoveries = 0;
                        if (flow > 0)
                            gs.log("[NAV] Рух ок, flow=" + std::to_string(flow).substr(0,4));
                    }
                }
            }
        }

        // ── Memory навігація (пріоритет над мінімапою якщо увімкнено) ─────────
        if (gs.cfg.navigation.enabled && !gs.kl_mobs.empty()) {
            auto kl_mobs = gs.kl_mobs;
            kl_mobs.erase(std::remove_if(kl_mobs.begin(), kl_mobs.end(),
                [&gs](const L2Character& mob) {
                    return gs.is_blacklisted && gs.is_blacklisted(mob.objectID);
                }), kl_mobs.end());
            if (!kl_mobs.empty()) {
                std::optional<L2Character> nearest;
                if (gs.cfg.weighted_target.enabled && gs.select_target) {
                    nearest = gs.select_target(kl_mobs, gs.player_x, gs.player_y);
                } else if (gs.find_nearest_mob) {
                    nearest = gs.find_nearest_mob(kl_mobs, gs.player_x, gs.player_y,
                                                  gs.cfg.weighted_target.max_range);
                }
                if (nearest.has_value() && gs.navigate_to_mob) {
                    bool navigated = gs.navigate_to_mob(*nearest);
                    if (navigated) {
                        std::string who = nearest->name.empty()
                            ? ("ID=" + std::to_string(nearest->objectID))
                            : nearest->name;
                        gs.log("[NAV-MEM] → " + who +
                            " hp=" + std::to_string((int)nearest->hpPercent()) + "%");
                        gs.hands.Send(150);
                        return ObjectiveResult::running();
                    }
                    // Запит шляху через GeodataWorker
                    if (!m_geo_path_ready && !m_pending_path_req.has_value()
                        && gs.geodata && gs.geodata->IsLoaded(nearest->x, nearest->y)
                        && gs.coords_valid) {
                        PathRequest req;
                        req.sx = gs.player_x; req.sy = gs.player_y; req.sz = gs.player_z;
                        req.ex = nearest->x;  req.ey = nearest->y;  req.ez = nearest->z;
                        req.maxRange = gs.cfg.knownlist_max_range;
                        req.id = ++m_path_req_id;
                        m_pending_path_req = req;
                    }
                }
            }
        }

        // ── Geodata waypoint following ─────────────────────────────────────────
        if (gs.cfg.navigation.enabled && m_geo_path_ready
            && m_geo_path_idx < m_geo_path.size() && gs.coords_valid) {
            auto [wx, wy] = m_geo_path[m_geo_path_idx];
            float dx_wp = wx - gs.player_x;
            float dy_wp = wy - gs.player_y;
            float dist_wp = std::sqrt(dx_wp * dx_wp + dy_wp * dy_wp);
            if (dist_wp < 300.f) {
                m_geo_path_idx++;
                gs.log("[GEO] Waypoint " + std::to_string(m_geo_path_idx) +
                    "/" + std::to_string(m_geo_path.size()) + " досягнуто");
            } else {
                L2Character wp{};
                wp.x = wx; wp.y = wy; wp.z = gs.player_z;
                wp.hp = 100.f; wp.hpMax = 100.f;
                if (gs.navigate_to_mob && gs.navigate_to_mob(wp)) {
                    gs.log("[GEO] → waypoint " + std::to_string(m_geo_path_idx) +
                        " dist=" + std::to_string((int)dist_wp));
                    gs.hands.Send(150);
                    return ObjectiveResult::running();
                }
            }
            if (m_geo_path_idx >= m_geo_path.size()) {
                m_geo_path_ready = false;
                gs.log("[GEO] Шлях пройдено");
            }
        }

        // ── RunTick вимкнено (дрейф від накопиченого кута) ────────────────────
        const bool should_run = false;
        if (should_run) {
            if (!m_running_to_mob) {
                m_running_to_mob = true;
                m_run_started = Now();
                m_walk_stuck_count = 0;
            }
            if (!(gs.cfg.nav_wall_detection && gs.eyes.IsWallAhead())) {
                gs.hands.RunTick(800);
                m_nav_prev_was_walk = true;
            }
        } else if (m_running_to_mob) {
            m_running_to_mob = false;
            gs.log("[TARGETING] Зупиняємо біг — мобів не видно");
        }

        // ── WalkForward якщо моб прямо попереду (dy < -15) ────────────────────
        if (map_ref && std::abs(map_ref->dx) <= kMinimapDxThreshold
            && map_ref->dy < -15 && !m_nav_prev_was_walk && gs.eyes.IsGroundAhead()) {
            gs.hands.WalkForward(RandMs(m_rd_walk.get(), gs, 700));
            m_nav_prev_was_walk = true;
            gs.log("[MAP] Моб прямо попереду (dy=" + std::to_string(map_ref->dy) +
                ") → WalkForward");
        }

        // ── Fallback ротація / Patrol / Розвідка ──────────────────────────────
        const bool minimap_empty = minimap_dots.empty();
        const bool long_search   = (m_macro_attempts >= 20)
                                   && (m_macro_attempts % 10 == 0);
        if (m_macro_attempts % 5 == 0 && (minimap_empty || long_search)) {
            gs.hands.Delay(50);
            m_step_count++;
            gs.stats.RecordTargetingFailure();

            const bool patrol_ready = gs.cfg.patrol_enabled
                && !gs.cfg.patrol_path.empty()
                && m_macro_attempts >= gs.cfg.patrol_trigger_attempts
                && m_macro_attempts % 5 == 0;

            if (patrol_ready) {
                const auto& step = gs.cfg.patrol_path[
                    m_patrol_step_idx % (int)gs.cfg.patrol_path.size()];
                int pat_step_display = m_patrol_step_idx % (int)gs.cfg.patrol_path.size() + 1;
                int pat_total = (int)gs.cfg.patrol_path.size();
                switch (step.dir) {
                    case Config::PatrolStep::Dir::Forward:
                        gs.hands.WalkForward(step.ms);
                        m_nav_prev_was_walk = true;
                        gs.log("[PATROL] Крок " + std::to_string(pat_step_display) +
                            "/" + std::to_string(pat_total) +
                            " → Forward(" + std::to_string(step.ms) + "мс)");
                        break;
                    case Config::PatrolStep::Dir::Back:
                        gs.hands.WalkBack(step.ms);
                        gs.log("[PATROL] Крок " + std::to_string(pat_step_display) +
                            "/" + std::to_string(pat_total) +
                            " → Back(" + std::to_string(step.ms) + "мс)");
                        break;
                    case Config::PatrolStep::Dir::RotateLeft:
                        gs.hands.RotateLeft(step.ms);
                        gs.log("[PATROL] Крок " + std::to_string(pat_step_display) +
                            "/" + std::to_string(pat_total) +
                            " → RotateLeft(" + std::to_string(step.ms) + "мс)");
                        break;
                    case Config::PatrolStep::Dir::RotateRight:
                        gs.hands.RotateRight(step.ms);
                        gs.log("[PATROL] Крок " + std::to_string(pat_step_display) +
                            "/" + std::to_string(pat_total) +
                            " → RotateRight(" + std::to_string(step.ms) + "мс)");
                        break;
                }
                m_patrol_step_idx++;
            } else {
                if ((m_macro_attempts / 5) % 2 == 0)
                    gs.hands.RotateRight(RandMs(m_rd_rotate.get(), gs, 350));
                else
                    gs.hands.RotateLeft(RandMs(m_rd_rotate.get(), gs, 350));
                const bool explore_trigger = (!patrol_ready && !m_nav_prev_was_walk
                    && gs.eyes.IsGroundAhead()
                    && ((minimap_empty && m_macro_attempts % 15 == 0)
                        || (!minimap_empty && m_macro_attempts % 20 == 0)));
                if (explore_trigger) {
                    gs.hands.WalkForward(1200);
                    m_nav_prev_was_walk = true;
                    gs.log("[TARGETING] Спроба " + std::to_string(m_macro_attempts) +
                        " — розвідка вперед (" +
                        (minimap_empty ? "мінімапа порожня" : "dot недосяжний") + ")");
                } else {
                    gs.log("[TARGETING] Спроба " + std::to_string(m_macro_attempts) +
                        " — ротація (мінімапа порожня)");
                }
            }
        } else if (m_macro_attempts % 5 == 0) {
            gs.stats.RecordTargetingFailure();
            gs.log("[TARGETING] Спроба " + std::to_string(m_macro_attempts) + " — шукаємо...");
        }

        // Попередження при довгому пошуку
        const int kWarnAt = gs.cfg.targeting_tuning.long_search_warn_at;
        if (kWarnAt > 0 && m_macro_attempts > kWarnAt && m_macro_attempts % kWarnAt == 0) {
            std::string dots_info = minimap_dots.empty()
                ? "мінімапа порожня"
                : ("dots=" + std::to_string(minimap_dots.size()) +
                   " dx=" + std::to_string(minimap_dots[0].dx));
            std::string kl_info = (gs.kl_alive_count > 0)
                ? " KL_alive=" + std::to_string(gs.kl_alive_count) : "";
            gs.log("[TARGETING] Довгий пошук ×" + std::to_string(m_macro_attempts) +
                " — " + dots_info + kl_info +
                " rot=" + std::to_string(m_minimap_rotate_count));
        }

        gs.hands.Send(150);
        return ObjectiveResult::running();
    }

    // ── Public: публічні поля що зберігаються між циклами ─────────────────────
    bool  m_attack_was_unreachable = false;
    int   m_macro_idx              = 0;  // зберігається між циклами

    // ── Public: geo path (для Brain::SetGeoPath / GetPendingPathRequest) ───────
    std::vector<std::pair<float,float>> m_geo_path;
    size_t   m_geo_path_idx   = 0;
    uint64_t m_geo_path_id    = 0;
    bool     m_geo_path_ready = false;
    std::optional<PathRequest> m_pending_path_req;
    uint64_t m_path_req_id    = 0;

private:
    int   m_macro_attempts       = 0;
    int   m_step_count           = 0;
    int   m_minimap_rotate_count = 0;
    int   m_far_rejects          = 0;
    bool  m_pokemon_targeted     = false;
    bool  m_pokemon_macro_fired  = false;
    int   m_dead_esc_count       = 0;
    int   m_dead_cycles_total    = 0;
    int   m_walk_stuck_count     = 0;
    bool  m_nav_prev_was_walk    = false;
    int   m_nav_stuck_recoveries = 0;
    int   m_patrol_step_idx      = 0;
    bool  m_running_to_mob       = false;
    TP    m_run_started{};
    int   m_not_ready_count      = 0;

    std::unique_ptr<RandomDelay> m_rd_rotate;
    std::unique_ptr<RandomDelay> m_rd_walk;
};

// ─────────────────────────────────────────────────────────────────────────────
// RestObjective — пауза при низькому HP або MP (між боями)
// ─────────────────────────────────────────────────────────────────────────────
class RestObjective : public TimedObjective {
public:
    RestObjective() : TimedObjective("Rest") {}

    bool canRun(const GameState& gs) const override {
        if (!gs.hp_valid || gs.is_dead || gs.in_grace) return false;
        // Активується якщо HP або MP нижче порогу І немає активного бою
        const bool low_hp = gs.hp > 0 && gs.hp < gs.cfg.hp_threshold;
        const bool low_mp = gs.mp > 0 && gs.mp < gs.cfg.mp_threshold;
        return (low_hp || low_mp) && !gs.has_target;
    }

    void onEnter(GameState& gs) override {
        m_rest_start = Now();
        gs.log("[REST] HP=" + std::to_string(gs.hp) +
               "% MP=" + std::to_string(gs.mp) +
               "% — чекаємо відновлення");
    }

    ObjectiveResult execute(GameState& gs) override {
        // Якщо з'явився таргет — переривати відпочинок
        if (gs.has_target)
            return ObjectiveResult::switchTo("Attack");

        // Вийти з відпочинку якщо HP і MP відновились або timeout 30с
        const bool hp_ok = gs.hp >= gs.cfg.hp_threshold || gs.hp == 0;
        const bool mp_ok = gs.mp >= gs.cfg.mp_threshold || gs.mp == 0;
        if ((hp_ok && mp_ok) || SecsSince(m_rest_start) > 30.0) {
            gs.log("[REST] Відновились → Target");
            return ObjectiveResult::switchTo("Target");
        }

        // Стоїмо нерухомо (не натискаємо нічого)
        gs.hands.Send(500);
        return ObjectiveResult::running();
    }

private:
    TP m_rest_start{};
};

// ─────────────────────────────────────────────────────────────────────────────
// ZoneObjective — повернення в зону фарму якщо персонаж вийшов за межі
// Потребує coords_valid (MemReader + відкалібровані XYZ offsets)
// ─────────────────────────────────────────────────────────────────────────────
class ZoneObjective : public TimedObjective {
public:
    // center_x, center_y — центр зони, radius — радіус в L2 units
    ZoneObjective(float center_x, float center_y, float radius)
        : TimedObjective("Zone")
        , m_cx(center_x), m_cy(center_y), m_radius(radius) {}

    bool canRun(const GameState& gs) const override {
        if (!gs.coords_valid) return false; // потрібні координати
        // Активується якщо персонаж вийшов з зони І немає таргету
        const float dx = gs.player_x - m_cx;
        const float dy = gs.player_y - m_cy;
        const float dist = std::sqrt(dx*dx + dy*dy);
        return dist > m_radius && !gs.has_target;
    }

    ObjectiveResult execute(GameState& gs) override {
        if (!gs.coords_valid) return ObjectiveResult::done("немає координат");

        const float dx   = m_cx - gs.player_x;
        const float dy   = m_cy - gs.player_y;
        const float dist = std::sqrt(dx*dx + dy*dy);

        if (dist <= m_radius) {
            gs.log("[ZONE] Повернулись в зону");
            return ObjectiveResult::switchTo("Target");
        }

        // Якщо є Geodata — використати FindPath
        // Інакше — рух до центру через WalkForward
        gs.log("[ZONE] Поза зоною (dist=" + std::to_string((int)dist) +
               ") → повертаємось до (" +
               std::to_string((int)m_cx) + "," + std::to_string((int)m_cy) + ")");

        // Рух до центру (без heading: WalkForward, з heading: rotate+walk)
        if (!gs.cfg.navigation.use_heading) {
            gs.hands.WalkForward(800);
        }
        // TODO: NavigateToPoint(m_cx, m_cy) через heading якщо відкалібровано

        gs.hands.Send(200);
        return ObjectiveResult::running();
    }

private:
    float m_cx, m_cy, m_radius;
};
