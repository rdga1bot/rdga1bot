// Dashboard.h включається ПЕРШИМ (тягне Eyes.h → opencv).
#include "Dashboard.h"

#include <algorithm>
#include <clocale>
#include <cstring>
#include <ctime>
#include <cstdio>

#include <ncurses.h>

// ─── Конструктор / деструктор ──────────────────────────────────────────────

Dashboard::Dashboard() = default;

Dashboard::~Dashboard() {
    if (m_active) Shutdown();
}

// ─── Init ──────────────────────────────────────────────────────────────────

void Dashboard::Init() {
    setlocale(LC_ALL, "");
    initscr();
    cbreak();
    noecho();
    curs_set(0);
    keypad(stdscr, TRUE);
    timeout(1);
    mousemask(BUTTON1_RELEASED, nullptr); // htop: тільки release, без press/clicked
    mouseinterval(0);

    if (!has_colors()) { endwin(); return; }

    start_color();
    use_default_colors();

    init_pair(COLOR_NORMAL,     COLOR_WHITE,   -1);
    init_pair(COLOR_TITLE,      COLOR_CYAN,    -1);
    init_pair(COLOR_IDLE,       COLOR_WHITE,   -1);
    init_pair(COLOR_TARGET,     COLOR_YELLOW,  -1);
    init_pair(COLOR_ATTACK,     COLOR_GREEN,   -1);
    init_pair(COLOR_LOOT,       COLOR_CYAN,    -1);
    init_pair(COLOR_DEAD,       COLOR_RED,     -1);
    init_pair(COLOR_BUFF,       COLOR_MAGENTA, -1);
    init_pair(COLOR_HP_BAR,     COLOR_RED,     -1);
    init_pair(COLOR_MP_BAR,     COLOR_BLUE,    -1);
    init_pair(COLOR_CP_BAR,     COLOR_YELLOW,  -1);
    init_pair(COLOR_LOG_SEP,    COLOR_YELLOW,  -1);
    init_pair(COLOR_DIM,        COLOR_WHITE,   -1);
    init_pair(COLOR_TAB_ACTIVE, COLOR_BLACK,   COLOR_CYAN);
    init_pair(COLOR_TAB_INACT,  COLOR_WHITE,   -1);
    init_pair(COLOR_METER_GOOD, COLOR_GREEN,   -1);
    init_pair(COLOR_METER_WARN, COLOR_YELLOW,  -1);
    init_pair(COLOR_METER_BAD,  COLOR_RED,     -1);

    m_t_start = time(nullptr);
    getmaxyx(stdscr, m_rows, m_cols);
    RecreateWindows();
    m_active = true;
}

// ─── RecreateWindows ───────────────────────────────────────────────────────
//
// Рядки (приклад 24-рядковий термінал):
//  0   header
//  1   ── sep ──
//  2   status row 0
//  3   status row 1
//  4   status row 2
//  5   ── sep ──
//  6   tabbar
//  7   ── sep ──
//  8   content start
//  ...
//  22  content end
//  23  ── sep ──   ← якщо термінал < 14 рядків пропускаємо
//  24  footer
//
void Dashboard::RecreateWindows() {
    auto del = [](WINDOW*& w) { if (w) { delwin(w); w = nullptr; } };
    del(m_win_header);
    del(m_win_status);
    del(m_win_tabbar);
    del(m_win_content);
    del(m_win_footer);

    getmaxyx(stdscr, m_rows, m_cols);

    // Фіксовані висоти + 4 sep-рядки
    const int kFixed = 1 + 1 + 3 + 1 + 1 + 1 + 1 + 1; // = 10
    const int content_h = std::max(3, m_rows - kFixed);

    int r = 0;
    m_win_header  = newwin(1,         m_cols, r, 0); r += 2; // +1 sep
    m_win_status  = newwin(3,         m_cols, r, 0); r += 4; // +1 sep
    m_win_tabbar  = newwin(1,         m_cols, r, 0); r += 2; // +1 sep
    m_win_content = newwin(content_h, m_cols, r, 0); r += content_h + 1;
    m_win_footer  = newwin(1,         m_cols, r, 0);

    scrollok(m_win_content, FALSE);
}

// ─── Shutdown ──────────────────────────────────────────────────────────────

void Dashboard::Shutdown() {
    if (!m_active) return;
    auto del = [](WINDOW*& w) { if (w) { delwin(w); w = nullptr; } };
    del(m_win_header);
    del(m_win_status);
    del(m_win_tabbar);
    del(m_win_content);
    del(m_win_footer);
    endwin();
    m_active = false;
}

// ─── AddLog ────────────────────────────────────────────────────────────────

void Dashboard::AddLog(const std::string& msg) {
    time_t now = time(nullptr);
    struct tm* t = localtime(&now);
    char ts[16];
    strftime(ts, sizeof(ts), "%H:%M:%S", t);

    std::lock_guard<std::mutex> lock(m_log_mutex);
    m_log.push_back(std::string(ts) + " " + msg);
    while ((int)m_log.size() > MAX_LOG) m_log.pop_front();
}

