#include "BotBehaviorTree.h"
#include "Input.h"
#include "Geodata.h"
#include "navmesh_builder.h"
#include <iostream>
#include <cmath>
#include <algorithm>

thread_local BotBehaviorTree* BotBehaviorTree::s_self = nullptr;

BotBehaviorTree::BotBehaviorTree() {
    m_last_buff      = Clock::now() - std::chrono::hours(1);
    m_last_kill_time = Clock::now() - std::chrono::hours(1);
}

// ── init ──────────────────────────────────────────────────────────────────────
// ВАЖЛИВО: будуємо дерево в BFS-порядку — спочатку всі прямі нащадки root,
// потім нащадки кожної гілки. Це необхідно для коректного childStart/childCount
// у плоскому масиві m_children (lazy-assign у addChild).
void BotBehaviorTree::init(const Config& cfg) {
    m_zone_cx      = cfg.zone_x;
    m_zone_cy      = cfg.zone_y;
    m_zone_r       = cfg.zone_radius;
    m_zone_enabled = cfg.zone_enabled;

    auto& bt = m_bt;

    uint16_t root = bt.addSelector();
    bt.setRoot(root);

    // ── Фаза 1: створюємо всі гілки та листові вузли ─────────────────────────
    constexpr uint16_t NONE = BehaviorTree::INVALID;

    uint16_t seq_dead = bt.addSequence();
    uint16_t cnd_dead = bt.addCondition(&condIsDead);
    uint16_t act_dead = bt.addAction   (&actDead);

    uint16_t seq_rest = NONE, cnd_rest = NONE, act_rest = NONE;
    if (cfg.mp_threshold > 0) {
        seq_rest = bt.addSequence();
        cnd_rest = bt.addCondition(&condNeedsRest);
        act_rest = bt.addAction   (&actRest);
    }

    uint16_t seq_zone = NONE, cnd_zone = NONE, act_zone = NONE;
    if (cfg.zone_enabled) {
        seq_zone = bt.addSequence();
        cnd_zone = bt.addCondition(&condZoneViolated);
        act_zone = bt.addAction   (&actZone);
    }

    uint16_t seq_buff = NONE, cnd_buff = NONE, act_buff_n = NONE;
    if (cfg.buff_enabled) {
        seq_buff  = bt.addSequence();
        cnd_buff  = bt.addCondition(&condNeedsBuff);
        act_buff_n = bt.addAction  (&actBuff);
    }

    uint16_t seq_loot = bt.addSequence();
    uint16_t cnd_loot = bt.addCondition(&condLootPending);
    uint16_t act_loot = bt.addAction   (&actLoot);

    uint16_t seq_atk  = bt.addSequence();
    uint16_t cnd_atk  = bt.addCondition(&condHasTarget);
    uint16_t act_atk  = bt.addAction   (&actAttack);

    uint16_t act_tgt  = bt.addAction   (&actTarget);

    // ── Фаза 2: root отримує прямих нащадків (усі gілки підряд) ─────────────
    bt.addChild(root, seq_dead);
    if (seq_rest != NONE) bt.addChild(root, seq_rest);
    if (seq_zone != NONE) bt.addChild(root, seq_zone);
    if (seq_buff != NONE) bt.addChild(root, seq_buff);
    bt.addChild(root, seq_loot);
    bt.addChild(root, seq_atk);
    bt.addChild(root, act_tgt);   // fallback

    // ── Фаза 3: кожна гілка отримує своїх нащадків ───────────────────────────
    bt.addChild(seq_dead, cnd_dead);
    bt.addChild(seq_dead, act_dead);

    if (seq_rest != NONE) {
        bt.addChild(seq_rest, cnd_rest);
        bt.addChild(seq_rest, act_rest);
    }
    if (seq_zone != NONE) {
        bt.addChild(seq_zone, cnd_zone);
        bt.addChild(seq_zone, act_zone);
    }
    if (seq_buff != NONE) {
        bt.addChild(seq_buff, cnd_buff);
        bt.addChild(seq_buff, act_buff_n);
    }
    bt.addChild(seq_loot, cnd_loot);
    bt.addChild(seq_loot, act_loot);

    bt.addChild(seq_atk, cnd_atk);
    bt.addChild(seq_atk, act_atk);

    std::cerr << "[BT] Ініціалізовано: " << bt.nodeCount() << " вузлів\n";
}

// ── tick ──────────────────────────────────────────────────────────────────────
std::string BotBehaviorTree::tick(GameState& gs) {
    s_self = this;
    uint32_t nowMs = (uint32_t)std::chrono::duration_cast<
        std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
    m_bt.tick(gs, nowMs);
    return m_active_branch;
}

// ── reset ─────────────────────────────────────────────────────────────────────
void BotBehaviorTree::reset() {
    m_bt.reset();
    m_dead_phase   = 0;
    m_buff_stage   = 0;
    m_buff_retries = 0;
    m_buff_tab_fallback   = false;
    m_buff_tab_click_pos  = {0, 0};
    m_loot_pending = false;
    m_loot_issued  = false;
    m_atk_first_attack        = true;
    m_atk_attack_idx          = 0;
    m_atk_no_target_count     = 0;
    m_atk_hp_zero_count       = 0;
    m_atk_last_hp             = -1;
    m_atk_mem_hp_valid        = false;
    m_atk_mem_hp_abs          = -1.f;
    m_atk_low_hp_timer_active = false;
    m_unreachable_flag        = false;
    m_active_branch = "None";
}

// ─────────────────────────────────────────────────────────────────────────────
// CONDITIONS
// ─────────────────────────────────────────────────────────────────────────────

bool BotBehaviorTree::condIsDead(GameState& gs) {
    return gs.is_dead;
}

bool BotBehaviorTree::condNeedsRest(GameState& gs) {
    if (!gs.hp_valid || gs.is_dead || gs.in_grace || gs.has_target) return false;
    const bool low_hp = gs.hp > 0 && gs.hp < gs.cfg.hp_threshold;
    const bool low_mp = gs.mp > 0 && gs.mp < gs.cfg.mp_threshold;
    return low_hp || low_mp;
}

bool BotBehaviorTree::condZoneViolated(GameState& gs) {
    if (!s_self || !s_self->m_zone_enabled || !gs.coords_valid) return false;
    if (gs.has_target) return false;
    float dx = gs.player_x - s_self->m_zone_cx;
    float dy = gs.player_y - s_self->m_zone_cy;
    return (dx*dx + dy*dy) > (s_self->m_zone_r * s_self->m_zone_r);
}

bool BotBehaviorTree::condNeedsBuff(GameState& gs) {
    if (!gs.cfg.buff_enabled || gs.is_dead || gs.in_grace)
        return false;
    if (!s_self) return false;
    // Якщо баф вже у процесі (stage > 0) — продовжуємо незалежно від has_target.
    // actBuff сам закриє ALT+B і скине stage якщо є таргет.
    if (s_self->m_buff_stage > 0) return true;
    // Новий баф: не запускаємо якщо є таргет або cooldown активний
    if (gs.has_target) return false;
    if (secsSince(s_self->m_last_kill_time) < 2.0) return false;
    return gs.buff_needed();
}

