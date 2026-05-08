#include <iostream>
#include <string>
#include <optional>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>
#include <iomanip>
#include <map>

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
#ifndef _WIN32
#include <dirent.h>
#endif
#include <cctype>
#include <fstream>
#include "MemReader.h"
#include "offset_scanner.h"
#include "ProcessMemory.h"
#include "world_state.h"
#include "Geodata.h"
#include "vision_worker.h"
#include "geodata_worker.h"
#include "navmesh_builder.h"
#include "navmesh_worker.h"
#include "tools/diag.h"
#ifdef __linux__
#include <sched.h>
#include <pthread.h>
#endif

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

    // RandomDelay: оновлюємо параметри варіативних затримок
    hands.SetDelays(cfg.delays);

    // MemReader: оновлюємо offsets якщо передано
    if (mem && cfg.mem_enabled) {
        MemReader::Offsets off;
        off.enabled      = true;
        off.use_kl_base  = cfg.mem_use_kl_base;
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
        off.heading_off  = cfg.mem_heading_off;
        mem->SetOffsets(off);
        // Авто-завантаження калібрування з попередньої сесії
        if (off.use_kl_base) mem->LoadCalib("mem_calib.json");
        if (!mem->IsOpen())
            mem->Open(cfg.mem_proc_name);
    }
}



static bool SetThreadAffinity(int core) {
#ifdef __linux__
    if (core < 0) return false;
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET((size_t)core, &cpuset);
    return pthread_setaffinity_np(pthread_self(),
                                  sizeof(cpu_set_t), &cpuset) == 0;
#else
    return false;
#endif
}

int main(int argc, char* argv[]) {
    // Завжди рядковий буфер stdout (важливо коли не TTY)
    setlinebuf(stdout);

    // Аргументи командного рядка
    bool quick   = false;
    bool no_tui  = false;
    bool dump_objects  = false;
    bool hp_calibrate  = false;
    std::string config_path = "rdga1bot.ini";

    bool calibrate = false;
    bool heading_monitor = false;
    bool watch_pos = false;
    bool map_mode  = false;
    bool find_pos  = false;
    bool scan_pos  = false;
    bool discover_klist = false;
    bool diff_scan = false;
    uintptr_t override_pb = 0;  // --pb 0xADDR — переназначити PlayerBase для watch-pos/map
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--quick")           quick          = true;
        if (a == "--no-tui")          no_tui         = true;
        if (a == "--dump-objects")    dump_objects   = true;
        if (a == "--calibrate")       calibrate      = true;
        if (a == "--hp-calibrate")    hp_calibrate   = true;
        if (a == "--heading-monitor") heading_monitor = true;
        if (a == "--watch-pos")       watch_pos      = true;
        if (a == "--map")             map_mode       = true;
        if (a == "--find-pos")        find_pos       = true;
        if (a == "--scan-pos")        scan_pos       = true;
        if (a == "--discover-klist")  discover_klist = true;
        if (a == "--diff-scan")       diff_scan      = true;
        if (a == "--pb" && i + 1 < argc) {
            override_pb = (uintptr_t)std::stoull(argv[++i], nullptr, 16);
        }
        if (a == "--config" && i + 1 < argc) config_path = argv[++i];
    }

    // ─── --dump-objects: дамп об'єктів KnownList для калібровки offsets ──────
    if (dump_objects)    { runDumpObjects(config_path);                return 0; }
    if (discover_klist)  { runDiscoverKlist(config_path);              return 0; }
    if (calibrate)       { runCalibrate(config_path, argc, argv);     return 0; }
    if (heading_monitor) { runHeadingMonitor(config_path);            return 0; }
    if (hp_calibrate)    { runHpCalibrate(config_path);              return 0; }
    if (find_pos)  { runFindPos(config_path);                  return 0; }
    if (watch_pos) { runWatchPos(config_path, override_pb);    return 0; }
    if (diff_scan) { runDiffScan(config_path);                 return 0; }

    if (map_mode) { runMapMode(config_path, override_pb); return 0; }
    if (scan_pos) { runScanPos(config_path); return 0; }
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
#ifndef _WIN32
        // MR66: встановлюємо input backend (xtest/xsendevent/hybrid)
        hands.GetIntercept().SetBackend(cfg.input_backend);
