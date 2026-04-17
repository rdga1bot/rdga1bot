// SPDX-License-Identifier: GPL-3.0-only
#include "BotBehaviorTree.h"
#include <cassert>
#include "FeatureExtractor.h"
#include "RewardCalculator.h"
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
    m_rl_last_features = Eigen::VectorXf::Zero(LinearQModel::NUM_FEATURES);
}

BotBehaviorTree::~BotBehaviorTree() {
    shutdownRL();
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

    // Target піддерево (MR28)
    uint16_t tgt_root     = bt.addSelector();
    uint16_t act_tgt_init = bt.addAction(&actTgtInit);
    uint16_t act_tgt_dead = bt.addAction(&actTgtDeadTarget);
    uint16_t act_tgt_map  = bt.addAction(&actTgtMinimap);
    uint16_t act_tgt_f2   = bt.addAction(&actTgtF2AndMacro);
    uint16_t act_tgt_nav  = bt.addAction(&actTgtNavigation);
    uint16_t act_tgt_geo  = bt.addAction(&actTgtGeoPath);
    uint16_t act_tgt_pat  = bt.addAction(&actTgtPatrol);

    // ── Фаза 2: root отримує прямих нащадків (усі gілки підряд) ─────────────
    bt.addChild(root, seq_dead);
    if (seq_rest != NONE) bt.addChild(root, seq_rest);
    if (seq_zone != NONE) bt.addChild(root, seq_zone);
    if (seq_buff != NONE) bt.addChild(root, seq_buff);
    bt.addChild(root, seq_loot);
    bt.addChild(root, seq_atk);
    bt.addChild(root, tgt_root);  // Target піддерево (MR28)

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

    // Target піддерево — BFS: всі прямі нащадки tgt_root після інших sequences
    bt.addChild(tgt_root, act_tgt_init);
    bt.addChild(tgt_root, act_tgt_dead);
    bt.addChild(tgt_root, act_tgt_map);
    bt.addChild(tgt_root, act_tgt_f2);
    bt.addChild(tgt_root, act_tgt_nav);
    bt.addChild(tgt_root, act_tgt_geo);
    bt.addChild(tgt_root, act_tgt_pat);

    if (m_log_fn) m_log_fn("[BT] Ініціалізовано: " + std::to_string(bt.nodeCount()) + " вузлів");
    else std::cerr << "[BT] Ініціалізовано: " << bt.nodeCount() << " вузлів\n";

    // RL ініціалізація (після побудови дерева)
    initRL(cfg);
}

// ── tick ──────────────────────────────────────────────────────────────────────
std::string BotBehaviorTree::tick(GameState& gs) {
    assert(s_self == nullptr || s_self == this); // single-thread contract
    s_self = this;
    uint32_t nowMs = (uint32_t)std::chrono::duration_cast<
        std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();

    if (m_rl_model) rlPreTick(gs);

    m_bt.tick(gs, nowMs);

    if (m_rl_model) rlPostTick(gs);

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
    return gs.is_dead && !s_self->inGrace();
}

