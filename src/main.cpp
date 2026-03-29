#include <iostream>
#include <string>
#include <optional>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>

#include <opencv2/opencv.hpp>

#include "Capture.h"
#include "Window.h"
#include "Eyes.h"
#include "Hands.h"
#include "Brain.h"
#include "Config.h"
#include "Dashboard.h"
#include "FPS.h"
#include "Utils.h"
#include "Intercept.h"
#include <dirent.h>
#include <cctype>
#include <fstream>
#include "MemReader.h"
#include "offset_scanner.h"
#include "world_state.h"

// ─── Signal handling (збереження stats при Ctrl+C / kill) ──────────────────
static std::function<void()> g_cleanup;
static void signal_handler(int /*sig*/) {
    if (g_cleanup) g_cleanup();
    _exit(0);
}

// ─── F12 HSV auto-suggest ──────────────────────────────────────────────────
// Аналізує зображення бару і повертає рядок з рекомендованими HSV значеннями
static std::string HSVSuggest(const cv::Mat& bar_bgr, const std::string& name) {
    if (bar_bgr.empty()) return "";
    cv::Mat hsv;
    cv::cvtColor(bar_bgr, hsv, cv::COLOR_BGR2HSV);
    // Маска: ігноруємо дуже темні пікселі (фон)
    cv::Mat mask;
    cv::inRange(hsv, cv::Scalar(0, 30, 30), cv::Scalar(180, 255, 255), mask);
    if (cv::countNonZero(mask) < 10) return "";
    cv::Scalar mean_hsv, stddev_hsv;
    cv::meanStdDev(hsv, mean_hsv, stddev_hsv, mask);
    auto clamp = [](double v, double lo, double hi) { return std::max(lo, std::min(hi, v)); };
    int h0 = (int)clamp(mean_hsv[0] - stddev_hsv[0] * 2, 0, 179);
    int s0 = (int)clamp(mean_hsv[1] - stddev_hsv[1] * 2, 0, 255);
    int v0 = (int)clamp(mean_hsv[2] - stddev_hsv[2] * 2, 0, 255);
    int h1 = (int)clamp(mean_hsv[0] + stddev_hsv[0] * 2, 0, 179);
    int s1 = (int)clamp(mean_hsv[1] + stddev_hsv[1] * 2, 0, 255);
    int v1 = (int)clamp(mean_hsv[2] + stddev_hsv[2] * 2, 0, 255);
    return "[CALIB] " + name + "FromHSV = " + std::to_string(h0) + "," +
           std::to_string(s0) + "," + std::to_string(v0) + "\n"
           "[CALIB] " + name + "ToHSV   = " + std::to_string(h1) + "," +
           std::to_string(s1) + "," + std::to_string(v1);
}

// Застосувати всі клавіші та налаштування з Config до Hands, Eyes, Brain
static void ApplyConfig(const Config& cfg, Hands& hands, Eyes& eyes, Brain& brain,
                        MemReader* mem = nullptr) {
    hands.m_next_target_key   = cfg.next_target_key;
    hands.m_target_macro_keys = cfg.target_macro_keys;
    hands.m_attack_keys       = cfg.attack_keys;
    hands.m_buff_keys         = cfg.buff_keys;
    hands.m_spoil_key         = cfg.spoil_key;
    hands.m_sweep_key         = cfg.sweep_key;
    hands.m_pick_up_key       = cfg.loot_key;
    hands.m_restore_hp_key    = cfg.hp_key;
    hands.m_restore_mp_key    = cfg.mp_key;
    hands.m_restore_cp_key    = cfg.cp_key;
    hands.m_move_forward      = cfg.move_forward;
    hands.m_move_back         = cfg.move_back;
    hands.m_rotate_left       = cfg.rotate_left;
    hands.m_rotate_right      = cfg.rotate_right;

    eyes.m_use_robust_bar = cfg.use_robust_bar;
    eyes.SetColors(cfg.colors);
    eyes.SetTargetWnd(cfg.target_wnd_x, cfg.target_wnd_y, cfg.target_wnd_w, cfg.target_wnd_h);

    brain.SetLogLevel(static_cast<Brain::LogLevel>(cfg.log_level));

    // MemReader: оновлюємо offsets якщо передано
    if (mem && cfg.mem_enabled) {
        MemReader::Offsets off;
        off.enabled      = true;
        off.player_ptr   = cfg.mem_player_ptr;
        off.ptr_chain    = cfg.mem_ptr_chain;
        off.hp_off       = cfg.mem_hp_off;
        off.max_hp_off   = cfg.mem_max_hp_off;
        off.mp_off       = cfg.mem_mp_off;
        off.max_mp_off   = cfg.mem_max_mp_off;
        off.cp_off       = cfg.mem_cp_off;
        off.max_cp_off   = cfg.mem_max_cp_off;
        off.pos_x_off    = cfg.mem_pos_x_off;
        off.pos_y_off    = cfg.mem_pos_y_off;
        off.pos_z_off    = cfg.mem_pos_z_off;
        mem->SetOffsets(off);
        if (!mem->IsOpen())
            mem->Open(cfg.mem_proc_name);
    }
}