bool BotBehaviorTree::condLootPending(GameState& gs) {
    (void)gs;
    return s_self && s_self->m_loot_pending;
}

bool BotBehaviorTree::condHasTarget(GameState& gs) {
    return !gs.is_dead && !gs.in_grace && gs.has_target;
}

// ─────────────────────────────────────────────────────────────────────────────
// ACTIONS
// ─────────────────────────────────────────────────────────────────────────────

BTStatus BotBehaviorTree::actDead(GameState& gs) {
    if (!s_self) return BTStatus::Failure;
    auto& self = *s_self;
    self.m_active_branch = "Dead";

    if (!gs.hands_ready) return BTStatus::Running;

    switch (self.m_dead_phase) {
    case 0:
        gs.log("[DEAD] Фаза 0: Enter");
        gs.hands.PressKeyboardKey(Input::KeyboardKey::Enter);
        gs.hands.Send(5000);
        self.m_dead_phase = 1;
        return BTStatus::Running;

    case 1:
        gs.log("[DEAD] Фаза 1: підтвердження, чекаємо 20с");
        gs.hands.PressKeyboardKey(Input::KeyboardKey::Enter);
        gs.hands.Send(20000);
        self.m_dead_phase = 2;
        return BTStatus::Running;

    case 2:
        gs.log("[DEAD] Фаза 2: відроджено, grace 30с");
        self.m_respawn_until = futureBy(30.0);
        self.m_dead_phase    = 0;
        return BTStatus::Success;

    default:
        self.m_dead_phase = 0;
        return BTStatus::Success;
    }
}

BTStatus BotBehaviorTree::actRest(GameState& gs) {
    if (!s_self) return BTStatus::Failure;
    auto& self = *s_self;
    self.m_active_branch = "Rest";

    // Ініціалізація таймера при першому вході
    if (self.m_rest_start == TP{})
        self.m_rest_start = now();

    if (gs.has_target)
        { self.m_rest_start = TP{}; return BTStatus::Success; }

    const bool hp_ok = !gs.hp_valid || gs.hp == 0 || gs.hp >= gs.cfg.hp_threshold;
    const bool mp_ok = !gs.hp_valid || gs.mp == 0 || gs.mp >= gs.cfg.mp_threshold;

    if ((hp_ok && mp_ok) || secsSince(self.m_rest_start) > 30.0) {
        gs.log("[REST] Відновились");
        self.m_rest_start = TP{};
        return BTStatus::Success;
    }

    gs.hands.Send(500);
    return BTStatus::Running;
}

BTStatus BotBehaviorTree::actZone(GameState& gs) {
    if (!s_self) return BTStatus::Failure;
    auto& self = *s_self;
    self.m_active_branch = "Zone";

    if (!gs.coords_valid) return BTStatus::Success;

    float dx   = self.m_zone_cx - gs.player_x;
    float dy   = self.m_zone_cy - gs.player_y;
    float dist = std::sqrt(dx*dx + dy*dy);

    if (dist <= self.m_zone_r) {
        gs.log("[ZONE] Повернулись в зону");
        return BTStatus::Success;
    }

    gs.log("[ZONE] Поза зоною (dist=" + std::to_string((int)dist) + ")");
    if (gs.navigate_to_mob) {
        L2Character dummy{};
        dummy.x = self.m_zone_cx; dummy.y = self.m_zone_cy;
        dummy.hp = 100.f; dummy.hpMax = 100.f;
        gs.navigate_to_mob(dummy);
    } else {
        gs.hands.WalkForward(800);
    }
    gs.hands.Send(200);
    return BTStatus::Running;
}

