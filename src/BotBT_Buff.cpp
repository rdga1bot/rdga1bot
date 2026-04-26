// BotBT_Buff.cpp
// Buff branch: actBuff.
//
// actBuff — ALT+B macro (template matching для підтвердження).
// m_buff_stage: 0=idle, 1=opening, 2=waiting, 3=confirming.
// m_buff_after_death: форсує баф після respawn незалежно від close_threat.
// RL override: BuffNow action з confidence > 0.5.
//
// Залежності: Eyes::DetectTemplate, Config::buff_interval, RandomDelay.
#include "BotBehaviorTree.h"
#include "Input.h"
#include <opencv2/opencv.hpp>

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
