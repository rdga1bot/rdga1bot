// Dashboard.h включається ПЕРШИМ (тягне Eyes.h → opencv).
// Це гарантує що OpenCV заголовки парсяться БЕЗ ncurses макросів.
#include "Dashboard.h"

#include <algorithm>
#include <clocale>
#include <cstring>
#include <ctime>

// ncurses включається ПІСЛЯ всіх OpenCV заголовків, щоб його макроси
// (clear, refresh, move, timeout, PANORAMA, тощо) не конфліктували з opencv.
#include <ncurses.h>

// ─── Конструктор / деструктор ──────────────────────────────────────────────

Dashboard::Dashboard() = default;

Dashboard::~Dashboard() {
    if (m_active) Shutdown();
}

// ─── Ініціалізація ─────────────────────────────────────────────────────────

void Dashboard::Init() {
    setlocale(LC_ALL, ""); // підтримка UTF-8

    initscr();
    cbreak();
    noecho();
    curs_set(0);     // прихований курсор
    keypad(stdscr, TRUE);
    timeout(1);      // HandleInput не блокує (1мс)

    if (!has_colors()) {
        endwin();
        return; // термінал без кольорів — залишаємось без TUI
    }

    start_color();
    use_default_colors();

    // Ініціалізація кольорових пар
    init_pair(COLOR_NORMAL,  COLOR_WHITE,   -1);
    init_pair(COLOR_TITLE,   COLOR_CYAN,    -1);
    init_pair(COLOR_IDLE,    COLOR_WHITE,   -1);
    init_pair(COLOR_TARGET,  COLOR_YELLOW,  -1);
    init_pair(COLOR_ATTACK,  COLOR_GREEN,   -1);
    init_pair(COLOR_LOOT,    COLOR_CYAN,    -1);
    init_pair(COLOR_DEAD,    COLOR_RED,     -1);
    init_pair(COLOR_BUFF,    COLOR_MAGENTA, -1);
    init_pair(COLOR_HP_BAR,  COLOR_RED,     -1);
    init_pair(COLOR_MP_BAR,  COLOR_BLUE,    -1);
    init_pair(COLOR_CP_BAR,  COLOR_YELLOW,  -1);
    init_pair(COLOR_LOG_SEP, COLOR_YELLOW,  -1);
    init_pair(COLOR_DIM,     COLOR_WHITE,   -1);

    getmaxyx(stdscr, m_rows, m_cols);
    RecreateWindows();

    m_active = true;
}

void Dashboard::RecreateWindows() {
    // Видаляємо старі вікна
    if (m_win_header)  { delwin(m_win_header);  m_win_header  = nullptr; }
    if (m_win_status)  { delwin(m_win_status);  m_win_status  = nullptr; }
    if (m_win_log)     { delwin(m_win_log);     m_win_log     = nullptr; }
    if (m_win_footer)  { delwin(m_win_footer);  m_win_footer  = nullptr; }

    getmaxyx(stdscr, m_rows, m_cols);

    // Розміщення: header(1) + border(1) + status(3) + border(1) + log(N) + border(1) + footer(1)
    int header_h  = 1;
    int status_h  = 3;
    int footer_h  = 1;
    int fixed_h   = header_h + 2 + status_h + 2 + footer_h + 1; // рядки рамки
    int log_h     = std::max(3, m_rows - fixed_h);

    int row = 0;
    m_win_header = newwin(header_h, m_cols, row, 0); row += header_h;
    // separator row — малюємо в stdscr
    row++; // border
    m_win_status = newwin(status_h, m_cols, row, 0); row += status_h;
    row++; // border
    m_win_log    = newwin(log_h,    m_cols, row, 0); row += log_h;
    row++; // border
    m_win_footer = newwin(footer_h, m_cols, row, 0);

    scrollok(m_win_log, FALSE); // малюємо вручну
}

// ─── Shutdown ──────────────────────────────────────────────────────────────

void Dashboard::Shutdown() {
    if (!m_active) return;
    if (m_win_header)  delwin(m_win_header);
    if (m_win_status)  delwin(m_win_status);
    if (m_win_log)     delwin(m_win_log);
    if (m_win_footer)  delwin(m_win_footer);
    endwin();
    m_active = false;
}

// ─── AddLog ────────────────────────────────────────────────────────────────

void Dashboard::AddLog(const std::string& msg) {
    // Поточний час
    time_t now = time(nullptr);
    struct tm* t = localtime(&now);
    char ts[16];
    strftime(ts, sizeof(ts), "%H:%M:%S", t);

    std::string line = std::string(ts) + " " + msg;

    std::lock_guard<std::mutex> lock(m_log_mutex);
    m_log.push_back(line);
    while ((int)m_log.size() > MAX_LOG) m_log.pop_front();
}