// ─── Update ────────────────────────────────────────────────────────────────

void Dashboard::Update(const Brain& brain, double fps) {
    if (!m_active) return;

    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    if (rows != m_rows || cols != m_cols) {
        endwin();
        refresh(); // один раз після endwin для reset — як у htop
        getmaxyx(stdscr, m_rows, m_cols);
        RecreateWindows();
        clearok(stdscr, TRUE); // примусова перемальовка після resize
    }

    // erase() — очищає буфер stdscr (фон + сепаратори), але НЕ примушує
    // повний repaint термінала (на відміну від clear()). ncurses шле тільки diff.
    erase();

    Eyes::Me    me_def{};
    Eyes::Target tgt_def{};
    const auto& me  = brain.Me().value_or(me_def);
    const auto& tgt = brain.Target().value_or(tgt_def);

    DrawHeader(brain.GetState(), fps, brain.IsPaused());

    // Розділювачі (позиції прив'язані до вікон через getbegy)
    auto sep = [&](WINDOW* win, int offset) {
        if (!win) return;
        int row = getbegy(win) + offset;
        if (row > 0 && row < m_rows) {
            attron(COLOR_PAIR(COLOR_DIM));
            mvhline(row, 0, ACS_HLINE, m_cols);
            attroff(COLOR_PAIR(COLOR_DIM));
        }
    };

    sep(m_win_status,  -1); // перед status
    DrawStatus(brain, me, tgt, brain.GetStats());

    sep(m_win_tabbar,  -1); // перед tabbar
    DrawTabBar();

    sep(m_win_content, -1); // перед content
    switch (m_tab) {
        case 0: DrawMainTab();                           break;
        case 1: DrawStatsTab(brain, brain.GetStats());  break;
        case 2: DrawMemoryTab(brain);                   break;
        case 3: DrawRLTab(brain);                       break;
    }

    sep(m_win_footer,  -1); // перед footer
    DrawFooter();

    // htop-style: накопичуємо всі зміни через wnoutrefresh,
    // потім один doupdate() відправляє в термінал — без мерехтіння.
    wnoutrefresh(stdscr);
    if (m_win_header)  wnoutrefresh(m_win_header);
    if (m_win_status)  wnoutrefresh(m_win_status);
    if (m_win_tabbar)  wnoutrefresh(m_win_tabbar);
    if (m_win_content) wnoutrefresh(m_win_content);
    if (m_win_footer)  wnoutrefresh(m_win_footer);
    doupdate();
}

// ─── DrawHeader ────────────────────────────────────────────────────────────

void Dashboard::DrawHeader(const std::string& state, double fps, bool paused) {
    if (!m_win_header) return;
    werase(m_win_header);

    wattron(m_win_header, COLOR_PAIR(COLOR_TITLE) | A_BOLD);
    mvwprintw(m_win_header, 0, 0, " rdga1bot");
    wattroff(m_win_header, COLOR_PAIR(COLOR_TITLE) | A_BOLD);

    if (paused) {
        wattron(m_win_header, COLOR_PAIR(COLOR_DEAD) | A_BOLD | A_BLINK);
        mvwprintw(m_win_header, 0, 11, " ПАУЗА ");
        wattroff(m_win_header, COLOR_PAIR(COLOR_DEAD) | A_BOLD | A_BLINK);
    }

    char right[64];
    snprintf(right, sizeof(right), "FPS:%-3d  %s ", (int)fps, StateEmoji(state));
    int rx = m_cols - (int)strlen(right);
    if (rx > 0) {
        wattron(m_win_header, COLOR_PAIR(StateColor(state)) | A_BOLD);
        mvwprintw(m_win_header, 0, rx, "%s", right);
        wattroff(m_win_header, COLOR_PAIR(StateColor(state)) | A_BOLD);
    }
}

// ─── DrawBar ───────────────────────────────────────────────────────────────

void Dashboard::DrawBar(WINDOW* win, int y, int x, int bar_w,
                        int pct, int color, const char* label) {
    pct = std::max(0, std::min(100, pct));
    int filled = bar_w * pct / 100;

    wattron(win, COLOR_PAIR(COLOR_DIM));
    mvwprintw(win, y, x, "%s ", label);
    wattroff(win, COLOR_PAIR(COLOR_DIM));

    int bx = x + (int)strlen(label) + 1;

    wattron(win, COLOR_PAIR(color) | A_BOLD);
    for (int i = 0; i < filled && i < bar_w; i++)
        mvwaddstr(win, y, bx + i, "█");
    wattroff(win, COLOR_PAIR(color) | A_BOLD);

    wattron(win, COLOR_PAIR(COLOR_DIM));
    for (int i = filled; i < bar_w; i++)
        mvwaddstr(win, y, bx + i, "░");
    wattroff(win, COLOR_PAIR(COLOR_DIM));

    wattron(win, COLOR_PAIR(color));
    mvwprintw(win, y, bx + bar_w + 1, "%3d%%", pct);
    wattroff(win, COLOR_PAIR(color));
}