BTStatus BotBehaviorTree::actBuff(GameState& gs) {
    if (!s_self) return BTStatus::Failure;
    auto& self = *s_self;
    self.m_active_branch = "Buff";

    if (!gs.hands_ready) return BTStatus::Running;

    // Safety: kill під час бафу
    if (secsSince(self.m_last_kill_time) < 2.0) {
        self.m_last_buff = now() - std::chrono::seconds(gs.cfg.buff_interval - 30);
        gs.log("[Buffs] Kill щойно → скасовуємо баф");
        self.m_buff_stage = 0;
        return BTStatus::Success;
    }

    // HP низький → перериваємо
    if (gs.hp_valid && gs.hp > 0 && gs.hp < gs.cfg.hp_threshold) {
        self.m_last_buff = now() - std::chrono::seconds(gs.cfg.buff_interval - 60);
        gs.log("[Buffs] HP " + std::to_string(gs.hp) + "% → перериваємо бафи!");
        self.m_buff_stage = 0;
        return BTStatus::Success;
    }

    // Є таргет → перериваємо
    if (gs.has_target) {
        if (self.m_buff_stage >= 1) {
            // Закриваємо ALT+B якщо відкритий
            gs.hands.KeyboardKeyDown(Input::KeyboardKey::LeftAlt);
            gs.hands.Delay(50);
            gs.hands.KeyboardKeyDown(Input::KeyboardKey::B);
            gs.hands.Delay(50);
            gs.hands.KeyboardKeyUp(Input::KeyboardKey::B);
            gs.hands.Delay(50);
            gs.hands.KeyboardKeyUp(Input::KeyboardKey::LeftAlt);
            gs.hands.Send();
        }
        self.m_last_buff = now() - std::chrono::seconds(gs.cfg.buff_interval - 10);
        gs.log("[Buffs] Є таргет → перериваємо бафи, retry через 10с");
        self.m_buff_stage = 0;
        return BTStatus::Success;
    }

    // Ініціалізація шаблонів (один раз)
    if (self.m_buff_tab_templ.empty())
        self.m_buff_tab_templ = cv::imread("template/buff_tab.png");
    if (self.m_buff_profile_templ.empty())
        self.m_buff_profile_templ = cv::imread("template/buff_profile.png");

    auto sendAltB = [&]() {
        gs.hands.KeyboardKeyDown(Input::KeyboardKey::LeftAlt);
        gs.hands.Delay(50);
        gs.hands.KeyboardKeyDown(Input::KeyboardKey::B);
        gs.hands.Delay(50);
        gs.hands.KeyboardKeyUp(Input::KeyboardKey::B);
        gs.hands.Delay(50);
        gs.hands.KeyboardKeyUp(Input::KeyboardKey::LeftAlt);
    };

    // Режим buff_keys (без ALT+B)
    if (!gs.cfg.buff_use_altb) {
        if (!gs.cfg.buff_keys.empty()) {
            gs.log("[Buffs] Застосовуємо бафи (" +
                std::to_string(gs.cfg.buff_keys.size()) + ")");
            for (size_t i = 0; i < gs.cfg.buff_keys.size(); i++) {
                gs.hands.PressKeyboardKey(gs.cfg.buff_keys[i]);
                gs.hands.Delay(800);
            }
            gs.hands.PressKeyboardKey(Input::KeyboardKey::Escape);
            gs.hands.Delay(300);
            gs.hands.Send();
        }
        self.m_last_buff = now();
        self.m_buff_stage = 0;
        gs.log("[Buffs] Завершено, наступний баф через " +
            std::to_string(gs.cfg.buff_interval) + "с");
        return BTStatus::Success;
    }

    // ALT+B режим — багатостадійний FSM з template matching
    switch (self.m_buff_stage) {

    case 0: { // Чекаємо скидання бойового стану L2 → ESC + ALT+B
        const double kCombatExpire = 15.0;
        if (secsSince(self.m_last_kill_time) < kCombatExpire) {
            if (self.m_buff_retries % 50 == 0) {
                gs.log("[Buffs] Чекаємо скидання бойового стану ще " +
                    std::to_string((int)(kCombatExpire - secsSince(self.m_last_kill_time))) + "с...");
            }
            ++self.m_buff_retries;
            return BTStatus::Running;
        }
        self.m_buff_retries = 0;
        gs.log("[Buffs] ESC + ALT+B → знімаємо таргет і відкриваємо вікно...");
        gs.hands.PressKeyboardKey(Input::KeyboardKey::Escape);
        gs.hands.Delay(200);
        sendAltB();
        gs.hands.Delay(800);
        gs.hands.Send();
        self.m_buff_stage = 1;
        break;
    }

    case 1: { // Знайти і натиснути вкладку "Баффер"
        float tab_score = 0.0f;
        auto tab_pt = self.m_buff_tab_templ.empty()
            ? std::optional<cv::Point>{}
            : gs.eyes.FindTemplate(self.m_buff_tab_templ, 0.50f, &tab_score);

        if (!tab_pt.has_value() && self.m_buff_retries < 3) {
            self.m_buff_retries++;
            gs.log("[Buffs] Баффер не знайдено (score="
                + std::to_string((int)(tab_score * 100)) + "%) — ALT+B retry "
                + std::to_string(self.m_buff_retries) + "/3");
            if (gs.cfg.debug)  // SaveFrame тільки в debug режимі — повільно (~750ms)
                gs.eyes.SaveFrame("tmp/buff_stage1_retry"
                    + std::to_string(self.m_buff_retries) + ".png");
            sendAltB();
            gs.hands.Delay(1000);
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
            if (gs.cfg.debug) gs.eyes.SaveFrame("tmp/buff_debug_Баффер.png");
            gs.log("[Buffs] Баффер не знайдено (score=" +
                std::to_string((int)(tab_score * 100)) + "%) → fallback (" +
                std::to_string(gs.cfg.buff_tab_x) + "," +
                std::to_string(gs.cfg.buff_tab_y) + ")");
            cx = gs.cfg.buff_tab_x; cy = gs.cfg.buff_tab_y;
            self.m_buff_tab_fallback = true;
        }

        self.m_buff_tab_click_pos = {cx, cy};
        gs.hands.MoveMouseTo({cx, cy});
        gs.hands.Delay(100);
        gs.hands.LeftMouseButtonClick();
        gs.hands.Delay(1500);
        gs.hands.Send();
        self.m_buff_stage = 2;
        break;
    }

    case 2: { // Знайти і натиснути профіль "tty"
        int fb_x = gs.cfg.buff_profile_x;
        int fb_y = gs.cfg.buff_profile_y;
        if (self.m_buff_tab_click_pos.x > 0 && gs.cfg.buff_tab_x > 0) {
            int dx = self.m_buff_tab_click_pos.x - gs.cfg.buff_tab_x;
            int dy = self.m_buff_tab_click_pos.y - gs.cfg.buff_tab_y;
            fb_x = gs.cfg.buff_profile_x + dx;
            fb_y = gs.cfg.buff_profile_y + dy;
        }
        float prof_score = 0.f;
        auto prof_pt = self.m_buff_profile_templ.empty()
            ? std::optional<cv::Point>{}
            : gs.eyes.FindTemplate(self.m_buff_profile_templ, 0.60f, &prof_score);

        int cx, cy;
        if (prof_pt.has_value()) {
            gs.log("[Buffs] tty знайдено (score=" +
                std::to_string((int)(prof_score * 100)) + "%): " +
                std::to_string(prof_pt->x) + "," + std::to_string(prof_pt->y));
            cx = prof_pt->x; cy = prof_pt->y;
        } else {
            if (gs.cfg.debug) gs.eyes.SaveFrame("tmp/buff_debug_tty.png");
            gs.log("[Buffs] tty не знайдено (score=" +
                std::to_string((int)(prof_score * 100)) + "%) → fallback (" +
                std::to_string(fb_x) + "," + std::to_string(fb_y) + ")");
            cx = fb_x; cy = fb_y;
        }
        gs.hands.MoveMouseTo({cx, cy});
        gs.hands.Delay(100);
        gs.hands.LeftMouseButtonClick();
        gs.hands.Delay(500);
        gs.hands.Send();
        self.m_buff_stage = 3;
        break;
    }

    case 3: // Закрити ALT+B
        sendAltB();
        gs.hands.Delay(200);
        gs.hands.Send();
        self.m_buff_stage = 4;
        break;

    case 4: // Готово
    default:
        self.m_buff_stage = 0;
        self.m_buff_retries = 0;
        if (self.m_buff_tab_fallback) {
            self.m_last_buff = now() - std::chrono::seconds(gs.cfg.buff_interval - 120);
            gs.log("[Buffs] Завершено (fallback), retry через 120с");
        } else {
            self.m_last_buff = now();
            gs.log("[Buffs] Завершено, наступний баф через " +
                std::to_string(gs.cfg.buff_interval) + "с");
        }
        self.m_buff_tab_fallback  = false;
        self.m_buff_tab_click_pos = {0, 0};
        gs.hands.PressKeyboardKey(Input::KeyboardKey::Escape);
        gs.hands.Delay(300);
        gs.hands.Send();
        return BTStatus::Success;
    }

    return BTStatus::Running;
}

BTStatus BotBehaviorTree::actLoot(GameState& gs) {
    if (!s_self) return BTStatus::Failure;
    auto& self = *s_self;
    self.m_active_branch = "Loot";

    if (!gs.hands_ready) return BTStatus::Running;

    if (!self.m_loot_issued) {
        // RecordKill
        gs.stats.RecordKill();
        gs.eyes.ResetTarget();
        if (gs.cfg.auto_save_kills > 0 &&
            gs.stats.kills % gs.cfg.auto_save_kills == 0)
            gs.stats.SaveToFile();
        gs.log("[LOOTING] Вбивство #" + std::to_string(gs.stats.kills));

        if (gs.cfg.loot_enabled) {
            gs.hands.PressKeyboardKey(Input::KeyboardKey::Escape);
            gs.hands.Delay(300);
            gs.hands.Send();
        }
        self.m_loot_issued = true;
        return BTStatus::Running;
    }

    // Лут завершено
    self.m_loot_pending = false;
    self.m_loot_issued  = false;
    self.m_tgt_active   = false; // сигнал actTarget: новий цикл таргетингу
    return BTStatus::Success;
}

