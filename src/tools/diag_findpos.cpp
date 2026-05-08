// tools/diag_findpos.cpp
// Позиційні діагностичні режими:
//   --find-pos:  сканує пам'ять на XYZ гравця (без PlayerBase)
//   --watch-pos: live monitor XYZ offsets від PlayerBase
//   --diff-scan: двофазне калібрування HP/MP/CP
//   --scan-pos:  region scan XYZ + пошук оффсетів у PlayerBase
#include "diag.h"
#include "../Config.h"
#include "../offset_scanner.h"
#include "../ProcessMemory.h"
#include "../MemReader.h"
#include "../offsets_config.h"
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <thread>
#include <chrono>
#ifndef _WIN32
#include <sys/uio.h>
#endif

// ─── --find-pos ───────────────────────────────────────────────────────────────
void runFindPos(const std::string& config_path) {
    (void)config_path;  // offsets_file не використовується — findPos сканує пам'ять напряму
    pid_t pid = findL2Pid();
    if (!pid) { std::cerr << "[find-pos] L2 процес не знайдено\n"; return; }
    std::cerr << "[find-pos] PID=" << pid << "\n";

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

    std::cerr << "[find-pos] Аналіз змін (поріг >150 L2u)...\n";
    int shown = 0;
    for (auto& h : hits) {
        float new_x = 0.f, new_y = 0.f, new_z = 0.f;
        if (!ProcessMemory::Read(pid, h.addr,   &new_x, 4)) continue;
        if (!ProcessMemory::Read(pid, h.addr+4, &new_y, 4)) continue;
        if (!ProcessMemory::Read(pid, h.addr+8, &new_z, 4)) continue;
        float dx = new_x - h.x0;
        if (std::fabsf(dx) < 150.f) continue;
        if (!isPopulatedCoord(new_x)) continue;
        if (!isPopulatedCoord(new_y)) continue;
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

// ─── --watch-pos ──────────────────────────────────────────────────────────────
void runWatchPos(const std::string& config_path, uintptr_t override_pb) {
    Config cfg; cfg.Load(config_path);

    pid_t pid = findL2Pid();
    if (!pid) { std::cerr << "L2 процес не знайдено\n"; return; }
    std::cerr << "[watch-pos] L2 pid=" << pid << "\n";

    uintptr_t base = override_pb;
    if (!base) {
        OffsetScanner scanner(pid);
        if (!scanner.loadOffsets(cfg.knownlist_offsets_file)) {
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

    struct PosCandidate { unsigned off; const char* name; float prev; };
    std::vector<PosCandidate> cands = {
        {0x24, "0x24(X)",    0.f}, {0x28, "0x28(Y)",    0.f}, {0x2C, "0x2C(Z)",    0.f},
        {0x5C, "0x5C(type)", 0.f},
        {0x78, "0x78(cX)",   0.f}, {0x7C, "0x7C(cY)",   0.f}, {0x80, "0x80(cZ)",   0.f},
        {0x88, "0x88(kl?)",  0.f}, {0x90, "0x90(objX)", 0.f},
        {0x120,"0x120(KL)",  0.f},
    };
    std::cerr << std::left << std::setw(7) << "t(мс)";
    for (auto& c : cands) std::cerr << std::setw(12) << c.name;
    std::cerr << "\n" << std::string(7 + 12 * (int)cands.size(), '-') << "\n";

    for (auto& c : cands) ProcessMemory::Read(pid, base + c.off, &c.prev, 4);

    auto t0 = std::chrono::steady_clock::now();
    int last_alive_sec = -1;
    while (true) {
        auto now = std::chrono::steady_clock::now();
        int ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(now - t0).count();
        int sec = ms / 1000;

        std::vector<float> vals(cands.size());
        bool any_changed = false;
        for (size_t i = 0; i < cands.size(); i++) {
            ProcessMemory::Read(pid, base + cands[i].off, &vals[i], 4);
            if (std::fabsf(vals[i] - cands[i].prev) > 10.f) any_changed = true;
        }

        bool periodic = (sec != last_alive_sec);
        if (periodic) {
            last_alive_sec = sec;
            std::cerr << "[alive " << sec << "s] ";
            for (size_t i = 0; i < cands.size(); i++)
                std::cerr << cands[i].name << "=" << (int)vals[i] << " ";
            std::cerr << "\n";
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

// ─── --diff-scan ──────────────────────────────────────────────────────────────
void runDiffScan(const std::string& config_path) {
    Config cfg; cfg.Load(config_path);

    pid_t pid = findL2Pid();
    if (!pid) { std::cerr << "[DIFF] L2 процес не знайдено\n"; return; }
    std::cerr << "[DIFF] L2 pid=" << pid << "\n";

    OffsetScanner scanner(pid);

    uintptr_t pb = 0;
    if (!cfg.knownlist_offsets_file.empty() && scanner.loadOffsets(cfg.knownlist_offsets_file)) {
        uintptr_t cached = scanner.playerBaseCache;
        if (cached) {
            float cx = scanner.rpm_pub<float>(cached + OFF_PLAYER_X);
            float cy = scanner.rpm_pub<float>(cached + OFF_PLAYER_Y);
            bool valid = std::isfinite(cx) && std::isfinite(cy)
                      && std::fabsf(cx) > 200.f && std::fabsf(cy) > 200.f;
            if (valid) {
                pb = cached;
                std::cerr << "[DIFF] PlayerBase з кешу: 0x" << std::hex << pb << std::dec
                          << " XY=(" << (int)cx << "," << (int)cy << ")\n";
            }
        }
    }
    if (!pb) {
        std::cerr << "[DIFF] blindScan()...\n";
        pb = scanner.blindScan();
    }
    if (!pb) { std::cerr << "[DIFF] PlayerBase не знайдено. L2 запущено?\n"; return; }

    float px = scanner.rpm_pub<float>(pb + OFF_PLAYER_X);
    float py = scanner.rpm_pub<float>(pb + OFF_PLAYER_Y);
    float pz = scanner.rpm_pub<float>(pb + OFF_PLAYER_Z);
    std::cerr << "[DIFF] PlayerBase=0x" << std::hex << pb << std::dec
              << "  XYZ=(" << (int)px << "," << (int)py << "," << (int)pz << ")\n\n";

    constexpr uintptr_t kScanMax = 0x400;
    constexpr size_t    kN       = kScanMax / 4;
    auto takeSnapshot = [&]() {
        std::vector<uint32_t> buf(kN, 0);
        ProcessMemory::Read(pid, pb, buf.data(), kN * 4);
        return buf;
    };

    auto matchPct = [](uint32_t cur, uint32_t mx, int pct, int tol = 5) -> bool {
        if (mx < 100u || mx > 500000u) return false;
        if (cur == 0u || cur > mx)     return false;
        int ratio = (int)((uint64_t)cur * 100u / mx);
        return std::abs(ratio - pct) <= tol;
    };

    std::cerr << "[DIFF] Введи поточні HP% MP% CP% (дивися на екран гри).\n"
              << "[DIFF] Наприклад: 87 100 100\n"
              << "[DIFF] HP% MP% CP%: ";
    int hp1 = 100, mp1 = 100, cp1 = 100;
    std::cin >> hp1 >> mp1 >> cp1;
    std::cin.ignore(4096, '\n');

    auto snap1 = takeSnapshot();
    std::cerr << "[DIFF] Snapshot 1 готовий  HP=" << hp1 << "% MP=" << mp1 << "% CP=" << cp1 << "%\n\n";
    std::cerr << "[DIFF] Отримай урон у грі (або використай скіл щоб витратити MP/CP).\n"
              << "[DIFF] Після зміни HP/MP/CP — натисни Enter...\n";
    std::string dummy;
    std::getline(std::cin, dummy);

    auto snap2 = takeSnapshot();
    std::cerr << "[DIFF] Введи нові HP% MP% CP% після зміни: ";
    int hp2 = 100, mp2 = 100, cp2 = 100;
    std::cin >> hp2 >> mp2 >> cp2;
    std::cin.ignore(4096, '\n');

    std::cerr << "[DIFF] Snapshot 2 готовий  HP=" << hp2 << "% MP=" << mp2 << "% CP=" << cp2 << "%\n\n";

    constexpr size_t kWindow = 16;
    struct Candidate {
        uintptr_t cur_off, max_off;
        uint32_t  cur1, max1, cur2, max2;
        int       pct1_got, pct2_got;
        char      stat;
        int       score;
    };
    std::vector<Candidate> all_cands;

    for (size_t i = 0; i < kN; ++i) {
        if (snap1[i] == snap2[i]) continue;
        uint32_t cur1 = snap1[i], cur2 = snap2[i];
        size_t jend = std::min(i + kWindow, kN);
        for (size_t j = i + 1; j < jend; ++j) {
            uint32_t mx1 = snap1[j], mx2 = snap2[j];
            if (mx1 < 100u || mx1 > 500000u) continue;
            if (mx2 < 100u || mx2 > 500000u) continue;
            int mx_delta = (int)mx2 - (int)mx1;
            if (std::abs(mx_delta) > (int)(mx1 / 50 + 1)) continue;
            bool m1h = matchPct(cur1, mx1, hp1), m2h = matchPct(cur2, mx2, hp2);
            bool m1m = matchPct(cur1, mx1, mp1), m2m = matchPct(cur2, mx2, mp2);
            bool m1c = matchPct(cur1, mx1, cp1), m2c = matchPct(cur2, mx2, cp2);
            auto addCand = [&](char stat, int p1, int p2) {
                int got1 = (int)((uint64_t)cur1 * 100u / mx1);
                int got2 = (int)((uint64_t)cur2 * 100u / mx2);
                all_cands.push_back({i*4, j*4, cur1, mx1, cur2, mx2, got1, got2, stat,
                                     std::abs(got1-p1) + std::abs(got2-p2)});
            };
            if (m1h && m2h) addCand('H', hp1, hp2);
            if (m1m && m2m) addCand('M', mp1, mp2);
            if (m1c && m2c) addCand('C', cp1, cp2);
        }
    }

    auto bestFor = [&](char stat) -> Candidate* {
        Candidate* best = nullptr;
        for (auto& c : all_cands)
            if (c.stat == stat && (!best || c.score < best->score)) best = &c;
        return best;
    };
    auto printCand = [](const char* tag, const Candidate* c) {
        if (!c) { std::cerr << "[DIFF] " << tag << ": не знайдено\n"; return; }
        std::cerr << "[DIFF] " << tag << " offset candidate:"
                  << "  cur=+0x" << std::hex << c->cur_off
                  << "  max=+0x" << c->max_off << std::dec
                  << "  snap1=(" << c->cur1 << "/" << c->max1 << " → " << c->pct1_got << "%)"
                  << "  snap2=(" << c->cur2 << "/" << c->max2 << " → " << c->pct2_got << "%)\n";
    };

    Candidate* bH = bestFor('H'), *bM = bestFor('M'), *bC = bestFor('C');
    std::cerr << "\n── Результати diff-scan ──────────────────────────────\n";
    printCand("HP", bH); printCand("MP", bM); printCand("CP", bC);
    std::cerr << "─────────────────────────────────────────────────────\n";

    if (!bH && !bM && !bC) {
        std::cerr << "[DIFF] Нічого не знайдено.\n"
                  << "[DIFF] Поради:\n"
                  << "[DIFF]   1. Переконайся що HP/MP/CP реально змінились між снімками\n"
                  << "[DIFF]   2. Зміна має бути помітною (≥5%)\n"
                  << "[DIFF]   3. Введи точні значення % з екрану гри\n";
        return;
    }

    MemReader::AutoCalibResult r;
    if (bH) { r.hp_off = bH->cur_off; r.max_hp_off = bH->max_off; r.found_hp = true; }
    if (bM) { r.mp_off = bM->cur_off; r.max_mp_off = bM->max_off; r.found_mp = true; }
    if (bC) { r.cp_off = bC->cur_off; r.max_cp_off = bC->max_off; r.found_cp = true; }

    MemReader mr;
    mr.SaveCalib(r);
    std::cerr << "[DIFF] Offsets збережено в mem_calib.json\n"
              << "[DIFF] Перевір: ./rdga1bot --watch-pos і порівняй з HP на екрані.\n";
}

// ─── --scan-pos ───────────────────────────────────────────────────────────────
void runScanPos(const std::string& config_path) {
    Config cfg; cfg.Load(config_path);
    pid_t pid = findL2Pid();
    if (!pid) { std::cerr << "[SCAN] l2.exe не знайдено\n"; return; }

    OffsetScanner scanner(pid);
    scanner.loadOffsets(cfg.knownlist_offsets_file);

    std::cerr << "[SCAN] blindScan()...\n";
    uintptr_t pb = scanner.blindScan();
    if (!pb) { std::cerr << "[SCAN] PlayerBase не знайдено\n"; return; }
    std::cerr << "[SCAN] PlayerBase=0x" << std::hex << pb << std::dec << "\n\n";

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
        return;
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
}

// ─── --dump-gobj ──────────────────────────────────────────────────────────────
// Знаходить game_obj через playerBase+0x58 і виводить всі uint32 > 1000
// в діапазоні +0x0000..+0x4000. Допомагає знайти реальний HP offset вручну.
// Використання: запусти під час бою, подивись які значення змінюються.
void runDumpGobj(const std::string& config_path) {
    Config cfg; cfg.Load(config_path);
    pid_t pid = findL2Pid();
    if (!pid) { std::cerr << "[dump-gobj] L2 процес не знайдено\n"; return; }

    OffsetScanner scanner(pid);
    if (!scanner.loadOffsets(cfg.knownlist_offsets_file)) {
        std::cerr << "[dump-gobj] offsets.json не знайдено — запусти --find-pos спочатку\n";
        return;
    }
    uintptr_t pb = scanner.playerBaseCache;
    if (!pb) { std::cerr << "[dump-gobj] playerBase не знайдено в offsets.json\n"; return; }
    std::cerr << "[dump-gobj] PlayerBase=0x" << std::hex << pb << std::dec << "\n";

    uint32_t gobj_raw = 0;
    ProcessMemory::Read(pid, pb + 0x58, &gobj_raw, 4);
    uintptr_t gobj = (uintptr_t)gobj_raw;
    if (gobj < 0x10000u || gobj > 0x7FFFFFFFu) {
        std::cerr << "[dump-gobj] game_obj pointer невалідний: 0x" << std::hex << gobj << "\n";
        return;
    }
    std::cerr << "[dump-gobj] game_obj=0x" << std::hex << gobj << std::dec << "\n";
    std::cerr << "[dump-gobj] Сканую +0x0000..+0x4000, виводжу значення > 1000:\n\n";

    constexpr size_t kSlots = 0x4000 / 4;
    std::vector<uint32_t> buf(kSlots, 0);
    ProcessMemory::Read(pid, gobj, buf.data(), kSlots * 4);

    // Два snapshot з паузою — показуємо які значення ЗМІНИЛИСЬ (потенційний cur HP)
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    std::vector<uint32_t> buf2(kSlots, 0);
    ProcessMemory::Read(pid, gobj, buf2.data(), kSlots * 4);

    std::cerr << std::left
              << std::setw(8)  << "Offset"
              << std::setw(10) << "Val1"
              << std::setw(10) << "Val2"
              << "Delta\n"
              << std::string(35, '-') << "\n";

    for (size_t i = 0; i < kSlots; i++) {
        uint32_t v1 = buf[i], v2 = buf2[i];
        if (v1 < 1000u && v2 < 1000u) continue;
        if (v1 > 500000u && v2 > 500000u) continue;
        int delta = (int)v2 - (int)v1;
        std::cerr << "+0x" << std::hex << std::setw(5) << std::setfill('0') << i*4 << std::dec
                  << std::setfill(' ')
                  << std::setw(10) << v1
                  << std::setw(10) << v2
                  << (delta ? "  <-- ЗМІНА" : "") << "\n";
    }
    std::cerr << "\n[dump-gobj] Готово. Шукай max_hp (наприклад 15202) та cur_hp поруч.\n";
}