// ─── DrawMeter (htop-style) ────────────────────────────────────────────────
// Label[████░░░  260]

void Dashboard::DrawMeter(WINDOW* win, int y, int x, int label_w, int bar_w,
                           double val, double max_val, int color,
                           const char* label, const char* val_fmt) {
    wattron(win, COLOR_PAIR(COLOR_NORMAL));
    mvwprintw(win, y, x, "%-*s[", label_w, label);
    wattroff(win, COLOR_PAIR(COLOR_NORMAL));

    int bx = x + label_w + 1;
    int filled = (max_val > 0.0)
        ? std::max(0, std::min(bar_w, (int)(bar_w * val / max_val)))
        : 0;

    wattron(win, COLOR_PAIR(color) | A_BOLD);
    for (int i = 0; i < filled; i++)
        mvwaddstr(win, y, bx + i, "█");
    wattroff(win, COLOR_PAIR(color) | A_BOLD);

    wattron(win, COLOR_PAIR(COLOR_DIM));
    for (int i = filled; i < bar_w; i++)
        mvwaddstr(win, y, bx + i, "░");
    wattroff(win, COLOR_PAIR(COLOR_DIM));

    char val_str[32];
    snprintf(val_str, sizeof(val_str), val_fmt, val);
    wattron(win, COLOR_PAIR(color));
    mvwprintw(win, y, bx + bar_w, " %s]", val_str);
    wattroff(win, COLOR_PAIR(color));
}

// ─── DrawStatus ────────────────────────────────────────────────────────────

void Dashboard::DrawStatus(const Brain& brain, const Eyes::Me& me,
                           const Eyes::Target& tgt, const Stats& stats) {
    if (!m_win_status) return;
    werase(m_win_status);

    int bar_w = std::max(10, m_cols / 3 - 14);

    DrawBar(m_win_status, 0, 1, bar_w, me.cp, COLOR_CP_BAR, "CP");
    DrawBar(m_win_status, 1, 1, bar_w, me.hp, COLOR_HP_BAR, "HP");
    DrawBar(m_win_status, 2, 1, bar_w, me.mp, COLOR_MP_BAR, "MP");

    int mid  = m_cols / 3 + 4;
    int bar_w2 = std::max(8, m_cols / 3 - 14);

    if (tgt.hp > 0) {
        DrawBar(m_win_status, 0, mid, bar_w2, tgt.hp, COLOR_HP_BAR, "Mob");
    } else {
        wattron(m_win_status, COLOR_PAIR(COLOR_DIM));
        mvwprintw(m_win_status, 0, mid, "Mob: ---");
        wattroff(m_win_status, COLOR_PAIR(COLOR_DIM));
    }

    wattron(m_win_status, COLOR_PAIR(COLOR_NORMAL));
    mvwprintw(m_win_status, 1, mid, "K:%-4d D:%-3d", stats.kills, stats.deaths);
    char kd_buf[32];
    if (stats.deaths > 0)
        snprintf(kd_buf, sizeof(kd_buf), "%.1f", (double)stats.kills / stats.deaths);
    else
        snprintf(kd_buf, sizeof(kd_buf), "%d", stats.kills);
    mvwprintw(m_win_status, 2, mid, "K/D:%-6s P:%d",
              kd_buf, stats.hp_potions + stats.mp_potions);
    wattroff(m_win_status, COLOR_PAIR(COLOR_NORMAL));

    int rx = m_cols * 2 / 3 + 2;
    if (rx + 20 < m_cols) {
        const auto& mp = brain.GetMemPlayerState();
        WorldState* ws = brain.GetWorldState();

        const char* mode_str;
        int mode_color;
        if (mp.valid && ws)   { mode_str = "[HYBRID]"; mode_color = COLOR_ATTACK; }
        else if (mp.valid)    { mode_str = "[MEM]";    mode_color = COLOR_TARGET; }
        else                  { mode_str = "[OPENCV]"; mode_color = COLOR_DIM;    }

        wattron(m_win_status, COLOR_PAIR(mode_color) | A_BOLD);
        mvwprintw(m_win_status, 0, rx, "%s", mode_str);
        wattroff(m_win_status, COLOR_PAIR(mode_color) | A_BOLD);

        if (mp.valid) {
            wattron(m_win_status, COLOR_PAIR(COLOR_NORMAL));
            mvwprintw(m_win_status, 1, rx, "X:%-7d Y:%-7d", (int)mp.x, (int)mp.y);
            wattroff(m_win_status, COLOR_PAIR(COLOR_NORMAL));
        } else {
            wattron(m_win_status, COLOR_PAIR(COLOR_DIM));
            mvwprintw(m_win_status, 1, rx, "X:---     Y:---");
            wattroff(m_win_status, COLOR_PAIR(COLOR_DIM));
        }

        if (ws) {
            int alive = ws->aliveCount();
            wattron(m_win_status, COLOR_PAIR(alive > 0 ? COLOR_NORMAL : COLOR_DIM));
            if (mp.valid && alive > 0) {
                auto mobs = ws->mobs();
                float min_dist = 9999.f;
                for (const auto& mob : mobs)
                    if (!mob.isDead) {
                        float d = mob.distanceTo(mp.x, mp.y);
                        if (d < min_dist) min_dist = d;
                    }
                mvwprintw(m_win_status, 2, rx, "Mobs:%-2d Dist:%-5d", alive, (int)min_dist);
            } else {
                mvwprintw(m_win_status, 2, rx, "Mobs:%-2d", alive);
            }
            wattroff(m_win_status, COLOR_PAIR(alive > 0 ? COLOR_NORMAL : COLOR_DIM));
        } else {
            wattron(m_win_status, COLOR_PAIR(COLOR_DIM));
            mvwprintw(m_win_status, 2, rx, "KL:---");
            wattroff(m_win_status, COLOR_PAIR(COLOR_DIM));
        }
    }
}

