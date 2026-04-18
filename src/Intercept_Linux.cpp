#ifndef _WIN32

#include "Intercept.h"

#include <linux/input.h>
#include <string>
#include <algorithm>
#include <iostream>

#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>

// Map PS/2 E0-extended scan codes to Linux evdev codes.
// Non-E0 scan codes equal their evdev counterparts directly.
static int e0_scancode_to_evdev(int code)
{
    switch (code) {
        case 0x1D: return KEY_RIGHTCTRL;
        case 0x37: return KEY_SYSRQ;      // PrintScreen
        case 0x38: return KEY_RIGHTALT;
        case 0x47: return KEY_HOME;
        case 0x48: return KEY_UP;
        case 0x49: return KEY_PAGEUP;
        case 0x4B: return KEY_LEFT;
        case 0x4D: return KEY_RIGHT;
        case 0x4F: return KEY_END;
        case 0x50: return KEY_DOWN;
        case 0x51: return KEY_PAGEDOWN;
        case 0x52: return KEY_INSERT;
        case 0x53: return KEY_DELETE;
        case 0x5B: return KEY_LEFTMETA;
        default:   return -1;
    }
}

static int scancode_to_evdev(int code, bool e0)
{
    if (e0) return e0_scancode_to_evdev(code);
    return code;
}

// X11 keycode = evdev code + 8
static int evdev_to_x11(int evdev) { return evdev + 8; }

Intercept::Intercept() :
    m_display       {nullptr},
    m_game_window   {0},
    m_last_mouse_pos{0, 0},
    m_pressed_keyboard_keys {},
    m_pressed_mouse_buttons {},
    m_mouse_delta   {},
    m_keyboard_mtx  {},
    m_mouse_mtx     {}
{
    m_display = ::XOpenDisplay(nullptr);
    if (!m_display) {
        throw InterceptionDriverNotFoundError{};
    }

    Display* dpy = reinterpret_cast<Display*>(m_display);
    ::Window root = DefaultRootWindow(dpy);
    ::Window child;
    int rx, ry, wx, wy;
    unsigned int mask;
    ::XQueryPointer(dpy, root, &root, &child, &rx, &ry, &wx, &wy, &mask);
    m_last_mouse_pos = {rx, ry};
}

Intercept::~Intercept()
{
    if (m_display) {
        ::XCloseDisplay(reinterpret_cast<Display*>(m_display));
    }
}

void Intercept::SetGameWindow(unsigned long wnd)
{
    m_game_window = wnd;
}

void Intercept::SetBackend(const std::string& name)
{
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (lower == "xtest")
        m_backend = LinuxInputBackend::XTest;
    else if (lower == "xsendevent")
        m_backend = LinuxInputBackend::XSendEvent;
    else
        m_backend = LinuxInputBackend::Hybrid; // "hybrid" or unknown → Hybrid
}

void Intercept::SetWindowRect(int x, int y, int /*w*/, int /*h*/)
{
    m_window_rect_x = x;
    m_window_rect_y = y;
}

void Intercept::EnsureGameFocused() const
{
    // Opt: перевіряємо фокус один раз на серію подій (m_focus_verified — кеш)
    if (m_focus_verified || m_game_window == 0 || !m_display) return;
    Display* dpy = reinterpret_cast<Display*>(m_display);
    ::Window focused = None;
    int revert = 0;
    ::XGetInputFocus(dpy, &focused, &revert);
    if (focused != static_cast<::Window>(m_game_window)) {
        ::XSetInputFocus(dpy, static_cast<::Window>(m_game_window),
                         RevertToParent, CurrentTime);
        ::XFlush(dpy); // потрібен одразу щоб фокус застосувався до наступної клавіші
    }
    m_focus_verified = true;
}

void Intercept::FlushEvents() const
{
    if (!m_display) return;
    ::XFlush(reinterpret_cast<Display*>(m_display));
}

void Intercept::ResetFocusCache() const
{
    m_focus_verified = false;
}

