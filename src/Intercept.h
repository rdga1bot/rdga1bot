#pragma once

#include <memory>
#include <array>
#include <mutex>
#include <stdexcept>

#ifdef _WIN32
#include "interception.h"
#endif

class Intercept
{
public:
    struct InterceptionDriverNotFoundError : public std::runtime_error
        { InterceptionDriverNotFoundError() : std::runtime_error("Interception driver not found") {} };

    static constexpr std::size_t KEYBOARD_KEY_MAX = 256;

    enum class KeyboardKeyEvent : unsigned short
    {
        Down    = 0,    // INTERCEPTION_KEY_DOWN
        Up      = 1     // INTERCEPTION_KEY_UP
    };

    enum class MouseButtonEvent : unsigned short
    {
        LeftDown    = 0x0001,   // INTERCEPTION_MOUSE_LEFT_BUTTON_DOWN
        LeftUp      = 0x0002,   // INTERCEPTION_MOUSE_LEFT_BUTTON_UP
        RightDown   = 0x0004,   // INTERCEPTION_MOUSE_RIGHT_BUTTON_DOWN
        RightUp     = 0x0008    // INTERCEPTION_MOUSE_RIGHT_BUTTON_UP
    };

    enum class MouseButton
    {
        Left    = 1,
        Right   = 2,
        Middle  = 3,
        Fourth  = 4,
        Fifth   = 5,

        Max     = 6
    };

    struct Point { int x, y; };

    Intercept(); // throws InterceptionDriverNotFoundError
    ~Intercept();

    void SetGameWindow(unsigned long wnd);

    void SendMouseMoveEvent(const Point &point) const;
    void SendMouseButtonEvent(MouseButtonEvent event) const;
    void SendKeyboardKeyEvent(int code, KeyboardKeyEvent event, bool e0, bool e1) const;

    // Opt: один XFlush після всієї серії подій (не після кожної)
    void FlushEvents() const;
    // Opt: скидає кеш фокусу — викликати на початку кожної серії подій
    void ResetFocusCache() const;

    bool MouseButtonPressed(MouseButton button);
    bool KeyboardKeyPressed(int code);

    Point MouseDelta();

private:
#ifdef _WIN32
    struct InterceptionContextDestroyer
    {
        using pointer = ::InterceptionContext;
        void operator()(::InterceptionContext context) const { ::interception_destroy_context(context); }
    };

    int m_screen_width, m_screen_height;
    std::unique_ptr<::InterceptionContext, InterceptionContextDestroyer> m_context;
    ::InterceptionDevice m_keyboard_device;
    ::InterceptionDevice m_mouse_device;
#else
    void*         m_display;        // X11 Display* (opaque to avoid header pollution)
    unsigned long m_game_window;    // X11 Window XID of the game window
    Point         m_last_mouse_pos;
    mutable bool  m_focus_verified = false; // кеш фокусу для поточної серії подій
    void EnsureGameFocused() const;         // перевіряє/встановлює фокус один раз
#endif

    std::array<bool, KEYBOARD_KEY_MAX> m_pressed_keyboard_keys;
    std::array<bool, static_cast<std::size_t>(MouseButton::Max)> m_pressed_mouse_buttons;
    Point m_mouse_delta;
    std::mutex m_keyboard_mtx;
    std::mutex m_mouse_mtx;
};