BTStatus BotBehaviorTree::actAttack(GameState& gs) {
    if (!s_self) return BTStatus::Failure;
    auto& self = *s_self;
    self.m_active_branch = "Attack";

    if (!gs.hands_ready) return BTStatus::Running;

    // Screen-Y filter: перевіряємо першу атаку (таргет щойно знайдено)
    if (self.m_atk_first_attack) {
        const int kNearbyYThreshold = gs.cfg.nearby_y_threshold;
        const int kMaxFarRejects    = gs.cfg.max_far_rejects;
        if (kNearbyYThreshold > 0 && !gs.cfg.target_macro_keys.empty()
            && self.m_tgt_far_rejects < kMaxFarRejects) {
            auto npcs = gs.eyes.DetectNPCs();
            for (const auto& npc : npcs) {
                if (npc.Selected() && npc.center.y < kNearbyYThreshold) {
                    gs.log("[TARGETING] Моб далеко (cy=" + std::to_string(npc.center.y)
                        + ", reject=" + std::to_string(self.m_tgt_far_rejects + 1)
                        + "/" + std::to_string(kMaxFarRejects) + ") → ESC");
                    gs.hands.PressKeyboardKey(Input::KeyboardKey::Escape);
                    self.m_tgt_far_rejects++;
                    gs.hands.Send(150);
                    return BTStatus::Running; // чекаємо наступний тік (has_target=false)
                }
            }
        }
        self.m_tgt_far_rejects = 0; // прийняли таргет
    }

    // Ініціалізація watchdog при першій атаці
    if (self.m_atk_first_attack && self.m_atk_watchdog_start == TP{}) {
        self.m_atk_watchdog_start = now();
    }

    // Re-detect target bar кожні 200мс
    if (secsSince(self.m_atk_last_redetect) >= 0.2) {
        gs.eyes.ResetTarget();
        const_cast<GameState&>(gs).target = gs.eyes.DetectTarget();
        const_cast<GameState&>(gs).has_target =
            gs.target.has_value() && gs.target->hp > 0;
        self.m_atk_last_redetect = now();
    }

    // KnownList kill detection + HP з пам'яті
    if (!gs.kl_mobs.empty() && !self.m_atk_first_attack) {
        if (gs.kl_mob_died) {
            gs.log("[ATTACKING] [KnownList] Таргет мертвий → Loot");
            self.notifyKill();
            self.resetAttackState(gs);
            return BTStatus::Success; // → Selector спробує Loot
        }

        // HP моба з пам'яті (якщо увімкнено) — моб з мінімальним HP (той що атакується)
        if (gs.cfg.mem_use_for_target_hp && gs.target.has_value()) {
            float min_hp_abs = 1e9f;
            float min_pct    = 101.f;
            bool  has_pct    = false;
            for (const auto& mob : gs.kl_mobs) {
                if (!mob.isAlive()) continue;
                if (mob.hpMax > 0.f) {
                    float p = mob.hpPercent();
                    if (p < min_pct) min_pct = p;
                    has_pct = true;
                } else if (mob.hpAbs() < min_hp_abs) {
                    min_hp_abs = mob.hpAbs();
                }
            }
            if (has_pct && min_pct <= 100.f) {
                self.m_atk_mem_hp_valid = true;
                const_cast<GameState&>(gs).target->hp = (int)min_pct;
                const_cast<GameState&>(gs).has_target = (min_pct > 2.f);
            } else if (!has_pct && min_hp_abs < 1e8f) {
                self.m_atk_mem_hp_valid = true;
                self.m_atk_mem_hp_abs   = min_hp_abs;
            }
        }
    }

    // Kill detection: HP ≤ 2% — 3 тіки debounce
    if (gs.target.has_value() && gs.target->hp <= 2) {
        self.m_atk_hp_zero_count++;
        if (self.m_atk_hp_zero_count >= 3) {
            if (self.m_atk_first_attack) {
                gs.log("[ATTACKING] Kill(hp=0%) вже мертвий до першої атаки → Target");
                self.resetAttackState(gs);
                return BTStatus::Failure; // → Selector → Target
            }
            gs.log("[ATTACKING] Kill(hp≤2%) → Loot");
            self.notifyKill();
            self.resetAttackState(gs);
            return BTStatus::Success; // → Selector → Loot
        }
    } else {
        self.m_atk_hp_zero_count = 0;
    }

    // Таргет зник: 8 тіків debounce
    if (!gs.has_target) {
        self.m_atk_no_target_count++;
        if (self.m_atk_no_target_count == 1) {
            std::string hp_str = gs.target.has_value()
                ? std::to_string(gs.target->hp) + "%" : "?";
            gs.log("[ATTACKING] Таргет зник (hp=" + hp_str + ", no_target ×1..8)");
        }
        if (self.m_atk_no_target_count >= 8) {
            if (self.m_atk_first_attack) {
                gs.log("[ATTACKING] NoTarget ×8 (first_attack) → Target");
                self.resetAttackState(gs);
                return BTStatus::Failure;
            }
            gs.log("[ATTACKING] NoTarget ×8 → Loot");
            self.notifyKill();
            self.resetAttackState(gs);
            return BTStatus::Success;
        }
        gs.hands.Send(100);
        return BTStatus::Running;
    }
    self.m_atk_no_target_count = 0;

    // HP-stable: якщо HP не змінився 5с після першої атаки — моб недосяжний
    if (!self.m_atk_first_attack && gs.target.has_value() && gs.target->hp > 0) {
        int hp_for_stable = (self.m_atk_mem_hp_valid && self.m_atk_mem_hp_abs >= 0.f)
                            ? (int)self.m_atk_mem_hp_abs
                            : gs.target->hp;
        if (hp_for_stable != self.m_atk_last_hp) {
            self.m_atk_last_hp        = hp_for_stable;
            self.m_atk_hp_stable_since = now();
        } else if (secsSince(self.m_atk_hp_stable_since) > 5.0) {
            gs.log("[ATTACKING] HP стабільний 5с (моб недосяжний) → Target [unreachable]");
            self.blacklistCurrentTarget(gs);
            gs.hands.PressKeyboardKey(Input::KeyboardKey::Escape);
            gs.hands.Send(200);
            self.m_unreachable_flag = true;
            self.resetAttackState(gs);
            return BTStatus::Failure; // → Selector → Target
        }
    }

    // Watchdog
    if (secsSince(self.m_atk_watchdog_start) > gs.cfg.attack_watchdog) {
        gs.log("[ATTACKING] Watchdog: таймаут → Loot");
        self.notifyKill();
        self.resetAttackState(gs);
        return BTStatus::Success;
    }

    // Атака з кулдауном
    double eff_delay = (gs.cfg.delays.enabled && self.m_atk_rd)
        ? (double)self.m_atk_rd->Get() / 1000.0
        : gs.cfg.GetAttackDelay(self.m_atk_attack_idx);

    if (secsSince(self.m_atk_last_attack) >= eff_delay) {
        if (self.m_atk_first_attack && gs.cfg.IsSpoiler()) {
            gs.hands.PressKeyboardKey(gs.cfg.spoil_key);
            gs.hands.Delay(200);
        }
        gs.hands.AttackSkill(self.m_atk_attack_idx++);
        gs.hands.Send(50);
        self.m_atk_last_attack  = now();
        self.m_atk_first_attack = false;
        gs.stats.RecordAttack();
    }

    return BTStatus::Running;
}