void Intercept::SendKeyboardKeyEvent(int code, KeyboardKeyEvent event, bool e0, bool /*e1*/) const
{
    if (!m_display) return;

    Display* dpy = reinterpret_cast<Display*>(m_display);

    // Opt: фокус перевіряємо один раз на серію (кешовано).
    // Потрібен навіть для XSendEvent — Wine маршрутизує WM_ через фокус.
    EnsureGameFocused();

    const int evdev_code = scancode_to_evdev(code, e0);
    if (evdev_code < 0) return;

    const int x11_kc = evdev_to_x11(evdev_code);

    // ── Hybrid/XSendEvent: надсилаємо напряму у вікно гри ─────────────────
    // Fallback на XTest якщо m_game_window не відомий.
    bool use_xsend = (m_backend == LinuxInputBackend::XSendEvent ||
                      m_backend == LinuxInputBackend::Hybrid);
    if (use_xsend && m_game_window == 0) {
        // Fallback з попередженням (тільки один раз на запуск)
        static bool warned = false;
        if (!warned) {
            std::cerr << "[Intercept] WARNING: Hybrid keyboard — m_game_window=0, fallback to XTest\n";
            warned = true;
        }
        use_xsend = false;
    }

    if (use_xsend) {
        // XSendEvent: клавіатурні події напряму у вікно гри (курсор не рухається)
        XKeyEvent xev = {};
        xev.display     = dpy;
        xev.window      = static_cast<::Window>(m_game_window);
        xev.root        = DefaultRootWindow(dpy);
        xev.subwindow   = None;
        xev.time        = CurrentTime;
        xev.x = xev.y  = 1;
        xev.x_root = xev.y_root = 1;
        xev.same_screen = True;
        xev.keycode     = static_cast<unsigned int>(x11_kc);
        xev.state       = 0;
        xev.type        = (event == KeyboardKeyEvent::Down) ? KeyPress : KeyRelease;
        ::XSendEvent(dpy, static_cast<::Window>(m_game_window),
                     True, KeyPressMask | KeyReleaseMask,
                     reinterpret_cast<XEvent*>(&xev));
    } else {
        // XTest: глобальна ін'єкція (переміщує курсор — XTest mode)
        const Bool press = (event == KeyboardKeyEvent::Down) ? True : False;
        ::XTestFakeKeyEvent(dpy, static_cast<unsigned int>(x11_kc), press, CurrentTime);
    }
    // Opt: XFlush прибрано — буде один FlushEvents() в кінці серії (Input::Send)
}

void Intercept::SendMouseMoveEvent(const Point &point) const
{
    if (!m_display) return;
    Display* dpy = reinterpret_cast<Display*>(m_display);
    // Mouse move: завжди через XTest навіть у Hybrid режимі.
    // Wine/DirectInput ігнорує XSendEvent MotionNotify — глобальний курсор потрібен
    // для camera drag (правою кнопкою + рух = RotateCamera).
    ::XTestFakeMotionEvent(dpy, -1, point.x, point.y, CurrentTime);
    // Opt: XFlush прибрано — один FlushEvents() в кінці серії

    // Оновлюємо window-relative позицію для XSendEvent кнопок миші.
    // Const_cast safe — m_last_mouse_pos це мutable логічний стан, не фізична мутація.
    const_cast<Intercept*>(this)->m_last_mouse_pos =
        {point.x - m_window_rect_x, point.y - m_window_rect_y};
}

void Intercept::SendMouseButtonEvent(MouseButtonEvent event) const
{
    if (!m_display) return;
    Display* dpy = reinterpret_cast<Display*>(m_display);

    int button = 0;
    bool press = false;

    switch (event) {
        case MouseButtonEvent::LeftDown:  button = 1; press = true;  break;
        case MouseButtonEvent::LeftUp:    button = 1; press = false; break;
        case MouseButtonEvent::RightDown: button = 3; press = true;  break;
        case MouseButtonEvent::RightUp:   button = 3; press = false; break;
    }

    if (button <= 0) return;

    // ── XSendEvent (тільки в режимі XSendEvent, не Hybrid) ──────────────────
    // Hybrid: кнопки миші → XTest (Wine ігнорує XSendEvent ButtonPress у більшості версій).
    // Клавіатура у Hybrid через XSendEvent — працює; миша — ні (Wine-версійно залежна).
    bool use_xsend = (m_backend == LinuxInputBackend::XSendEvent);
    if (use_xsend && m_game_window == 0) {
        use_xsend = false;
    }

    if (use_xsend) {
        // XSendEvent ButtonPress/ButtonRelease з window-relative координатами.
        // m_last_mouse_pos вже в window-relative після SendMouseMoveEvent.
        XButtonEvent xev = {};
        xev.display     = dpy;
        xev.window      = static_cast<::Window>(m_game_window);
        xev.root        = DefaultRootWindow(dpy);
        xev.subwindow   = None;
        xev.time        = CurrentTime;
        xev.same_screen = True;
        xev.x           = m_last_mouse_pos.x;
        xev.y           = m_last_mouse_pos.y;
        xev.x_root      = m_last_mouse_pos.x + m_window_rect_x;
        xev.y_root      = m_last_mouse_pos.y + m_window_rect_y;
        xev.button      = static_cast<unsigned int>(button);
        xev.state       = 0;
        xev.type        = press ? ButtonPress : ButtonRelease;
        ::XSendEvent(dpy, static_cast<::Window>(m_game_window),
                     True, ButtonPressMask | ButtonReleaseMask,
                     reinterpret_cast<XEvent*>(&xev));
    } else {
        ::XTestFakeButtonEvent(dpy, static_cast<unsigned int>(button),
                               press ? True : False, CurrentTime);
    }
    // Opt: XFlush прибрано — один FlushEvents() в кінці серії
}