// ─── DrawTabBar ────────────────────────────────────────────────────────────

void Dashboard::DrawTabBar() {
    if (!m_win_tabbar) return;
    werase(m_win_tabbar);

    static const char* names[4] = { " Main ", " Stats ", " Memory ", " RL " };

    // Зберігаємо межі вкладок для mouse click detection в HandleInput
    int x = 0;
    for (int i = 0; i < 4 && x < m_cols - 2; i++) {
        m_tab_start[i] = x;
        m_tab_end[i]   = x + (int)strlen(names[i]) - 1;
        bool active = (i == m_tab);

        if (active) wattron(m_win_tabbar, COLOR_PAIR(COLOR_TAB_ACTIVE) | A_BOLD);
        else         wattron(m_win_tabbar, COLOR_PAIR(COLOR_TAB_INACT));

        mvwprintw(m_win_tabbar, 0, x, "%s", names[i]);
        x += (int)strlen(names[i]);

        if (active) wattroff(m_win_tabbar, COLOR_PAIR(COLOR_TAB_ACTIVE) | A_BOLD);
        else         wattroff(m_win_tabbar, COLOR_PAIR(COLOR_TAB_INACT));
    }
}

// ─── DrawFooter ────────────────────────────────────────────────────────────

void Dashboard::DrawFooter() {
    if (!m_win_footer) return;
    werase(m_win_footer);

    wattron(m_win_footer, COLOR_PAIR(COLOR_DIM));
    mvwprintw(m_win_footer, 0, 0,
        " Q=стоп  ScrLk=стоп  P=пауза  S=налаштування  R=скинути бари"
        "  Tab=вкладка  F12=calibrate");
    wattroff(m_win_footer, COLOR_PAIR(COLOR_DIM));
}

// ─── DrawMainTab ───────────────────────────────────────────────────────────

void Dashboard::DrawMainTab() {
    if (!m_win_content) return;
    werase(m_win_content);

    int rows, cols;
    getmaxyx(m_win_content, rows, cols);

    std::lock_guard<std::mutex> lock(m_log_mutex);
    int start = std::max(0, (int)m_log.size() - rows);
    int row = 0;
    for (int i = start; i < (int)m_log.size() && row < rows; i++, row++) {
        const std::string& line = m_log[i];
        bool is_sep = (line.find("[OBJ]") != std::string::npos &&
                      (line.find("Enter:")   != std::string::npos ||
                       line.find("Exit:")    != std::string::npos ||
                       line.find("Preempt:") != std::string::npos));

        if (is_sep) wattron(m_win_content, COLOR_PAIR(COLOR_LOG_SEP) | A_BOLD);
        else         wattron(m_win_content, COLOR_PAIR(COLOR_NORMAL));

        std::string display = line;
        if ((int)display.size() > cols - 1)
            display = display.substr(0, cols - 1);
        mvwprintw(m_win_content, row, 0, "%s", display.c_str());

        if (is_sep) wattroff(m_win_content, COLOR_PAIR(COLOR_LOG_SEP) | A_BOLD);
        else         wattroff(m_win_content, COLOR_PAIR(COLOR_NORMAL));
    }
}

// ─── DrawStatsTab ──────────────────────────────────────────────────────────

