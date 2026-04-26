// BotBT_Dead.cpp
// Survival branches: actDead, actRest, actZone, actLoot.
//
// actDead  — respawn flow (3 фази: wait isDead, press OK, grace 30s).
// actRest  — пауза при HP/MP < threshold; ігнорує якщо моби поряд або HP падає.
// actZone  — повернення до зони фарму (навігація стрілками або breadcrumbs).
// actLoot  — ESC + 300ms після kill (server-side loot на ElmoreLab, без pickup key).
//
// НЕ додавати рух через W/A/S/D — лише стрілки ↑↓←→.
#include "BotBehaviorTree.h"
#include "Input.h"
#include <cmath>

BTStatus BotBehaviorTree::actDead(GameState& gs) {
    if (!s_self) return BTStatus::Failure;
    auto& self = *s_self;
    self.m_active_branch = "Dead";

    if (!gs.hands_ready) return BTStatus::Running;

    switch (self.m_dead_phase) {
    case 0:
        gs.log("[DEAD] Фаза 0: Enter");
        gs.stats.RecordDeath();               // рівно 1 раз на реальну смерть
        if (gs.cb.notify_death_fn) gs.cb.notify_death_fn();
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
    if (gs.cb.navigate_to_mob) {
        L2Character dummy{};
        dummy.x = self.m_zone_cx; dummy.y = self.m_zone_cy;
        dummy.hp = 100.f; dummy.hpMax = 100.f;
        gs.cb.navigate_to_mob(dummy);
    } else {
        gs.hands.WalkForward(800);
    }
    gs.hands.Send(200);
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