int main(int argc, char* argv[]) {
    // Завжди рядковий буфер stdout (важливо коли не TTY)
    setlinebuf(stdout);

    // Аргументи командного рядка
    bool quick   = false;
    bool no_tui  = false;
    std::string config_path = "rdga1bot.ini";

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--quick")   quick  = true;
        if (a == "--no-tui")  no_tui = true;
        if (a == "--config" && i + 1 < argc) config_path = argv[++i];
    }

    Config cfg;
    cfg.Load(config_path);
    cfg.Validate();

    // TUI налаштування при першому запуску (якщо не --quick)
    if (!quick) {
        if (!cfg.InteractiveSetup()) {
            std::cout << "Скасовано.\n";
            return 0;
        }
        cfg.Save(config_path);
    }

    // Режим: ncurses dashboard або простий stdout
    bool use_tui = !no_tui;

    try {
        Hands    hands;
        Eyes     eyes;
        Brain    brain(eyes, hands, cfg);
        Capture  capture;
        FPS<100> fps;
        Dashboard dashboard;
        MemReader mem_reader;

        ApplyConfig(cfg, hands, eyes, brain, &mem_reader);

        // KnownList: OffsetScanner + WorldState (незалежно від [MemReader])
        // blindScan() не потребує координат — знаходить PlayerBase структурно.
        std::unique_ptr<OffsetScanner> kl_scanner;
        pid_t kl_pid = 0; // PID l2.exe для KnownList (може відрізнятись від mem_reader)
        if (cfg.knownlist_enabled) {
            // Шукаємо PID l2.exe незалежно від mem_reader
            kl_pid = mem_reader.IsOpen()
                ? mem_reader.GetPid()
                : [&]() -> pid_t {
                    // Мінімальний пошук PID через /proc (як в MemReader::FindPid)
                    DIR* dir = opendir("/proc");
                    if (!dir) return 0;
                    struct dirent* e;
                    pid_t found = 0;
                    while ((e = readdir(dir)) != nullptr && !found) {
                        bool num = true;
                        for (char c : std::string(e->d_name))
                            if (!isdigit(c)) { num = false; break; }
                        if (!num) continue;
                        std::ifstream cmd("/proc/" + std::string(e->d_name) + "/cmdline");
                        std::string line; std::getline(cmd, line, '\0');
                        auto p = line.rfind('/');
                        std::string exe = (p != std::string::npos) ? line.substr(p+1) : line;
                        auto lc = [](std::string s){ for(auto& c:s) c=(char)tolower((unsigned char)c); return s; };
                        if (lc(exe) == "l2.exe") { found = (pid_t)std::stoi(e->d_name); }
                    }
                    closedir(dir);
                    return found;
                }();

            if (kl_pid) {
                kl_scanner = std::make_unique<OffsetScanner>(kl_pid);
                // Спробуємо завантажити кешовані offsets
                if (kl_scanner->loadOffsets(cfg.knownlist_offsets_file)) {
                    // offsets.json вже є — WorldState буде створено після blindScan
                    // PlayerBase ще не відомий, але offsets завантажені
                    std::cerr << "[KnownList] Offsets завантажено, blind scan знайде PlayerBase\n";
                }
            } else {
                std::cerr << "[KnownList] l2.exe не знайдено → KnownList вимкнено\n";
            }
        }

        // Підключаємо лог-callback до Dashboard
        if (use_tui) {
            dashboard.Init();
            brain.SetLogCallback([&dashboard](const std::string& msg) {
                dashboard.AddLog(msg);
            });
        }

        // Signal handlers — зберігають stats при Ctrl+C / kill
        g_cleanup = [&]() {
            brain.GetStats().SaveToFile();
            if (use_tui) dashboard.Shutdown();
        };
        std::signal(SIGINT,  signal_handler);
        std::signal(SIGTERM, signal_handler);

        // CLI stdin thread для --no-tui режиму (q/p/r/s команди)
        std::atomic<int> cli_command{0};
        std::thread cli_thread;
        if (!use_tui) {
            cli_thread = std::thread([&cli_command]() {
                std::string line;
                while (std::getline(std::cin, line)) {
                    if (line == "q" || line == "quit")   { cli_command = 'q'; break; }
                    if (line == "p" || line == "pause" ||
                        line == "resume")                  cli_command = 'p';
                    if (line == "r" || line == "reset")    cli_command = 'r';
                    if (line == "s" || line == "status")   cli_command = 's';
                }
            });
            cli_thread.detach();
        }

        bool first = true;
        int  restart_attempts = 0;
        const int MAX_RESTART = 3;
        // Fix #1: кешуємо handle+rect вікна — Window::Find() дуже дорогий (XQueryTree)
        WindowHandle cached_hwnd = 0;
        struct Window::Rect cached_rect = {0, 0, 0, 0};
        bool window_found = false;
        double current_fps = 0.0;
        // Opt: Dashboard throttle — оновлюємо TUI 10 FPS (100мс), не кожен тік
        auto last_dashboard_update = std::chrono::steady_clock::now();
        constexpr auto DASHBOARD_INTERVAL = std::chrono::milliseconds(100);

        // ─── Головний цикл з автовідновленням ──────────────────────────
        while (restart_attempts < MAX_RESTART) {
            try {

        // Звільняємо застряглий ESC (може застрягти якщо процес вбили під час XTest KeyDown)
        hands.KeyboardKeyUp(Input::KeyboardKey::Escape);
        hands.Send(100);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // Grace period: ігноруємо ESC 0.5с після старту
        auto loop_start_time = std::chrono::steady_clock::now();
        constexpr auto ESC_GRACE = std::chrono::milliseconds(500);

        while (true) {
            // ─── Keyboard + TUI input (завжди, без кадру) ──────────────
            if (hands.KeyboardKeyPressed(Input::KeyboardKey::ScrollLock)) {
                std::string msg = "ScrollLock → зупинка.";
                if (use_tui) dashboard.AddLog(msg);
                else         std::cout << msg << "\n";
                goto bot_exit;
            }
            if (hands.IsReady() &&
                std::chrono::steady_clock::now() - loop_start_time >= ESC_GRACE &&
                hands.KeyboardKeyPressed(Input::KeyboardKey::Escape)) {
                std::string msg = "ESC → зупинка.";
                if (use_tui) dashboard.AddLog(msg);
                else         std::cout << msg << "\n";
                goto bot_exit;
            }

            if (use_tui) {
                int key = dashboard.HandleInput();
                if (key == 'q') {
                    dashboard.AddLog("Зупинка за командою Q");
                    goto bot_exit;
                }
                if (key == 'p') {
                    brain.TogglePause();
                    dashboard.AddLog(brain.IsPaused() ? "[PAUSE] Бот на паузі" : "[PAUSE] Продовження");
                }
                if (key == 's') {
                    dashboard.ShowSettings(cfg, config_path);
                    brain.ReloadConfig(cfg);
                    ApplyConfig(cfg, hands, eyes, brain);
                }
                if (key == 'r') {
                    eyes.Reset();
                    dashboard.AddLog("[INFO] HP/MP/CP бари скинуто");
                }
            }

            if (!use_tui) {
                int cmd = cli_command.exchange(0);
                if (cmd == 'q') goto bot_exit;
                if (cmd == 'p') {
                    brain.TogglePause();
                    std::cout << (brain.IsPaused() ? "PAUSED\n" : "RESUMED\n");
                }
                if (cmd == 'r') { eyes.Reset(); std::cout << "Bars reset.\n"; }
                if (cmd == 's') {
                    auto& st = brain.GetStats();
                    std::cout << "State: " << Brain::StateName(brain.GetState())
                              << " | Kills: " << st.kills
                              << " | Deaths: " << st.deaths
                              << " | Uptime: " << st.UptimeStr() << "\n";
                }
            }

            // ─── Fix #4: пропускаємо захоплення кадру якщо дії в польоті ──
            if (!hands.IsReady()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                if (use_tui) {
                    auto now = std::chrono::steady_clock::now();
                    if (now - last_dashboard_update >= DASHBOARD_INTERVAL) {
                        dashboard.Update(brain, current_fps);
                        last_dashboard_update = now;
                    }
                }
                continue;
            }

            // ─── Fix #1: вікно кешується, шукаємо тільки якщо втрачено ───
            if (!window_found) {
                const auto w = Window::Find(cfg.window_title);
                if (!w.has_value()) {
                    std::string msg = "Вікно \"" + cfg.window_title + "\" не знайдено! Чекаємо...";
                    if (use_tui) dashboard.AddLog(msg);
                    else         std::cout << msg << "\n";
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                    continue;
                }
                cached_hwnd = w->Handle();
                cached_rect = w->Rect();
                window_found = true;
                if (first) {
                    hands.SetGameWindow(static_cast<unsigned long>(cached_hwnd));
                    w->BringToForeground();
                    brain.Init();
                    first = false;
                }
            }

            // ─── Захоплення кадру ──────────────────────────────────────
            capture.Clear();
            const auto bitmap = capture.Grab({cached_rect.x, cached_rect.y,
                                              cached_rect.width, cached_rect.height});
            if (!bitmap.has_value()) { window_found = false; continue; }

            auto image = BitmapToImage(bitmap.value());
            if (!image.has_value()) { window_found = false; continue; }

            // ─── Обробка ───────────────────────────────────────────────
            hands.SetWindowRect({cached_rect.x, cached_rect.y, cached_rect.width, cached_rect.height});
            eyes.Open(image.value());
            // Memory Reading: оновлюємо стан гравця з пам'яті (якщо увімкнено)
            if (cfg.mem_enabled && mem_reader.IsOpen()) {
                brain.SetMemPlayerState(mem_reader.ReadPlayer());
            }

            // KnownList: blind scan у фоновому thread — не блокує головний цикл
            // blindScan() сканує ~1GB heap (~20с) — занадто довго для main loop
            if (cfg.knownlist_enabled && kl_scanner && !brain.HasPlayerBase()) {
                static std::atomic<bool>     kl_scan_running{false};
                static std::atomic<uintptr_t> kl_scan_result{0};
                static int kl_scan_attempts = 0;
                static auto kl_last_attempt =
                    std::chrono::steady_clock::now() - std::chrono::seconds(10);

                // Перевіряємо чи фоновий scan завершився
                if (kl_scan_result.load() != 0) {
                    uintptr_t base = kl_scan_result.exchange(0);
                    brain.SetPlayerBase(base);
                    if (!brain.GetWorldState()) {
                        brain.SetWorldState(
                            std::make_unique<WorldState>(kl_pid, *kl_scanner));
                    }
                    kl_scanner->saveOffsets(cfg.knownlist_offsets_file);
                    std::cerr << "[KnownList] PlayerBase=0x" << std::hex << base
                              << std::dec << " WorldState активовано\n";
                }

                // Запускаємо новий scan кожні 30с якщо попередній завершився
                auto kl_now = std::chrono::steady_clock::now();
                if (!kl_scan_running.load() &&
                    kl_now - kl_last_attempt >= std::chrono::seconds(30)) {
                    kl_last_attempt = kl_now;
                    kl_scan_attempts++;
                    kl_scan_running = true;
                    std::cerr << "[KnownList] blind scan спроба #" << kl_scan_attempts << " (фон)\n";
                    auto* scanner_ptr = kl_scanner.get();
                    auto* running_ptr = &kl_scan_running;
                    auto* result_ptr  = &kl_scan_result;
                    std::thread([scanner_ptr, running_ptr, result_ptr]() {
                        uintptr_t base = scanner_ptr->blindScan();
                        result_ptr->store(base);
                        running_ptr->store(false);
                    }).detach();
                }
            }

            brain.Process(cfg.debug && !use_tui);
            eyes.Close();

            current_fps = fps.Get();

            // ─── ncurses Dashboard (throttle: 10 FPS) ──────────────────
            if (use_tui) {
                auto now = std::chrono::steady_clock::now();
                if (now - last_dashboard_update >= DASHBOARD_INTERVAL) {
                    dashboard.Update(brain, current_fps);
                    last_dashboard_update = now;
                }
            }

            // ─── OpenCV debug overlay (тільки без TUI) ─────────────────
            if (!use_tui && cfg.debug) {
                Eyes::Me   me_def{};
                Eyes::Target tgt_def{};
                const auto& me  = brain.Me().value_or(me_def);
                const auto& tgt = brain.Target().value_or(tgt_def);

                std::string overlay =
                    std::string("State: ") + Brain::StateName(brain.GetState()) +
                    "  HP:" + std::to_string(me.hp) +
                    "%  MP:" + std::to_string(me.mp) +
                    "%  Target:" + std::to_string(tgt.hp) +
                    "%  FPS:" + std::to_string((int)current_fps);

                cv::putText(image.value(), overlay,
                    {5, 20}, cv::FONT_HERSHEY_PLAIN, 1.2, {0, 255, 255}, 1);
                cv::putText(image.value(),
                    "ESC=exit  PrintScr=screenshot  Space=reset  F12=calibrate",
                    {5, image->rows - 10}, cv::FONT_HERSHEY_PLAIN, 0.9, {200, 200, 200}, 1);
                static bool cv_window_created = false;
                if (!cv_window_created) {
                    cv::namedWindow("rdga1bot", cv::WINDOW_NORMAL);
                    cv::resizeWindow("rdga1bot", image->cols * 2 / 3, image->rows * 2 / 3);
                    cv_window_created = true;
                }
                cv::imshow("rdga1bot", image.value());
                cv::waitKey(1);
            }

            // ─── PrintScreen / F12 / Space ─────────────────────────────
            if (hands.KeyboardKeyPressed(Input::KeyboardKey::PrintScreen)) {
                cv::imwrite("tmp/shot.png", image.value());
                std::string msg = "Скріншот: tmp/shot.png";
                if (use_tui) dashboard.AddLog(msg);
                else         std::cout << msg << "\n";
            }
            if (hands.KeyboardKeyPressed(Input::KeyboardKey::F12)) {
                cv::imwrite("tmp/calibrate.png", image.value());
                std::string msg;
                if (eyes.MyBars().has_value()) {
                    auto bars = eyes.MyBars().value();
                    cv::Mat hp_roi = image.value()(bars.hp_bar);
                    cv::Mat mp_roi = image.value()(bars.mp_bar);
                    cv::Mat cp_roi = image.value()(bars.cp_bar);
                    cv::imwrite("tmp/calibrate_hp.png", hp_roi);
                    cv::imwrite("tmp/calibrate_mp.png", mp_roi);
                    cv::imwrite("tmp/calibrate_cp.png", cp_roi);
                    msg = "Калібрування збережено: tmp/calibrate_*.png";
                    auto hp_s = HSVSuggest(hp_roi, "HP");
                    auto mp_s = HSVSuggest(mp_roi, "MP");
                    auto cp_s = HSVSuggest(cp_roi, "CP");
                    for (auto& s : {hp_s, mp_s, cp_s}) {
                        if (!s.empty()) {
                            if (use_tui) dashboard.AddLog(s);
                            else         std::cout << s << "\n";
                        }
                    }
                } else {
                    msg = "HP/MP бари не знайдено! Збережено tmp/calibrate.png";
                }
                if (eyes.TargetHPBar().has_value()) {
                    cv::imwrite("tmp/calibrate_target.png",
                        image.value()(eyes.TargetHPBar().value()));
                }
                if (use_tui) dashboard.AddLog(msg);
                else         std::cout << msg << "\n";
            }
            if (hands.KeyboardKeyPressed(Input::KeyboardKey::Space)) {
                eyes.Reset();
                std::string msg = "HP/MP/CP бари скинуто.";
                if (use_tui) dashboard.AddLog(msg);
                else         std::cout << msg << "\n";
            }
        } // while(true)

        break; // нормальний вихід з restart-циклу

            } catch (const std::exception& e) {
                std::string err = std::string("[CRASH] ") + e.what();
                if (use_tui) dashboard.AddLog(err);
                else         std::cerr << err << "\n";

                restart_attempts++;
                if (restart_attempts < MAX_RESTART) {
                    std::string msg = "[RECOVERY] Перезапуск (" +
                        std::to_string(restart_attempts) + "/" +
                        std::to_string(MAX_RESTART) + ") через 5с...";
                    if (use_tui) dashboard.AddLog(msg);
                    else         std::cerr << msg << "\n";
                    std::this_thread::sleep_for(std::chrono::seconds(5));
                    eyes.Reset();
                    first = true;
                }
            }
        } // restart loop

bot_exit:
        if (use_tui) dashboard.Shutdown();

        brain.GetStats().PrintSummary();
        brain.GetStats().SaveToFile();
        // destroyAllWindows тільки якщо debug-вікно могло бути відкрите
        if (cfg.debug && !use_tui) cv::destroyAllWindows();

    } catch (Intercept::InterceptionDriverNotFoundError& e) {
        std::cerr << "Помилка Intercept: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