bool BotBehaviorTree::condNeedsRest(GameState& gs) {
    if (!gs.hp_valid || gs.is_dead || gs.in_grace || gs.has_target) return false;
    const bool low_hp = gs.hp > 0 && gs.hp < gs.cfg.hp_threshold;
    const bool low_mp = gs.mp > 0 && gs.mp < gs.cfg.mp_threshold;
    if (low_hp || low_mp) {
        // Якщо буф вже прострочений — не блокуємо Buff гілку через Rest
        if (gs.cfg.buff_enabled && gs.buff_needed()) {
            gs.log("[REST] Буф потрібен (buff_in=" +
                   std::to_string((int)(gs.cfg.buff_interval - gs.secs_since_last_buff)) +
                   "с) → пропускаємо Rest");
            return false;
        }
        // Близькі моби (dist<70px на мінімапі) АБО HP падає → атакуємо, не відпочиваємо.
        // TH: Vampiric Rage / крити лікують під час атаки → зупинка = смерть.
        // Перевірка по dist відкидає ДАЛЕКІ моби (/target "Name" зона) що завжди видно.
        if (gs.hp_falling || gs.minimap_close_threat) {
            gs.log("[REST] " + std::string(gs.hp_falling ? "HP падає" : "Моби поряд (minimap)") +
                   " → атакуємо замість відпочинку");
            return false;
        }
        // Нещодавній kill (< 15с) — активна зона бою, не відпочивати.
        // Атакер може агресуватись ззовні 70px minimap (close_threat=false),
        // а потіон стабілізує HP між тіками (hp_falling=false) → бот застряє в Rest і гине.
        if (s_self && secsSince(s_self->m_last_kill_time) < 15.0) {
            gs.log("[REST] Нещодавній kill (<15с) → активна зона, не відпочиваємо");
            return false;
        }
        return true;
    }

    // RL override: форсувати відпочинок раніше розкладу (RestNow)
    if (s_self && s_self->m_rl_model
        && s_self->m_rl_suggested_action == LinearQModel::Action::RestNow
        && s_self->m_rl_action_confidence > 0.5f) {
        return true;
    }
    return false;
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
    // Близькі моби (dist<70px) АБО HP падає → атакуємо, не починаємо баф.
    // minimap_dots ЗАВЖДИ непорожній в зоні фарму — перевіряємо ВІДСТАНЬ точок.
    // Виняток: m_buff_after_death — після respawn баф критичний (без нього миттєва смерть).
    if (!s_self->m_buff_after_death) {
        if (gs.hp_falling || gs.minimap_close_threat) return false;
    }

    // RL override: форсувати баф раніше розкладу
    if (s_self->m_rl_model
        && s_self->m_rl_suggested_action == LinearQModel::Action::BuffNow
        && s_self->m_rl_action_confidence > 0.5f
        && !gs.is_dead && !gs.has_target) {
        return true;
    }
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
        gs.log("[DEAD] Фаза 2: відроджено, grace 30с → форс-баф після grace");
        self.m_respawn_until   = futureBy(30.0);
        self.m_dead_phase      = 0;
        self.m_buff_after_death = true;
        self.notifyDeathRL();
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
        self.m_buff_after_death = false;
        gs.log("[Buffs] Завершено, наступний баф через " +
            std::to_string(gs.cfg.buff_interval) + "с");
        return BTStatus::Success;
    }

    // ALT+B режим — багатостадійний FSM з template matching
    switch (self.m_buff_stage) {

    case 0: { // Чекаємо виходу з флагу (combat state) L2 → ESC + ALT+B
        // Близькі моби (dist<70px) АБО HP падає під час очікування → abort, атакуємо.
        // minimap_dots завжди є в зоні фарму — перевіряємо дистанцію.
        // Виняток: m_buff_after_death — після respawn minimap_close_threat ігнорується,
        // бо без бафів (Vampiric Rage) смерть гарантована.
        const bool minimap_block = gs.minimap_close_threat && !self.m_buff_after_death;
        if (gs.hp_falling || minimap_block) {
            self.m_last_buff = now() - std::chrono::seconds(gs.cfg.buff_interval - 30);
            gs.log("[Buffs] " + std::string(gs.hp_falling ? "HP падає" : "Моби поряд") +
                   " → переривати очікування виходу з флагу");
            self.m_buff_stage = 0;
            self.m_buff_retries = 0;
            return BTStatus::Success;
        }
        const double kCombatExpire = 15.0;
        if (secsSince(self.m_last_kill_time) < kCombatExpire) {
            if (self.m_buff_retries % 50 == 0) {
                gs.log("[Buffs] Чекаємо виходу з флагу (combat state) ще " +
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
        self.notifyBuffRL();
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
        self.m_buff_after_death   = false;
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
            self.notifyKillRL();
            self.resetAttackState(gs);
            return BTStatus::Success; // → Selector спробує Loot
        }

        // HP моба з пам'яті (якщо увімкнено) — НАЙБЛИЖЧИЙ моб (= поточний таргет).
        // Раніше: мінімальний HP по всьому KL → вмираючий сусідній моб B (2% HP)
        // підмінював target->hp і тригерив false kill / has_target=false на поточному A.
        // Результат: 3-4 моби з ~0% HP гоняться за ботом + "unreachable" кожні 7-8 атак.
        // Тепер: беремо моба найближчого до гравця (той якого ми атакуємо).
        if (gs.cfg.mem_use_for_target_hp && gs.target.has_value() && gs.coords_valid) {
            float best_dist = 1e12f;
            const L2Character* best = nullptr;
            for (const auto& mob : gs.kl_mobs) {
                if (!mob.isAlive()) continue;
                float d = mob.distanceTo(gs.player_x, gs.player_y);
                // dist < 100 = player-об'єкт або garbage (player coords == mob coords)
                if (d < 100.0f) continue;
                if (d < best_dist) { best_dist = d; best = &mob; }
            }
            if (best) {
                self.m_atk_mem_hp_valid = true;
                const float absHp = best->hpAbs();
                if (best->hpMax > 0.f) {
                    float pct = best->hpPercent();
                    const_cast<GameState&>(gs).target->hp = (int)pct;
                    // НЕ перезаписуємо has_target: kill detection через kl_mob_died + hp≤2% debounce
                    if (absHp != self.m_atk_kl_hp_prev_abs) {
                        self.m_atk_kl_hp_prev_abs = absHp;
                        gs.log("[KL-HP] nearest dist=" + std::to_string((int)best_dist) +
                            " hp%=" + std::to_string((int)pct) +
                            " ocr=" + std::to_string(gs.target->hp));
                    }
                } else {
                    self.m_atk_mem_hp_abs = absHp;
                    if (absHp != self.m_atk_kl_hp_prev_abs) {
                        self.m_atk_kl_hp_prev_abs = absHp;
                        gs.log("[KL-HP] nearest dist=" + std::to_string((int)best_dist) +
                            " hpAbs=" + std::to_string((int)absHp) +
                            " ocr=" + std::to_string(gs.target->hp));
                    }
                }
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
            self.notifyKillRL();
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
            self.notifyKillRL();
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
            self.m_atk_unreachable_streak++;
            gs.log("[ATTACKING] HP стабільний 5с (моб недосяжний) → Target [unreachable] ×"
                + std::to_string(self.m_atk_unreachable_streak));
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
        self.notifyKillRL();
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

// actTarget — замінено піддеревом tgt_root в MR28. Не використовується.
BTStatus BotBehaviorTree::actTarget(GameState& gs) {
    (void)gs;
    return BTStatus::Failure;
}

// ─────────────────────────────────────────────────────────────────────────────
// TARGET ПІДДЕРЕВО (MR28) — статичні BT callback вузли
// ─────────────────────────────────────────────────────────────────────────────

BTStatus BotBehaviorTree::actTgtInit(GameState& gs) {
    if (!s_self) return BTStatus::Failure;
    auto& self = *s_self;
    self.m_active_branch = "Target";

    if (!gs.hands_ready) {
        self.m_tgt_not_ready_count++;
        if (self.m_tgt_not_ready_count == 20 || self.m_tgt_not_ready_count % 100 == 0)
            gs.log("[WARNING] TARGETING: IsReady=false вже " +
                std::to_string(self.m_tgt_not_ready_count) + " тіків — Input thread завис?");
        return BTStatus::Running;
    }
    self.m_tgt_not_ready_count = 0;

    if (!self.m_tgt_active) {
        self.resetTargetState(gs);
        self.m_tgt_active = true;
    }

    if (gs.coords_valid)
        self.addCrumb(gs.player_x, gs.player_y, gs.player_z, gs.cfg.breadcrumbs);

    return BTStatus::Failure; // передати до actTgtDeadTarget
}

BTStatus BotBehaviorTree::actTgtDeadTarget(GameState& gs) {
    if (!s_self) return BTStatus::Failure;
    auto r = s_self->tgtHandleDeadTarget(gs);
    return r.value_or(BTStatus::Failure);
}

BTStatus BotBehaviorTree::actTgtMinimap(GameState& gs) {
    if (!s_self) return BTStatus::Failure;
    auto& self = *s_self;

    self.m_tgt_macro_attempts++;

    // Обчислюємо і зберігаємо map_ref для downstream вузлів
    const auto& minimap_dots = gs.minimap_dots;
    self.m_tgt_map_ref          = nullptr;
    self.m_tgt_map_ref_selected = false;
    for (const auto& d : minimap_dots) {
        if (d.selected) {
            self.m_tgt_map_ref          = &d;
            self.m_tgt_map_ref_selected = true;
            break;
        }
    }
    if (!self.m_tgt_map_ref && !minimap_dots.empty())
        self.m_tgt_map_ref = &minimap_dots[0];

    self.tgtHandleMinimap(gs, self.m_tgt_map_ref, self.m_tgt_map_ref_selected);
    return BTStatus::Failure; // передати до actTgtF2AndMacro
}

BTStatus BotBehaviorTree::actTgtF2AndMacro(GameState& gs) {
    if (!s_self) return BTStatus::Failure;
    auto& self = *s_self;
    // streak==0 = атаки йдуть, HP моба змінюється → таргет не міняємо.
    // streak>0 = атаки не йдуть (ідемо до макро-таргету або застрягли) → F2 шукає ближчого.
    if (gs.has_target && self.m_atk_unreachable_streak == 0)
        return BTStatus::Failure;
    s_self->tgtSendF2AndMacro(gs);
    return BTStatus::Failure; // передати до actTgtNavigation
}

BTStatus BotBehaviorTree::actTgtNavigation(GameState& gs) {
    if (!s_self) return BTStatus::Failure;
    auto& self = *s_self;
    auto r = self.tgtHandleNavigation(gs, self.m_tgt_map_ref);
    return r.value_or(BTStatus::Failure);
}

BTStatus BotBehaviorTree::actTgtGeoPath(GameState& gs) {
    if (!s_self) return BTStatus::Failure;
    auto& self = *s_self;
    auto r = self.tgtHandleGeoPath(gs, self.m_tgt_map_ref);
    return r.value_or(BTStatus::Failure);
}

BTStatus BotBehaviorTree::actTgtPatrol(GameState& gs) {
    if (!s_self) return BTStatus::Failure;
    auto& self = *s_self;
    self.tgtHandlePatrolAndRotate(gs, self.m_tgt_map_ref);
    gs.hands.Send(150);
    return BTStatus::Running; // останній вузол — завжди Running
}

// ─────────────────────────────────────────────────────────────────────────────
// actTarget ПІДФУНКЦІЇ
// ─────────────────────────────────────────────────────────────────────────────

std::optional<BTStatus> BotBehaviorTree::tgtHandleDeadTarget(GameState& gs) {
    if (!gs.target.has_value() || gs.target->hp != 0) {
        m_tgt_dead_esc_count = 0;
        if (gs.target.has_value() && gs.target->hp > 0)
            m_tgt_dead_cycles_total = 0;
        return std::nullopt;
    }

    if (m_tgt_pokemon_targeted) {
        gs.log("[Pokemon] sweep (чекаємо анімацію)...");
        gs.hands.Delay(1500);
        gs.hands.PressKeyboardKey(Input::KeyboardKey::Escape);
        m_tgt_pokemon_targeted = false;
        m_tgt_dead_esc_count = 0;
        gs.hands.Send(100);
        return BTStatus::Running;
    }
    m_tgt_dead_esc_count++;
    if (m_tgt_dead_esc_count == 1) {
        gs.log("[TARGETING] Мертвий таргет hp=0 ×1 → чекаємо підтвердження...");
        gs.hands.Send(250);
        return BTStatus::Running;
    }
    gs.log("[TARGETING] Мертвий таргет hp=0 → ESC ×" +
        std::to_string(m_tgt_dead_esc_count - 1));
    if (m_tgt_dead_esc_count <= 6) {
        gs.hands.PressKeyboardKey(Input::KeyboardKey::Escape);
        gs.hands.Send(100);
        return BTStatus::Running;
    }
    // Після 5 спроб — fallthrough до F2/macro
    m_tgt_dead_esc_count = 0;
    m_tgt_dead_cycles_total++;
    if (m_tgt_dead_cycles_total >= gs.cfg.targeting_tuning.dead_cycles_macro_switch
        && !gs.cfg.target_macro_keys.empty()) {
        gs.log("[TARGETING] Dead-target loop ×" +
            std::to_string(m_tgt_dead_cycles_total) + " → /target макрос");
        m_tgt_macro_attempts++;
        gs.hands.TargetMacro(m_tgt_macro_idx);
        m_tgt_macro_idx = (m_tgt_macro_idx + 1) % (int)gs.cfg.target_macro_keys.size();
        // 1000ms: сервер повинен встигнути обробити /target і оновити таргет.
        // 300ms було замало → chain ×3→×4→×5 в логах.
        gs.hands.Send(1000);
        return BTStatus::Running;
    }
    gs.log("[TARGETING] Мертвий таргет не зникає після 5 ESC → пробуємо F2");
    return std::nullopt;
}

void BotBehaviorTree::tgtHandleMinimap(GameState& gs,
        const Eyes::MinimapDot* map_ref, bool map_ref_selected) {
    const int kMinimapDxThreshold = gs.cfg.targeting_tuning.minimap_dx_threshold;
    const int kMinimapRotateLimit = gs.cfg.targeting_tuning.minimap_rotate_limit;

    if (map_ref && m_tgt_minimap_rotate_count < kMinimapRotateLimit) {
        const int dx = map_ref->dx;
        const int dy = map_ref->dy;
        const char* who = map_ref_selected ? "Вибраний" : "Найближчий";
        if (dx < -kMinimapDxThreshold) {
            gs.hands.RotateLeft(RandMs(m_tgt_rd_rotate.get(), gs, 120));
            m_tgt_minimap_rotate_count++;
            gs.log(std::string("[MAP] ") + who + " моб ліворуч (dx=" +
                std::to_string(dx) + ", rot=" +
                std::to_string(m_tgt_minimap_rotate_count) + ") → RotateLeft");
        } else if (dx > kMinimapDxThreshold) {
            gs.hands.RotateRight(RandMs(m_tgt_rd_rotate.get(), gs, 120));
            m_tgt_minimap_rotate_count++;
            gs.log(std::string("[MAP] ") + who + " моб праворуч (dx=" +
                std::to_string(dx) + ", rot=" +
                std::to_string(m_tgt_minimap_rotate_count) + ") → RotateRight");
        } else {
            m_tgt_minimap_rotate_count = 0;
            if (dy > 30) {
                gs.hands.RotateRight(RandMs(m_tgt_rd_rotate.get(), gs, 700));
                gs.log(std::string("[MAP] ") + who + " моб позаду (dy=" +
                    std::to_string(dy) + ") → розворот 180°");
            }
        }
    } else if (map_ref && m_tgt_minimap_rotate_count >= kMinimapRotateLimit) {
        m_tgt_minimap_rotate_count = 0;
        gs.log("[MAP] Ліміт ротацій → WalkForward до моба (dx=" +
            std::to_string(map_ref->dx) + ")");
        if (gs.eyes.IsGroundAhead()) {
            gs.hands.WalkForward(RandMs(m_tgt_rd_walk.get(), gs, 600));
            m_tgt_nav_prev_was_walk = true;
        }
    } else if (!map_ref) {
        m_tgt_minimap_rotate_count = 0;
    }
}

void BotBehaviorTree::tgtSendF2AndMacro(GameState& gs) {
    // Основний метод: F2 /nexttarget
    gs.hands.NextTarget();

    // Скидаємо unreachable flag тільки якщо є БЛИЗЬКІ моби (minimap_close_threat)
    // І streak невеликий (< 5 послідовних unreachable).
    // Якщо streak >= 5 — ігноруємо minimap, форсуємо повний цикл macro / patrol,
    // бо бот застряг в unreachable-loop з одним і тим же мобом.
    static constexpr int kUnreachStreakForceRetarget = 5;
    if (gs.minimap_close_threat && m_attack_was_unreachable
        && m_atk_unreachable_streak < kUnreachStreakForceRetarget) {
        m_attack_was_unreachable = false;
        gs.log("[TARGETING] Близькі моби (minimap) → скидаємо unreachable, F2 вже відіслано");
        return;
    }
    if (m_atk_unreachable_streak >= kUnreachStreakForceRetarget) {
        m_atk_streak_force_count++;
        gs.log("[TARGETING] Streak ×" + std::to_string(m_atk_unreachable_streak)
            + " force#" + std::to_string(m_atk_streak_force_count)
            + " → форсуємо повний цикл (ігноруємо minimap_close_threat)");
        m_atk_unreachable_streak = 0;

        // Після 3 форс-циклів поспіль (~75с без kills) — ESC таргет + пауза,
        // щоб бот фізично змінив позицію через patrol/nav замість Pokemon-loop.
        static constexpr int kForceEscAfter = 3;
        if (m_atk_streak_force_count >= kForceEscAfter) {
            gs.log("[TARGETING] force#" + std::to_string(m_atk_streak_force_count)
                + " → ESC таргет, patrol шукатиме нову ціль");
            gs.hands.PressKeyboardKey(Input::KeyboardKey::Escape);
            gs.hands.Send(300);
            const_cast<GameState&>(gs).has_target = false;
            m_atk_streak_force_count = 0;
            m_attack_was_unreachable = false;
            return; // не надсилаємо F2/macro — patrol сам знайде
        }
    }

    // /target макроси: тільки після N невдалих F2
    const int kMacroFallbackAfterUnreach = gs.cfg.targeting_tuning.macro_fallback_unreach;
    const int kMacroFallbackAfter = gs.cfg.targeting_tuning.macro_fallback_after > 0
                                    ? gs.cfg.targeting_tuning.macro_fallback_after : 10;
    const bool macro_fallback = !gs.cfg.target_macro_keys.empty()
                                && m_tgt_macro_attempts > (m_attack_was_unreachable
                                    ? kMacroFallbackAfterUnreach : kMacroFallbackAfter)
                                && m_tgt_macro_attempts % 2 == 0;
    if (macro_fallback) {
        gs.hands.Delay(80);
        gs.hands.TargetMacro(m_tgt_macro_idx);
        m_tgt_macro_idx = (m_tgt_macro_idx + 1) % (int)gs.cfg.target_macro_keys.size();
    }

    // Pokemon макрос: ОДИН РАЗ на початку TARGETING циклу
    if (gs.cfg.has_pokemon_key && !m_tgt_pokemon_fired && m_tgt_macro_attempts == 1) {
        m_tgt_pokemon_fired = true;
        gs.hands.Delay(50);
        gs.hands.PressKeyboardKey(gs.cfg.pokemon_key);
        m_tgt_pokemon_targeted = true;
        gs.log("[Pokemon] макрос");
    }
}

std::optional<BTStatus> BotBehaviorTree::tgtHandleNavigation(GameState& gs,
        const Eyes::MinimapDot* map_ref) {
    (void)map_ref;

    // ── Navigation stuck detection ────────────────────────────────────────────
    if (gs.cfg.nav_stuck_detection) {
        const bool is_moving = gs.eyes.IsCharacterMoving();

        if (m_tgt_nav_prev_was_walk) {
            m_tgt_nav_prev_was_walk = false;

            if (m_tgt_running_to_mob) {
                const double run_secs = secsSince(m_tgt_run_started);
                if (run_secs >= 15.0) {
                    const int rotation_ms = std::min(
                        900 + (m_tgt_nav_stuck_recoveries / 2) * 450, 1800);
                    gs.hands.WalkBack(300);
                    if (m_tgt_nav_stuck_recoveries % 2 == 0) {
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
                    m_tgt_nav_stuck_recoveries++;
                    m_tgt_run_started = now();
                    m_tgt_nav_prev_was_walk = true;
                }
            } else {
                const float flow    = gs.cfg.nav_flow_detection
                    ? gs.eyes.GetMovementFlow() : -1.0f;
                const float mm_flow = gs.cfg.nav_flow_detection
                    ? gs.eyes.GetMinimapFlow() : -1.0f;
                const bool actually_moved = is_moving || (flow > 1.5f) || (mm_flow > 0.3f);
                if (!actually_moved) {
                    m_tgt_walk_stuck_count++;
                    gs.log("[NAV] Не рухаємось після WalkForward ×" +
                        std::to_string(m_tgt_walk_stuck_count) +
                        (flow >= 0 ? " flow=" + std::to_string(flow).substr(0,4) : ""));
                    if (m_tgt_walk_stuck_count >= gs.cfg.nav_stuck_threshold) {
                        m_tgt_walk_stuck_count = 0;
                        const int rotation_ms = std::min(
                            900 + (m_tgt_nav_stuck_recoveries / 2) * 450, 1800);
                        gs.hands.WalkBack(300);
                        if (m_tgt_nav_stuck_recoveries % 2 == 0) {
                            gs.hands.RotateRight(rotation_ms);
                            gs.log("[NAV] Застряг ×" +
                                std::to_string(m_tgt_nav_stuck_recoveries + 1) +
                                " → WalkBack + RotateRight(" +
                                std::to_string(rotation_ms) + "мс)");
                        } else {
                            gs.hands.RotateLeft(rotation_ms);
                            gs.log("[NAV] Застряг ×" +
                                std::to_string(m_tgt_nav_stuck_recoveries + 1) +
                                " → WalkBack + RotateLeft(" +
                                std::to_string(rotation_ms) + "мс)");
                        }
                        gs.hands.WalkForward(500);
                        m_tgt_nav_prev_was_walk = true;
                        m_tgt_nav_stuck_recoveries++;

                        // Breadcrumbs: ініціація backtrack
                        if (gs.cfg.breadcrumbs.enabled && gs.coords_valid
                            && !m_backtracking
                            && m_tgt_nav_stuck_recoveries >= gs.cfg.breadcrumbs.stuck_threshold) {
                            auto bc = findBacktrackCrumb(gs.player_x, gs.player_y,
                                                         gs.cfg.breadcrumbs.backtrack_range);
                            if (bc.has_value()) {
                                m_backtracking = true;
                                gs.log("[BREADCRUMB] Застряг ×" +
                                    std::to_string(m_tgt_nav_stuck_recoveries) +
                                    " → backtrack до (" +
                                    std::to_string((int)bc->x) + "," +
                                    std::to_string((int)bc->y) + ")");
                            }
                        }
                    }
                } else {
                    m_tgt_walk_stuck_count = 0;
                    m_tgt_nav_stuck_recoveries = 0;
                    if (flow > 0)
                        gs.log("[NAV] Рух ок, flow=" + std::to_string(flow).substr(0,4));
                }
            }
        }
    }

    // ── Breadcrumbs: виконання повернення ────────────────────────────────────
    if (m_backtracking && gs.coords_valid && gs.navigate_to_mob) {
        auto bc = findBacktrackCrumb(gs.player_x, gs.player_y,
                                     gs.cfg.breadcrumbs.backtrack_range);
        if (bc.has_value()) {
            L2Character dummy{};
            dummy.x = bc->x; dummy.y = bc->y; dummy.z = bc->z;
            dummy.hp = 100.f; dummy.hpMax = 100.f;
            if (!gs.navigate_to_mob(dummy)) {
                m_backtracking = false;
                m_tgt_nav_stuck_recoveries = 0;
                gs.log("[BREADCRUMB] Точку досягнуто → відновлюємо пошук");
            } else {
                gs.hands.Send(200);
                return BTStatus::Running;
            }
        } else {
            m_backtracking = false;
            gs.log("[BREADCRUMB] Крихти вичерпано");
        }
    }

    // ── Memory навігація ──────────────────────────────────────────────────────
    // RL override: NavigateMemory форсує навігацію навіть якщо cfg.navigation.enabled=false
    const bool rl_nav_forced = m_rl_model
        && m_rl_suggested_action == LinearQModel::Action::NavigateMemory
        && m_rl_action_confidence > 0.5f
        && !gs.kl_mobs.empty()
        && gs.coords_valid && gs.navigate_to_mob;

    if ((gs.cfg.navigation.enabled || rl_nav_forced) && !gs.kl_mobs.empty()) {
        auto kl_mobs = gs.kl_mobs;
        kl_mobs.erase(std::remove_if(kl_mobs.begin(), kl_mobs.end(),
            [&gs](const L2Character& mob) {
                return gs.is_blacklisted && gs.is_blacklisted(mob.objectID);
            }), kl_mobs.end());
        if (!kl_mobs.empty()) {
            std::optional<L2Character> nearest;
            if (rl_nav_forced && !gs.cfg.navigation.enabled) {
                // NavigateMemory override: завжди найближчий (не зважений)
                gs.log("[RL] NavigateMemory override → nav до найближчого моба");
                if (gs.find_nearest_mob)
                    nearest = gs.find_nearest_mob(kl_mobs, gs.player_x, gs.player_y,
                                                  gs.cfg.weighted_target.max_range);
            } else if (gs.cfg.weighted_target.enabled && gs.select_target) {
                nearest = gs.select_target(kl_mobs, gs.player_x, gs.player_y);
            } else if (gs.find_nearest_mob) {
                nearest = gs.find_nearest_mob(kl_mobs, gs.player_x, gs.player_y,
                                              gs.cfg.weighted_target.max_range);
            }
            // RL: підтвердження TargetNearest / TargetWeighted (BT вже виконує цю дію)
            if (m_rl_model && nearest.has_value()) {
                const bool is_nearest  = m_rl_suggested_action == LinearQModel::Action::TargetNearest;
                const bool is_weighted = m_rl_suggested_action == LinearQModel::Action::TargetWeighted;
                if (is_nearest || is_weighted) {
                    const std::string who = nearest->name.empty()
                        ? ("ID=" + std::to_string(nearest->objectID)) : nearest->name;
                    gs.log("[RL] " + std::string(is_nearest ? "TargetNearest" : "TargetWeighted")
                        + " підтверджено → " + who);
                }
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
                if (!m_tgt_geo_path_ready && !m_tgt_pending_path.has_value()
                    && gs.geodata && gs.geodata->IsLoaded(nearest->x, nearest->y)
                    && gs.coords_valid) {
                    PathRequest req;
                    req.sx = gs.player_x; req.sy = gs.player_y; req.sz = gs.player_z;
                    req.ex = nearest->x;  req.ey = nearest->y;  req.ez = nearest->z;
                    req.maxRange = gs.cfg.knownlist_max_range;
                    req.id = ++m_tgt_path_req_id;
                    m_tgt_pending_path = req;
                }
            }
        }
    }

    return std::nullopt;
}

std::optional<BTStatus> BotBehaviorTree::tgtHandleGeoPath(GameState& gs,
        const Eyes::MinimapDot* map_ref) {
    const int kMinimapDxThreshold = gs.cfg.targeting_tuning.minimap_dx_threshold;

    // ── NavMesh FindPath ──────────────────────────────────────────────────────
    if (gs.navmesh && gs.navmesh->IsValid()
        && gs.coords_valid && !gs.kl_mobs.empty()
        && !m_tgt_geo_path_ready) {
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
                m_tgt_geo_path       = nm_path;
                m_tgt_geo_path_idx   = 0;
                m_tgt_geo_path_id    = ++m_tgt_path_req_id;
                m_tgt_geo_path_ready = true;
                gs.log("[NAVMESH] Шлях: " + std::to_string(nm_path.size()) + " точок");
            }
        }
    }

    // ── Geodata waypoint following ────────────────────────────────────────────
    if (gs.cfg.navigation.enabled && m_tgt_geo_path_ready
        && m_tgt_geo_path_idx < m_tgt_geo_path.size() && gs.coords_valid) {
        auto [wx, wy] = m_tgt_geo_path[m_tgt_geo_path_idx];
        float dx_wp = wx - gs.player_x;
        float dy_wp = wy - gs.player_y;
        float dist_wp = std::sqrt(dx_wp * dx_wp + dy_wp * dy_wp);
        if (dist_wp < 300.f) {
            m_tgt_geo_path_idx++;
            gs.log("[GEO] Waypoint " + std::to_string(m_tgt_geo_path_idx) +
                "/" + std::to_string(m_tgt_geo_path.size()) + " досягнуто");
        } else {
            L2Character wp{};
            wp.x = wx; wp.y = wy; wp.z = gs.player_z;
            wp.hp = 100.f; wp.hpMax = 100.f;
            if (gs.navigate_to_mob && gs.navigate_to_mob(wp)) {
                gs.log("[GEO] → waypoint " + std::to_string(m_tgt_geo_path_idx) +
                    " dist=" + std::to_string((int)dist_wp));
                gs.hands.Send(150);
                return BTStatus::Running;
            }
        }
        if (m_tgt_geo_path_idx >= m_tgt_geo_path.size()) {
            m_tgt_geo_path_ready = false;
            gs.log("[GEO] Шлях пройдено");
        }
    }

    // WalkForward коли є таргет але атаки не йдуть → можливо застряг у текстурах.
    // Broken geodata на free серверах: бот застряє в невидимих стінах.
    if (gs.has_target && m_atk_unreachable_streak > 2 && !m_tgt_nav_prev_was_walk) {
        gs.hands.WalkForward(RandMs(m_tgt_rd_walk.get(), gs, 500));
        m_tgt_nav_prev_was_walk = true;
        gs.log("[NAV] streak=" + std::to_string(m_atk_unreachable_streak)
            + " → WalkForward (вихід з текстур)");
    }
    (void)map_ref;

    return std::nullopt;
}

void BotBehaviorTree::tgtHandlePatrolAndRotate(GameState& gs,
        const Eyes::MinimapDot* map_ref) {
    const auto& minimap_dots = gs.minimap_dots;
    const bool minimap_empty = minimap_dots.empty();
    const bool long_search   = (m_tgt_macro_attempts >= 20)
                               && (m_tgt_macro_attempts % 10 == 0);

    if (m_tgt_macro_attempts % 5 == 0 && (minimap_empty || long_search)) {
        gs.hands.Delay(50);
        m_tgt_step_count++;
        gs.stats.RecordTargetingFailure();
        m_rl_sig_targeting_failed = true;

        // RL override: активувати patrol раніше порогу якщо RL рекомендує
        const bool rl_patrol_boost = m_rl_model
            && m_rl_suggested_action == LinearQModel::Action::Patrol
            && m_rl_action_confidence > 0.5f;
        const bool patrol_ready = gs.cfg.patrol_enabled
            && !gs.cfg.patrol_path.empty()
            && (
                (m_tgt_macro_attempts >= gs.cfg.patrol_trigger_attempts
                 && m_tgt_macro_attempts % 5 == 0)
                || rl_patrol_boost  // RL override: без % 5 обмеження
            );

        if (patrol_ready) {
            if (rl_patrol_boost)
                gs.log("[RL] Patrol override → patrol крок " +
                    std::to_string(m_tgt_patrol_step_idx % (int)gs.cfg.patrol_path.size() + 1));
            const auto& step = gs.cfg.patrol_path[
                m_tgt_patrol_step_idx % (int)gs.cfg.patrol_path.size()];
            int pat_step_display = m_tgt_patrol_step_idx % (int)gs.cfg.patrol_path.size() + 1;
            int pat_total = (int)gs.cfg.patrol_path.size();
            switch (step.dir) {
                case Config::PatrolStep::Dir::Forward:
                    gs.hands.WalkForward(step.ms);
                    m_tgt_nav_prev_was_walk = true;
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
            m_tgt_patrol_step_idx++;
        } else {
            if ((m_tgt_macro_attempts / 5) % 2 == 0)
                gs.hands.RotateRight(RandMs(m_tgt_rd_rotate.get(), gs, 350));
            else
                gs.hands.RotateLeft(RandMs(m_tgt_rd_rotate.get(), gs, 350));
            const bool explore_trigger = (!patrol_ready && !m_tgt_nav_prev_was_walk
                && gs.eyes.IsGroundAhead()
                && ((minimap_empty && m_tgt_macro_attempts % 15 == 0)
                    || (!minimap_empty && m_tgt_macro_attempts % 20 == 0)));
            if (explore_trigger) {
                gs.hands.WalkForward(1200);
                m_tgt_nav_prev_was_walk = true;
                gs.log("[TARGETING] Спроба " + std::to_string(m_tgt_macro_attempts) +
                    " — розвідка вперед (" +
                    (minimap_empty ? "мінімапа порожня" : "dot недосяжний") + ")");
            } else {
                gs.log("[TARGETING] Спроба " + std::to_string(m_tgt_macro_attempts) +
                    " — ротація (мінімапа порожня)");
            }
        }
    } else if (m_tgt_macro_attempts % 5 == 0) {
        gs.stats.RecordTargetingFailure();
        m_rl_sig_targeting_failed = true;
        gs.log("[TARGETING] Спроба " + std::to_string(m_tgt_macro_attempts) + " — шукаємо...");
    }

    // Попередження при довгому пошуку
    const int kWarnAt = gs.cfg.targeting_tuning.long_search_warn_at;
    if (kWarnAt > 0 && m_tgt_macro_attempts > kWarnAt
        && m_tgt_macro_attempts % kWarnAt == 0) {
        std::string dots_info = minimap_dots.empty()
            ? "мінімапа порожня"
            : ("dots=" + std::to_string(minimap_dots.size()) +
               " dx=" + std::to_string(minimap_dots[0].dx));
        std::string kl_info = (gs.kl_alive_count > 0)
            ? " KL_alive=" + std::to_string(gs.kl_alive_count) : "";
        gs.log("[TARGETING] Довгий пошук ×" + std::to_string(m_tgt_macro_attempts) +
            " — " + dots_info + kl_info +
            " rot=" + std::to_string(m_tgt_minimap_rotate_count));
    }

    (void)map_ref; // map_ref доступний для майбутніх розширень
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

// ─────────────────────────────────────────────────────────────────────────────
// RL METHODS
// ─────────────────────────────────────────────────────────────────────────────

void BotBehaviorTree::initRL(const Config& cfg) {
    const auto& lc = cfg.learning;
    if (!lc.enabled) return;

    m_rl_weights_file = lc.weights_file;

    m_rl_model  = std::make_shared<LinearQModel>(lc.huber_delta);
    m_rl_buffer = std::make_shared<ExperienceBuffer>(lc.buffer_size);
    m_rl_worker = std::make_unique<LearningWorker>(
        m_rl_model, m_rl_buffer,
        lc.batch_size, lc.learning_rate, lc.discount_factor);

    if (m_log_fn) m_rl_worker->setLogFn(m_log_fn);

    m_rl_model->loadWeights(lc.weights_file);

    m_rl_epsilon       = lc.epsilon_start;
    m_rl_has_prev      = false;
    m_rl_last_features = Eigen::VectorXf::Zero(LinearQModel::NUM_FEATURES);

    m_rl_worker->start(/*core_id=*/-1);
    if (m_log_fn) m_log_fn("[RL] LearningWorker запущено. epsilon=" + std::to_string(m_rl_epsilon));
    else std::cerr << "[RL] LearningWorker запущено. epsilon=" << m_rl_epsilon << "\n";
}

void BotBehaviorTree::shutdownRL() {
    if (m_rl_worker) {
        m_rl_worker->stop();
        m_rl_worker.reset();
    }
    if (m_rl_model) {
        const std::string path = m_rl_weights_file.empty() ? "./weights.json" : m_rl_weights_file;
        m_rl_model->saveWeights(path);
        if (m_log_fn) m_log_fn("[RL] Ваги збережено: " + path);
        m_rl_model.reset();
    }
    m_rl_buffer.reset();
}

void BotBehaviorTree::rlPreTick(GameState& gs) {
    if (!m_rl_model) return;

    m_rl_sig_kill             = false;
    m_rl_sig_death            = false;
    m_rl_sig_targeting_failed = false;
    m_rl_sig_buff_done        = false;

    Eigen::VectorXf phi = FeatureExtractor::extract(gs);

    LinearQModel::Action action =
        m_rl_model->selectAction(phi, m_rl_epsilon, m_rl_rng);

    Eigen::VectorXf q_vals    = m_rl_model->getQValues(phi);
    // Softmax confidence: частка обраної дії → завжди в (0,1]
    // Уникає проблеми від'ємних Q-значень (maxCoeff < 0 → override ніколи не спрацьовував)
    {
        Eigen::VectorXf shifted = q_vals.array() - q_vals.maxCoeff();
        Eigen::VectorXf sm = shifted.array().exp();
        sm /= sm.sum();
        m_rl_action_confidence = sm((int)action);
    }
    m_rl_suggested_action     = action;

    m_rl_last_features = phi;
    m_rl_last_action   = action;
    m_rl_has_prev      = true;

    // Periodic feature vector log
    const int fli = gs.cfg.learning.feature_log_interval;
    if (fli > 0 && m_log_fn) {
        m_rl_feature_log_ticks++;
        if (m_rl_feature_log_ticks >= fli) {
            m_rl_feature_log_ticks = 0;
            static const char* names[10] = {
                "hp","mp","has_tgt","tgt_hp",
                "kl_alive","minimap","secs_kill","secs_buff",
                "is_dead","in_grace"
            };
            std::string s = "[RL-F] features:";
            for (int i = 0; i < (int)phi.size(); i++) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), " %s=%.3f", names[i], phi[i]);
                s += buf;
            }
            char buf[64];
            std::snprintf(buf, sizeof(buf), " | eps=%.4f conf=%.3f act=%d",
                          m_rl_epsilon, m_rl_action_confidence, (int)action);
            s += buf;
            m_log_fn(s);
        }
    }
}

void BotBehaviorTree::rlPostTick(GameState& gs) {
    if (!m_rl_model || !m_rl_has_prev) return;

    const auto& lc = gs.cfg.learning;

    RewardCalculator::RewardSignals sig;
    sig.kill_happened      = m_rl_sig_kill;
    sig.death_happened     = m_rl_sig_death;
    sig.targeting_failed   = m_rl_sig_targeting_failed;
    sig.buff_done          = m_rl_sig_buff_done;
    float reward = RewardCalculator::compute(sig, gs.stats);

    Eigen::VectorXf phi_next = FeatureExtractor::extract(gs);
    bool done = m_rl_sig_death;

    m_rl_worker->pushExperience(Experience{
        m_rl_last_features,
        (int)m_rl_last_action,
        reward,
        phi_next,
        done
    });

    m_rl_ticks_since_update++;
    if (m_rl_ticks_since_update >= lc.update_frequency) {
        m_rl_ticks_since_update = 0;
        m_rl_worker->requestUpdate();
        // Decay epsilon кожен update step (кожні UpdateFrequency тіків).
        // Раніше decay був тільки на смерть → epsilon не спадав за сесію.
        m_rl_epsilon = std::max(lc.epsilon_min, m_rl_epsilon * lc.epsilon_decay);
    }

    if (done) {
        // Додатковий decay на смерть (кінець епізоду = більший штраф на exploration)
        m_rl_epsilon = std::max(lc.epsilon_min, m_rl_epsilon * lc.epsilon_decay);
        if (m_log_fn) m_log_fn("[RL] Епізод завершено (смерть). epsilon=" + std::to_string(m_rl_epsilon));
        else std::cerr << "[RL] Епізод завершено. epsilon=" << m_rl_epsilon << "\n";
    }

    if (m_rl_kills_since_save >= lc.save_frequency) {
        m_rl_kills_since_save = 0;
        m_rl_model->saveWeights(lc.weights_file);
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
