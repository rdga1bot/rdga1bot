// tools/diag_calibrate.cpp
// Calibration diagnostic modes:
//   --calibrate:       дамп першого об'єкту KnownList + HSV suggest
//   --heading-monitor: live monitor змін у playerBase struct
//   --hp-calibrate:    знаходить HP/isDead offsets через region scan
#include "diag.h"
#include "../Config.h"
#include "../offset_scanner.h"
#include "../knownlist_reader.h"
#include "../offsets_config.h"
#include <opencv2/opencv.hpp>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include <iomanip>
#include <cstring>

// HSVSuggest — аналізує зображення бару і повертає рядок рекомендованих HSV значень
static std::string HSVSuggest(const cv::Mat& bar_bgr, const std::string& name) {
    if (bar_bgr.empty()) return "";
    cv::Mat hsv;
    cv::cvtColor(bar_bgr, hsv, cv::COLOR_BGR2HSV);
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

// ─── --calibrate ──────────────────────────────────────────────────────────────
void runCalibrate(const std::string& config_path, int argc, char* argv[]) {
    (void)config_path;
    pid_t pid = findL2Pid();
    if (!pid) { std::cerr << "L2 process not found\n"; return; }
    OffsetScanner scanner(pid);
    std::cerr << "[CAL] Running blindScan...\n";
    uintptr_t playerBase = scanner.blindScan();
    if (!playerBase) {
        std::cerr << "[CAL] PlayerBase not found. Is L2 running?\n";
        return;
    }

    uintptr_t klPtr = scanner.rpm_pub<uint32_t>(playerBase + 0x120);
    std::cerr << "[CAL] PlayerBase=0x" << std::hex << playerBase
              << " KnownListPtr=0x" << klPtr << std::dec << "\n";
    std::cerr << "[CAL] Player XYZ: "
              << scanner.rpm_pub<float>(playerBase + 0x24) << " / "
              << scanner.rpm_pub<float>(playerBase + 0x28) << " / "
              << scanner.rpm_pub<float>(playerBase + 0x2C) << "\n";

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
            if (std::isfinite(v) && std::fabsf(v) > 500.f && std::fabsf(v) < 400000.f)
                std::cerr << " [0x" << std::hex << off << "=" << std::dec << (int)v << "]";
        }
        std::cerr << "\n";
    }

    std::cerr << "\n[CAL] Player XYZ: "
              << (int)scanner.rpm_pub<float>(playerBase + 0x24) << " / "
              << (int)scanner.rpm_pub<float>(playerBase + 0x28) << " / "
              << (int)scanner.rpm_pub<float>(playerBase + 0x2C) << "\n";
    std::cerr << "[CAL] Знайди offset де значення ≈ XYZ гравця або мобів поряд.\n"
              << "[CAL] Перевір int32 значення поряд з XYZ для typeID (0=mob,1=player,2=item).\n";

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

    std::cerr << "\n[CAL] === Heading scan ===\n";
    scanner.calibrateHeadingOffset(playerBase);
    std::cerr << "[CAL] Usage: --calibrate [--name \"MobName\"]\n";
}

// ─── --heading-monitor ────────────────────────────────────────────────────────
void runHeadingMonitor(const std::string& config_path) {
    (void)config_path;
    pid_t pid = findL2Pid();
    if (!pid) { std::cerr << "[HeadingMon] l2.exe не знайдено\n"; return; }
    OffsetScanner scanner(pid);
    scanner.loadOffsets("offsets.json");
    std::cerr << "[HeadingMon] blindScan()...\n";
    uintptr_t playerBase = scanner.blindScan();
    if (!playerBase) { std::cerr << "[HeadingMon] PlayerBase не знайдено\n"; return; }
    std::cerr << "[HeadingMon] PlayerBase=0x" << std::hex << playerBase << std::dec << "\n";
    scanner.headingMonitor(playerBase);
}

// ─── --hp-calibrate ───────────────────────────────────────────────────────────
void runHpCalibrate(const std::string& config_path) {
    (void)config_path;
    pid_t pid = findL2Pid();
    if (!pid) { std::cerr << "[HP-CAL] l2.exe не знайдено\n"; return; }

    OffsetScanner scanner(pid);
    scanner.loadOffsets("offsets.json");

    std::cerr << "[HP-CAL] blindScan()...\n";
    uintptr_t playerBase = scanner.blindScan();
    if (!playerBase) { std::cerr << "[HP-CAL] PlayerBase не знайдено\n"; return; }

    float px = scanner.rpm_pub<float>(playerBase + 0x24);
    float py = scanner.rpm_pub<float>(playerBase + 0x28);
    float pz = scanner.rpm_pub<float>(playerBase + 0x2C);
    std::cerr << "[HP-CAL] PlayerBase=0x" << std::hex << playerBase << std::dec
              << " XYZ=(" << (int)px << "," << (int)py << "," << (int)pz << ")\n\n";

    KnownListReader reader(pid, scanner);
    auto mobs = reader.readMobsRegionScan(playerBase, 2000.f);

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
        std::cerr << "  Off  | int32       | float           | Примітка\n";
        std::cerr << "  -----|-------------|-----------------|-------------------\n";
        for (uintptr_t off = 0x100; off <= 0x3C0; off += 4) {
            uint32_t raw = scanner.rpm_pub<uint32_t>(mob.memPtr + off);
            int32_t  as_i = (int32_t)raw;
            float    as_f; std::memcpy(&as_f, &raw, 4);
            std::string note;
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
}
