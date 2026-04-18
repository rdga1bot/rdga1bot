#pragma once

#include <vector>
#include "Input.h"
#include "RandomDelay.h"
#include "Config.h"

class Hands : public Input
{
public:
    // Клавіші для ротації атак і бафів
    KeyboardKey m_next_target_key = KeyboardKey::F2; // /nexttarget макрос
    std::vector<KeyboardKey> m_target_macro_keys; // клавіші /target макросів
    std::vector<KeyboardKey> m_attack_keys = {KeyboardKey::F1}; // ротація атак
    std::vector<KeyboardKey> m_buff_keys;  // бафи

    // Стандартні клавіші
    KeyboardKey m_spoil_key         = KeyboardKey::F3;
    KeyboardKey m_sweep_key         = KeyboardKey::F4;
    KeyboardKey m_pick_up_key       = KeyboardKey::F5;
    KeyboardKey m_restore_hp_key    = KeyboardKey::F6;
    KeyboardKey m_restore_mp_key    = KeyboardKey::F7;
    KeyboardKey m_restore_cp_key    = KeyboardKey::F8;

    struct Rect { int x, y, width, height; };

    Hands() :
        Input           {},
        m_window_rect   {}
    {}

    void SetWindowRect(const Rect &rect) {
        m_window_rect = rect;
#ifndef _WIN32
        // Propagate window position to Intercept for XSendEvent window-relative coords (MR66)
        GetIntercept().SetWindowRect(rect.x, rect.y, rect.width, rect.height);
#endif
    }
    void SetGameWindow(unsigned long wnd) { Input::SetGameWindow(wnd); }

    void ResetUI()
        { MoveMouseSmoothly(WindowCenter()); PressKeyboardKeyCombination({KeyboardKey::LeftAlt, KeyboardKey::L}); }

    void MoveMouseTo(const Point &point)    { MoveMouseSmoothly(WindowPoint(point)); }
    void GoTo(const Point &point)           { MoveMouseTo(point); Delay(200); LeftMouseButtonClick(); }

    void SelectTarget() { LeftMouseButtonClick(); }
    void CancelTarget() { PressKeyboardKey(KeyboardKey::Escape); }
    void ResetCamera()  { RightMouseButtonClick(); }

    void Spoil()        { PressKeyboardKey(m_spoil_key, 200); }
    void Sweep()        { PressKeyboardKey(m_sweep_key, 200); }
    void PickUp()       { PressKeyboardKey(m_pick_up_key, 3500); }
    void RestoreHP()    { PressKeyboardKey(m_restore_hp_key); }
    void RestoreMP()    { PressKeyboardKey(m_restore_mp_key); }
    void RestoreCP()    { PressKeyboardKey(m_restore_cp_key); }

    // /nexttarget (F2)
    void NextTarget() { PressKeyboardKey(m_next_target_key); }

    // Натискання макросу /target з ротацією
    void TargetMacro(int idx) {
        if (!m_target_macro_keys.empty())
            PressKeyboardKey(m_target_macro_keys[idx % (int)m_target_macro_keys.size()]);
    }

    // Натискання скіла атаки з ротацією
    void AttackSkill(int idx) {
        if (!m_attack_keys.empty())
            PressKeyboardKey(m_attack_keys[idx % (int)m_attack_keys.size()]);
    }

    // Натискання скіла бафу з ротацією
    void BuffSkill(int idx) {
        if (!m_buff_keys.empty())
            PressKeyboardKey(m_buff_keys[idx % (int)m_buff_keys.size()]);
    }

    // Клавіші руху (конфігуровані; дефолт — стрілки, НІКОЛИ W/S/A/D = чат в L2)
    KeyboardKey m_move_forward  = KeyboardKey::Up;
    KeyboardKey m_move_back     = KeyboardKey::Down;
    KeyboardKey m_rotate_left   = KeyboardKey::Left;
    KeyboardKey m_rotate_right  = KeyboardKey::Right;

    // Рух персонажа
    void WalkForward(int ms = 500) { HoldKeyboardKey(m_move_forward,  ms); }
    void WalkBack(int ms = 500)    { HoldKeyboardKey(m_move_back,     ms); }
    void RotateLeft(int ms = 300)  { HoldKeyboardKey(m_rotate_left,   ms); }
    void RotateRight(int ms = 300) { HoldKeyboardKey(m_rotate_right,  ms); }

    // Безперервний біг: тримати UP весь тік (200мс), потім відпускати і одразу знову.
    // Викликається кожен тік поки m_running_to_mob=true.
    // L2 читає key-repeat, тому серія HoldKeyboardKey(up, 200) без паузи = безперервний рух.
    void RunTick(int ms = 150) { HoldKeyboardKey(m_move_forward, ms); }

    // ── Варіативні затримки (антидетект) ─────────────────────────────────────
    // Оновити RandomDelay параметри з конфігурації (викликається з ApplyConfig)
    void SetDelays(const Config::DelayConfig& d) {
        m_delay_attack.SetParams(d.attack_mean_ms,  d.attack_std_ms);
        m_delay_rotate.SetParams(d.rotate_mean_ms,  d.rotate_std_ms);
        m_delay_walk.SetParams  (d.walk_mean_ms,     d.walk_std_ms);
        m_delay_potion.SetParams(d.potion_mean_ms,   d.potion_std_ms);
    }

    // AttackSkill + варіативна затримка після атаки (антидетект версія)
    void AttackSkillRand(int idx) {
        AttackSkill(idx);
        Delay(m_delay_attack.Get());
    }

    // RotateLeft/Right з варіативною тривалістю
    void RotateLeftRand()  { HoldKeyboardKey(m_rotate_left,  m_delay_rotate.Get()); }
    void RotateRightRand() { HoldKeyboardKey(m_rotate_right, m_delay_rotate.Get()); }

    // WalkForward з варіативною тривалістю
    void WalkForwardRand() { HoldKeyboardKey(m_move_forward, m_delay_walk.Get()); }

    // RestoreHP/MP/CP з варіативною затримкою
    void RestoreHPRand() { PressKeyboardKey(m_restore_hp_key); Delay(m_delay_potion.Get()); }
    void RestoreMPRand() { PressKeyboardKey(m_restore_mp_key); Delay(m_delay_potion.Get()); }
    void RestoreCPRand() { PressKeyboardKey(m_restore_cp_key); Delay(m_delay_potion.Get()); }

    void LookAround()
    {
        const auto center = WindowCenter();
        MoveMouseSmoothly({center.x + 40, center.y + 40});
        LeftMouseButtonClick();
        Delay(500);
        ResetCamera();
    }

private:
    Rect m_window_rect;

    // RandomDelay екземпляри (налаштовуються через SetDelays)
    RandomDelay m_delay_attack{500.f, 75.f};
    RandomDelay m_delay_rotate{350.f, 50.f};
    RandomDelay m_delay_walk  {800.f, 120.f};
    RandomDelay m_delay_potion{ 50.f, 15.f};

    Point WindowPoint(const Point &point) const { return {m_window_rect.x + point.x, m_window_rect.y + point.y}; }

    Point WindowCenter() const
        { return {m_window_rect.x + m_window_rect.width / 2, m_window_rect.y + m_window_rect.height / 2}; }
};