bool Intercept::KeyboardKeyPressed(int code)
{
    if (!m_display) return false;
    Display* dpy = reinterpret_cast<Display*>(m_display);

    char keys[32] = {};
    ::XQueryKeymap(dpy, keys);

    auto check = [&](int x11_kc) -> bool {
        if (x11_kc < 8 || x11_kc > 255) return false;
        return (keys[x11_kc / 8] >> (x11_kc % 8)) & 1;
    };

    if (check(evdev_to_x11(code))) return true;

    const int e0_evdev = e0_scancode_to_evdev(code);
    if (e0_evdev >= 0 && check(evdev_to_x11(e0_evdev))) return true;

    return false;
}

bool Intercept::MouseButtonPressed(MouseButton button)
{
    if (!m_display) return false;
    Display* dpy = reinterpret_cast<Display*>(m_display);

    ::Window root = DefaultRootWindow(dpy);
    ::Window child;
    int rx, ry, wx, wy;
    unsigned int mask;
    ::XQueryPointer(dpy, root, &root, &child, &rx, &ry, &wx, &wy, &mask);

    switch (button) {
        case MouseButton::Left:   return (mask & Button1Mask) != 0;
        case MouseButton::Right:  return (mask & Button3Mask) != 0;
        case MouseButton::Middle: return (mask & Button2Mask) != 0;
        case MouseButton::Fourth: return (mask & Button4Mask) != 0;
        case MouseButton::Fifth:  return (mask & Button5Mask) != 0;
        default:                  return false;
    }
}

Intercept::Point Intercept::MouseDelta()
{
    // MouseDelta використовує XQueryPointer (абсолютні координати) незалежно від
    // режиму backend. m_last_mouse_pos у Hybrid режимі зберігає window-relative
    // координати для XSendEvent кнопок, тому для delta використовуємо окремий
    // статичний трекер абсолютної позиції.
    if (m_display) {
        Display* dpy = reinterpret_cast<Display*>(m_display);
        ::Window root = DefaultRootWindow(dpy);
        ::Window child;
        int rx, ry, wx, wy;
        unsigned int mask;
        ::XQueryPointer(dpy, root, &root, &child, &rx, &ry, &wx, &wy, &mask);

        std::lock_guard<std::mutex> lock{m_mouse_mtx};
        // У XTest режимі m_last_mouse_pos — абсолютна; у Hybrid — window-relative.
        // Для delta завжди берємо абсолютні з XQueryPointer.
        const int prev_abs_x = (m_backend == LinuxInputBackend::XTest)
            ? m_last_mouse_pos.x
            : (m_last_mouse_pos.x + m_window_rect_x);
        const int prev_abs_y = (m_backend == LinuxInputBackend::XTest)
            ? m_last_mouse_pos.y
            : (m_last_mouse_pos.y + m_window_rect_y);
        m_mouse_delta.x += std::abs(rx - prev_abs_x);
        m_mouse_delta.y += std::abs(ry - prev_abs_y);
        // Оновлюємо відповідно до поточного режиму
        if (m_backend == LinuxInputBackend::XTest) {
            m_last_mouse_pos = {rx, ry};
        } else {
            m_last_mouse_pos = {rx - m_window_rect_x, ry - m_window_rect_y};
        }
        return m_mouse_delta;
    }

    std::lock_guard<std::mutex> lock{m_mouse_mtx};
    return m_mouse_delta;
}

#endif // !_WIN32