BTStatus BotBehaviorTree::actTarget(GameState& gs) {
    if (!s_self) return BTStatus::Failure;
    auto& self = *s_self;
    self.m_active_branch = "Target";

    if (!gs.hands_ready) {
        self.m_tgt_not_ready_count++;
        if (self.m_tgt_not_ready_count == 20 || self.m_tgt_not_ready_count % 100 == 0) {
            gs.log("[WARNING] TARGETING: IsReady=false вже " +
                std::to_string(self.m_tgt_not_ready_count) + " тіків — Input thread завис?");
        }
        return BTStatus::Running;
    }
    self.m_tgt_not_ready_count = 0;

    // ── Ініціалізація при вході (аналог onEnter) ──────────────────────────────
    if (!self.m_tgt_active) {
        self.resetTargetState(gs);
        self.m_tgt_active = true;
    }

    // ── Breadcrumbs: запис позиції ─────────────────────────────────────────────
    if (gs.coords_valid)
        self.addCrumb(gs.player_x, gs.player_y, gs.player_z, gs.cfg.breadcrumbs);

    // ── Мертвий таргет (hp=0) ──────────────────────────────────────────────────
    // (has_target=false але target.hp==0 означає мертва ціль)
    if (gs.target.has_value() && gs.target->hp == 0) {
        if (self.m_tgt_pokemon_targeted) {
            gs.log("[Pokemon] sweep (чекаємо анімацію)...");
            gs.hands.Delay(1500);
            gs.hands.PressKeyboardKey(Input::KeyboardKey::Escape);
            self.m_tgt_pokemon_targeted = false;
            self.m_tgt_dead_esc_count = 0;
            gs.hands.Send(100);
            return BTStatus::Running;
        }
        self.m_tgt_dead_esc_count++;
        if (self.m_tgt_dead_esc_count == 1) {
            gs.log("[TARGETING] Мертвий таргет hp=0 ×1 → чекаємо підтвердження...");
            gs.hands.Send(250);
            return BTStatus::Running;
        }
        gs.log("[TARGETING] Мертвий таргет hp=0 → ESC ×" +
            std::to_string(self.m_tgt_dead_esc_count - 1));
        if (self.m_tgt_dead_esc_count <= 6) {
            gs.hands.PressKeyboardKey(Input::KeyboardKey::Escape);
            gs.hands.Send(100);
            return BTStatus::Running;
        }
        // Після 5 спроб — fallthrough до F2/macro
        self.m_tgt_dead_esc_count = 0;
        self.m_tgt_dead_cycles_total++;
        if (self.m_tgt_dead_cycles_total >= gs.cfg.targeting_tuning.dead_cycles_macro_switch
            && !gs.cfg.target_macro_keys.empty()) {
            gs.log("[TARGETING] Dead-target loop ×" +
                std::to_string(self.m_tgt_dead_cycles_total) + " → /target макрос");
            self.m_tgt_macro_attempts++;
            gs.hands.TargetMacro(self.m_tgt_macro_idx);
            self.m_tgt_macro_idx = (self.m_tgt_macro_idx + 1) %
                                   (int)gs.cfg.target_macro_keys.size();
            gs.hands.Send(300);
            return BTStatus::Running;
        }
        gs.log("[TARGETING] Мертвий таргет не зникає після 5 ESC → пробуємо F2");
    } else {
        self.m_tgt_dead_esc_count = 0;
        if (gs.target.has_value() && gs.target->hp > 0)
            self.m_tgt_dead_cycles_total = 0;
    }

    self.m_tgt_macro_attempts++;

    // ── Мінімапа: повернутись до найближчого моба перед F2 ────────────────────
    const int kMinimapDxThreshold = gs.cfg.targeting_tuning.minimap_dx_threshold;
    const int kMinimapRotateLimit = gs.cfg.targeting_tuning.minimap_rotate_limit;

    const auto& minimap_dots = gs.minimap_dots;

    const Eyes::MinimapDot* map_ref = nullptr;
    bool map_ref_selected = false;
    for (const auto& d : minimap_dots) {
        if (d.selected) { map_ref = &d; map_ref_selected = true; break; }
    }
    if (!map_ref && !minimap_dots.empty()) map_ref = &minimap_dots[0];

    if (map_ref && self.m_tgt_minimap_rotate_count < kMinimapRotateLimit) {
        const int dx = map_ref->dx;
        const int dy = map_ref->dy;
        const char* who = map_ref_selected ? "Вибраний" : "Найближчий";
        if (dx < -kMinimapDxThreshold) {
            gs.hands.RotateLeft(RandMs(self.m_tgt_rd_rotate.get(), gs, 120));
            self.m_tgt_minimap_rotate_count++;
            gs.log(std::string("[MAP] ") + who + " моб ліворуч (dx=" +
                std::to_string(dx) + ", rot=" +
                std::to_string(self.m_tgt_minimap_rotate_count) + ") → RotateLeft");
        } else if (dx > kMinimapDxThreshold) {
            gs.hands.RotateRight(RandMs(self.m_tgt_rd_rotate.get(), gs, 120));
            self.m_tgt_minimap_rotate_count++;
            gs.log(std::string("[MAP] ") + who + " моб праворуч (dx=" +
                std::to_string(dx) + ", rot=" +
                std::to_string(self.m_tgt_minimap_rotate_count) + ") → RotateRight");
        } else {
            self.m_tgt_minimap_rotate_count = 0;
            if (dy > 30) {
                gs.hands.RotateRight(RandMs(self.m_tgt_rd_rotate.get(), gs, 700));
                gs.log(std::string("[MAP] ") + who + " моб позаду (dy=" +
                    std::to_string(dy) + ") → розворот 180°");
            }
        }
    } else if (map_ref && self.m_tgt_minimap_rotate_count >= kMinimapRotateLimit) {
        self.m_tgt_minimap_rotate_count = 0;
        gs.log("[MAP] Ліміт ротацій → WalkForward до моба (dx=" +
            std::to_string(map_ref->dx) + ")");
        if (gs.eyes.IsGroundAhead()) {
            gs.hands.WalkForward(RandMs(self.m_tgt_rd_walk.get(), gs, 600));
            self.m_tgt_nav_prev_was_walk = true;
        }
    } else if (minimap_dots.empty()) {
        self.m_tgt_minimap_rotate_count = 0;
    }

    // Основний метод: F2 /nexttarget
    gs.hands.NextTarget();

    // Скидаємо unreachable flag якщо мінімапа показала мобів
    if (!minimap_dots.empty() && self.m_attack_was_unreachable) {
        self.m_attack_was_unreachable = false;
        gs.log("[TARGETING] Мінімапа: моби знайдені → скидаємо unreachable flag");
    }

    // /target макроси: тільки після N невдалих F2
    const int  kMacroFallbackAfterUnreach = gs.cfg.targeting_tuning.macro_fallback_unreach;
    const int  kMacroFallbackAfter = gs.cfg.targeting_tuning.macro_fallback_after > 0
                                     ? gs.cfg.targeting_tuning.macro_fallback_after : 10;
    const bool macro_fallback = !gs.cfg.target_macro_keys.empty()
                                && self.m_tgt_macro_attempts > (self.m_attack_was_unreachable
                                    ? kMacroFallbackAfterUnreach : kMacroFallbackAfter)
                                && self.m_tgt_macro_attempts % 2 == 0;
    if (macro_fallback) {
        gs.hands.Delay(80);
        gs.hands.TargetMacro(self.m_tgt_macro_idx);
        self.m_tgt_macro_idx = (self.m_tgt_macro_idx + 1) %
                               (int)gs.cfg.target_macro_keys.size();
    }

    // Pokemon макрос: ОДИН РАЗ на початку TARGETING циклу
    if (gs.cfg.has_pokemon_key && !self.m_tgt_pokemon_fired && self.m_tgt_macro_attempts == 1) {
        self.m_tgt_pokemon_fired = true;
        gs.hands.Delay(50);
        gs.hands.PressKeyboardKey(gs.cfg.pokemon_key);
        self.m_tgt_pokemon_targeted = true;
        gs.log("[Pokemon] макрос");
    }

    // ── Navigation stuck detection ─────────────────────────────────────────────
    if (gs.cfg.nav_stuck_detection) {
        const bool is_moving = gs.eyes.IsCharacterMoving();

        if (self.m_tgt_nav_prev_was_walk) {
            self.m_tgt_nav_prev_was_walk = false;

            if (self.m_tgt_running_to_mob) {
                const double run_secs = secsSince(self.m_tgt_run_started);
                if (run_secs >= 15.0) {
                    const int rotation_ms = std::min(900 + (self.m_tgt_nav_stuck_recoveries / 2) * 450, 1800);
                    gs.hands.WalkBack(300);
                    if (self.m_tgt_nav_stuck_recoveries % 2 == 0) {
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
                    self.m_tgt_nav_stuck_recoveries++;
                    self.m_tgt_run_started = now();
                    self.m_tgt_nav_prev_was_walk = true;
                }
            } else {
                const float flow    = gs.cfg.nav_flow_detection
                    ? gs.eyes.GetMovementFlow() : -1.0f;
                const float mm_flow = gs.cfg.nav_flow_detection
                    ? gs.eyes.GetMinimapFlow() : -1.0f;
                const bool actually_moved = is_moving || (flow > 1.5f) || (mm_flow > 0.3f);
                if (!actually_moved) {
                    self.m_tgt_walk_stuck_count++;
                    gs.log("[NAV] Не рухаємось після WalkForward ×" +
                        std::to_string(self.m_tgt_walk_stuck_count) +
                        (flow >= 0 ? " flow=" + std::to_string(flow).substr(0,4) : ""));
                    if (self.m_tgt_walk_stuck_count >= gs.cfg.nav_stuck_threshold) {
                        self.m_tgt_walk_stuck_count = 0;
                        const int rotation_ms = std::min(900 + (self.m_tgt_nav_stuck_recoveries / 2) * 450, 1800);
                        gs.hands.WalkBack(300);
                        if (self.m_tgt_nav_stuck_recoveries % 2 == 0) {
                            gs.hands.RotateRight(rotation_ms);
                            gs.log("[NAV] Застряг ×" +
                                std::to_string(self.m_tgt_nav_stuck_recoveries + 1) +
                                " → WalkBack + RotateRight(" +
                                std::to_string(rotation_ms) + "мс)");
                        } else {
                            gs.hands.RotateLeft(rotation_ms);
                            gs.log("[NAV] Застряг ×" +
                                std::to_string(self.m_tgt_nav_stuck_recoveries + 1) +
                                " → WalkBack + RotateLeft(" +
                                std::to_string(rotation_ms) + "мс)");
                        }
                        gs.hands.WalkForward(500);
                        self.m_tgt_nav_prev_was_walk = true;
                        self.m_tgt_nav_stuck_recoveries++;

                        // Breadcrumbs: ініціація backtrack
                        if (gs.cfg.breadcrumbs.enabled && gs.coords_valid
                            && !self.m_backtracking
                            && self.m_tgt_nav_stuck_recoveries >= gs.cfg.breadcrumbs.stuck_threshold) {
                            auto bc = self.findBacktrackCrumb(gs.player_x, gs.player_y,
                                                               gs.cfg.breadcrumbs.backtrack_range);
                            if (bc.has_value()) {
                                self.m_backtracking = true;
                                gs.log("[BREADCRUMB] Застряг ×" +
                                    std::to_string(self.m_tgt_nav_stuck_recoveries) +
                                    " → backtrack до (" +
                                    std::to_string((int)bc->x) + "," +
                                    std::to_string((int)bc->y) + ")");
                            }
                        }
                    }
                } else {
                    self.m_tgt_walk_stuck_count = 0;
                    self.m_tgt_nav_stuck_recoveries = 0;
                    if (flow > 0)
                        gs.log("[NAV] Рух ок, flow=" + std::to_string(flow).substr(0,4));
                }
            }
        }
    }

    // ── Breadcrumbs: виконання повернення ─────────────────────────────────────
    if (self.m_backtracking && gs.coords_valid && gs.navigate_to_mob) {
        auto bc = self.findBacktrackCrumb(gs.player_x, gs.player_y,
                                          gs.cfg.breadcrumbs.backtrack_range);
        if (bc.has_value()) {
            L2Character dummy{};
            dummy.x = bc->x; dummy.y = bc->y; dummy.z = bc->z;
            dummy.hp = 100.f; dummy.hpMax = 100.f;
            if (!gs.navigate_to_mob(dummy)) {
                self.m_backtracking = false;
                self.m_tgt_nav_stuck_recoveries = 0;
                gs.log("[BREADCRUMB] Точку досягнуто → відновлюємо пошук");
            } else {
                gs.hands.Send(200);
                return BTStatus::Running;
            }
        } else {
            self.m_backtracking = false;
            gs.log("[BREADCRUMB] Крихти вичерпано");
        }
    }

    // ── Memory навігація (пріоритет над мінімапою якщо увімкнено) ─────────────
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
                    return BTStatus::Running;
                }
                // Запит шляху через GeodataWorker
                if (!self.m_tgt_geo_path_ready && !self.m_tgt_pending_path.has_value()
                    && gs.geodata && gs.geodata->IsLoaded(nearest->x, nearest->y)
                    && gs.coords_valid) {
                    PathRequest req;
                    req.sx = gs.player_x; req.sy = gs.player_y; req.sz = gs.player_z;
                    req.ex = nearest->x;  req.ey = nearest->y;  req.ez = nearest->z;
                    req.maxRange = gs.cfg.knownlist_max_range;
                    req.id = ++self.m_tgt_path_req_id;
                    self.m_tgt_pending_path = req;
                }
            }
        }
    }

    // ── NavMesh FindPath ───────────────────────────────────────────────────────
    if (gs.navmesh && gs.navmesh->IsValid()
        && gs.coords_valid && !gs.kl_mobs.empty()
        && !self.m_tgt_geo_path_ready) {
        const L2Character* tgt = nullptr;
        float best_d2 = 1e12f;
        for (const auto& mob : gs.kl_mobs) {
            if (mob.isDead || !mob.isAlive()) continue;
            float dx = mob.x - gs.player_x, dy = mob.y - gs.player_y;
            float d2 = dx*dx + dy*dy;
            if (d2 < best_d2) { best_d2 = d2; tgt = &mob; }
        }
        if (tgt) {
            auto nm_path = gs.navmesh->FindPath(
                gs.player_x, gs.player_y, gs.player_z,
                tgt->x,      tgt->y,      tgt->z);
            if (!nm_path.empty()) {
                self.m_tgt_geo_path       = nm_path;
                self.m_tgt_geo_path_idx   = 0;
                self.m_tgt_geo_path_id    = ++self.m_tgt_path_req_id;
                self.m_tgt_geo_path_ready = true;
                gs.log("[NAVMESH] Шлях: " + std::to_string(nm_path.size()) + " точок");
            }
        }
    }

    // ── Geodata waypoint following ─────────────────────────────────────────────
    if (gs.cfg.navigation.enabled && self.m_tgt_geo_path_ready
        && self.m_tgt_geo_path_idx < self.m_tgt_geo_path.size() && gs.coords_valid) {
        auto [wx, wy] = self.m_tgt_geo_path[self.m_tgt_geo_path_idx];
        float dx_wp = wx - gs.player_x;
        float dy_wp = wy - gs.player_y;
        float dist_wp = std::sqrt(dx_wp * dx_wp + dy_wp * dy_wp);
        if (dist_wp < 300.f) {
            self.m_tgt_geo_path_idx++;
            gs.log("[GEO] Waypoint " + std::to_string(self.m_tgt_geo_path_idx) +
                "/" + std::to_string(self.m_tgt_geo_path.size()) + " досягнуто");
        } else {
            L2Character wp{};
            wp.x = wx; wp.y = wy; wp.z = gs.player_z;
            wp.hp = 100.f; wp.hpMax = 100.f;
            if (gs.navigate_to_mob && gs.navigate_to_mob(wp)) {
                gs.log("[GEO] → waypoint " + std::to_string(self.m_tgt_geo_path_idx) +
                    " dist=" + std::to_string((int)dist_wp));
                gs.hands.Send(150);
                return BTStatus::Running;
            }
        }
        if (self.m_tgt_geo_path_idx >= self.m_tgt_geo_path.size()) {
            self.m_tgt_geo_path_ready = false;
            gs.log("[GEO] Шлях пройдено");
        }
    }

    // ── WalkForward якщо моб прямо попереду (dy < -15) ─────────────────────────
    if (map_ref && std::abs(map_ref->dx) <= kMinimapDxThreshold
        && map_ref->dy < -15 && !self.m_tgt_nav_prev_was_walk && gs.eyes.IsGroundAhead()) {
        gs.hands.WalkForward(RandMs(self.m_tgt_rd_walk.get(), gs, 700));
        self.m_tgt_nav_prev_was_walk = true;
        gs.log("[MAP] Моб прямо попереду (dy=" + std::to_string(map_ref->dy) +
            ") → WalkForward");
    }

    // ── Fallback ротація / Patrol / Розвідка ──────────────────────────────────
    const bool minimap_empty = minimap_dots.empty();
    const bool long_search   = (self.m_tgt_macro_attempts >= 20)
                               && (self.m_tgt_macro_attempts % 10 == 0);
    if (self.m_tgt_macro_attempts % 5 == 0 && (minimap_empty || long_search)) {
        gs.hands.Delay(50);
        self.m_tgt_step_count++;
        gs.stats.RecordTargetingFailure();

        const bool patrol_ready = gs.cfg.patrol_enabled
            && !gs.cfg.patrol_path.empty()
            && self.m_tgt_macro_attempts >= gs.cfg.patrol_trigger_attempts
            && self.m_tgt_macro_attempts % 5 == 0;

        if (patrol_ready) {
            const auto& step = gs.cfg.patrol_path[
                self.m_tgt_patrol_step_idx % (int)gs.cfg.patrol_path.size()];
            int pat_step_display = self.m_tgt_patrol_step_idx % (int)gs.cfg.patrol_path.size() + 1;
            int pat_total = (int)gs.cfg.patrol_path.size();
            switch (step.dir) {
                case Config::PatrolStep::Dir::Forward:
                    gs.hands.WalkForward(step.ms);
                    self.m_tgt_nav_prev_was_walk = true;
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
            self.m_tgt_patrol_step_idx++;
        } else {
            if ((self.m_tgt_macro_attempts / 5) % 2 == 0)
                gs.hands.RotateRight(RandMs(self.m_tgt_rd_rotate.get(), gs, 350));
            else
                gs.hands.RotateLeft(RandMs(self.m_tgt_rd_rotate.get(), gs, 350));
            const bool explore_trigger = (!patrol_ready && !self.m_tgt_nav_prev_was_walk
                && gs.eyes.IsGroundAhead()
                && ((minimap_empty && self.m_tgt_macro_attempts % 15 == 0)
                    || (!minimap_empty && self.m_tgt_macro_attempts % 20 == 0)));
            if (explore_trigger) {
                gs.hands.WalkForward(1200);
                self.m_tgt_nav_prev_was_walk = true;
                gs.log("[TARGETING] Спроба " + std::to_string(self.m_tgt_macro_attempts) +
                    " — розвідка вперед (" +
                    (minimap_empty ? "мінімапа порожня" : "dot недосяжний") + ")");
            } else {
                gs.log("[TARGETING] Спроба " + std::to_string(self.m_tgt_macro_attempts) +
                    " — ротація (мінімапа порожня)");
            }
        }
    } else if (self.m_tgt_macro_attempts % 5 == 0) {
        gs.stats.RecordTargetingFailure();
        gs.log("[TARGETING] Спроба " + std::to_string(self.m_tgt_macro_attempts) + " — шукаємо...");
    }

    // Попередження при довгому пошуку
    const int kWarnAt = gs.cfg.targeting_tuning.long_search_warn_at;
    if (kWarnAt > 0 && self.m_tgt_macro_attempts > kWarnAt && self.m_tgt_macro_attempts % kWarnAt == 0) {
        std::string dots_info = minimap_dots.empty()
            ? "мінімапа порожня"
            : ("dots=" + std::to_string(minimap_dots.size()) +
               " dx=" + std::to_string(minimap_dots[0].dx));
        std::string kl_info = (gs.kl_alive_count > 0)
            ? " KL_alive=" + std::to_string(gs.kl_alive_count) : "";
        gs.log("[TARGETING] Довгий пошук ×" + std::to_string(self.m_tgt_macro_attempts) +
            " — " + dots_info + kl_info +
            " rot=" + std::to_string(self.m_tgt_minimap_rotate_count));
    }

    gs.hands.Send(150);
    return BTStatus::Running;
}