// ─── Update ────────────────────────────────────────────────────────────────

void Dashboard::Update(const Brain& brain, double fps) {
    if (!m_active) return;

    // Перевірка зміни розміру термінала (SIGWINCH)
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    if (rows != m_rows || cols != m_cols) {
        endwin();
        refresh();
        getmaxyx(stdscr, m_rows, m_cols);
        RecreateWindows();
    }

    clear();

    Eyes::Me  me_def{};
    Eyes::Target tgt_def{};
    const auto& me  = brain.Me().value_or(me_def);
    const auto& tgt = brain.Target().value_or(tgt_def);

    DrawHeader(brain.GetState(), fps, brain.IsPaused());

    // Горизонтальний роздільник після header
    attron(COLOR_PAIR(COLOR_DIM));
    mvhline(1, 0, ACS_HLINE, m_cols);
    attroff(COLOR_PAIR(COLOR_DIM));

    DrawStatus(me, tgt, brain.GetStats());

    // Роздільник після status
    attron(COLOR_PAIR(COLOR_DIM));
    mvhline(1 + 1 + 3, 0, ACS_HLINE, m_cols);
    attroff(COLOR_PAIR(COLOR_DIM));

    DrawLog();

    // Роздільник перед footer
    int log_end_row = 1 + 1 + 3 + 1 + std::max(3, m_rows - (1 + 2 + 3 + 2 + 1 + 1));
    attron(COLOR_PAIR(COLOR_DIM));
    mvhline(log_end_row, 0, ACS_HLINE, m_cols);
    attroff(COLOR_PAIR(COLOR_DIM));

    DrawFooter();

    refresh();
    if (m_win_header)  wrefresh(m_win_header);
    if (m_win_status)  wrefresh(m_win_status);
    if (m_win_log)     wrefresh(m_win_log);
    if (m_win_footer)  wrefresh(m_win_footer);
}

// ─── DrawHeader ────────────────────────────────────────────────────────────

void Dashboard::DrawHeader(Brain::State state, double fps, bool paused) {
    if (!m_win_header) return;
    werase(m_win_header);

    // Ліва частина: назва + версія
    wattron(m_win_header, COLOR_PAIR(COLOR_TITLE) | A_BOLD);
    mvwprintw(m_win_header, 0, 0, " rdga1bot");
    wattroff(m_win_header, COLOR_PAIR(COLOR_TITLE) | A_BOLD);

    // Індикатор паузи
    if (paused) {
        wattron(m_win_header, COLOR_PAIR(COLOR_DEAD) | A_BOLD | A_BLINK);
        mvwprintw(m_win_header, 0, 11, " ПАУЗА ");
        wattroff(m_win_header, COLOR_PAIR(COLOR_DEAD) | A_BOLD | A_BLINK);
    }

    // Права частина: FPS + стан
    int state_color = StateColor(state);
    const char* state_name = Brain::StateName(state);

    char right[64];
    snprintf(right, sizeof(right), "FPS:%-3d  %s ", (int)fps, state_name);
    int rx = m_cols - (int)strlen(right);
    if (rx > 0) {
        wattron(m_win_header, COLOR_PAIR(state_color) | A_BOLD);
        mvwprintw(m_win_header, 0, rx, "%s", right);
        wattroff(m_win_header, COLOR_PAIR(state_color) | A_BOLD);
    }
}

// ─── DrawBar ───────────────────────────────────────────────────────────────

void Dashboard::DrawBar(WINDOW* win, int y, int x, int bar_width,
                        int percent, int color_pair, const char* label) {
    percent = std::max(0, std::min(100, percent));
    int filled = bar_width * percent / 100;

    wattron(win, COLOR_PAIR(COLOR_DIM));
    mvwprintw(win, y, x, "%s ", label);
    wattroff(win, COLOR_PAIR(COLOR_DIM));

    int bx = x + (int)strlen(label) + 1;

    // Заповнені блоки
    wattron(win, COLOR_PAIR(color_pair) | A_BOLD);
    for (int i = 0; i < filled && i < bar_width; i++) {
        mvwaddstr(win, y, bx + i, "\u2588"); // █
    }
    wattroff(win, COLOR_PAIR(color_pair) | A_BOLD);

    // Порожні блоки
    wattron(win, COLOR_PAIR(COLOR_DIM));
    for (int i = filled; i < bar_width; i++) {
        mvwaddstr(win, y, bx + i, "\u2591"); // ░
    }
    wattroff(win, COLOR_PAIR(COLOR_DIM));

    // Число після бару
    wattron(win, COLOR_PAIR(color_pair));
    mvwprintw(win, y, bx + bar_width + 1, "%3d%%", percent);
    wattroff(win, COLOR_PAIR(color_pair));
}

