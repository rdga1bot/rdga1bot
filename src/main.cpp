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

// ─── KnownList калібровка: дамп пам'яті об'єктів ───────────────────────────
// Запуск: ./rdga1bot --dump-objects
// Виводить hex dump перших N об'єктів KnownList для ручного визначення offsets.
static pid_t findL2Pid() {
#ifdef _WIN32
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32 pe = {};
    pe.dwSize = sizeof(pe);
    pid_t found = 0;
    if (Process32First(snap, &pe)) {
        do {
            char name[MAX_PATH];
            strncpy(name, pe.szExeFile, sizeof(name) - 1);
            for (auto& c : name) c = (char)tolower((unsigned char)c);
            if (std::string(name) == "l2.exe") { found = pe.th32ProcessID; break; }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return found;
#else
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
        if (lc(exe) == "l2.exe") found = (pid_t)std::stoi(e->d_name);
    }
    closedir(dir);
    return found;
#endif
}

// ─── --find-pos: сканує ВСЮ пам'ять L2 на XYZ гравця (як Cheat Engine) ──────
// Запуск у фоні: ./rdga1bot --find-pos &
// 1) Відразу клікни мишею в L2 вікно (переключи фокус)
// 2) Стоїш нерухомо 5с (перший скан)
// 3) "ПЕРЕМІЩУЙСЯ ДАЛЕКО!" → клікни мишею далеко від поточного місця (300+ L2u)
// 4) Показуємо адреси де X змінився на > 150 L2u — це реальний PlayerBase XYZ
//
// НЕ потребує blindScan/PlayerBase — шукає значення прямо по значенню.
// Кандидати: float X,Y обидва в |val| > 30000 AND < 327000 (не ближня зона до 0).
static void findPos(const std::string& /*offsets_file*/) {
    pid_t pid = findL2Pid();
    if (!pid) { std::cerr << "[find-pos] L2 процес не знайдено\n"; return; }
    std::cerr << "[find-pos] PID=" << pid << "\n";

    // Зчитуємо readable регіони пам'яті Wine процесу (пропускаємо >32MB)
    auto getRegions = [&]() {
        std::vector<std::pair<uintptr_t,size_t>> regions;
        std::ifstream maps("/proc/" + std::to_string(pid) + "/maps");
        std::string line;
        while (std::getline(maps, line)) {
            if (line.find('r') == std::string::npos) continue;
            if (line.find("vvar") != std::string::npos ||
                line.find("vsyscall") != std::string::npos) continue;
            uintptr_t lo = 0, hi = 0;
            if (sscanf(line.c_str(), "%lx-%lx", &lo, &hi) != 2) continue;
            size_t sz = hi - lo;
            if (sz > 32*1024*1024 || sz < 12) continue;
            regions.push_back({lo, sz});
        }
        return regions;
    };

    // Перевірка: значення > 30000 і < 327000 (не нульова зона і не за межами світу)
    // Виключаємо [−30000..30000] — це де blindScan знаходить garbage
    auto isPopulatedCoord = [](float v) -> bool {
        float abs_v = std::fabsf(v);
        return std::isfinite(v) && abs_v > 30000.f && abs_v < 327000.f;
    };
    auto isValidZ = [](float v) -> bool {
        return std::isfinite(v) && v > -16384.f && v < 16384.f;
    };

    std::cerr << "[find-pos] Перший скан — стій нерухомо 5с...\n";
    std::this_thread::sleep_for(std::chrono::seconds(5));

    auto regions = getRegions();
    std::cerr << "[find-pos] Регіонів: " << regions.size()
              << " — сканую (float X,Y обидва > 30000)...\n";

    // Перший скан: зберігаємо (addr, x0) де float X i Y обидва в populated range
    struct Hit { uintptr_t addr; float x0; float y0; };
    std::vector<Hit> hits;
    hits.reserve(4096);
    {
        std::vector<uint8_t> buf;
        for (auto& [lo, sz] : regions) {
            buf.resize(sz);
            struct iovec lv = { buf.data(), sz };
            struct iovec rv = { (void*)lo, sz };
            ssize_t rd = process_vm_readv(pid, &lv, 1, &rv, 1, 0);
            if (rd < 12) continue;
            for (size_t i = 0; i + 12 <= (size_t)rd; i += 4) {
                float fx, fy, fz;
                memcpy(&fx, buf.data()+i,   4);
                memcpy(&fy, buf.data()+i+4, 4);
                memcpy(&fz, buf.data()+i+8, 4);
                if (!isPopulatedCoord(fx)) continue;
                if (!isPopulatedCoord(fy)) continue;
                if (!isValidZ(fz))         continue;
                // Додаткова перевірка: X і Y не рівні (у реальних об'єктів X≠Y)
                if (std::fabsf(fx - fy) < 1.f) continue;
                hits.push_back({lo + (uintptr_t)i, fx, fy});
            }
        }
    }
    std::cerr << "[find-pos] Кандидатів: " << hits.size() << "\n";
    if (hits.empty()) {
        std::cerr << "[find-pos] Нічого — переконайся що персонаж зайшов у зону фарму (далеко від спавну)\n";
        return;
    }

    std::cerr << "\n[find-pos] !!! ЗАРАЗ ПЕРЕМІЩУЙСЯ ДАЛЕКО (300+ L2u) МИШЕЮ В L2 !!!\n";
    std::cerr << "[find-pos] Чекаємо 12 секунд...\n\n";
    std::this_thread::sleep_for(std::chrono::seconds(12));

    // Другий скан: шукаємо адреси де X змінився на > 150 L2u
    std::cerr << "[find-pos] Аналіз змін (поріг >150 L2u)...\n";
    int shown = 0;
    for (auto& h : hits) {
        float new_x = 0.f, new_y = 0.f, new_z = 0.f;
        if (!ProcessMemory::Read(pid, h.addr,   &new_x, 4)) continue;
        if (!ProcessMemory::Read(pid, h.addr+4, &new_y, 4)) continue;
        if (!ProcessMemory::Read(pid, h.addr+8, &new_z, 4)) continue;
        float dx = new_x - h.x0;
        if (std::fabsf(dx) < 150.f) continue;  // моби теж рухаються, але < 150 L2u/10с
        if (!isPopulatedCoord(new_x)) continue; // перевіряємо що нове значення теж валідне
        if (!isPopulatedCoord(new_y)) continue;

        // Шукаємо відстань від blindScan PlayerBase (для відладки)
        std::cerr << "[find-pos] >>> addr=0x" << std::hex << h.addr << std::dec
                  << "  X: " << (int)h.x0 << " → " << (int)new_x
                  << " (Δ" << (int)dx << ")"
                  << "  Y=" << (int)new_y << "  Z=" << (int)new_z
                  << "  [potential pb = 0x" << std::hex << (h.addr - 0x24) << std::dec << "]\n";
        if (++shown >= 20) break;
    }
    if (shown == 0)
        std::cerr << "[find-pos] Жодна адреса не змінилась на >150 L2u\n"
                  << "[find-pos] → гравець не рухався під час очікування\n"
                  << "[find-pos] Запусти у фоні (&), клікни на L2 вікно, клікни ДАЛЕКО від персонажа\n";
    else
        std::cerr << "\n[find-pos] potential pb = addr_X - 0x24 (якщо X offset = 0x24)\n"
                  << "[find-pos] Перевір: у offsets.json playerBase = potential_pb\n"
                  << "[find-pos] Запусти ./rdga1bot --watch-pos для верифікації\n";
}

// ─── --watch-pos: моніторинг XYZ з кількох offsets під час руху ─────────────
// Запуск: ./rdga1bot --watch-pos
// Виводить координати кожні 200мс — знайди offset що оновлюється плавно під час руху.
// override_pb != 0 → використовувати вказану адресу замість offsets.json
static void watchPos(const std::string& offsets_file, uintptr_t override_pb = 0) {
    pid_t pid = findL2Pid();
    if (!pid) { std::cerr << "L2 процес не знайдено\n"; return; }
    std::cerr << "[watch-pos] L2 pid=" << pid << "\n";

    uintptr_t base = override_pb;
    if (!base) {
        OffsetScanner scanner(pid);
        if (!scanner.loadOffsets(offsets_file)) {
            std::cerr << "[watch-pos] offsets.json не знайдено\n"
                      << "[watch-pos] Запусти --find-pos → отримай pb адресу → запусти --watch-pos --pb 0xADDR\n";
            return;
        }
        base = scanner.playerBaseCache;
        if (!base) {
            std::cerr << "[watch-pos] playerBase не знайдено в offsets.json\n"
                      << "[watch-pos] Запусти --find-pos → потім --watch-pos --pb 0xADDR\n";
            return;
        }
    }
    std::cerr << "[watch-pos] PlayerBase=0x" << std::hex << base << std::dec << "\n";
    std::cerr << "[watch-pos] Рухай персонажа МИШЕЮ в L2 (клікай далеко). Ctrl+C щоб зупинити.\n";
    std::cerr << "[watch-pos] Показуємо тільки рядки де хоча б один offset змінився.\n\n";

    // Читаємо набір кандидатів: 0x24/28/2C + 0x60/64/68 + 0x6C/70/74 + 0x78/7C/80
    // З --find-pos: pb+0x60, pb+0x6C, pb+0x78 мали великі Δ при кліку
    struct PosCandidate { unsigned off; const char* name; float prev; };
    std::vector<PosCandidate> cands = {
        {0x24, "0x24(X)",  0.f}, {0x28, "0x28(Y)",  0.f}, {0x2C, "0x2C(Z)",  0.f},
        {0x60, "0x60(X2)", 0.f}, {0x64, "0x64(Y2)", 0.f}, {0x68, "0x68(Z2)", 0.f},
        {0x6C, "0x6C(X3)", 0.f}, {0x70, "0x70(Y3)", 0.f}, {0x74, "0x74(Z3)", 0.f},
        {0x78, "0x78(X4)", 0.f}, {0x7C, "0x7C(Y4)", 0.f}, {0x80, "0x80(Z4)", 0.f},
    };
    // Заголовок
    std::cerr << std::left << std::setw(7) << "t(мс)";
    for (auto& c : cands) std::cerr << std::setw(12) << c.name;
    std::cerr << "\n" << std::string(7 + 12 * (int)cands.size(), '-') << "\n";

    // Ініціалізуємо prev
    for (auto& c : cands) ProcessMemory::Read(pid, base + c.off, &c.prev, 4);

    auto t0 = std::chrono::steady_clock::now();
    while (true) {
        auto now = std::chrono::steady_clock::now();
        int ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(now - t0).count();

        std::vector<float> vals(cands.size());
        bool any_changed = false;
        for (size_t i = 0; i < cands.size(); i++) {
            ProcessMemory::Read(pid, base + cands[i].off, &vals[i], 4);
            if (std::fabsf(vals[i] - cands[i].prev) > 10.f) any_changed = true;
        }

        if (any_changed) {
            std::cerr << std::left << std::setw(7) << ms;
            for (size_t i = 0; i < cands.size(); i++) {
                int delta = (int)(vals[i] - cands[i].prev);
                std::string cell = std::to_string((int)vals[i]);
                if (delta != 0) cell += "(Δ" + std::to_string(delta) + ")";
                std::cerr << std::setw(12) << cell;
            }
            std::cerr << "\n";
            for (size_t i = 0; i < cands.size(); i++) cands[i].prev = vals[i];
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

static void dumpKnownListObjects(const std::string& offsets_file) {
    // Wine 32-bit user space goes up to 0xBFFFFFFF (not 0x7FFFFFFF!)
    auto isValidPtr32 = [](uint32_t v) { return v > 0x10000 && v < 0xBFFF0000; };
    auto isL2XY  = [](float v) { return std::isfinite(v) && v > -330000.f && v < 330000.f && std::fabsf(v) > 200.f; };
    auto isL2Z   = [](float v) { return std::isfinite(v) && v > -17000.f  && v < 17000.f  && std::fabsf(v) > 5.f; };

    pid_t pid = findL2Pid();
    if (!pid) { std::cout << "[dump] l2.exe не знайдено у /proc\n"; return; }
    std::cout << "[dump] l2.exe PID=" << pid << "\n";

    OffsetScanner scanner(pid);
    if (scanner.loadOffsets(offsets_file))
        std::cout << "[dump] offsets.json завантажено (KnownList=0x"
                  << std::hex << scanner.knownListOff << std::dec << ")\n";

    std::cout << "[dump] blindScan() — може зайняти 10-30с...\n";
    std::flush(std::cout);
    uintptr_t playerBase = scanner.blindScan();
    if (!playerBase) { std::cout << "[dump] PlayerBase не знайдено\n"; return; }

    uint8_t pb_buf[0x140] = {};
    scanner.readBytesPublic(playerBase, pb_buf, sizeof(pb_buf));
    float px = 0.f, py = 0.f, pz = 0.f;
    std::memcpy(&px, pb_buf + 0x24, 4);
    std::memcpy(&py, pb_buf + 0x28, 4);
    std::memcpy(&pz, pb_buf + 0x2C, 4);
    std::cout << std::hex << "[dump] PlayerBase=0x" << playerBase << std::dec
              << " XYZ=(" << (int)px << "," << (int)py << "," << (int)pz << ")\n\n";

    uint32_t klPtr = 0;
    std::memcpy(&klPtr, pb_buf + scanner.knownListOff, 4);
    if (!isValidPtr32(klPtr)) {
        std::cout << "[dump] knownListPtr невалідний: 0x" << std::hex << klPtr << "\n";
        return;
    }
    std::cout << "[dump] KnownListObj=0x" << std::hex << klPtr << std::dec << "\n\n";

    // ── Крок 1: дамп самого KnownList об'єкту (0x00..0xA0) ──────────────────
    // 0x853b204 — це C++ KnownList об'єкт (vtable + поля), НЕ масив ptr-ів!
    // Шукаємо поле з вказівником на внутрішній масив об'єктів.
    uint8_t kl_buf[0xA0] = {};
    scanner.readBytesPublic(klPtr, kl_buf, sizeof(kl_buf));

    std::cout << "── KnownList об'єкт @ 0x" << std::hex << klPtr << " ──\n" << std::dec;
    std::cout << " Off | ptr32 (hex)  | int32       | Примітка\n";
    std::cout << " ----|--------------|-------------|---------------------------\n";
    for (int off = 0; off < 0xA0; off += 4) {
        uint32_t uval = 0; int32_t ival = 0;
        std::memcpy(&uval, kl_buf + off, 4);
        std::memcpy(&ival, kl_buf + off, 4);
        std::string note;
        if (isValidPtr32(uval))                  note += "PTR  ";
        if (ival > 0 && ival < 5000)             note += "COUNT?";
        std::cout << " 0x" << std::hex << std::setw(2) << std::setfill('0') << off
                  << " | 0x" << std::setw(8) << uval << " | "
                  << std::dec << std::setw(11) << std::setfill(' ') << ival
                  << " | " << note << "\n";
    }
    std::cout << "\n";

    // ── Крок 2: пошук масиву L2 об'єктів (2 рівні глибини) ─────────────────────
    // Для кожного PTR поля в klObj → перевіряємо як масив (1 рівень),
    // а якщо прямий об'єкт не має L2 coords → слідуємо його PTR полям (2 рівень).
    // Потрібно для HashMap: klObj.buckets[i] → EntryNode → L2Object*

    // Лямбда: скануємо buf[0..buflen] на пару floats у L2 bounds
    auto hasL2Coords = [&isL2XY, &isL2Z](const uint8_t* buf, int buflen) -> bool {
        for (int off = 0; off + 8 < buflen; off += 4) {
            float fx = 0.f, fy = 0.f;
            std::memcpy(&fx, buf + off,     4);
            std::memcpy(&fy, buf + off + 4, 4);
            if (isL2XY(fx) && isL2XY(fy)) return true;
        }
        return false;
    };

    struct ArrayCandidate { int kl_off; uint32_t arr_ptr; int valid_ptrs; int l2_direct; int l2_deep; };
    std::vector<ArrayCandidate> candidates;

    for (int kl_off = 0; kl_off < 0xA0; kl_off += 4) {
        uint32_t arr_ptr = 0;
        std::memcpy(&arr_ptr, kl_buf + kl_off, 4);
        if (!isValidPtr32(arr_ptr)) continue;

        uint8_t arr_buf[0x60] = {};
        if (!scanner.readBytesPublic(arr_ptr, arr_buf, sizeof(arr_buf))) continue;

        int valid_ptrs = 0, l2_direct = 0, l2_deep = 0;
        for (int i = 0; i < 12; ++i) {
            uint32_t op = 0;
            std::memcpy(&op, arr_buf + i * 4, 4);
            if (!isValidPtr32(op)) break;
            ++valid_ptrs;

            // Рівень 1: чи сам об'єкт має L2 coords? (0x00..0x200)
            uint8_t obj1[0x200] = {};
            if (!scanner.readBytesPublic(op, obj1, sizeof(obj1))) continue;
            if (hasL2Coords(obj1, sizeof(obj1))) { ++l2_direct; continue; }

            // Рівень 2: слідуємо кожному PTR полю об'єкту (0x00..0x40) → перевіряємо там
            for (int ptr_off = 0; ptr_off < 0x40; ptr_off += 4) {
                uint32_t inner = 0;
                std::memcpy(&inner, obj1 + ptr_off, 4);
                if (!isValidPtr32(inner)) continue;
                uint8_t obj2[0x200] = {};
                if (!scanner.readBytesPublic(inner, obj2, sizeof(obj2))) continue;
                if (hasL2Coords(obj2, sizeof(obj2))) { ++l2_deep; break; }
            }
        }
        if (valid_ptrs > 0)
            candidates.push_back({kl_off, arr_ptr, valid_ptrs, l2_direct, l2_deep});
    }

    std::cout << "── Пошук масиву об'єктів всередині KnownList ──\n";
    std::cout << " KL.off | arrayPtr   | ptrs | l2_direct | l2_deep | Статус\n";
    std::cout << " -------|------------|------|-----------|---------|--------\n";
    int best_kl_off = -1; int best_score = -1;
    for (auto& c : candidates) {
        int score = c.l2_direct * 3 + c.l2_deep * 2 + c.valid_ptrs;
        std::string status;
        if (c.l2_direct > 0)  status = "*** L2 direct!";
        else if (c.l2_deep > 0) status = "** L2 deep!";
        else                  status = "no L2 coords";
        if (score > best_score) { best_score = score; best_kl_off = c.kl_off; }
        std::cout << " 0x" << std::hex << std::setw(4) << std::setfill('0') << c.kl_off
                  << " | 0x" << std::setw(8) << c.arr_ptr << " | " << std::dec
                  << std::setw(4) << c.valid_ptrs << " | "
                  << std::setw(9) << c.l2_direct << " | "
                  << std::setw(7) << c.l2_deep << " | " << status << "\n"
                  << std::setfill(' ');
    }
    std::cout << "\n";

    // ── Крок 3: повний дамп перших об'єктів з найкращого масиву ─────────────────
    uint32_t best_arr_ptr = 0;
    if (best_kl_off >= 0)
        std::memcpy(&best_arr_ptr, kl_buf + best_kl_off, 4);
    else if (!candidates.empty()) {
        best_kl_off = candidates[0].kl_off;
        best_arr_ptr = candidates[0].arr_ptr;
    }

    if (!best_arr_ptr) {
        std::cout << "[dump] Жодного масиву не знайдено. Мабуть KnownList offset != 0x120.\n";
        return;
    }

    auto& best = *std::find_if(candidates.begin(), candidates.end(),
                               [&](const ArrayCandidate& c){ return c.kl_off == best_kl_off; });
    std::cout << "── Дамп об'єктів: KL+0x" << std::hex << best_kl_off
              << " → 0x" << best_arr_ptr << std::dec
              << " (ptrs=" << best.valid_ptrs
              << " direct=" << best.l2_direct << " deep=" << best.l2_deep << ") ──\n\n";

    // Лямбда для виводу одного об'єкту
    auto dumpObj = [&](uint32_t ptr, const std::string& label) {
        uint8_t buf[0x250] = {};
        scanner.readBytesPublic(ptr, buf, sizeof(buf));
        std::cout << "  " << label << " ptr=0x" << std::hex << ptr << std::dec << "\n";
        std::cout << "  Off  | int32          | float           | Примітка\n";
        std::cout << "  -----|----------------|-----------------|-------------------\n";
        for (int off = 0; off < 0x240; off += 4) {
            int32_t ival = 0; float fval = 0.f;
            std::memcpy(&ival, buf + off, 4);
            std::memcpy(&fval, buf + off, 4);
            std::string note;
            if (isL2XY(fval))                    note += "*** L2XY ";
            else if (isL2Z(fval))                note += "*** L2Z  ";
            else if (std::isfinite(fval) && fval > 10.f && fval < 100000.f
                     && fval != (float)(int)fval) note += "HP?  ";
            if (ival > 0 && ival <= 10)          note += "TYPE?";
            if (isValidPtr32((uint32_t)ival))    note += "ptr  ";
            if (!note.empty() || off < 0x40) {
                std::cout << "  0x" << std::hex << std::setw(3) << std::setfill('0') << off
                          << " | " << std::dec << std::setw(14) << ival
                          << " | " << std::setw(15) << std::fixed << std::setprecision(2) << fval
                          << " | " << note << "\n" << std::setfill(' ');
            }
        }
        // Автопошук XYZ в об'єкті
        bool found_xyz = false;
        for (int off = 0; off + 8 < 0x240; off += 4) {
            float fx = 0.f, fy = 0.f, fz = 0.f;
            std::memcpy(&fx, buf + off,     4);
            std::memcpy(&fy, buf + off + 4, 4);
            std::memcpy(&fz, buf + off + 8, 4);
            if (isL2XY(fx) && isL2XY(fy) && isL2Z(fz)) {
                std::cout << "  *** XYZ @ 0x" << std::hex << off
                          << " X=" << std::dec << (int)fx
                          << " Y=" << (int)fy << " Z=" << (int)fz << "\n";
                found_xyz = true;
            }
        }
        if (!found_xyz) std::cout << "  (XYZ не знайдено — мабуть hashmap node, дивись PTR поля)\n";
        std::cout << "\n";
    };

    for (int i = 0; i < 3; ++i) {
        uint32_t obj_ptr = 0;
        scanner.readBytesPublic((uintptr_t)best_arr_ptr + (uintptr_t)i * 4, &obj_ptr, 4);
        if (!isValidPtr32(obj_ptr)) break;
        dumpObj(obj_ptr, "arr[" + std::to_string(i) + "]");

        // Якщо цей об'єкт не має L2 coords → дамп його PTR полів (1 рівень)
        uint8_t obj1[0x50] = {};
        scanner.readBytesPublic(obj_ptr, obj1, sizeof(obj1));
        for (int ptr_off = 0; ptr_off < 0x40; ptr_off += 4) {
            uint32_t inner = 0;
            std::memcpy(&inner, obj1 + ptr_off, 4);
            if (!isValidPtr32(inner)) continue;
            uint8_t obj2[0x50] = {};
            if (!scanner.readBytesPublic(inner, obj2, sizeof(obj2))) continue;
            if (hasL2Coords(obj2, sizeof(obj2))) {
                dumpObj(inner, "  arr[" + std::to_string(i) + "]+0x"
                        + [ptr_off]{ char b[8]; snprintf(b, sizeof(b), "%x", ptr_off); return std::string(b); }());
            }
        }
    }

    // ── Крок 3b: повний hashmap traversal з brute-force XYZ scan ────────────────
    // Для кожного KL хеш-ноду → читаємо 0x200 байт → шукаємо XYZ поблизу гравця
    // НЕ припускаємо знайомий struct layout.
    {
        uint32_t bucketArrayPtr = 0;
        std::memcpy(&bucketArrayPtr, kl_buf + 0x1c, 4);
        uint32_t bucketCount = 0;
        std::memcpy(&bucketCount, kl_buf + 0x60, 4);
        if (isValidPtr32(bucketArrayPtr) && bucketCount > 0 && bucketCount <= 512) {
            std::cout << "── Hashmap node traversal (brute-force XYZ): buckets=" << bucketCount << " ──\n";

            std::vector<uint8_t> bkt_buf(bucketCount * 4, 0);
            scanner.readBytesPublic(bucketArrayPtr, bkt_buf.data(), bkt_buf.size());

            // Report statistics: how many nodes total, how many with nearby XYZ
            int total_nodes = 0, nodes_with_nearby = 0;
            std::vector<std::tuple<uint32_t,float,float,float,int>> nearby_found; // node, x,y,z, xyz_off
            const float nearby_range = 3000.f;

            for (uint32_t b = 0; b < bucketCount; ++b) {
                uint32_t nodePtr = 0;
                std::memcpy(&nodePtr, bkt_buf.data() + b * 4, 4);

                for (int step = 0; step < 500 && isValidPtr32(nodePtr); ++step) {
                    uint8_t node_buf[0x240] = {};
                    if (!scanner.readBytesPublic(nodePtr, node_buf, sizeof(node_buf))) break;

                    uint32_t nextPtr = 0;
                    std::memcpy(&nextPtr, node_buf, 4);
                    ++total_nodes;

                    // Brute-force: scan every 4-byte aligned offset for XYZ near player
                    bool found = false;
                    for (int o = 0; o + 12 <= (int)sizeof(node_buf) && !found; o += 4) {
                        float fx, fy, fz;
                        std::memcpy(&fx, node_buf + o,     4);
                        std::memcpy(&fy, node_buf + o + 4, 4);
                        std::memcpy(&fz, node_buf + o + 8, 4);
                        if (std::isfinite(fx) && std::isfinite(fy) && std::isfinite(fz)
                            && std::fabsf(fx - px) < nearby_range
                            && std::fabsf(fy - py) < nearby_range
                            && std::fabsf(fz - pz) < nearby_range
                            && (std::fabsf(fx - px) + std::fabsf(fy - py)) > 10.f) {
                            nearby_found.emplace_back(nodePtr, fx, fy, fz, o);
                            ++nodes_with_nearby;
                            found = true;
                        }
                    }
                    // Also follow each PTR field within first 0x40 bytes
                    if (!found) {
                        for (int o2 = 4; o2 < 0x40; o2 += 4) {
                            uint32_t ptrVal = 0;
                            std::memcpy(&ptrVal, node_buf + o2, 4);
                            if (!isValidPtr32(ptrVal)) continue;
                            uint8_t pbuf[0x60] = {};
                            if (!scanner.readBytesPublic(ptrVal, pbuf, sizeof(pbuf))) continue;
                            for (int o = 0; o + 12 <= (int)sizeof(pbuf) && !found; o += 4) {
                                float fx, fy, fz;
                                std::memcpy(&fx, pbuf + o,     4);
                                std::memcpy(&fy, pbuf + o + 4, 4);
                                std::memcpy(&fz, pbuf + o + 8, 4);
                                if (std::isfinite(fx) && std::isfinite(fy) && std::isfinite(fz)
                                    && std::fabsf(fx - px) < nearby_range
                                    && std::fabsf(fy - py) < nearby_range
                                    && std::fabsf(fz - pz) < nearby_range
                                    && (std::fabsf(fx - px) + std::fabsf(fy - py)) > 10.f) {
                                    std::cout << "  node=0x" << std::hex << nodePtr
                                              << " → ptr@+" << o2 << "=0x" << ptrVal
                                              << " XYZ@+" << o << " (" << std::dec
                                              << (int)fx << "," << (int)fy << "," << (int)fz << ")\n";
                                    ++nodes_with_nearby;
                                    found = true;
                                }
                            }
                        }
                    }

                    if (nextPtr == nodePtr || !isValidPtr32(nextPtr)) break;
                    nodePtr = nextPtr;
                }
            }
            std::cout << "  Всього nodes=" << total_nodes
                      << " з nearby XYZ=" << nodes_with_nearby << "\n";
            if (nodes_with_nearby > 0 && !nearby_found.empty()) {
                std::cout << " node       | XYZ_off | X          | Y          | Z\n";
                std::cout << " -----------|---------|------------|------------|---\n";
                for (auto& [np, fx, fy, fz, off] : nearby_found) {
                    std::cout << " 0x" << std::hex << std::setw(8) << std::setfill('0') << np
                              << " | +" << std::setw(5) << off
                              << " | " << std::dec << std::setw(10) << (int)fx
                              << " | " << std::setw(10) << (int)fy
                              << " | " << std::setw(6) << (int)fz << "\n"
                              << std::setfill(' ');
                }
            }
            if (nodes_with_nearby == 0)
                std::cout << "  (жодного node з nearby coords — KL+0x1c не є nearby-mob list)\n";
            std::cout << "\n";
        }
    }

    // ── Крок 3c: сканування всіх полів playerBase[0..0x300] на nearby objects ────
    // Знаходимо правильний offset KnownList без припущень.
    // Для кожного PTR поля → 2 рівні пошуку → чи є поблизу гравця?
    {
        std::cout << "── Сканування playerBase полів на nearby objects (Y=" << (int)py << "±3000) ──\n";
        std::cout << " pbOff  | ptr        | level | XYZ знайдено\n";
        std::cout << " -------|------------|-------|-------------\n";

        uint8_t pb_scan[0x300] = {};
        scanner.readBytesPublic(playerBase, pb_scan, sizeof(pb_scan));

        auto nearXYZ = [&](uint32_t ptr, int depth, int pb_off) -> bool {
            if (!isValidPtr32(ptr)) return false;
            uint8_t buf[0x250] = {};
            if (!scanner.readBytesPublic(ptr, buf, sizeof(buf))) return false;
            // Scan for XYZ triplet near player
            for (int o = 0; o + 12 <= 0x250; o += 4) {
                float fx, fy, fz;
                std::memcpy(&fx, buf + o,     4);
                std::memcpy(&fy, buf + o + 4, 4);
                std::memcpy(&fz, buf + o + 8, 4);
                if (std::isfinite(fx) && std::isfinite(fy) && std::isfinite(fz)
                    && std::fabsf(fx - px) < 3000.f && std::fabsf(fy - py) < 3000.f
                    && std::fabsf(fz - pz) < 3000.f
                    && (std::fabsf(fx - px) > 1.f || std::fabsf(fy - py) > 1.f)) {
                    std::cout << " 0x" << std::hex << std::setw(4) << std::setfill('0') << pb_off
                              << " | 0x" << std::setw(8) << ptr
                              << " | L" << depth
                              << " @ +0x" << std::setw(3) << o
                              << " | X=" << std::dec << (int)fx
                              << " Y=" << (int)fy << " Z=" << (int)fz
                              << "\n" << std::setfill(' ');
                    return true;
                }
            }
            return false;
        };

        int found_nearby = 0;
        for (int pb_off = 0; pb_off < 0x300; pb_off += 4) {
            uint32_t ptr1 = 0;
            std::memcpy(&ptr1, pb_scan + pb_off, 4);
            if (!isValidPtr32(ptr1)) continue;

            // L1: does ptr1 contain nearby XYZ?
            if (nearXYZ(ptr1, 1, pb_off)) { ++found_nearby; continue; }

            // L2: does any ptr within ptr1[0..0x80] contain nearby XYZ?
            uint8_t buf1[0x80] = {};
            if (!scanner.readBytesPublic(ptr1, buf1, sizeof(buf1))) continue;
            for (int o2 = 0; o2 < 0x80; o2 += 4) {
                uint32_t ptr2 = 0;
                std::memcpy(&ptr2, buf1 + o2, 4);
                if (nearXYZ(ptr2, 2, pb_off)) { ++found_nearby; break; }
            }
        }
        if (found_nearby == 0)
            std::cout << " (жодного поля в playerBase не веде до nearby objects)\n";
        std::cout << "\n";
    }

    // ── Крок 4: пряме сканування пам'яті на float triplets поблизу гравця ──────
    // Надійний метод: ігнорує структуру контейнера, шукає XYZ напряму в heap.
    // Знаходить БУДЬ-ЯКІ об'єкти що мають float координати поблизу гравця.
    std::cout << "── Сканування пам'яті: floats поблизу гравця (±3000 units) ──\n";
    std::cout << "[scan] Гравець X=" << (int)px << " Y=" << (int)py << " Z=" << (int)pz << "\n";
    std::cout << "[scan] Шукаємо float triplets X±5000, Y±5000, Z±5000...\n";
    std::flush(std::cout);

    // Шукаємо як float ТАК І int32 — Kamael може зберігати coords як int
    const float xlo = px - 2000.f, xhi = px + 2000.f;
    const float ylo = py - 2000.f, yhi = py + 2000.f;
    const float zlo = pz - 2000.f, zhi = pz + 2000.f;
    const int32_t ixlo = (int32_t)px - 2000, ixhi = (int32_t)px + 2000;
    const int32_t iylo = (int32_t)py - 2000, iyhi = (int32_t)py + 2000;
    const int32_t izlo = (int32_t)pz - 2000, izhi = (int32_t)pz + 2000;

    // Читаємо /proc/<pid>/maps для readable регіонів
    std::ifstream maps_f("/proc/" + std::to_string(pid) + "/maps");
    struct NearObj { uintptr_t addr; float x, y, z; bool is_int; };
    std::vector<NearObj> found;
    std::string maps_line;
    while (std::getline(maps_f, maps_line) && found.size() < 500) {
        if (maps_line.size() < 20) continue;
        uintptr_t a0 = 0, a1 = 0; char perms[8] = {};
        if (std::sscanf(maps_line.c_str(), "%lx-%lx %7s", &a0, &a1, perms) < 3) continue;
        if (perms[0] != 'r') continue;
        if (maps_line.find("[vdso]") != std::string::npos) continue;
        if (maps_line.find("[stack]") != std::string::npos) continue;
        size_t sz = a1 - a0;
        if (sz < 12 || sz > 64*1024*1024) continue;

        std::vector<uint8_t> rbuf(sz);
        if (!scanner.readBytesPublic(a0, rbuf.data(), sz)) continue;

        for (size_t i = 0; i + 12 <= sz; i += 4) {
            // Float scan
            float fx = 0.f, fy = 0.f, fz = 0.f;
            std::memcpy(&fx, rbuf.data() + i,     4);
            std::memcpy(&fy, rbuf.data() + i + 4, 4);
            std::memcpy(&fz, rbuf.data() + i + 8, 4);
            if (std::isfinite(fx) && std::isfinite(fy) && std::isfinite(fz)
                && fx >= xlo && fx <= xhi && fy >= ylo && fy <= yhi
                && fz >= zlo && fz <= zhi
                // Виключаємо точну позицію гравця (shadow buffer copies)
                && (std::fabsf(fx-px) > 5.f || std::fabsf(fy-py) > 5.f || std::fabsf(fz-pz) > 5.f)) {
                found.push_back({a0 + i, fx, fy, fz, false});
                continue;
            }
            // Int32 scan (якщо float не збігається)
            int32_t ix = 0, iy = 0, iz = 0;
            std::memcpy(&ix, rbuf.data() + i,     4);
            std::memcpy(&iy, rbuf.data() + i + 4, 4);
            std::memcpy(&iz, rbuf.data() + i + 8, 4);
            if (ix >= ixlo && ix <= ixhi && iy >= iylo && iy <= iyhi
                && iz >= izlo && iz <= izhi
                && (std::abs(ix-(int32_t)px) > 5 || std::abs(iy-(int32_t)py) > 5 || std::abs(iz-(int32_t)pz) > 5)) {
                found.push_back({a0 + i, (float)ix, (float)iy, (float)iz, true});
            }
        }
    }

    std::cout << "[scan] Знайдено " << found.size() << " кандидатів:\n";
    std::cout << "  Addr       | X          | Y          | Z          | dist_from_player\n";
    std::cout << "  -----------|------------|------------|------------|----------------\n";

    // Групуємо: якщо кілька адрес поряд — один і той самий об'єкт
    for (size_t i = 0; i < found.size() && i < 30; ++i) {
        float dx = found[i].x - px, dy = found[i].y - py, dz = found[i].z - pz;
        float dist = std::sqrt(dx*dx + dy*dy + dz*dz);
        std::cout << "  0x" << std::hex << std::setw(8) << std::setfill('0') << found[i].addr
                  << " | " << std::dec << std::setw(10) << (int)found[i].x
                  << " | " << std::setw(10) << (int)found[i].y
                  << " | " << std::setw(10) << (int)found[i].z
                  << " | " << std::setw(14) << std::setprecision(0) << std::fixed << dist
                  << "\n" << std::setfill(' ');
    }

    if (!found.empty()) {
        // Беремо перший результат, знаходимо base object за різними можливими offsets
        std::cout << "\n[scan] Аналіз структури за першим кандидатом @ 0x"
                  << std::hex << found[0].addr << std::dec << ":\n";
        std::cout << "[scan] Якщо XYZ @ addr, тоді L2Object base може бути addr - OFF_OBJ_X\n";
        std::cout << "[scan] Перевірте offsets 0x00..0x80 від addr в gridi по 4:\n";

        // Для кожного з перших 3 знахідок — читаємо 0x80 байт ДО і ПІСЛЯ адреси
        for (size_t ni = 0; ni < std::min(found.size(), (size_t)3); ++ni) {
            uintptr_t base = found[ni].addr;
            uint8_t nbuf[0x100] = {};
            if (!scanner.readBytesPublic(base > 0x80 ? base - 0x80 : base, nbuf, sizeof(nbuf))) continue;
            uintptr_t off0 = (base > 0x80) ? 0x80 : 0;  // де в буфері X знаходиться
            std::cout << "\n  Об'єкт #" << ni << " @ 0x" << std::hex << base << std::dec
                      << " (X=" << (int)found[ni].x << " Y=" << (int)found[ni].y
                      << " Z=" << (int)found[ni].z << ")\n";
            std::cout << "  Off від X | int32     | float      | Примітка\n";
            for (int d = -(int)off0; d <= 0x60; d += 4) {
                size_t buf_off = (size_t)((int)off0 + d);
                if (buf_off + 4 > sizeof(nbuf)) break;
                int32_t ival = 0; float fval = 0.f;
                std::memcpy(&ival, nbuf + buf_off, 4);
                std::memcpy(&fval, nbuf + buf_off, 4);
                std::string note;
                if (d == 0) note += "<== X  ";
                else if (d == 4) note += "<== Y  ";
                else if (d == 8) note += "<== Z  ";
                if (isValidPtr32((uint32_t)ival)) note += "ptr  ";
                if (ival > 0 && ival <= 10)       note += "TYPE?";
                if (!note.empty() || (d >= -0x30 && d <= 0x30)) {
                    std::cout << "  " << std::showpos << std::setw(8) << d << " | "
                              << std::noshowpos << std::setw(9) << ival << " | "
                              << std::setw(10) << std::setprecision(1) << fval
                              << " | " << note << "\n";
                }
            }
        }

        std::cout << "\n[scan] Запишіть в offsets.json:\n";
        std::cout << "  OFF_OBJ_X = " << (int)(found[0].addr & 0xFF)
                  << " (нижній byte addr: 0x" << std::hex << (found[0].addr & 0xFF) << ")\n"
                  << std::dec;
        std::cout << "  OFF_OBJ_Y = OFF_OBJ_X + 4\n  OFF_OBJ_Z = OFF_OBJ_X + 8\n";
        std::cout << "  OFF_OBJ_TYPE: знайдіть поле int [0..3] ВИЩЕ від X в дампі вище\n";
        std::cout << "  OFF_KNOWN_LIST: перевірте чи playerBase + 0x120 → вказує\n"
                  << "    на контейнер звідки можна дістатись до цих об'єктів\n";
    } else {
        std::cout << "[scan] Не знайдено — перевірте що стоїте поруч з мобами і гра запущена.\n";
    }
    std::cout << "\n[dump] Гравець: X=" << (int)px << " Y=" << (int)py << " Z=" << (int)pz << "\n";
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
        if (a == "--pb" && i + 1 < argc) {
            override_pb = (uintptr_t)std::stoull(argv[++i], nullptr, 16);
        }
        if (a == "--config" && i + 1 < argc) config_path = argv[++i];
    }

    // ─── --dump-objects: дамп об'єктів KnownList для калібровки offsets ──────
    if (dump_objects) {
        Config cfg; cfg.Load(config_path);
        dumpKnownListObjects(cfg.knownlist_offsets_file);
        return 0;
    }

    // ─── --discover-klist: CE-style reverse pointer scan ─────────────────────
    // Запуск: стань поряд з 3+ мобами у L2 → ./rdga1bot --discover-klist
    // Знаходить PlayerBase, сканує heap для mob XYZ, потім CE reverse pointer
    // scan щоб знайти реальний OFF_KNOWN_LIST для Kamael клієнту.
    // Результат зберігається в offsets.json. Займає ~10-30с.
    if (discover_klist) {
        pid_t pid = findL2Pid();
        if (!pid) { std::cerr << "[discover-klist] L2 процес не знайдено\n"; return 1; }
        std::cerr << "[discover-klist] PID=" << pid << "\n";

        Config cfg; cfg.Load(config_path);
        OffsetScanner scanner(pid);

        // Крок 1: PlayerBase — з кешу (з верифікацією!) або blindScan
        uintptr_t pb = 0;
        if (!cfg.knownlist_offsets_file.empty()) {
            scanner.loadOffsets(cfg.knownlist_offsets_file);
            uintptr_t cached = scanner.playerBaseCache;
            if (cached) {
                // Верифікація: перевіряємо координати поточної сесії
                float cpx = scanner.rpm_pub<float>(cached + OFF_PLAYER_X);
                float cpy = scanner.rpm_pub<float>(cached + OFF_PLAYER_Y);
                // Обидві координати мають бути ненульовими (Y=0 → невалідна/стара адреса)
                bool valid = std::isfinite(cpx) && std::isfinite(cpy)
                          && std::fabsf(cpx) > 200.f && std::fabsf(cpy) > 200.f;
                if (valid) {
                    pb = cached;
                    std::cerr << "[discover-klist] PlayerBase з кешу: 0x" << std::hex << pb
                              << " XY=(" << std::dec << (int)cpx << "," << (int)cpy << ")\n";
                } else {
                    std::cerr << "[discover-klist] Кешований pb=0x" << std::hex << cached
                              << " невалідний (X=" << std::dec << cpx << ") → blindScan\n";
                }
            }
        }
        if (!pb) {
            std::cerr << "[discover-klist] blindScan (30с)...\n";
            pb = scanner.blindScan(30000);
        }
        if (!pb) {
            std::cerr << "[discover-klist] PlayerBase не знайдено. "
                      << "Запусти --find-pos спочатку.\n";
            return 1;
        }
        {
            float dpx = scanner.rpm_pub<float>(pb + OFF_PLAYER_X);
            float dpy = scanner.rpm_pub<float>(pb + OFF_PLAYER_Y);
            float dpz = scanner.rpm_pub<float>(pb + OFF_PLAYER_Z);
            std::cerr << "[discover-klist] PlayerBase=0x" << std::hex << pb
                      << " XYZ=(" << std::dec << (int)dpx << "," << (int)dpy
                      << "," << (int)dpz << ")\n";
        }

        // Крок 2: region scan для знаходження реальних mob addresses
        KnownListReader reader(pid, scanner);
        std::cerr << "[discover-klist] Region scan для мобів (max 2500u)...\n";
        auto mobs = reader.readMobsRegionScan(pb, 2500.f);
        if (mobs.empty()) {
            std::cerr << "[discover-klist] Мобів не знайдено region scan'ом. "
                      << "Стань ближче до мобів.\n";
            return 1;
        }
        std::cerr << "[discover-klist] Знайдено " << mobs.size() << " мобів (всього)\n";

        // Відбір для CE reverse scan: топ-30 за HP, мінімум HP≥1000.
        // HP<1000 → false positive (render-буфер, navmesh node тощо).
        // Лімітуємо 30 адресами — достатньо для autoDiscoverKnownList,
        // і reverse scan завершується за ~30с замість 10+ хвилин.
        std::sort(mobs.begin(), mobs.end(),
                  [](const L2Character& a, const L2Character& b){ return a.hp > b.hp; });
        std::vector<uintptr_t> mobAddrs;
        std::cerr << "[discover-klist] Топ мобів для CE scan (HP≥1000):\n";
        for (const auto& m : mobs) {
            if (m.hp < 1000.f) break; // відсортовано за спаданням, далі все менше
            if (mobAddrs.size() >= 30) break;
            std::cerr << "  memPtr=0x" << std::hex << m.memPtr
                      << " X=" << std::dec << (int)m.x
                      << " Y=" << (int)m.y
                      << " HP=" << (int)m.hp << "\n";
            mobAddrs.push_back(m.memPtr);
        }
        if (mobAddrs.empty()) {
            std::cerr << "[discover-klist] Жодного моба з HP≥1000. "
                      << "Стань ближче до живих мобів.\n";
            return 1;
        }
        std::cerr << "[discover-klist] " << std::dec << mobAddrs.size()
                  << " адрес передано CE reverse scan.\n";

        // ── Дамп структури першого моба для діагностики ──────────────────────────
        if (!mobs.empty()) {
            const auto& m0 = mobs[0];
            std::cerr << "[discover-klist] Dump mob[0] @ 0x"
                      << std::hex << m0.memPtr << " X=" << std::dec << (int)m0.x << ":\n";
            for (int d = 0; d < 64; d++) {
                uint32_t v = scanner.rpm_pub<uint32_t>(m0.memPtr + (uintptr_t)d * 4);
                float vf = 0.f;
                std::memcpy(&vf, &v, 4);
                std::cerr << "  +" << std::setw(3) << std::hex << d*4
                          << " = 0x" << std::setw(8) << std::setfill('0') << v
                          << std::setfill(' ');
                if (std::isfinite(vf) && std::fabsf(vf) > 100.f && std::fabsf(vf) < 327000.f)
                    std::cerr << " (float:" << std::dec << (int)vf << ")";
                else if (v > 0x10000u && v < 0xBFFF0000u)
                    std::cerr << " (ptr)";
                std::cerr << "\n";
            }
        }

        // ── Range reverse pointer scan ±0x200 для першого моба ──────────────────
        // Знаходимо ХТО тримає pointer близько до mob address (КL може зберігати
        // іншу базу того самого об'єкту, наприклад mobBase±N).
        if (!mobAddrs.empty()) {
            uintptr_t target = mobAddrs[0];
            uintptr_t range  = 0x200;
            std::cerr << "[discover-klist] Range ptr scan mob[0]=0x"
                      << std::hex << target << " ±0x" << range << ":\n";
            // Читаємо регіони (спрощено: перші 64MB процесу)
            bool found_any = false;
            char maps_path[64];
            std::snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps", (int)pid);
            FILE* mf = std::fopen(maps_path, "r");
            if (mf) {
                char ln[512];
                while (std::fgets(ln, sizeof(ln), mf)) {
                    uintptr_t rbase = 0, rend = 0;
                    char rperms[8] = {};
                    std::sscanf(ln, "%lx-%lx %4s", &rbase, &rend, rperms);
                    if (rperms[0] != 'r') continue;
                    if (rbase < 0x10000u || rbase >= 0x70000000u) continue;
                    size_t rsz = rend - rbase;
                    if (rsz > 64u*1024*1024) continue;
                    std::vector<uint8_t> rbuf(rsz);
                    if (!scanner.readBytesPublic(rbase, rbuf.data(), rsz)) continue;
                    for (size_t ri = 0; ri + 4 <= rsz; ri += 4) {
                        uint32_t rv;
                        std::memcpy(&rv, rbuf.data() + ri, 4);
                        if (rv >= target - range && rv <= target + range) {
                            uintptr_t holder = rbase + ri;
                            std::cerr << "  0x" << std::hex << holder
                                      << " → 0x" << rv
                                      << " (off from target: "
                                      << std::dec << (int)(rv - (uint32_t)target) << ")\n";
                            found_any = true;
                            // Перевіряємо якщо holder близько до pb
                            if (holder >= pb && holder < pb + 0x2000)
                                std::cerr << "    *** IN PLAYER STRUCT @ pb+0x"
                                          << std::hex << (holder - pb) << " ***\n";
                        }
                    }
                }
                std::fclose(mf);
            }
            if (!found_any)
                std::cerr << "  (жодного pointer в heap)\n";
        }

        // ── KnownList структурний probe ──────────────────────────────────────────
        // pb+0x120 → KnownList C++ object. Всередині є sub-struct з vtable, count,
        // data ptr та bucket array. Шукаємо реальні L2Character об'єкти з XY@+0x24.
        {
            uint32_t klHead = scanner.rpm_pub<uint32_t>(pb + 0x120);
            std::cerr << "[klist-probe] klHead=0x" << std::hex << klHead << "\n";
            if (scanner.isValidPtr_pub(klHead)) {
                // Sub-struct starts at klHead+0x1c (where vtable appears in dump)
                uint32_t elemCount = scanner.rpm_pub<uint32_t>((uintptr_t)klHead + 0x20);
                uint32_t dataPtr   = scanner.rpm_pub<uint32_t>((uintptr_t)klHead + 0x24);
                uint32_t buckPtr   = scanner.rpm_pub<uint32_t>((uintptr_t)klHead + 0x2c);
                uint32_t buckCount = scanner.rpm_pub<uint32_t>((uintptr_t)klHead + 0x30);
                std::cerr << "[klist-probe] elemCount=" << std::dec << elemCount
                          << " dataPtr=0x" << std::hex << dataPtr
                          << " buckPtr=0x" << buckPtr
                          << " buckCount=" << std::dec << buckCount << "\n";

                // Dump raw content of dataPtr and buckPtr (first 20 dwords each)
                for (auto [tag, dptr] : std::initializer_list<std::pair<const char*, uint32_t>>{
                        {"dataPtr", dataPtr}, {"buckPtr", buckPtr}}) {
                    if (!scanner.isValidPtr_pub(dptr)) continue;
                    std::cerr << "[klist-probe] " << tag << " 0x" << std::hex << dptr << " raw[0..19]:";
                    for (int di = 0; di < 20; di++) {
                        uint32_t dv = scanner.rpm_pub<uint32_t>((uintptr_t)dptr + (uintptr_t)di*4);
                        std::cerr << " " << std::hex << dv;
                    }
                    std::cerr << "\n";
                }

                // Follow linked list from firstElem via +0x00 (next ptr), check XY
                // at +0x24/+0x28 (player-style) and +0x90/+0x94 (render-style)
                uint32_t llCur = scanner.rpm_pub<uint32_t>(klHead);
                std::cerr << "[klist-probe] LL from klHead+0 chain (max 30):\n";
                static const uintptr_t LL_XY_OFFS[] = {0x18,0x1c,0x24,0x28,0x48,0x60,0x64,0x90,0x94};
                float ppx2 = scanner.rpm_pub<float>(pb + 0x24);
                float ppy2 = scanner.rpm_pub<float>(pb + 0x28);
                for (int li = 0; li < 30 && scanner.isValidPtr_pub(llCur) && llCur != klHead; li++) {
                    bool found_xy = false;
                    for (uintptr_t xoff : LL_XY_OFFS) {
                        float fx = scanner.rpm_pub<float>((uintptr_t)llCur + xoff);
                        float fy = scanner.rpm_pub<float>((uintptr_t)llCur + xoff + 4);
                        if (!std::isfinite(fx) || !std::isfinite(fy)) continue;
                        if (std::fabsf(fx) < 5000.f || std::fabsf(fx) > 330000.f) continue;
                        if (std::fabsf(fy) < 1000.f || std::fabsf(fy) > 330000.f) continue;
                        float dx = fx - ppx2, dy = fy - ppy2;
                        std::cerr << "  LL[" << std::dec << li << "] 0x" << std::hex << llCur
                                  << " XY@+0x" << xoff << "=(" << std::dec << (int)fx << "," << (int)fy
                                  << ") dist=" << (int)std::sqrtf(dx*dx+dy*dy) << "\n";
                        found_xy = true; break;
                    }
                    if (!found_xy)
                        std::cerr << "  LL[" << std::dec << li << "] 0x" << std::hex << llCur << " (no XY)\n";
                    // follow next ptr at +0x00
                    uint32_t nxt = scanner.rpm_pub<uint32_t>((uintptr_t)llCur);
                    if (nxt == llCur) break; // self-loop = end
                    llCur = nxt;
                }

                // Probe firstElem sub-ptrs for L2Character (XY @ +0x24/+0x28)
                uint32_t firstElem = scanner.rpm_pub<uint32_t>(klHead);
                std::cerr << "[klist-probe] firstElem=0x" << std::hex << firstElem << "\n";
                if (scanner.isValidPtr_pub(firstElem)) {
                    static const uintptr_t FE_OFFS[] = {
                        0x08,0x0c,0x10,0x14,0x18,0x1c,
                        0x38,0x3c,0x40,0x44,0x48,
                        0x68,0x6c,0x70,0x74,0x78,
                        0xb0,0xb4,0xb8
                    };
                    for (uintptr_t fo : FE_OFFS) {
                        uint32_t sub = scanner.rpm_pub<uint32_t>((uintptr_t)firstElem + fo);
                        if (!scanner.isValidPtr_pub(sub)) continue;
                        float fx = scanner.rpm_pub<float>((uintptr_t)sub + 0x24);
                        float fy = scanner.rpm_pub<float>((uintptr_t)sub + 0x28);
                        float fz = scanner.rpm_pub<float>((uintptr_t)sub + 0x2c);
                        bool coord_xy = std::isfinite(fx) && std::fabsf(fx) > 5000.f
                                     && std::isfinite(fy) && std::fabsf(fy) > 1000.f
                                     && std::fabsf(fx) < 330000.f
                                     && std::fabsf(fy) < 330000.f;
                        std::cerr << "  firstElem+0x" << std::hex << fo
                                  << " → 0x" << sub
                                  << " XY@+0x24=(" << std::dec << (int)fx << "," << (int)fy
                                  << "," << (int)fz << ")"
                                  << (coord_xy ? " *** L2CHAR? ***" : "") << "\n";
                    }
                }

                // Scan first buckets of hash map at buckPtr — check XY at multiple offsets
                if (scanner.isValidPtr_pub(buckPtr) && buckCount > 0 && buckCount < 100000u) {
                    std::cerr << "[klist-probe] Scanning " << std::dec << buckCount
                              << " buckets @ 0x" << std::hex << buckPtr << "...\n";
                    // XY offset candidates: player uses +0x24, render objs use +0x90
                    static const uintptr_t XY_TRY[] = {0x18,0x1c,0x24,0x28,0x48,0x60,0x64,0x90,0x94};
                    int mobs_found = 0, bucket_dumps = 0;
                    float ppx = scanner.rpm_pub<float>(pb + 0x24);
                    float ppy = scanner.rpm_pub<float>(pb + 0x28);
                    for (uint32_t bi = 0; bi < buckCount && mobs_found < 10; bi++) {
                        uint32_t nodePtr = scanner.rpm_pub<uint32_t>((uintptr_t)buckPtr + (uintptr_t)bi * 4);
                        if (!scanner.isValidPtr_pub(nodePtr)) continue;
                        // Dump raw structure of first few non-null bucket nodes
                        if (bucket_dumps < 3) {
                            std::cerr << "  bucket[" << std::dec << bi
                                      << "] node=0x" << std::hex << nodePtr << " raw[0..12]:";
                            for (int di = 0; di < 13; di++) {
                                uint32_t dv = scanner.rpm_pub<uint32_t>((uintptr_t)nodePtr + (uintptr_t)di*4);
                                std::cerr << " " << std::hex << dv;
                            }
                            std::cerr << "\n";
                            bucket_dumps++;
                        }
                        // Try node and node+4..node+16 as L2Character*, check XY at multiple offsets
                        for (uintptr_t ni = 0; ni <= 0x10; ni += 4) {
                            uint32_t charPtr = (ni == 0) ? nodePtr
                                : scanner.rpm_pub<uint32_t>((uintptr_t)nodePtr + ni);
                            if (!scanner.isValidPtr_pub(charPtr)) continue;
                            for (uintptr_t xoff : XY_TRY) {
                                float fx = scanner.rpm_pub<float>((uintptr_t)charPtr + xoff);
                                float fy = scanner.rpm_pub<float>((uintptr_t)charPtr + xoff + 4);
                                if (!std::isfinite(fx) || std::fabsf(fx) < 5000.f
                                 || !std::isfinite(fy) || std::fabsf(fy) < 1000.f
                                 || std::fabsf(fx) > 330000.f || std::fabsf(fy) > 330000.f) continue;
                                float dx = fx - ppx, dy = fy - ppy;
                                float dist = std::sqrtf(dx*dx + dy*dy);
                                std::cerr << "  bucket[" << std::dec << bi
                                          << "] node+0x" << std::hex << ni << "→0x" << charPtr
                                          << " XY@+0x" << xoff << "=("
                                          << std::dec << (int)fx << "," << (int)fy
                                          << ") dist=" << (int)dist << " *** L2CHAR xoff=0x"
                                          << std::hex << xoff << " ***\n";
                                mobs_found++;
                                break;
                            }
                            if (mobs_found >= 10) break;
                        }
                    }
                    if (!mobs_found)
                        std::cerr << "[klist-probe] Жодного L2Char у hash buckets.\n";
                }
            }
        }

        // Крок 3: CE reverse pointer scan → autoDiscoverKnownList
        uintptr_t klOff = scanner.autoDiscoverKnownList(pb, mobAddrs);
        if (klOff) {
            std::cerr << "[discover-klist] SUCCESS! OFF_KNOWN_LIST=0x"
                      << std::hex << klOff << "\n"
                      << "[discover-klist] Оновіть offsets_config.h: "
                      << "constexpr uintptr_t OFF_KNOWN_LIST = 0x"
                      << klOff << ";\n" << std::dec;
            if (!cfg.knownlist_offsets_file.empty())
                scanner.saveOffsets(cfg.knownlist_offsets_file);
        }
        return 0;
    }

    // ─── --calibrate: швидкий дамп першого об'єкту KnownList ─────────────────
    if (calibrate) {
        pid_t pid = findL2Pid();
        if (!pid) { std::cerr << "L2 process not found\n"; return 1; }
        OffsetScanner scanner(pid);
        std::cerr << "[CAL] Running blindScan...\n";
        uintptr_t playerBase = scanner.blindScan();
        if (!playerBase) {
            std::cerr << "[CAL] PlayerBase not found. Is L2 running?\n";
            return 1;
        }

        uintptr_t klPtr = scanner.rpm_pub<uint32_t>(playerBase + 0x120);
        std::cerr << "[CAL] PlayerBase=0x" << std::hex << playerBase
                  << " KnownListPtr=0x" << klPtr << std::dec << "\n";
        std::cerr << "[CAL] Player XYZ: "
                  << scanner.rpm_pub<float>(playerBase + 0x24) << " / "
                  << scanner.rpm_pub<float>(playerBase + 0x28) << " / "
                  << scanner.rpm_pub<float>(playerBase + 0x2C) << "\n";

        // Знайти перший читабельний об'єкт (не all-zeros)
        auto dumpObjFull = [&](uintptr_t oPtr, int idx) {
            std::cerr << "\n[CAL] obj[" << idx << "] ptr=0x" << std::hex
                      << oPtr << std::dec << " — full dump (0x00..0x140):\n";
            std::cerr << std::left
                      << std::setw(8)  << "offset"
                      << std::setw(14) << "hex_uint32"
                      << std::setw(14) << "as_int32"
                      << "as_float\n";
            std::cerr << std::string(50, '-') << "\n";
            for (uintptr_t off = 0; off <= 0x140; off += 4) {
                uint32_t raw  = scanner.rpm_pub<uint32_t>(oPtr + off);
                int32_t  as_i = (int32_t)raw;
                float    as_f = 0.f;
                std::memcpy(&as_f, &raw, 4);
                // Позначаємо рядки що виглядають як L2-координати
                bool coord_like = std::isfinite(as_f)
                    && std::fabsf(as_f) > 500.f && std::fabsf(as_f) < 400000.f;
                bool icoord_like = std::abs(as_i) > 500 && std::abs(as_i) < 400000;
                std::cerr << "0x" << std::hex << std::setw(4) << off << "  "
                          << "0x" << std::setw(10) << raw << "  "
                          << std::dec << std::setw(12) << as_i << "  "
                          << as_f;
                if (coord_like || icoord_like) std::cerr << "  ← coord?";
                std::cerr << "\n";
            }
        };

        // Шукаємо перший читабельний об'єкт (не all-zeros, не нульовий ptr)
        int first_readable = -1;
        {
            int null_s2 = 0;
            for (int i = 0; i < 20; ++i) {
                uintptr_t oPtr = scanner.rpm_pub<uint32_t>(klPtr + (uintptr_t)i * 4);
                if (!scanner.isValidPtr_pub(oPtr)) {
                    if (++null_s2 >= 8) break;
                    continue;
                }
                null_s2 = 0;
                // Перевіряємо чи перші 4 байти не нулі
                uint32_t v0 = scanner.rpm_pub<uint32_t>(oPtr);
                uint32_t v4 = scanner.rpm_pub<uint32_t>(oPtr + 4);
                if (v0 != 0 || v4 != 0) { first_readable = i; break; }
            }
        }
        if (first_readable >= 0) {
            uintptr_t oPtr = scanner.rpm_pub<uint32_t>(klPtr + (uintptr_t)first_readable * 4);
            dumpObjFull(oPtr, first_readable);
        } else {
            std::cerr << "[CAL] Жодного читабельного об'єкту не знайдено в KL array\n";
        }

        // Перші 20 об'єктів: розширений XYZ scan (0x10..0x100)
        std::cerr << "\n[CAL] First 20 objects — XYZ scan (offsets 0x10..0x100):\n";
        int null_s = 0;
        for (int i = 0; i < 20; ++i) {
            uintptr_t oPtr = scanner.rpm_pub<uint32_t>(klPtr + (uintptr_t)i * 4);
            if (!scanner.isValidPtr_pub(oPtr)) {
                if (++null_s >= 8) break;
                continue;
            }
            null_s = 0;
            std::cerr << "  obj[" << i << "] 0x" << std::hex << oPtr << std::dec;
            for (uintptr_t off = 0x10; off <= 0x100; off += 4) {
                float v = scanner.rpm_pub<float>(oPtr + off);
                if (std::isfinite(v) && std::fabsf(v) > 500.f
                                     && std::fabsf(v) < 400000.f)
                    std::cerr << " [0x" << std::hex << off << "="
                              << std::dec << (int)v << "]";
            }
            std::cerr << "\n";
        }

        std::cerr << "\n[CAL] Player XYZ: "
                  << (int)scanner.rpm_pub<float>(playerBase + 0x24) << " / "
                  << (int)scanner.rpm_pub<float>(playerBase + 0x28) << " / "
                  << (int)scanner.rpm_pub<float>(playerBase + 0x2C) << "\n";
        std::cerr << "[CAL] Знайди offset де значення ≈ XYZ гравця або мобів поряд.\n"
                  << "[CAL] Перевір int32 значення поряд з XYZ для typeID (0=mob,1=player,2=item).\n";

        // --- Name scan (якщо передано --name "NpcName") ---
        std::string cal_name;
        for (int i = 2; i < argc - 1; ++i) {
            if (std::string(argv[i]) == "--name") {
                cal_name = argv[i + 1];
                break;
            }
        }
        if (!cal_name.empty()) {
            std::cerr << "\n[CAL] === Name offset scan для \"" << cal_name << "\" ===\n";
            uintptr_t nameOff = scanner.findNameOffset(playerBase, cal_name);
            if (nameOff)
                std::cerr << "[CAL] OFF_OBJ_NAME = 0x" << std::hex << nameOff
                          << std::dec << " → оновити offsets_config.h\n";
        } else {
            std::cerr << "\n[CAL] Для пошуку назви: --calibrate --name \"Ім'я моба\"\n";
        }

        // --- Heading calibration ---
        std::cerr << "\n[CAL] === Heading scan ===\n";
        scanner.calibrateHeadingOffset(playerBase);
        std::cerr << "[CAL] Usage: --calibrate [--name \"MobName\"]\n";

        return 0;
    }

    // ─── --heading-monitor: live monitor змін у playerBase struct ────────────────
    if (heading_monitor) {
        pid_t pid = findL2Pid();
        if (!pid) { std::cerr << "[HeadingMon] l2.exe не знайдено\n"; return 1; }
        OffsetScanner scanner(pid);
        scanner.loadOffsets("offsets.json");
        std::cerr << "[HeadingMon] blindScan()...\n";
        uintptr_t playerBase = scanner.blindScan();
        if (!playerBase) { std::cerr << "[HeadingMon] PlayerBase не знайдено\n"; return 1; }
        std::cerr << "[HeadingMon] PlayerBase=0x" << std::hex << playerBase << std::dec << "\n";
        scanner.headingMonitor(playerBase);
        return 0;
    }

    // ─── --hp-calibrate: знаходить HP/isDead offsets через region scan ──────────
    // Запускати двічі: до і після атаки моба. Float що зменшився = HP offset.
    if (hp_calibrate) {
        pid_t pid = findL2Pid();
        if (!pid) { std::cerr << "[HP-CAL] l2.exe не знайдено\n"; return 1; }

        OffsetScanner scanner(pid);
        scanner.loadOffsets("offsets.json");

        std::cerr << "[HP-CAL] blindScan()...\n";
        uintptr_t playerBase = scanner.blindScan();
        if (!playerBase) { std::cerr << "[HP-CAL] PlayerBase не знайдено\n"; return 1; }

        float px = scanner.rpm_pub<float>(playerBase + 0x24);
        float py = scanner.rpm_pub<float>(playerBase + 0x28);
        float pz = scanner.rpm_pub<float>(playerBase + 0x2C);
        std::cerr << "[HP-CAL] PlayerBase=0x" << std::hex << playerBase << std::dec
                  << " XYZ=(" << (int)px << "," << (int)py << "," << (int)pz << ")\n\n";

        KnownListReader reader(pid, scanner);
        auto mobs = reader.readMobsRegionScan(playerBase, 2000.f);

        // Сортуємо за відстанню
        std::sort(mobs.begin(), mobs.end(), [px, py](const L2Character& a, const L2Character& b){
            return a.distanceTo(px, py) < b.distanceTo(px, py);
        });

        std::cerr << "[HP-CAL] Знайдено " << mobs.size() << " об'єктів. Показую 3 найближчих:\n";

        int shown = 0;
        for (auto& mob : mobs) {
            if (!mob.memPtr || shown >= 3) break;
            float dist = mob.distanceTo(px, py);
            std::cerr << "\n── addr=0x" << std::hex << mob.memPtr << std::dec
                      << "  dist=" << (int)dist
                      << "  XYZ=(" << (int)mob.x << "," << (int)mob.y << "," << (int)mob.z << ")\n";
            // Широкий скан 0x100..0x3C0 — HP може бути поза стандартним 0x1F4
            std::cerr << "  Off  | int32       | float           | Примітка\n";
            std::cerr << "  -----|-------------|-----------------|-------------------\n";
            for (uintptr_t off = 0x100; off <= 0x3C0; off += 4) {
                uint32_t raw = scanner.rpm_pub<uint32_t>(mob.memPtr + off);
                int32_t  as_i = (int32_t)raw;
                float    as_f; std::memcpy(&as_f, &raw, 4);
                std::string note;
                // HP зазвичай: велике додатне ціле або float 10..200000
                if (std::isfinite(as_f) && as_f >= 10.f && as_f <= 500000.f) note = "HP/MP?";
                if (as_i == 0 || as_i == 1)                                   note = "FLAG?";
                if (!note.empty()) {
                    std::cerr << "  0x" << std::hex << std::setw(3) << std::setfill('0') << off
                              << " | " << std::dec << std::setw(11) << std::setfill(' ') << as_i
                              << " | " << std::setw(15) << std::fixed << std::setprecision(2) << as_f
                              << " | " << note << "\n";
                }
            }
            ++shown;
        }

        if (mobs.empty())
            std::cerr << "[HP-CAL] Об'єктів не знайдено — запусти поряд з мобами.\n";

        std::cerr << "\n[HP-CAL] Інструкція:\n"
                  << "  1. Запусти --hp-calibrate (моб на ПОВНОМУ HP) — запиши значення\n"
                  << "  2. Атакуй моба один раз\n"
                  << "  3. Запусти --hp-calibrate знову — порівняй\n"
                  << "  Float що ЗМЕНШИВСЯ = OFF_CHAR_HP\n"
                  << "  Float що НЕ ЗМІНИВСЯ (більший) = OFF_CHAR_HP_MAX\n"
                  << "  int32 що став 1 після смерті = OFF_CHAR_IS_DEAD\n";
        return 0;
    }

    // ─── --find-pos: пошук реального offset поточної позиції в пам'яті ─────────
    if (find_pos) {
        Config cfg; cfg.Load(config_path);
        findPos(cfg.knownlist_offsets_file);
        return 0;
    }

    // ─── --watch-pos: моніторинг XYZ координат під час руху ─────────────────────
    if (watch_pos) {
        Config cfg; cfg.Load(config_path);
        watchPos(cfg.knownlist_offsets_file, override_pb);
        return 0;
    }

    // ─── --map: режим запису NavMesh точок вручну ─────────────────────────────
    // Запуск: ./rdga1bot --map [--config rdga1bot.ini]
    // Веди персонажа вручну навколо перешкод (колони, стіни) — записуються точки прибуття.
    // Ctrl+C → зберігає navmesh_points.pts (додає до існуючого файлу).
    if (map_mode) {
        Config cfg; cfg.Load(config_path);
        const std::string pts_file = cfg.navmesh_cfg.points_file.empty()
                                   ? "navmesh_points.pts"
                                   : cfg.navmesh_cfg.points_file;
        const float collect_dist = cfg.navmesh_cfg.collect_dist > 0.f
                                 ? cfg.navmesh_cfg.collect_dist : 30.f;

        pid_t pid = findL2Pid();
        if (!pid) { std::cerr << "[MAP] l2.exe не знайдено\n"; return 1; }

        OffsetScanner scanner(pid);
        // Структурні offsets з кешу (knownListOff, objX/Y/Z тощо)
        scanner.loadOffsets(cfg.knownlist_offsets_file);
        // PlayerBase: якщо задано --pb → використовуємо його; інакше blindScan
        uintptr_t playerBase = override_pb;
        if (!playerBase) {
            std::cerr << "[MAP] blindScan()...\n";
            playerBase = scanner.blindScan();
            if (!playerBase) { std::cerr << "[MAP] PlayerBase не знайдено\n"
                               << "[MAP] Спробуй: ./rdga1bot --find-pos  →  ./rdga1bot --map --pb 0xADDR\n";
                               return 1; }
        }
        // Показуємо поточну позицію — перевір що вона відповідає місцю в грі!
        float _cx = scanner.rpm_pub<float>(playerBase + OFF_PLAYER_X);
        float _cy = scanner.rpm_pub<float>(playerBase + OFF_PLAYER_Y);
        float _cz = scanner.rpm_pub<float>(playerBase + 0x2C);
        std::cerr << "[MAP] PlayerBase=0x" << std::hex << playerBase << std::dec
                  << "  поточна позиція: X=" << (int)_cx
                  << " Y=" << (int)_cy << " Z=" << (int)_cz << "\n"
                  << "[MAP] Якщо координати невірні → запусти --find-pos, отримай правильний pb\n";

        // Завантажуємо існуючі точки (щоб не дублювати)
        std::vector<std::array<float,3>> pts;
        {
            std::ifstream fin(pts_file, std::ios::binary);
            if (fin) {
                uint32_t cnt = 0;
                fin.read((char*)&cnt, 4);
                pts.resize(cnt);
                for (auto& p : pts) fin.read((char*)p.data(), 12);
                std::cerr << "[MAP] Завантажено " << cnt << " існуючих точок з " << pts_file << "\n";
            }
        }

        float last_x = 0.f, last_y = 0.f;
        bool  have_last = false;

        // Збереження при Ctrl+C
        static std::vector<std::array<float,3>>* g_pts_ptr = nullptr;
        static std::string                        g_pts_file;
        g_pts_ptr  = &pts;
        g_pts_file = pts_file;
        std::signal(SIGINT, [](int) {
            if (!g_pts_ptr) _exit(0);
            {
                // Явний flush+close — _exit() не викликає деструктори
                std::ofstream fout(g_pts_file, std::ios::binary | std::ios::trunc);
                uint32_t cnt = (uint32_t)g_pts_ptr->size();
                fout.write((char*)&cnt, 4);
                for (auto& p : *g_pts_ptr) fout.write((char*)p.data(), 12);
                fout.flush();
                fout.close();
                std::cerr << "\n[MAP] Збережено " << cnt << " точок → " << g_pts_file << "\n";
                std::cerr << "[MAP] Тепер збудуй mesh:\n"
                          << "  ./tools/build_navmesh " << g_pts_file << " navmesh.bin\n";
            }
            _exit(0);
        });

        std::cerr << "[MAP] Веди персонажа вручну навколо перешкод. Ctrl+C щоб зберегти.\n";
        std::cerr << "[MAP] dist=" << collect_dist << " L2u  |  точок: " << pts.size() << "\n\n";

        while (true) {
            float px = scanner.rpm_pub<float>(playerBase + OFF_PLAYER_X);
            float py = scanner.rpm_pub<float>(playerBase + OFF_PLAYER_Y);
            float pz = scanner.rpm_pub<float>(playerBase + OFF_PLAYER_Z);

            // Базова перевірка валідності (L2 world bounds)
            bool valid = (px > -327000.f && px < 327000.f &&
                          py > -327000.f && py < 327000.f &&
                          pz > -16384.f  && pz < 16384.f  &&
                          !(px == 0.f && py == 0.f));

            if (valid) {
                float dx = px - last_x, dy = py - last_y;
                float dist2 = dx*dx + dy*dy;
                if (!have_last || dist2 >= collect_dist * collect_dist) {
                    pts.push_back({px, py, pz});
                    last_x = px; last_y = py; have_last = true;
                    std::cerr << "\r[MAP] #" << std::setw(4) << pts.size()
                              << "  X=" << std::setw(8) << (int)px
                              << " Y=" << std::setw(8) << (int)py
                              << " Z=" << std::setw(6) << (int)pz
                              << "          " << std::flush;
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        return 0;
    }

    // ─── --scan-pos: знайти реальний XYZ offset гравця у PlayerBase ──────────
    // Алгоритм:
    //   1. blindScan() → PlayerBase
    //   2. Region scan → мобиKnownList → центроїд (середня XY мобів)
    //   3. Скануємо pb+0x10..0x160 крок 4 як float
    //   4. Виводимо ті що близько до центроїду мобів (±5000 L2u)
    //   5. Повторюємо 3 рази з паузою 2с щоб побачити динаміку (чи змінюються)
    if (scan_pos) {
        Config cfg; cfg.Load(config_path);
        pid_t pid = findL2Pid();
        if (!pid) { std::cerr << "[SCAN] l2.exe не знайдено\n"; return 1; }

        OffsetScanner scanner(pid);
        scanner.loadOffsets(cfg.knownlist_offsets_file);

        std::cerr << "[SCAN] blindScan()...\n";
        uintptr_t pb = scanner.blindScan();
        if (!pb) { std::cerr << "[SCAN] PlayerBase не знайдено\n"; return 1; }
        std::cerr << "[SCAN] PlayerBase=0x" << std::hex << pb << std::dec << "\n\n";

        // Region scan → centroid мобів
        auto getMobCentroid = [&](float& cx, float& cy, float& cz) -> int {
            const uintptr_t SCAN_BASE = 0x3F0000, SCAN_END = 0x440000;
            const size_t CHUNK = 65536;
            const float WORLD_MIN = -327000.f, WORLD_MAX = 327000.f;
            const float Z_MIN = -16384.f, Z_MAX = 16384.f;
            std::vector<uint8_t> buf(CHUNK);
            cx = cy = cz = 0.f;
            int cnt = 0;
            for (uintptr_t a = SCAN_BASE; a < SCAN_END; a += CHUNK) {
                size_t sz = std::min((size_t)(SCAN_END - a), CHUNK);
                if (!scanner.readBytesPublic(a, buf.data(), sz)) continue;
                for (size_t i = 0; i + 12 <= sz; i += 4) {
                    float x, y, z;
                    memcpy(&x, buf.data()+i,   4);
                    memcpy(&y, buf.data()+i+4, 4);
                    memcpy(&z, buf.data()+i+8, 4);
                    if (x < WORLD_MIN || x > WORLD_MAX) continue;
                    if (y < WORLD_MIN || y > WORLD_MAX) continue;
                    if (z < Z_MIN    || z > Z_MAX)      continue;
                    float dx = x - cx/(cnt?cnt:1);
                    float dy = y - cy/(cnt?cnt:1);
                    if (cnt > 0 && sqrtf(dx*dx+dy*dy) > 4000.f) continue;
                    cx += x; cy += y; cz += z;
                    cnt++;
                    if (cnt >= 20) break;
                }
                if (cnt >= 20) break;
            }
            if (cnt > 0) { cx /= cnt; cy /= cnt; cz /= cnt; }
            return cnt;
        };

        float mob_cx = 0, mob_cy = 0, mob_cz = 0;
        int mob_cnt = getMobCentroid(mob_cx, mob_cy, mob_cz);
        if (mob_cnt < 3) {
            std::cerr << "[SCAN] Мало мобів у region scan (" << mob_cnt
                      << "). Запусти в зоні фарму!\n";
            return 1;
        }
        std::cerr << "[SCAN] Центроїд " << mob_cnt << " мобів: X=" << (int)mob_cx
                  << " Y=" << (int)mob_cy << " Z=" << (int)mob_cz << "\n";
        std::cerr << "[SCAN] Шукаємо offsets у pb+0x10..0x160 близько до центроїду (±5000 L2u)\n\n";

        const float NEAR = 5000.f;
        for (int pass = 0; pass < 3; pass++) {
            std::cerr << "=== Pass " << (pass+1) << "/3 ===\n";
            std::cerr << std::left << std::setw(10) << "Offset"
                      << std::setw(14) << "float"
                      << std::setw(14) << "int32"
                      << "  ← близько до X/Y/Z?\n";
            std::cerr << std::string(52, '-') << "\n";
            for (unsigned off = 0x10; off <= 0x160; off += 4) {
                float  fv = scanner.rpm_pub<float>(pb + off);
                int32_t iv; memcpy(&iv, &fv, 4);
                bool near_x = fabsf(fv - mob_cx) < NEAR;
                bool near_y = fabsf(fv - mob_cy) < NEAR;
                bool near_z = fabsf(fv - mob_cz) < NEAR && fabsf(mob_cz) > 100.f;
                if (near_x || near_y || near_z) {
                    std::cerr << "  +0x" << std::hex << std::setw(4) << std::left << off
                              << "  " << std::dec << std::setw(12) << (int)fv
                              << "  " << std::setw(12) << iv
                              << "  ← " << (near_x?"X ":"") << (near_y?"Y ":"") << (near_z?"Z":"")
                              << "\n";
                }
            }
            std::cerr << "\n";
            if (pass < 2) std::this_thread::sleep_for(std::chrono::seconds(2));
        }

        std::cerr << "[SCAN] Готово. Порівняй з очікуваною позицією X=" << (int)mob_cx
                  << " Y=" << (int)mob_cy << "\n";
        std::cerr << "[SCAN] Знайдений offset → задай OFF_PLAYER_X в offsets_config.h і --map\n";
        return 0;
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
        int  kl_scan_attempts = 0;
        bool kl_cache_tried   = false; // спробували playerBaseCache з offsets.json
        bool mem_calib_done   = false; // авто-калібрування HP/MP/CP вже запускалось
        int  mem_calib_stable = 0;     // лічильник стабільних тіків для калібрування
        int  mem_calib_prev_hp = -1;   // HP з попереднього тіку
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
                if (!mem_calib_done && cfg.mem_use_kl_base && brain.HasPlayerBase()) {
                    const auto& ocr_me = brain.Me();
                    if (ocr_me.has_value() && ocr_me->hp > 0) {
                        const int hp = ocr_me->hp, mp = ocr_me->mp, cp = ocr_me->cp;
                        const bool not_full = (hp < 98 || mp < 98 || cp < 98);
                        if (hp == mem_calib_prev_hp && (not_full || mem_calib_stable > 30)) {
                            ++mem_calib_stable;
                        } else {
                            mem_calib_stable = 0;
                        }
                        mem_calib_prev_hp = hp;

                        if (mem_calib_stable >= 3 || (!not_full && mem_calib_stable >= 60)) {
                            auto result = MemReader::AutoCalibratePlayer(
                                kl_pid, brain.GetPlayerBase(), hp, mp, cp);
                            if (result.found_hp || result.found_mp) {
                                auto off = mem_reader.GetOffsets();
                                if (result.found_hp) {
                                    off.hp_off     = result.hp_off;
                                    off.max_hp_off = result.max_hp_off;
                                }
                                if (result.found_mp) {
                                    off.mp_off     = result.mp_off;
                                    off.max_mp_off = result.max_mp_off;
                                }
                                if (result.found_cp) {
                                    off.cp_off     = result.cp_off;
                                    off.max_cp_off = result.max_cp_off;
                                }
                                mem_reader.SetOffsets(off);
                                mem_reader.SaveCalib(result, "mem_calib.json");
                            }
                            mem_calib_done = true; // незалежно від результату — не повторювати
                        }
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
                    bool cache_valid = (std::isfinite(cx) && std::fabsf(cx) < 327000.f
                                     && std::isfinite(cy) && std::fabsf(cy) < 327000.f
                                     && (std::fabsf(cx) > 100.f || std::fabsf(cy) > 100.f));
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
                    std::thread([scanner_ptr, running_ptr, result_ptr, bs_timeout]() {
                        uintptr_t base = scanner_ptr->blindScan(bs_timeout);
                        result_ptr->store(base);
                        running_ptr->store(false);
                    }).detach();
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
        if (cfg.navmesh_cfg.save_on_exit)
            brain.SaveNavMeshPoints();

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