void Dashboard::DrawStatsTab(const Brain& brain, const Stats& stats) {
    if (!m_win_content) return;
    werase(m_win_content);

    int rows, cols;
    getmaxyx(m_win_content, rows, cols);

    // Uptime та K/год
    time_t now = time(nullptr);
    double uptime_s = (m_t_start > 0) ? difftime(now, m_t_start) : 1.0;
    double kph      = uptime_s > 0 ? stats.kills / (uptime_s / 3600.0) : 0.0;
    int h = (int)(uptime_s / 3600);
    int m = ((int)uptime_s % 3600) / 60;
    int s = (int)uptime_s % 60;

    // Рядок заголовка
    wattron(m_win_content, COLOR_PAIR(COLOR_NORMAL) | A_BOLD);
    mvwprintw(m_win_content, 0, 1,
              "Kills:%-6d  Deaths:%-4d  Uptime:%02d:%02d:%02d  K/год:%.0f",
              stats.kills, stats.deaths, h, m, s, kph);
    wattroff(m_win_content, COLOR_PAIR(COLOR_NORMAL) | A_BOLD);

    if (rows < 3) return;

    // Метри (htop-style)
    const int lw = 8;
    const int bw = std::max(10, cols - lw - 14);
    int row = 2;

    // K/год
    DrawMeter(m_win_content, row++, 1, lw, bw,
              kph, std::max(kph * 1.5, 300.0),
              COLOR_METER_GOOD, "K/год", "%.0f");

    // K/D
    double kd = stats.deaths > 0
        ? (double)stats.kills / stats.deaths
        : (double)stats.kills;
    DrawMeter(m_win_content, row++, 1, lw, bw,
              kd, std::max(kd * 1.5, 50.0),
              COLOR_METER_GOOD, "K/D", "%.1f");

    // Deaths
    DrawMeter(m_win_content, row++, 1, lw, bw,
              (double)stats.deaths, std::max((double)stats.deaths * 1.5, 10.0),
              stats.deaths > 0 ? COLOR_METER_BAD : COLOR_METER_GOOD,
              "Deaths", "%.0f");

    // Potions
    int pots = stats.hp_potions + stats.mp_potions;
    DrawMeter(m_win_content, row++, 1, lw, bw,
              (double)pots, std::max((double)pots * 1.5, 20.0),
              COLOR_METER_WARN, "Potions", "%.0f");

    // Attacks
    DrawMeter(m_win_content, row++, 1, lw, bw,
              (double)stats.attacks, std::max((double)stats.attacks * 1.5, 1000.0),
              COLOR_METER_GOOD, "Attacks", "%.0f");

    // RL статус (якщо увімкнено)
    if (row < rows - 1 && brain.GetBotBT().isLearningEnabled()) {
        row++;
        wattron(m_win_content, COLOR_PAIR(COLOR_BUFF));
        mvwprintw(m_win_content, row, 1,
                  "[RL] eps=%.3f  loss=%.5f  upd=%d",
                  brain.GetBotBT().getRLEpsilon(),
                  brain.GetBotBT().getRLLastLoss(),
                  brain.GetBotBT().getRLUpdateCount());
        wattroff(m_win_content, COLOR_PAIR(COLOR_BUFF));
    }
}

// ─── DrawMemoryTab ─────────────────────────────────────────────────────────

void Dashboard::DrawMemoryTab(const Brain& brain) {
    if (!m_win_content) return;
    werase(m_win_content);

    const auto& mp = brain.GetMemPlayerState();
    WorldState* ws = brain.GetWorldState();
    uintptr_t   pb = brain.GetPlayerBase();
    int row = 0;

    // PlayerBase
    wattron(m_win_content, COLOR_PAIR(COLOR_NORMAL) | A_BOLD);
    mvwprintw(m_win_content, row, 1, "PlayerBase:");
    wattroff(m_win_content, COLOR_PAIR(COLOR_NORMAL) | A_BOLD);
    if (pb) {
        wattron(m_win_content, COLOR_PAIR(COLOR_ATTACK));
        mvwprintw(m_win_content, row++, 13, "0x%08lX", (unsigned long)pb);
        wattroff(m_win_content, COLOR_PAIR(COLOR_ATTACK));
    } else {
        wattron(m_win_content, COLOR_PAIR(COLOR_DEAD));
        mvwprintw(m_win_content, row++, 13, "SEARCHING...");
        wattroff(m_win_content, COLOR_PAIR(COLOR_DEAD));
    }

    // XYZ
    if (mp.valid) {
        wattron(m_win_content, COLOR_PAIR(COLOR_NORMAL));
        mvwprintw(m_win_content, row++, 1, "XYZ:         %d  %d  %d",
                  (int)mp.x, (int)mp.y, (int)mp.z);
        wattroff(m_win_content, COLOR_PAIR(COLOR_NORMAL));
    } else {
        wattron(m_win_content, COLOR_PAIR(COLOR_DIM));
        mvwprintw(m_win_content, row++, 1, "XYZ:         N/A");
        wattroff(m_win_content, COLOR_PAIR(COLOR_DIM));
    }

    // HP Offset calibration
    bool hp_ok = (mp.hp > 0 && mp.max_hp > 0);
    wattron(m_win_content, COLOR_PAIR(COLOR_NORMAL) | A_BOLD);
    mvwprintw(m_win_content, row, 1, "HP Offset:");
    wattroff(m_win_content, COLOR_PAIR(COLOR_NORMAL) | A_BOLD);
    if (hp_ok) {
        wattron(m_win_content, COLOR_PAIR(COLOR_ATTACK) | A_BOLD);
        mvwprintw(m_win_content, row++, 12, "CONFIRMED  hp=%d/%d", mp.hp, mp.max_hp);
        wattroff(m_win_content, COLOR_PAIR(COLOR_ATTACK) | A_BOLD);
    } else {
        wattron(m_win_content, COLOR_PAIR(COLOR_DEAD));
        mvwprintw(m_win_content, row++, 12, "PENDING  (AutoCalib active...)");
        wattroff(m_win_content, COLOR_PAIR(COLOR_DEAD));
    }

    row++; // порожній рядок

    // ShadowMode
    wattron(m_win_content, COLOR_PAIR(COLOR_NORMAL) | A_BOLD);
    mvwprintw(m_win_content, row++, 1, "ShadowMode:");
    wattroff(m_win_content, COLOR_PAIR(COLOR_NORMAL) | A_BOLD);

    if (brain.isShadowModeActive()) {
        size_t cmp  = brain.getShadowComparisons();
        size_t diff = brain.getShadowDiscrepancies();
        double pct  = cmp > 0 ? (double)diff / (double)cmp * 100.0 : 0.0;
        int sc = (pct < 5.0) ? COLOR_ATTACK : COLOR_METER_WARN;
        wattron(m_win_content, COLOR_PAIR(sc));
        mvwprintw(m_win_content, row++, 3,
                  "cmp:%-8zu  diff:%-8zu  avg diff:%.3f%%", cmp, diff, pct);
        wattroff(m_win_content, COLOR_PAIR(sc));
    } else {
        wattron(m_win_content, COLOR_PAIR(COLOR_DIM));
        mvwprintw(m_win_content, row++, 3, "inactive");
        wattroff(m_win_content, COLOR_PAIR(COLOR_DIM));
    }

    row++; // порожній рядок

    // KnownList
    wattron(m_win_content, COLOR_PAIR(COLOR_NORMAL) | A_BOLD);
    mvwprintw(m_win_content, row++, 1, "KnownList:");
    wattroff(m_win_content, COLOR_PAIR(COLOR_NORMAL) | A_BOLD);

    if (ws) {
        int alive = ws->aliveCount();
        int color = alive > 0 ? COLOR_ATTACK : COLOR_DIM;
        wattron(m_win_content, COLOR_PAIR(color));
        mvwprintw(m_win_content, row++, 3, "alive=%d", alive);
        wattroff(m_win_content, COLOR_PAIR(color));
    } else {
        wattron(m_win_content, COLOR_PAIR(COLOR_DIM));
        mvwprintw(m_win_content, row++, 3, "not initialized");
        wattroff(m_win_content, COLOR_PAIR(COLOR_DIM));
    }
}

