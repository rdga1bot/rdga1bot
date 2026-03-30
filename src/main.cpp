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

// ─── KnownList калібровка: дамп пам'яті об'єктів ───────────────────────────
// Запуск: ./rdga1bot --dump-objects
// Виводить hex dump перших N об'єктів KnownList для ручного визначення offsets.
static pid_t findL2Pid() {
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

int main(int argc, char* argv[]) {
    // Завжди рядковий буфер stdout (важливо коли не TTY)
    setlinebuf(stdout);

    // Аргументи командного рядка
    bool quick   = false;
    bool no_tui  = false;
    bool dump_objects = false;
    std::string config_path = "rdga1bot.ini";

    bool calibrate = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--quick")        quick  = true;
        if (a == "--no-tui")       no_tui = true;
        if (a == "--dump-objects") dump_objects = true;
        if (a == "--calibrate")    calibrate = true;
        if (a == "--config" && i + 1 < argc) config_path = argv[++i];
    }

    // ─── --dump-objects: дамп об'єктів KnownList для калібровки offsets ──────
    if (dump_objects) {
        Config cfg; cfg.Load(config_path);
        dumpKnownListObjects(cfg.knownlist_offsets_file);
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
            if (cfg.knownlist_enabled && kl_scanner && brain.HasPlayerBase()) {
                // Fix 2: перевірка валідності PlayerBase кожні 30с
                // (якщо L2 перезапустився або гравець respawn з новою адресою)
                static auto kl_validity_check =
                    std::chrono::steady_clock::now() - std::chrono::seconds(35);
                auto kl_now_v = std::chrono::steady_clock::now();
                if (kl_now_v - kl_validity_check >= std::chrono::seconds(30)) {
                    kl_validity_check = kl_now_v;
                    uintptr_t cur_base = brain.GetPlayerBase();
                    float vx = kl_scanner->rpm_pub<float>(cur_base + 0x24);
                    float vy = kl_scanner->rpm_pub<float>(cur_base + 0x28);
                    bool base_valid = (std::isfinite(vx) && std::fabsf(vx) > 500.f &&
                                       std::isfinite(vy) && std::fabsf(vy) > 500.f);
                    if (!base_valid) {
                        std::cerr << "[KnownList] PlayerBase невалідний → скидаємо для re-scan\n";
                        brain.SetPlayerBase(0);
                    }
                }
            } else if (cfg.knownlist_enabled && kl_scanner && !brain.HasPlayerBase()) {
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
