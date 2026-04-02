#pragma once

#include <string>
#include <deque>
#include <functional>
#include <mutex>

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
    COLOR_NORMAL  = 1,
    COLOR_TITLE   = 2,  // cyan bold
    COLOR_IDLE    = 3,  // сірий
    COLOR_TARGET  = 4,  // жовтий
    COLOR_ATTACK  = 5,  // зелений
    COLOR_LOOT    = 6,  // блакитний
    COLOR_DEAD    = 7,  // червоний
    COLOR_BUFF    = 8,  // magenta
    COLOR_HP_BAR  = 9,  // червоний
    COLOR_MP_BAR  = 10, // синій
    COLOR_CP_BAR  = 11, // жовтий
    COLOR_LOG_SEP = 12, // жовтий (роздільник переходів)
    COLOR_DIM     = 13, // сірий (підказки)
};

class Dashboard {
public:
    Dashboard();
    ~Dashboard();

    // Ініціалізація ncurses. Викликати один раз перед циклом.
    void Init();

    // Оновити весь dashboard. Викликати кожен тік.
    void Update(const Brain& brain, double fps);

    // Додати рядок до лог-панелі (потокобезпечно).
    void AddLog(const std::string& msg);

    // Обробити ввід без блокування.
    // Повертає: 0=нічого, 'q'=стоп, 'p'=пауза, 's'=settings, 'r'=reset
    int HandleInput();

    // Показати overlay налаштувань (блокуючий діалог).
    // Зміни застосовуються до cfg і зберігаються в rdga1bot.ini.
    void ShowSettings(Config& cfg, const std::string& config_path);

    // Завершення ncurses.
    void Shutdown();

    bool IsActive() const { return m_active; }

private:
    bool m_active = false;
    bool m_paused = false;

    // Рядки лог-панелі (останні MAX_LOG)
    std::deque<std::string> m_log;
    std::mutex m_log_mutex;
    static constexpr int MAX_LOG = 200;

    // Субвікна ncurses
    WINDOW* m_win_header  = nullptr; // рядок заголовку
    WINDOW* m_win_status  = nullptr; // HP/MP/CP + статистика
    WINDOW* m_win_log     = nullptr; // прокручуваний лог
    WINDOW* m_win_footer  = nullptr; // підказки клавіш

    int m_rows = 0; // розмір термінала
    int m_cols = 0;

    // ─── Малювання ───────────────────────────────────────────────
    void RecreateWindows();
    void DrawHeader(Brain::State state, double fps, bool paused);
    void DrawStatus(const Brain& brain, const Eyes::Me& me, const Eyes::Target& tgt, const Stats& stats);
    void DrawLog();
    void DrawFooter();

    // Малює кольоровий бар (символами ██░░)
    void DrawBar(WINDOW* win, int y, int x, int bar_width,
                 int percent, int color_pair, const char* label);

    // Колір для стану
    static int StateColor(Brain::State s);
    static const char* StateEmoji(Brain::State s);

    // ─── Панель налаштувань ───────────────────────────────────────
    void DrawSettingsOverlay(WINDOW* win, const Config& cfg, int sel, int edit_mode);
    bool EditTextField(WINDOW* win, int y, int x, int w, std::string& value);
};