// ─── DrawRLTab ─────────────────────────────────────────────────────────────

void Dashboard::DrawRLTab(const Brain& brain) {
    if (!m_win_content) return;
    werase(m_win_content);

    int rows, cols;
    getmaxyx(m_win_content, rows, cols);

    if (!brain.GetBotBT().isLearningEnabled()) {
        wattron(m_win_content, COLOR_PAIR(COLOR_DIM));
        mvwprintw(m_win_content, 1, 1, "RL вимкнено — [Learning] Enabled=false");
        wattroff(m_win_content, COLOR_PAIR(COLOR_DIM));
        return;
    }

    double eps  = brain.GetBotBT().getRLEpsilon();
    double loss = brain.GetBotBT().getRLLastLoss();
    int    upd  = brain.GetBotBT().getRLUpdateCount();

    int row = 0;
    int lw  = 8;
    int bw  = std::max(10, cols - lw - 14);

    // Заголовок
    wattron(m_win_content, COLOR_PAIR(COLOR_BUFF) | A_BOLD);
    mvwprintw(m_win_content, row++, 1,
              "ε=%.3f  loss=%.6f  updates=%d", eps, loss, upd);
    wattroff(m_win_content, COLOR_PAIR(COLOR_BUFF) | A_BOLD);

    row++;

    // Epsilon (0=жадібний, 1=випадковий)
    DrawMeter(m_win_content, row++, 1, lw, bw,
              eps, 1.0,
              eps < 0.1 ? COLOR_METER_GOOD : COLOR_METER_WARN,
              "Epsilon", "%.3f");

    // Loss (менше = краще)
    DrawMeter(m_win_content, row++, 1, lw, bw,
              std::min(loss, 1.0), 1.0,
              loss < 0.01 ? COLOR_METER_GOOD : COLOR_METER_WARN,
              "Loss", "%.5f");

    // Updates
    double upd_max = std::max((double)upd * 1.5, 1000.0);
    DrawMeter(m_win_content, row++, 1, lw, bw,
              (double)upd, upd_max,
              COLOR_METER_GOOD, "Updates", "%.0f");

    if (row < rows - 1) {
        row++;
        wattron(m_win_content, COLOR_PAIR(COLOR_DIM));
        mvwprintw(m_win_content, row, 1,
                  "Дії: Rest | Attack | Target | Patrol | Rotate | Loot");
        wattroff(m_win_content, COLOR_PAIR(COLOR_DIM));
    }
}

// ─── HandleInput ───────────────────────────────────────────────────────────