// ─── DrawStatus ────────────────────────────────────────────────────────────

void Dashboard::DrawStatus(const Eyes::Me& me, const Eyes::Target& tgt, const Stats& stats) {
    if (!m_win_status) return;
    werase(m_win_status);

    // Ширина бару: половина екрану мінус мітки та числа
    int bar_w = std::max(10, m_cols / 2 - 16);

    DrawBar(m_win_status, 0, 1, bar_w, me.hp, COLOR_HP_BAR, "HP");
    DrawBar(m_win_status, 1, 1, bar_w, me.mp, COLOR_MP_BAR, "MP");
    DrawBar(m_win_status, 2, 1, bar_w, me.cp, COLOR_CP_BAR, "CP");

    // Права колонка: статистика і target HP
    int rx = m_cols / 2 + 2;
    if (tgt.hp > 0) {
        DrawBar(m_win_status, 0, rx, bar_w, tgt.hp, COLOR_HP_BAR, "Target");
    } else {
        wattron(m_win_status, COLOR_PAIR(COLOR_DIM));
        mvwprintw(m_win_status, 0, rx, "Target: ---");
        wattroff(m_win_status, COLOR_PAIR(COLOR_DIM));
    }

    wattron(m_win_status, COLOR_PAIR(COLOR_NORMAL));
    mvwprintw(m_win_status, 1, rx, "Kills:%-4d Deaths:%-3d",
              stats.kills, stats.deaths);
    // K/D ratio і potions
    char kd_buf[32];
    if (stats.deaths > 0)
        snprintf(kd_buf, sizeof(kd_buf), "%.1f", (double)stats.kills / stats.deaths);
    else
        snprintf(kd_buf, sizeof(kd_buf), "%d", stats.kills);
    mvwprintw(m_win_status, 2, rx, "K/D:%-6s Pots:%d",
              kd_buf, stats.hp_potions + stats.mp_potions);
    wattroff(m_win_status, COLOR_PAIR(COLOR_NORMAL));
}

// ─── DrawLog ───────────────────────────────────────────────────────────────

void Dashboard::DrawLog() {
    if (!m_win_log) return;
    werase(m_win_log);

    int rows, cols;
    getmaxyx(m_win_log, rows, cols);

    std::lock_guard<std::mutex> lock(m_log_mutex);

    // Показуємо останні `rows` рядків
    int start = std::max(0, (int)m_log.size() - rows);
    int row = 0;
    for (int i = start; i < (int)m_log.size() && row < rows; i++, row++) {
        const std::string& line = m_log[i];

        // Виявляємо рядки переходу стану (містять "──────")
        bool is_separator = (line.find("->") != std::string::npos &&
                             (line.find("[STATE]") != std::string::npos ||
                              line.find("ATTACKING") != std::string::npos ||
                              line.find("LOOTING") != std::string::npos ||
                              line.find("TARGETING") != std::string::npos));

        if (is_separator) {
            wattron(m_win_log, COLOR_PAIR(COLOR_LOG_SEP) | A_BOLD);
        } else {
            wattron(m_win_log, COLOR_PAIR(COLOR_NORMAL));
        }

        // Обрізаємо якщо довше за ширину
        std::string display = line;
        if ((int)display.size() > cols - 1) {
            display = display.substr(0, cols - 1);
        }
        mvwprintw(m_win_log, row, 0, "%s", display.c_str());

        if (is_separator) {
            wattroff(m_win_log, COLOR_PAIR(COLOR_LOG_SEP) | A_BOLD);
        } else {
            wattroff(m_win_log, COLOR_PAIR(COLOR_NORMAL));
        }
    }
}

// ─── DrawFooter ────────────────────────────────────────────────────────────

void Dashboard::DrawFooter() {
    if (!m_win_footer) return;
    werase(m_win_footer);

    wattron(m_win_footer, COLOR_PAIR(COLOR_DIM));
    mvwprintw(m_win_footer, 0, 0,
        " Q=стоп  ScrLk=стоп  P=пауза  S=налаштування  R=скинути бари  F12=calibrate");
    wattroff(m_win_footer, COLOR_PAIR(COLOR_DIM));
}

// ─── HandleInput ───────────────────────────────────────────────────────────

