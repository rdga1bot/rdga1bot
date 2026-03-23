#pragma once

#include <optional>
#include <string>

#ifdef _WIN32
#define WIN32_MEAN_AND_LEAN
#include <Windows.h>
using WindowHandle = ::HWND;
#else
using WindowHandle = unsigned long; // X11 Window (XID)
#endif

class Window
{
public:
    struct Rect { int x, y, width, height; };
    struct Point { int x, y; };

    static std::optional<Window> Find(const std::string &window_title);

    const Rect &Rect() const { return m_rect; }
    WindowHandle Handle() const { return m_hwnd; }

    void BringToForeground() const;

private:
    const WindowHandle m_hwnd;
    const struct Rect  m_rect;

    Window(WindowHandle hwnd, const struct Rect &rect) :
        m_hwnd{hwnd},
        m_rect{rect}
    {}

    static std::optional<struct Rect> HWNDRect(WindowHandle hwnd);

#ifdef _WIN32
    static std::optional<std::wstring> WidenString(const std::string &string);
#endif
};
