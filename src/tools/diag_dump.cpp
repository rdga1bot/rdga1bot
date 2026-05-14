// tools/diag_dump.cpp
// KnownList dump and discovery modes:
//   --dump-objects:   дамп пам'яті об'єктів KnownList
//   --discover-klist: CE-style reverse pointer scan
#include "diag.h"
#include "../Config.h"
#include "../offset_scanner.h"
#include "../knownlist_reader.h"
#include "../offsets_config.h"
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include <iomanip>
#include <cstring>
#include <cstdio>
#ifndef _WIN32
#include <sys/uio.h>
#endif

static void dumpKnownListObjectsImpl(const std::string& offsets_file) {
    // Wine 32-bit user space goes up to 0xBFFFFFFF (not 0x7FFFFFFF!)
    auto isValidPtr32 = [](uint32_t v) { return v > 0x10000 && v < 0xBFFF0000; };
    auto isL2XY  = [](float v) { return std::isfinite(v) && v > -330000.f && v < 330000.f && std::fabs(v) > 200.f; };
    auto isL2Z   = [](float v) { return std::isfinite(v) && v > -17000.f  && v < 17000.f  && std::fabs(v) > 5.f; };

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
                            && std::fabs(fx - px) < nearby_range
                            && std::fabs(fy - py) < nearby_range
                            && std::fabs(fz - pz) < nearby_range
                            && (std::fabs(fx - px) + std::fabs(fy - py)) > 10.f) {
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
                                    && std::fabs(fx - px) < nearby_range
                                    && std::fabs(fy - py) < nearby_range
                                    && std::fabs(fz - pz) < nearby_range
                                    && (std::fabs(fx - px) + std::fabs(fy - py)) > 10.f) {
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
                    && std::fabs(fx - px) < 3000.f && std::fabs(fy - py) < 3000.f
                    && std::fabs(fz - pz) < 3000.f
                    && (std::fabs(fx - px) > 1.f || std::fabs(fy - py) > 1.f)) {
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
                && (std::fabs(fx-px) > 5.f || std::fabs(fy-py) > 5.f || std::fabs(fz-pz) > 5.f)) {
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


void runDumpObjects(const std::string& config_path) {
    Config cfg; cfg.Load(config_path);
    dumpKnownListObjectsImpl(cfg.knownlist_offsets_file);
}

void runDiscoverKlist(const std::string& config_path) {
        pid_t pid = findL2Pid();
        if (!pid) { std::cerr << "[discover-klist] L2 процес не знайдено\n"; return; }
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
                          && std::fabs(cpx) > 200.f && std::fabs(cpy) > 200.f;
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
            return;
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
            return;
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
            return;
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
                if (std::isfinite(vf) && std::fabs(vf) > 100.f && std::fabs(vf) < 327000.f)
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
                        if (std::fabs(fx) < 5000.f || std::fabs(fx) > 330000.f) continue;
                        if (std::fabs(fy) < 1000.f || std::fabs(fy) > 330000.f) continue;
                        float dx = fx - ppx2, dy = fy - ppy2;
                        std::cerr << "  LL[" << std::dec << li << "] 0x" << std::hex << llCur
                                  << " XY@+0x" << xoff << "=(" << std::dec << (int)fx << "," << (int)fy
                                  << ") dist=" << (int)std::sqrt(dx*dx+dy*dy) << "\n";
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
                        bool coord_xy = std::isfinite(fx) && std::fabs(fx) > 5000.f
                                     && std::isfinite(fy) && std::fabs(fy) > 1000.f
                                     && std::fabs(fx) < 330000.f
                                     && std::fabs(fy) < 330000.f;
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
                                if (!std::isfinite(fx) || std::fabs(fx) < 5000.f
                                 || !std::isfinite(fy) || std::fabs(fy) < 1000.f
                                 || std::fabs(fx) > 330000.f || std::fabs(fy) > 330000.f) continue;
                                float dx = fx - ppx, dy = fy - ppy;
                                float dist = std::sqrt(dx*dx + dy*dy);
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
        return;
}