int Dashboard::HandleInput() {
    if (!m_active) return 0;
    int ch = getch(); // не блокує (timeout=1)
    if (ch == ERR) return 0;

    switch (ch) {
        case 'q': case 'Q': return 'q';
        case 'p': case 'P': return 'p';
        case 's': case 'S': return 's';
        case 'r': case 'R': return 'r';
        case KEY_RESIZE:
            RecreateWindows();
            return 0;
        default:
            return ch;
    }
}

// ─── StateColor / StateEmoji ───────────────────────────────────────────────

int Dashboard::StateColor(Brain::State s) {
    switch (s) {
        case Brain::State::Targeting: return COLOR_TARGET;
        case Brain::State::Attacking: return COLOR_ATTACK;
        case Brain::State::Looting:   return COLOR_LOOT;
        case Brain::State::Dead:      return COLOR_DEAD;
        case Brain::State::Buffing:   return COLOR_BUFF;
        default:                      return COLOR_IDLE;
    }
}

const char* Dashboard::StateEmoji(Brain::State s) {
    switch (s) {
        case Brain::State::Targeting: return "[SEARCH]";
        case Brain::State::Attacking: return "[ATTACK]";
        case Brain::State::Looting:   return "[LOOT]  ";
        case Brain::State::Dead:      return "[DEAD]  ";
        case Brain::State::Buffing:   return "[BUFF]  ";
        default:                      return "[IDLE]  ";
    }
}

// ─── EditTextField (мінімальний inline редактор) ───────────────────────────

bool Dashboard::EditTextField(WINDOW* win, int y, int x, int w, std::string& value) {
    echo();
    curs_set(1);
    char buf[256] = {};
    strncpy(buf, value.c_str(), sizeof(buf) - 1);
    mvwgetnstr(win, y, x, buf, std::min(w, (int)sizeof(buf) - 1));
    noecho();
    curs_set(0);
    if (buf[0] != '\0') {
        value = buf;
        return true;
    }
    return false;
}

// ─── ShowSettings ──────────────────────────────────────────────────────────