int Dashboard::HandleInput() {
    if (!m_active) return 0;
    int ch = getch();
    if (ch == ERR) return 0;

    switch (ch) {
        case 'q': case 'Q':   return 'q';
        case 'p': case 'P':   return 'p';
        case 's': case 'S':   return 's';
        case 'r': case 'R':   return 'r';
        case KEY_F(1):  m_tab = 0; return 0;
        case KEY_F(2):  m_tab = 1; return 0;
        case KEY_F(3):  m_tab = 2; return 0;
        case KEY_F(4):  m_tab = 3; return 0;
        case '\t':      m_tab = (m_tab + 1) % 4; return 0;
        case KEY_MOUSE: {
            MEVENT ev;
            if (getmouse(&ev) == OK && m_win_tabbar) {
                int tab_row = getbegy(m_win_tabbar);
                bool is_click = (ev.bstate & BUTTON1_RELEASED);
                if (ev.y == tab_row && is_click) {
                    for (int i = 0; i < 4; i++) {
                        if (ev.x >= m_tab_start[i] && ev.x <= m_tab_end[i]) {
                            m_tab = i;
                            break;
                        }
                    }
                }
            }
            return 0;
        }
        case KEY_RESIZE: RecreateWindows(); return 0;
        default:              return ch;
    }
}

// ─── StateColor / StateEmoji ───────────────────────────────────────────────

int Dashboard::StateColor(const std::string& s) {
    if (s == "Target") return COLOR_TARGET;
    if (s == "Attack") return COLOR_ATTACK;
    if (s == "Loot")   return COLOR_LOOT;
    if (s == "Dead")   return COLOR_DEAD;
    if (s == "Buff")   return COLOR_BUFF;
    return COLOR_IDLE;
}

const char* Dashboard::StateEmoji(const std::string& s) {
    if (s == "Target") return "[SEARCH]";
    if (s == "Attack") return "[ATTACK]";
    if (s == "Loot")   return "[LOOT]  ";
    if (s == "Dead")   return "[DEAD]  ";
    if (s == "Buff")   return "[BUFF]  ";
    return "[IDLE]  ";
}

// ─── EditTextField ─────────────────────────────────────────────────────────

bool Dashboard::EditTextField(WINDOW* win, int y, int x, int w, std::string& value) {
    echo();
    curs_set(1);
    char buf[256] = {};
    strncpy(buf, value.c_str(), sizeof(buf) - 1);
    mvwgetnstr(win, y, x, buf, std::min(w, (int)sizeof(buf) - 1));
    noecho();
    curs_set(0);
    if (buf[0] != '\0') { value = buf; return true; }
    return false;
}

// ─── ShowSettings ──────────────────────────────────────────────────────────

