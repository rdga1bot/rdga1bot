#ifndef _WIN32

#include "Capture.h"

#include <cstring>
#include <algorithm>
#include <sys/shm.h>

#include <X11/Xlib.h>
#include <X11/extensions/XShm.h>

Capture::Capture() :
    m_display   {nullptr},
    m_shm_image {nullptr},
    m_shm_id    {-1},
    m_shm_addr  {reinterpret_cast<void*>(-1)},
    m_screen    {0},
    m_data      {nullptr},
    m_rect      {0, 0, 0, 0}
{
    Display* dpy = ::XOpenDisplay(nullptr);
    if (!dpy) return;
    m_display = dpy;

    m_screen = DefaultScreen(dpy);
    const int w = DisplayWidth(dpy, m_screen);
    const int h = DisplayHeight(dpy, m_screen);
    m_rect = {0, 0, w, h};

    // Allocate full-screen XShm image
    XShmSegmentInfo* shm = new XShmSegmentInfo{};
    XImage* img = ::XShmCreateImage(
        dpy,
        DefaultVisual(dpy, m_screen),
        static_cast<unsigned int>(DefaultDepth(dpy, m_screen)),
        ZPixmap,
        nullptr,
        shm,
        static_cast<unsigned int>(w),
        static_cast<unsigned int>(h)
    );

    if (!img) {
        delete shm;
        return;
    }

    shm->shmid   = ::shmget(IPC_PRIVATE,
                             static_cast<std::size_t>(img->bytes_per_line * img->height),
                             IPC_CREAT | 0777);
    shm->shmaddr = img->data = static_cast<char*>(::shmat(shm->shmid, nullptr, 0));
    shm->readOnly = False;

    if (!::XShmAttach(dpy, shm)) {
        (*img->f.destroy_image)(img);
        ::shmdt(shm->shmaddr);
        ::shmctl(shm->shmid, IPC_RMID, nullptr);
        delete shm;
        return;
    }

    ::XSync(dpy, False);

    m_shm_image = img;
    m_shm_id    = shm->shmid;
    m_shm_addr  = shm->shmaddr;
    m_data      = reinterpret_cast<unsigned char*>(img->data);

    // Store shm pointer in the image's obdata slot for cleanup
    img->obdata = reinterpret_cast<char*>(shm);
}

Capture::~Capture()
{
    if (m_shm_image) {
        XImage* img = reinterpret_cast<XImage*>(m_shm_image);
        XShmSegmentInfo* shm = reinterpret_cast<XShmSegmentInfo*>(img->obdata);

        if (m_display) {
            Display* dpy = reinterpret_cast<Display*>(m_display);
            ::XShmDetach(dpy, shm);
        }

        (*img->f.destroy_image)(img);

        if (m_shm_addr != reinterpret_cast<void*>(-1)) ::shmdt(m_shm_addr);
        if (m_shm_id  >= 0)                            ::shmctl(m_shm_id, IPC_RMID, nullptr);

        delete shm;
    }

    if (m_display) {
        ::XCloseDisplay(reinterpret_cast<Display*>(m_display));
    }
}

void Capture::Clear()
{
    // No-op on Linux: XShmGetImage always overwrites the buffer
}

std::optional<Capture::Bitmap> Capture::Grab(const struct Rect &rect)
{
    if (!m_display || !m_shm_image) return {};
    if (rect.width <= 0 || rect.height <= 0) return {};

    Display* dpy = reinterpret_cast<Display*>(m_display);
    XImage*  img = reinterpret_cast<XImage*>(m_shm_image);
    ::Window root = RootWindow(dpy, m_screen);

    // Always capture full screen from (0,0) to avoid XShmGetImage BadMatch
    // when rect.x + screen_width > total virtual screen width.
    if (!::XShmGetImage(dpy, root, img, 0, 0, AllPlanes)) {
        return {};
    }

    // Offset data pointer into the full-screen buffer to start at (rect.x, rect.y).
    // When Utils.cpp creates cv::Mat(rows, cols, ..., data), the stride equals
    // img->bytes_per_line (= screen_width * bpp/8), so cropping to
    // {0, 0, rect.width, rect.height} yields exactly the window region.
    const int bpp           = img->bits_per_pixel / 8;
    const int stride        = img->bytes_per_line;
    const int cols          = img->width;
    const int rows          = img->height;
    const int clamped_x     = std::min(rect.x, cols - 1);
    const int clamped_y     = std::min(rect.y, rows - 1);
    const int clamped_w     = std::min(rect.width,  cols - clamped_x);
    const int clamped_h     = std::min(rect.height, rows - clamped_y);

    Bitmap bitmap = {};
    bitmap.data   = m_data + clamped_y * stride + clamped_x * bpp;
    bitmap.rows   = rows;
    bitmap.cols   = cols;
    bitmap.width  = clamped_w;
    bitmap.height = clamped_h;
    bitmap.bits   = img->bits_per_pixel;
    return bitmap;
}

#endif // !_WIN32