void Dashboard::ShowSettings(Config& cfg, const std::string& config_path) {
    // Overlay вікно поверх всього
    int ow = std::min(m_cols - 4, 66);
    int oh = 18;
    int oy = (m_rows - oh) / 2;
    int ox = (m_cols - ow) / 2;

    WINDOW* overlay = newwin(oh, ow, oy, ox);
    keypad(overlay, TRUE);
    timeout(0); // без блокування для overlay (власний getch)
    wtimeout(overlay, -1); // overlay — блокуючий

    // Поля: label, pointer to string/int, type
    struct Field {
        const char* label;
        enum Type { STR, INT, DOUBLE, BOOL } type;
        std::string* str_val;
        int*    int_val;
        double* dbl_val;
        bool*   bool_val;
        int     step;     // крок для ←→
    };

    // Серіалізуємо векторні поля у рядки для редагування
    auto keys_to_str = [&](const std::vector<Input::KeyboardKey>& keys) -> std::string {
        std::string r;
        for (size_t i = 0; i < keys.size(); i++) {
            if (i) r += ",";
            // Знаходимо ім'я через StringToKeyboardKey у зворотньому напрямку
            // Простий fallback: беремо номер скан-коду
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

    // Тимчасові рядки для редагування векторів
    std::string s_macros  = keys_to_str(cfg.target_macro_keys);
    std::string s_attack  = keys_to_str(cfg.attack_keys);
    std::string s_buffs   = keys_to_str(cfg.buff_keys);

    std::vector<Field> fields = {
        {"Клас [Mage/Archer/Spoiler]", Field::STR, &cfg.char_class,   nullptr, nullptr, nullptr, 0},
        {"Макроси /target (слоти)",    Field::STR, &s_macros,         nullptr, nullptr, nullptr, 0},
        {"Клавіші атаки",              Field::STR, &s_attack,         nullptr, nullptr, nullptr, 0},
        {"Attack wait (с)",            Field::DOUBLE, nullptr,        nullptr, &cfg.attack_wait, nullptr, 1},
        {"HP поріг %",                 Field::INT, nullptr,           &cfg.hp_threshold, nullptr, nullptr, 5},
        {"MP поріг %",                 Field::INT, nullptr,           &cfg.mp_threshold, nullptr, nullptr, 5},
        {"CP поріг %",                 Field::INT, nullptr,           &cfg.cp_threshold, nullptr, nullptr, 5},
        {"Лут кількість",              Field::INT, nullptr,           &cfg.loot_count, nullptr, nullptr, 1},
        {"Buff клавіші",               Field::STR, &s_buffs,          nullptr, nullptr, nullptr, 0},
        {"Buff інтервал (с)",          Field::INT, nullptr,           &cfg.buff_interval, nullptr, nullptr, 30},
        {"Debug overlay",              Field::BOOL, nullptr,          nullptr, nullptr, &cfg.debug, 0},
    };

    int sel = 0;
    bool running = true;

    while (running) {
        werase(overlay);
        box(overlay, 0, 0);

        // Заголовок
        wattron(overlay, COLOR_PAIR(COLOR_TITLE) | A_BOLD);
        mvwprintw(overlay, 0, 2, " Налаштування ");
        wattroff(overlay, COLOR_PAIR(COLOR_TITLE) | A_BOLD);

        wattron(overlay, COLOR_PAIR(COLOR_DIM));
        mvwprintw(overlay, 1, 2, "↑↓=навігація  ←→=змінити  Enter=редагувати  S=зберегти  ESC=вийти");
        wattroff(overlay, COLOR_PAIR(COLOR_DIM));

        // Поля
        for (int i = 0; i < (int)fields.size(); i++) {
            auto& f = fields[i];
            bool active = (i == sel);

            // Виділення активного поля
            if (active) wattron(overlay, A_REVERSE);

            // Мітка
            mvwprintw(overlay, i + 3, 2, "%-28s", f.label);

            // Значення
            char val[64] = {};
            switch (f.type) {
                case Field::STR:
                    snprintf(val, sizeof(val), "%s", f.str_val->c_str());
                    break;
                case Field::INT:
                    snprintf(val, sizeof(val), "%d", *f.int_val);
                    break;
                case Field::DOUBLE:
                    snprintf(val, sizeof(val), "%.1f", *f.dbl_val);
                    break;
                case Field::BOOL:
                    snprintf(val, sizeof(val), "%s", *f.bool_val ? "true" : "false");
                    break;
            }
            mvwprintw(overlay, i + 3, 32, "%-28s", val);

            if (active) wattroff(overlay, A_REVERSE);
        }

        // Підказка внизу
        wattron(overlay, COLOR_PAIR(COLOR_ATTACK) | A_BOLD);
        mvwprintw(overlay, oh - 2, 2, " S=Зберегти в %s  ESC=Скасувати ", config_path.c_str());
        wattroff(overlay, COLOR_PAIR(COLOR_ATTACK) | A_BOLD);

        wrefresh(overlay);

        int ch = wgetch(overlay);
        auto& f = fields[sel];

        switch (ch) {
            case KEY_UP:
                sel = (sel - 1 + (int)fields.size()) % (int)fields.size();
                break;
            case KEY_DOWN:
                sel = (sel + 1) % (int)fields.size();
                break;

            case KEY_LEFT:
                // Зменшити числове значення
                if (f.type == Field::INT && f.int_val)
                    *f.int_val = std::max(0, *f.int_val - f.step);
                else if (f.type == Field::DOUBLE && f.dbl_val)
                    *f.dbl_val = std::max(0.1, *f.dbl_val - 0.1);
                else if (f.type == Field::BOOL && f.bool_val)
                    *f.bool_val = !(*f.bool_val);
                break;

            case KEY_RIGHT:
                // Збільшити числове значення
                if (f.type == Field::INT && f.int_val)
                    *f.int_val += f.step;
                else if (f.type == Field::DOUBLE && f.dbl_val)
                    *f.dbl_val += 0.1;
                else if (f.type == Field::BOOL && f.bool_val)
                    *f.bool_val = !(*f.bool_val);
                break;

            case '\n': case KEY_ENTER:
                // Редагування текстового поля
                if (f.type == Field::STR && f.str_val) {
                    EditTextField(overlay, sel + 3, 32, 27, *f.str_val);
                }
                break;

            case 's': case 'S':
                // Застосувати рядки макросів назад до конфіга і зберегти
                cfg.Save(config_path);
                // Показати підтвердження
                wattron(overlay, COLOR_PAIR(COLOR_ATTACK) | A_BOLD);
                mvwprintw(overlay, oh - 2, 2, " Збережено! Натисни будь-яку клавішу...     ");
                wattroff(overlay, COLOR_PAIR(COLOR_ATTACK) | A_BOLD);
                wrefresh(overlay);
                wgetch(overlay);
                running = false;
                break;

            case 27: // ESC
                running = false;
                break;
        }
    }

    delwin(overlay);
    timeout(1); // відновлюємо non-blocking для головного циклу
    RecreateWindows(); // оновлюємо після overlay
}
