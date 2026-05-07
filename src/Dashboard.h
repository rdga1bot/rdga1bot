#pragma once

#include <string>
#include <deque>
#include <functional>
#include <mutex>
#include <ctime>

// ncurses: тільки forward declaration тут.
// <ncurses.h> включається ТІЛЬКИ в Dashboard.cpp (після undef конфліктних макросів)
struct _win_st;
typedef struct _win_st WINDOW;

#include "Brain.h"
#include "Config.h"
#include "Stats.h"
#include "Eyes.h"

// Кольорові пари (ініціалізуються в Init())
enum ColorPair {
    COLOR_NORMAL      = 1,
    COLOR_TITLE       = 2,
    COLOR_IDLE        = 3,
    COLOR_TARGET      = 4,
    COLOR_ATTACK      = 5,
    COLOR_LOOT        = 6,
    COLOR_DEAD        = 7,
    COLOR_BUFF        = 8,
    COLOR_HP_BAR      = 9,
    COLOR_MP_BAR      = 10,
    COLOR_CP_BAR      = 11,
    COLOR_LOG_SEP     = 12,
    COLOR_DIM         = 13,
    COLOR_TAB_ACTIVE  = 14, // активна вкладка: чорний на cyan
    COLOR_TAB_INACT   = 15, // неактивна: white на default
    COLOR_METER_GOOD  = 16, // зелений метр (kills, K/D)
    COLOR_METER_WARN  = 17, // жовтий метр (rest, epsilon)
    COLOR_METER_BAD   = 18, // червоний метр (deaths)
};

class Dashboard {
public:
    Dashboard();
    ~Dashboard();

    void Init();
    void Update(const Brain& brain, double fps);
    void AddLog(const std::string& msg);

    // Повертає: 0=нічого, 'q'=стоп, 'p'=пауза, 's'=settings, 'r'=reset
    int HandleInput();

    void ShowSettings(Config& cfg, const std::string& config_path);
    void Shutdown();

    bool IsActive() const { return m_active; }

private:
    bool   m_active  = false;
    bool   m_paused  = false;
    int    m_tab     = 0;      // 0=Main 1=Stats 2=Memory 3=RL
    time_t m_t_start = 0;

    std::deque<std::string> m_log;
    std::mutex m_log_mutex;
    static constexpr int MAX_LOG = 200;

    // Layout: header(1)|sep|status(3)|sep|tabbar(1)|sep|content(N)|sep|footer(1)
    WINDOW* m_win_header  = nullptr;
    WINDOW* m_win_status  = nullptr;
    WINDOW* m_win_tabbar  = nullptr;
    WINDOW* m_win_content = nullptr;
    WINDOW* m_win_footer  = nullptr;

    int m_rows = 0;
    int m_cols = 0;

    void RecreateWindows();

    // Спільні елементи (всі вкладки)
    void DrawHeader(const std::string& state, double fps, bool paused);
    void DrawStatus(const Brain& brain, const Eyes::Me& me,
                    const Eyes::Target& tgt, const Stats& stats);
    void DrawTabBar();
    void DrawFooter();

    // Вкладки (content area)
    void DrawMainTab();
    void DrawStatsTab(const Brain& brain, const Stats& stats);
    void DrawMemoryTab(const Brain& brain);
    void DrawRLTab(const Brain& brain);

    // Примітиви
    void DrawBar(WINDOW* win, int y, int x, int bar_w,
                 int pct, int color, const char* label);
    void DrawMeter(WINDOW* win, int y, int x, int label_w, int bar_w,
                   double val, double max_val, int color,
                   const char* label, const char* val_fmt);

    static int         StateColor(const std::string& s);
    static const char* StateEmoji(const std::string& s);

    void DrawSettingsOverlay(WINDOW* win, const Config& cfg, int sel, int edit_mode);
    bool EditTextField(WINDOW* win, int y, int x, int w, std::string& value);
};
