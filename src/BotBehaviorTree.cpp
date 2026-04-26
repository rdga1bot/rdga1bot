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
    if (!gs.cfg.buff_enabled || gs.is_dead) return false;
    if (!s_self) return false;
    // MR76: buff під час grace якщо m_buff_after_death — єдине безпечне вікно
    // в зоні агресивних мобів (вони атакують відразу після grace).
    // Root cause death loop: respawn → grace → targeting → grace ends → attack HP=1% no buffs
    if (gs.in_grace) {
        if (!s_self->m_buff_after_death) return false;
        return gs.buff_needed();  // bypass has_target — під час grace можна бафатися безпечно
    }
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
// ACTIONS  (actDead/actRest/actZone/actBuff/actLoot → BotBT_Dead.cpp / BotBT_Buff.cpp)
// ─────────────────────────────────────────────────────────────────────────────

// ── helpers ───────────────────────────────────────────────────────────────────