// ── helpers ───────────────────────────────────────────────────────────────────

void BotBehaviorTree::resetAttackState(GameState& gs) {
    m_atk_first_attack        = true;
    m_atk_attack_idx          = 0;
    m_atk_no_target_count     = 0;
    m_atk_hp_zero_count       = 0;
    m_atk_last_hp             = -1;
    m_atk_mem_hp_valid        = false;
    m_atk_mem_hp_abs          = -1.f;
    m_atk_low_hp_timer_active = false;
    m_atk_watchdog_start      = TP{};  // скидаємо, щоб ініціалізувалось в actAttack
    m_atk_last_attack         = now() - std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(gs.cfg.attack_wait + 0.1));
    m_atk_last_redetect       = now();
    m_tgt_active              = false; // сигнал actTarget: новий цикл таргетингу
    if (gs.cfg.delays.enabled)
        m_atk_rd = std::make_unique<RandomDelay>(
            gs.cfg.delays.attack_mean_ms, gs.cfg.delays.attack_std_ms);
    else
        m_atk_rd.reset();
}

void BotBehaviorTree::resetTargetState(GameState& gs) {
    m_tgt_macro_attempts       = 0;
    m_tgt_step_count           = 0;
    // m_tgt_macro_idx НЕ скидаємо — зберігається між циклами
    m_tgt_minimap_rotate_count = 0;
    m_tgt_far_rejects          = 0;
    m_tgt_pokemon_fired        = false;
    m_tgt_pokemon_targeted     = false;
    m_tgt_dead_esc_count       = 0;
    m_tgt_dead_cycles_total    = 0;
    m_tgt_walk_stuck_count     = 0;
    m_tgt_nav_prev_was_walk    = false;
    m_tgt_nav_stuck_recoveries = 0;
    m_tgt_patrol_step_idx      = 0;
    m_tgt_running_to_mob       = false;
    m_tgt_run_started          = TP{};
    m_tgt_not_ready_count      = 0;
    m_tgt_geo_path_ready       = false;
    m_tgt_geo_path_idx         = 0;
    m_tgt_pending_path         = std::nullopt;
    m_backtracking             = false;

    // Обробляємо unreachable прапор з Attack
    if (m_unreachable_flag) {
        m_attack_was_unreachable = true;
        if (!gs.cfg.target_macro_keys.empty())
            m_tgt_macro_idx = (m_tgt_macro_idx + 1) %
                              (int)gs.cfg.target_macro_keys.size();
        m_unreachable_flag = false;
    }

    if (gs.cfg.delays.enabled) {
        m_tgt_rd_rotate = std::make_unique<RandomDelay>(
            gs.cfg.delays.rotate_mean_ms, gs.cfg.delays.rotate_std_ms);
        m_tgt_rd_walk = std::make_unique<RandomDelay>(
            gs.cfg.delays.walk_mean_ms, gs.cfg.delays.walk_std_ms);
    } else {
        m_tgt_rd_rotate.reset();
        m_tgt_rd_walk.reset();
    }
}