void Dashboard::ShowSettings(Config& cfg, const std::string& config_path) {
    int ow = std::min(m_cols - 4, 66);
    int oh = 18;
    int oy = (m_rows - oh) / 2;
    int ox = (m_cols - ow) / 2;

    WINDOW* overlay = newwin(oh, ow, oy, ox);
    keypad(overlay, TRUE);
    timeout(0);
    wtimeout(overlay, -1);

    struct Field {
        const char* label;
        enum Type { STR, INT, DOUBLE, BOOL } type;
        std::string* str_val;
        int*    int_val;
        double* dbl_val;
        bool*   bool_val;
        int     step;
    };

    auto keys_to_str = [&](const std::vector<Input::KeyboardKey>& keys) -> std::string {
        std::string r;
        for (size_t i = 0; i < keys.size(); i++) {
            if (i) r += ",";
            switch (keys[i]) {
                case Input::KeyboardKey::F1: r += "F1"; break;
                case Input::KeyboardKey::F2: r += "F2"; break;
                case Input::KeyboardKey::F3: r += "F3"; break;
                case Input::KeyboardKey::F4: r += "F4"; break;
                case Input::KeyboardKey::F5: r += "F5"; break;
                case Input::KeyboardKey::F6: r += "F6"; break;
                case Input::KeyboardKey::F7: r += "F7"; break;
                case Input::KeyboardKey::F8: r += "F8"; break;
                case Input::KeyboardKey::Two:   r += "2"; break;
                case Input::KeyboardKey::Three: r += "3"; break;
                case Input::KeyboardKey::Four:  r += "4"; break;
                case Input::KeyboardKey::Five:  r += "5"; break;
                case Input::KeyboardKey::Six:   r += "6"; break;
                case Input::KeyboardKey::Seven: r += "7"; break;
                default: r += "?"; break;
            }
        }
        return r;
    };

    std::string s_macros = keys_to_str(cfg.target_macro_keys);
    std::string s_attack = keys_to_str(cfg.attack_keys);
    std::string s_buffs  = keys_to_str(cfg.buff_keys);

    std::vector<Field> fields = {
        {"Клас [Mage/Archer/Spoiler]", Field::STR,    &cfg.char_class,    nullptr,          nullptr,           nullptr,       0},
        {"Макроси /target (слоти)",    Field::STR,    &s_macros,          nullptr,          nullptr,           nullptr,       0},
        {"Клавіші атаки",              Field::STR,    &s_attack,          nullptr,          nullptr,           nullptr,       0},
        {"Attack wait (с)",            Field::DOUBLE, nullptr,            nullptr,          &cfg.attack_wait,  nullptr,       1},
        {"HP поріг %",                 Field::INT,    nullptr,            &cfg.hp_threshold,nullptr,           nullptr,       5},
        {"MP поріг %",                 Field::INT,    nullptr,            &cfg.mp_threshold,nullptr,           nullptr,       5},
        {"CP поріг %",                 Field::INT,    nullptr,            &cfg.cp_threshold,nullptr,           nullptr,       5},
        {"Лут кількість",              Field::INT,    nullptr,            &cfg.loot_count,  nullptr,           nullptr,       1},
        {"Buff клавіші",               Field::STR,    &s_buffs,           nullptr,          nullptr,           nullptr,       0},
        {"Buff інтервал (с)",          Field::INT,    nullptr,            &cfg.buff_interval,nullptr,          nullptr,      30},
        {"Debug overlay",              Field::BOOL,   nullptr,            nullptr,          nullptr,           &cfg.debug,    0},
    };

    int sel = 0;
    bool running = true;

    while (running) {
        werase(overlay);
        box(overlay, 0, 0);

        wattron(overlay, COLOR_PAIR(COLOR_TITLE) | A_BOLD);
        mvwprintw(overlay, 0, 2, " Налаштування ");
        wattroff(overlay, COLOR_PAIR(COLOR_TITLE) | A_BOLD);

        wattron(overlay, COLOR_PAIR(COLOR_DIM));
        mvwprintw(overlay, 1, 2, "↑↓=навігація  ←→=змінити  Enter=редагувати  S=зберегти  ESC=вийти");
        wattroff(overlay, COLOR_PAIR(COLOR_DIM));

        for (int i = 0; i < (int)fields.size(); i++) {
            auto& f = fields[i];
            bool active = (i == sel);
            if (active) wattron(overlay, A_REVERSE);

            mvwprintw(overlay, i + 3, 2, "%-28s", f.label);

            char val[64] = {};
            switch (f.type) {
                case Field::STR:    snprintf(val, sizeof(val), "%s",    f.str_val->c_str()); break;
                case Field::INT:    snprintf(val, sizeof(val), "%d",    *f.int_val);         break;
                case Field::DOUBLE: snprintf(val, sizeof(val), "%.1f",  *f.dbl_val);         break;
                case Field::BOOL:   snprintf(val, sizeof(val), "%s",    *f.bool_val ? "true" : "false"); break;
            }
            mvwprintw(overlay, i + 3, 32, "%-28s", val);

            if (active) wattroff(overlay, A_REVERSE);
        }

        wattron(overlay, COLOR_PAIR(COLOR_ATTACK) | A_BOLD);
        mvwprintw(overlay, oh - 2, 2, " S=Зберегти в %s  ESC=Скасувати ", config_path.c_str());
        wattroff(overlay, COLOR_PAIR(COLOR_ATTACK) | A_BOLD);

        wrefresh(overlay);

        int ch = wgetch(overlay);
        auto& f = fields[sel];

        switch (ch) {
            case KEY_UP:   sel = (sel - 1 + (int)fields.size()) % (int)fields.size(); break;
            case KEY_DOWN: sel = (sel + 1) % (int)fields.size(); break;
            case KEY_LEFT:
                if (f.type == Field::INT    && f.int_val)  *f.int_val  = std::max(0, *f.int_val - f.step);
                if (f.type == Field::DOUBLE && f.dbl_val)  *f.dbl_val  = std::max(0.1, *f.dbl_val - 0.1);
                if (f.type == Field::BOOL   && f.bool_val) *f.bool_val = !(*f.bool_val);
                break;
            case KEY_RIGHT:
                if (f.type == Field::INT    && f.int_val)  *f.int_val  += f.step;
                if (f.type == Field::DOUBLE && f.dbl_val)  *f.dbl_val  += 0.1;
                if (f.type == Field::BOOL   && f.bool_val) *f.bool_val = !(*f.bool_val);
                break;
            case '\n': case KEY_ENTER:
                if (f.type == Field::STR && f.str_val)
                    EditTextField(overlay, sel + 3, 32, 27, *f.str_val);
                break;
            case 's': case 'S':
                cfg.Save(config_path);
                wattron(overlay, COLOR_PAIR(COLOR_ATTACK) | A_BOLD);
                mvwprintw(overlay, oh - 2, 2, " Збережено! Натисни будь-яку клавішу...     ");
                wattroff(overlay, COLOR_PAIR(COLOR_ATTACK) | A_BOLD);
                wrefresh(overlay);
                wgetch(overlay);
                running = false;
                break;
            case 27:
                running = false;
                break;
        }
    }

    delwin(overlay);
    timeout(1);
    RecreateWindows();
}
