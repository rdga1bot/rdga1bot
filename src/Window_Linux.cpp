#ifndef _WIN32

#include "Window.h"

#include <cstring>
#include <string>
#include <vector>
#include <map>

// Rename X11's Window typedef to avoid conflict with class Window.
// All X11 API functions will use X11WinHandle instead of Window.
#define Window X11WinHandle
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#undef Window

using X11Win = X11WinHandle; // XID == unsigned long

// Suppress non-fatal X errors (e.g. BadWindow when windows disappear mid-enumeration)
static int x11_error_suppress(Display*, XErrorEvent*) { return 0; }

// Recursively enumerate all descendant windows
static void EnumX11Windows(Display* dpy, X11Win parent, std::vector<X11Win> &out)
{
    X11Win root_ret, parent_ret;
    X11Win* children = nullptr;
    unsigned int nchildren = 0;

    if (!::XQueryTree(dpy, parent, &root_ret, &parent_ret, &children, &nchildren)) {
        return;
    }

    for (unsigned int i = 0; i < nchildren; ++i) {
        out.push_back(children[i]);
        EnumX11Windows(dpy, children[i], out);
    }

    if (children) ::XFree(children);
}

// Get a window's title via _NET_WM_NAME (UTF-8) or WM_NAME (fallback)
static std::string GetWindowTitle(Display* dpy, X11Win w)
{
    Atom net_wm_name = ::XInternAtom(dpy, "_NET_WM_NAME", True);
    Atom utf8_string  = ::XInternAtom(dpy, "UTF8_STRING",   True);

    if (net_wm_name != None && utf8_string != None) {
        Atom actual_type;
        int  actual_format;
        unsigned long nitems, bytes_after;
        unsigned char* prop = nullptr;

        if (::XGetWindowProperty(dpy, w, net_wm_name, 0, 1024, False,
                                  utf8_string, &actual_type, &actual_format,
                                  &nitems, &bytes_after, &prop) == Success && prop) {
            std::string title(reinterpret_cast<char*>(prop));
            ::XFree(prop);
            return title;
        }
    }

    char* name = nullptr;
    if (::XFetchName(dpy, w, &name) && name) {
        std::string title(name);
        ::XFree(name);
        return title;
    }

    return {};
}

std::optional<Window> Window::Find(const std::string &window_title)
{
    Display* dpy = ::XOpenDisplay(nullptr);
    if (!dpy) return {};

    // Suppress BadWindow errors from windows disappearing during enumeration
    ::XSetErrorHandler(x11_error_suppress);

    X11Win root = DefaultRootWindow(dpy);
    std::vector<X11Win> windows;
    EnumX11Windows(dpy, root, windows);

    X11Win found = None;
    std::map<X11Win, std::string> titles;

    // Exact title match
    for (const auto &w : windows) {
        XWindowAttributes attr;
        std::memset(&attr, 0, sizeof(attr));
        if (!::XGetWindowAttributes(dpy, w, &attr)) continue;
        if (attr.map_state != IsViewable) continue;

        const std::string title = GetWindowTitle(dpy, w);
        if (title.empty()) continue;

        if (title == window_title) {
            found = w;
            break;
        }
        titles[w] = title;
    }

    // Partial title match
    if (found == static_cast<X11Win>(None)) {
        for (const auto &pair : titles) {
            if (pair.second.find(window_title) != std::string::npos) {
                found = pair.first;
                break;
            }
        }
    }

    if (found == static_cast<X11Win>(None)) {
        ::XCloseDisplay(dpy);
        return {};
    }

    const auto rect = HWNDRect(static_cast<WindowHandle>(found));
    ::XCloseDisplay(dpy);

    if (!rect.has_value()) return {};
    return Window{static_cast<WindowHandle>(found), rect.value()};
}

std::optional<struct Window::Rect> Window::HWNDRect(WindowHandle hwnd)
{
    if (hwnd == 0) return {};

    Display* dpy = ::XOpenDisplay(nullptr);
    if (!dpy) return {};

    X11Win xwin = static_cast<X11Win>(hwnd);
    X11Win root_ret;
    int x, y;
    unsigned int w, h, bw, depth;

    if (!::XGetGeometry(dpy, xwin, &root_ret, &x, &y, &w, &h, &bw, &depth)) {
        ::XCloseDisplay(dpy);
        return {};
    }

    // Translate window-local (0,0) to screen coordinates
    X11Win child;
    int screen_x, screen_y;
    ::XTranslateCoordinates(dpy, xwin, root_ret, 0, 0, &screen_x, &screen_y, &child);

    ::XCloseDisplay(dpy);
    return {{screen_x, screen_y, static_cast<int>(w), static_cast<int>(h)}};
}

void Window::BringToForeground() const
{
    Display* dpy = ::XOpenDisplay(nullptr);
    if (!dpy) return;

    X11Win xwin = static_cast<X11Win>(m_hwnd);
    ::XRaiseWindow(dpy, xwin);
    ::XSetInputFocus(dpy, xwin, RevertToParent, CurrentTime);
    ::XFlush(dpy);
    ::XCloseDisplay(dpy);
}

#endif // !_WIN32