#endif
        brain.LoadNavMeshPoints(); // завантажує існуючі точки з попередніх сесій

        // ── Threading: CPU affinity + workers ──────────────────────────────────
        VisionWorker  vision_worker;
        GeodataWorker geodata_worker;

        if (cfg.threading.enabled) {
            // Main thread affinity
            if (cfg.threading.cpu_affinity) {
                if (SetThreadAffinity(cfg.threading.main_core))
                    std::cerr << "[CPU] Main → Core " << cfg.threading.main_core << "\n";
                else
                    std::cerr << "[CPU] WARNING: affinity failed\n";
            }
            // GeodataWorker (тільки якщо Geodata завантажена)
            if (cfg.threading.geodata_thread && brain.GetGeodata()) {
                geodata_worker.Start(brain.GetGeodata(),
                                     cfg.threading.geodata_core);
            }
            // VisionWorker
            if (cfg.threading.vision_thread) {
                vision_worker.Start(cfg.threading.vision_core);
            }
        }

        // KnownList: OffsetScanner + WorldState (незалежно від [MemReader])
        // blindScan() не потребує координат — знаходить PlayerBase структурно.
        std::unique_ptr<OffsetScanner> kl_scanner;
        pid_t kl_pid = 0; // PID l2.exe для KnownList (може відрізнятись від mem_reader)
        if (cfg.knownlist_enabled) {
            // Шукаємо PID l2.exe незалежно від mem_reader
            kl_pid = mem_reader.IsOpen()
                ? mem_reader.GetPid()
                : findL2Pid();

            if (kl_pid) {
                kl_scanner = std::make_unique<OffsetScanner>(kl_pid);
                // Спробуємо завантажити кешовані offsets
                if (kl_scanner->loadOffsets(cfg.knownlist_offsets_file)) {
                    // offsets.json завантажено — playerBaseCache може містити валідний base.
                    // Якщо XYZ за кешованою адресою валідні → використаємо без blindScan.
                    // Якщо ні → blindScan (фон, кожні 30с).
                    std::cerr << "[KnownList] Offsets завантажено, blind scan знайде PlayerBase\n";
                }
            } else {
                std::cerr << "[KnownList] l2.exe не знайдено → KnownList вимкнено\n";
            }
        }

        // NavMesh: завантажуємо .bin якщо є і Enabled=true
        std::shared_ptr<NavMeshBuilder> navmesh_builder;
        std::unique_ptr<NavMeshWorker>  navmesh_worker;
        if (cfg.navmesh_cfg.enabled) {
            auto nb = std::make_shared<NavMeshBuilder>();
            if (nb->Load(cfg.navmesh_cfg.navmesh_file)) {
                brain.SetNavMeshBuilder(nb);
                navmesh_builder = nb;
                navmesh_worker = std::make_unique<NavMeshWorker>();
                navmesh_worker->Start(nb, /*core_id=*/4);
                std::cerr << "[NAVMESH] NavMeshWorker запущено\n";
            } else {
                std::cerr << "[NAVMESH] Файл не знайдено: "
                          << cfg.navmesh_cfg.navmesh_file
                          << " (спочатку запусти ./tools/build_navmesh)\n";
            }
        }

        // Geodata: завантаження L2J геодати (якщо увімкнено)
        if (cfg.geodata_enabled) {
            auto geo = std::make_shared<Geodata>();
            int n = geo->Load(cfg.geodata_path, cfg.geodata_use_jps);
            if (n > 0) {
                brain.SetGeodata(geo);
                std::cerr << "[GEODATA] Завантажено " << n << " регіонів з " << cfg.geodata_path << "\n";
            } else {
                std::cerr << "[GEODATA] Не знайдено .geo файлів в " << cfg.geodata_path << "\n";
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
            if (cfg.navmesh_cfg.save_on_exit) brain.SaveNavMeshPoints();
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

        // ── Змінні стану KnownList та VisionWorker (переживають restart) ──
        uint64_t vision_frame_id = 0;
        std::atomic<bool>      kl_scan_running{false};
        std::atomic<uintptr_t> kl_scan_result{0};
        std::thread            kl_scan_thread;  // MR74: join при shutdown (не detach)
        int  kl_scan_attempts = 0;
        bool kl_cache_tried   = false; // спробували playerBaseCache з offsets.json
        // Якщо mem_calib.json завантажено — не перекалібровувати (щоб не затерти валідний результат)
        bool        mem_calib_done = mem_reader.GetOffsets().hp_off > 0;
        HpAutoCalib hp_auto_calib;     // MR80: диференційне авто-калібрування HP offset
        bool        pgup_prev = false; // edge detection для PageUp паузи
        auto kl_last_attempt  =
            std::chrono::steady_clock::now() - std::chrono::seconds(10);
        auto kl_validity_check =
            std::chrono::steady_clock::now() - std::chrono::seconds(35);

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
            // Fn+PgUp (Pause/Break) — пауза/відновлення, аналог Fn+Home (ScrollLock)
            // Edge detection на відпусканні: toggle виникає один раз
            {
                bool pause_now = hands.KeyboardKeyPressed(Input::KeyboardKey::Pause);
                if (!pause_now && pgup_prev) {
                    brain.TogglePause();
                    std::string msg = brain.IsPaused() ? "[PAUSE] Pause/Break → пауза" : "[PAUSE] Pause/Break → продовження";
                    if (use_tui) dashboard.AddLog(msg);
                    else         std::cout << msg << "\n";
                }
                pgup_prev = pause_now;
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
                    std::cout << "State: " << brain.GetState()
                              << " | Kills: " << st.kills
                              << " | Deaths: " << st.deaths
                              << " | Uptime: " << st.UptimeStr() << "\n";
                }
            }

            // NavMesh: збір точок кожну ітерацію (навіть hands busy — щоб фіксувати рух)
            brain.TryRecordNavPoint();

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

            // ── VisionWorker: відправляємо кадр async (якщо увімкнено) ──────────
            if (cfg.threading.enabled && cfg.threading.vision_thread
                && vision_worker.IsRunning()) {
                vision_worker.SubmitFrame(image.value(), ++vision_frame_id, eyes);
            }

            // Memory Reading: оновлюємо стан гравця з пам'яті (якщо увімкнено)
            if (cfg.mem_enabled && mem_reader.IsOpen()) {
                // Авто-калібрування HP/MP/CP offsets (один раз на сесію).
                // Умова: playerBase відомий + OCR HP стабільний 3 тіки + хоча б 1 стат < 98%.
                // MR80: диференційне авто-калібрування HP offset
                if (!mem_calib_done && cfg.mem_use_kl_base && brain.HasPlayerBase()) {
                    int ocr_hp = brain.Me() ? brain.Me()->hp : -1;
                    if (auto res = hp_auto_calib.tick(kl_pid, brain.GetPlayerBase(), ocr_hp)) {
                        auto off       = mem_reader.GetOffsets();
                        off.hp_off     = res->hp_off;
                        off.max_hp_off = res->max_hp_off;
                        mem_reader.SetOffsets(off);
                        MemReader::AutoCalibResult save{};
                        save.hp_off = res->hp_off; save.max_hp_off = res->max_hp_off;
                        save.found_hp = true;
                        mem_reader.SaveCalib(save, "mem_calib.json");
                        mem_calib_done = true;
                    }
                }
                brain.SetMemPlayerState(mem_reader.ReadPlayer());
            }

            // KnownList: blind scan у фоновому thread — не блокує головний цикл
            // blindScan() сканує ~1GB heap (~20с) — занадто довго для main loop
            if (cfg.knownlist_enabled && kl_scanner && brain.HasPlayerBase()) {
                // Fix 2: перевірка валідності PlayerBase кожні 30с
                // (якщо L2 перезапустився або гравець respawn з новою адресою)
                auto kl_now_v = std::chrono::steady_clock::now();
                if (kl_now_v - kl_validity_check >= std::chrono::seconds(30)) {
                    kl_validity_check = kl_now_v;
                    uintptr_t cur_base = brain.GetPlayerBase();
                    float vx = kl_scanner->rpm_pub<float>(cur_base + OFF_PLAYER_X);
                    float vz = kl_scanner->rpm_pub<float>(cur_base + OFF_PLAYER_Z);
                    // OR-логіка: достатньо X>500 або Z>500.
                    // Y (0x28) може бути ~0 в певних зонах (LoA/ToI) → AND-перевірка хибно скидала базу.
                    bool base_valid = (std::isfinite(vx) && std::isfinite(vz) &&
                                       (std::fabsf(vx) > 500.f || std::fabsf(vz) > 500.f));
                    if (!base_valid) {
                        std::cerr << "[KnownList] PlayerBase невалідний → скидаємо для re-scan\n";
                        brain.SetPlayerBase(0);
                    }
                }
            } else if (cfg.knownlist_enabled && kl_scanner && !brain.HasPlayerBase()) {
                // ── Спроба 0: перевірити playerBaseCache з offsets.json ────────────
                // blindScan ненадійний (Kamael KnownList структура несумісна).
                // Якщо offsets.json містить валідну адресу — використати її напряму.
                if (!kl_cache_tried && kl_scanner->playerBaseCache != 0) {
                    kl_cache_tried = true;
                    uintptr_t cached = kl_scanner->playerBaseCache;
                    float cx = kl_scanner->rpm_pub<float>(cached + OFF_PLAYER_X);
                    float cy = kl_scanner->rpm_pub<float>(cached + OFF_PLAYER_Y);
                    // AND-логіка: обидві координати > 200 (OR раніше помилково приймало X=1098,Y=0).
                    // Y=0 → false positive (Wine .data секція, статична адреса).
                    bool cache_valid = (std::isfinite(cx) && std::fabsf(cx) > 200.f && std::fabsf(cx) < 327000.f
                                     && std::isfinite(cy) && std::fabsf(cy) > 200.f && std::fabsf(cy) < 327000.f);
                    if (cache_valid) {
                        std::cerr << "[KnownList] playerBaseCache=0x" << std::hex << cached
                                  << " валідний XYZ=(" << (int)cx << "," << (int)cy << ")"
                                  << std::dec << " — використовуємо без blindScan\n";
                        if (cfg.mem_enabled && cfg.mem_use_kl_base)
                            mem_reader.SetDirectBase(cached);
                        kl_scan_result.store(cached);
                    } else {
                        std::cerr << "[KnownList] playerBaseCache=0x" << std::hex << cached
                                  << std::dec << " невалідний — запускаємо blindScan\n";
                        kl_scanner->playerBaseCache = 0;
                    }
                }

                // ── Спроба 1: результат фонового blindScan ────────────────────────
                if (kl_scan_result.load() != 0) {
                    uintptr_t base = kl_scan_result.exchange(0);
                    brain.SetPlayerBase(base);
                    if (!brain.GetWorldState()) {
                        auto ws = std::make_unique<WorldState>(kl_pid, *kl_scanner);
                        ws->startBackground(base,
                            cfg.knownlist_max_range, 500.f);
                        brain.SetWorldState(std::move(ws));
                    } else {
                        // Re-scan після respawn/restart: оновити playerBase у bg thread
                        brain.GetWorldState()->setPlayerBase(base);
                    }
                    kl_scanner->playerBaseCache = base;  // зберігаємо для --watch-pos
                    kl_scanner->saveOffsets(cfg.knownlist_offsets_file);
                    // UseKLBase: передаємо playerBase до MemReader для прямого читання
                    if (cfg.mem_enabled && cfg.mem_use_kl_base)
                        mem_reader.SetDirectBase(base);
                    std::cerr << "[KnownList] PlayerBase=0x" << std::hex << base
                              << std::dec << " WorldState активовано\n";
                }

                // ── Спроба 2: новий blindScan кожні 30с (якщо кеш не спрацював) ──
                auto kl_now = std::chrono::steady_clock::now();
                if (!kl_scan_running.load() &&
                    kl_now - kl_last_attempt >= std::chrono::seconds(30)) {
                    kl_last_attempt = kl_now;
                    kl_scan_attempts++;
                    kl_scan_running = true;
                    std::cerr << "[KnownList] blind scan спроба #" << kl_scan_attempts << " (фон)\n";
                    auto* scanner_ptr  = kl_scanner.get();
                    auto* running_ptr  = &kl_scan_running;
                    auto* result_ptr   = &kl_scan_result;
                    int   bs_timeout   = cfg.mem_blindscan_timeout_ms > 0
                                         ? cfg.mem_blindscan_timeout_ms : 0;
                    if (kl_scan_thread.joinable()) kl_scan_thread.join();
                    kl_scan_thread = std::thread([scanner_ptr, running_ptr, result_ptr, bs_timeout]() {
                        uintptr_t base = scanner_ptr->blindScan(bs_timeout);
                        result_ptr->store(base);
                        running_ptr->store(false);
                    });
                }
            }

            // ── VisionWorker: отримуємо результат DetectNPCs (якщо готовий) ─────
            if (cfg.threading.enabled && cfg.threading.vision_thread) {
                if (auto vr = vision_worker.TryGetResult()) {
                    brain.SetAsyncNPCs(vr->npcs, vr->minimap);
                }
            }

            // ── GeodataWorker: отримуємо результат FindPath (якщо готовий) ──────
            if (cfg.threading.enabled && cfg.threading.geodata_thread) {
                if (auto gr = geodata_worker.TryGetResult()) {
                    brain.SetGeoPath(gr->path, gr->id);
                }
                // Відправляємо новий запит якщо Brain потребує шляху
                if (auto req = brain.GetPendingPathRequest()) {
                    geodata_worker.RequestPath(*req);
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
                    std::string("State: ") + brain.GetState() +
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
        // MR74: abort background blindScan thread before kl_scanner is destroyed
        if (kl_scanner) kl_scanner->abortScan();
        if (kl_scan_thread.joinable()) kl_scan_thread.join();

        if (cfg.navmesh_cfg.save_on_exit)
            brain.SaveNavMeshPoints();

        // MR76: очищаємо log callback перед Shutdown dashboard — інакше shutdownRL()
        // викличе dashboard.AddLog() після звільнення TUI буферів → double free crash
        brain.SetLogCallback(nullptr);
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