void BotBehaviorTree::deliverGeoPath(
        const std::vector<std::pair<float,float>>& path, uint64_t id) {
    if (id <= m_tgt_geo_path_id) return;
    m_tgt_geo_path       = path;
    m_tgt_geo_path_idx   = 0;
    m_tgt_geo_path_id    = id;
    m_tgt_geo_path_ready = !path.empty();
}

std::optional<PathRequest> BotBehaviorTree::takePendingPathRequest() {
    if (!m_tgt_pending_path.has_value()) return std::nullopt;
    auto req = std::move(m_tgt_pending_path);
    m_tgt_pending_path = std::nullopt;
    return req;
}

void BotBehaviorTree::addCrumb(float x, float y, float z, const Config::BreadcrumbConfig& cfg) {
    if (!cfg.enabled) return;
    if (!std::isfinite(x) || !std::isfinite(y)) return;
    if (!m_breadcrumbs.empty()) {
        float dx = x - m_breadcrumbs.back().x;
        float dy = y - m_breadcrumbs.back().y;
        if (dx*dx + dy*dy < cfg.record_distance * cfg.record_distance) return;
    }
    if ((int)m_breadcrumbs.size() >= cfg.max_count)
        m_breadcrumbs.pop_front();
    m_breadcrumbs.push_back({x, y, z});
}

std::optional<BotBehaviorTree::Crumb> BotBehaviorTree::findBacktrackCrumb(
        float px, float py, float range) const {
    for (int i = (int)m_breadcrumbs.size() - 2; i >= 0; --i) {
        const auto& c = m_breadcrumbs[i];
        float dx = c.x - px, dy = c.y - py;
        float d2 = dx*dx + dy*dy;
        if (d2 > 50.f*50.f && d2 <= range*range) return c;
    }
    return std::nullopt;
}

void BotBehaviorTree::blacklistCurrentTarget(GameState& gs) {
    if (!gs.blacklist_mob || gs.kl_mobs.empty()) return;
    int   bid     = 0;
    float best_hp = 1e9f, best_pct = 101.f;
    for (const auto& mob : gs.kl_mobs) {
        if (!mob.isAlive()) continue;
        if (mob.hpMax > 0.f) {
            float p = mob.hpPercent();
            if (p < best_pct) { best_pct = p; bid = mob.objectID; }
        } else if (mob.hpAbs() < best_hp) {
            best_hp = mob.hpAbs(); bid = mob.objectID;
        }
    }
    if (bid != 0) gs.blacklist_mob(bid, 60.f);
}
