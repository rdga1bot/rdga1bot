// SPDX-License-Identifier: GPL-3.0-only
#include "offset_scanner.h"
#include "ProcessMemory.h"
#include <fstream>
#include <iostream>
#include <iomanip>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <thread>
#include <future>
#include <chrono>
#include <vector>
#include <map>
#include <algorithm>
#include <unordered_set>
#include <csignal>
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif
#include <cerrno>

static constexpr size_t MAX_REGION_SIZE = 64 * 1024 * 1024; // 64 MB

OffsetScanner::OffsetScanner(pid_t pid) : m_pid(pid) {}

#ifdef _WIN32
// ── Читання регіонів через VirtualQueryEx (Windows) ──────────────────────────
static constexpr DWORD PAGE_READABLE_MASK =
    PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE;

std::vector<OffsetScanner::MemRegion> OffsetScanner::getReadableRegions() const {
    std::vector<MemRegion> result;
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, m_pid);
    if (!h) return result;

    MEMORY_BASIC_INFORMATION mbi = {};
    uintptr_t addr = 0;
    while (VirtualQueryEx(h, (LPCVOID)addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        if (mbi.State == MEM_COMMIT &&
            (mbi.Protect & PAGE_READABLE_MASK) &&
            !(mbi.Protect & PAGE_GUARD) &&
            mbi.RegionSize > 0 &&
            mbi.RegionSize <= MAX_REGION_SIZE) {
            MemRegionType rtype = (mbi.Type == MEM_PRIVATE)
                ? MemRegionType::Heap : MemRegionType::Misc;
            if (mbi.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE))
                rtype = MemRegionType::Exe;
            result.push_back({(uintptr_t)mbi.BaseAddress, mbi.RegionSize, rtype});
        }
        uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (next <= addr) break; // overflow guard
        addr = next;
    }
    CloseHandle(h);
    return result;
}
#else
// ── Читання регіонів з /proc/<pid>/maps (scanmem-style) ──────────────────────
std::vector<OffsetScanner::MemRegion> OffsetScanner::getReadableRegions() const {
    std::vector<MemRegion> result;
    std::string path = "/proc/" + std::to_string(m_pid) + "/maps";
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return result;

    char line[1024];
    while (std::fgets(line, sizeof(line), f)) {
        uintptr_t start = 0, end = 0;
        char perms[8] = {}, dev[16] = {}, pathname[512] = {};
        unsigned long offset = 0, inode = 0;

        int matched = std::sscanf(line, "%lx-%lx %7s %lx %15s %lu %511[^\n]",
                                  &start, &end, perms, &offset, dev, &inode, pathname);
        if (matched < 4) continue;
        if (perms[0] != 'r') continue;

        // kernel/system регіони
        if (std::strstr(pathname, "[vdso]"))     continue;
        if (std::strstr(pathname, "[vsyscall]")) continue;
        if (std::strstr(pathname, "[stack]"))    continue;
        if (std::strstr(pathname, "[vvar]"))     continue;

        // system SO бібліотеки (не Wine DLL)
        // Wine DLL шляхи: /home/.../.wine/... або /proc/...
        // system SO: /lib/..., /usr/lib/...
        if (std::strstr(pathname, "/lib/")    && std::strstr(pathname, ".so")) continue;
        if (std::strstr(pathname, "/usr/lib") && std::strstr(pathname, ".so")) continue;

        size_t sz = end - start;
        if (sz == 0 || sz > MAX_REGION_SIZE) continue;
        if (start < 0x10000u)     continue;  // нульова сторінка
        if (start >= 0xC0000000u) continue;  // Wine 32-bit: вище 3GB = kernel

        // Класифікація типу регіону — scanmem maps_get_pathname_type() стиль (пункт 3+4)
        MemRegionType rtype = MemRegionType::Misc;
        if (std::strstr(pathname, "[heap]")) {
            rtype = MemRegionType::Heap;
        } else if (pathname[0] == '\0') {
            // Анонімний регіон: з x-правами = Exe (Wine PE text / JIT),
            // без x і inode=0 = Heap (anonymous mmap, Wine heap)
            if (perms[2] == 'x')   rtype = MemRegionType::Exe;
            else if (inode == 0)   rtype = MemRegionType::Heap;
        } else if (perms[2] == 'x') {
            // Named executable: Wine PE секції (l2.exe .text, .data)
            rtype = MemRegionType::Exe;
        }

        result.push_back({start, sz, rtype});
    }
    std::fclose(f);
    return result;
}
#endif // _WIN32

// ── ReadBytes ─────────────────────────────────────────────────────────────────
bool OffsetScanner::readBytes(uintptr_t addr, void* buf, size_t len) const {
    return ProcessMemory::Read(m_pid, addr, buf, len);
}

// ── Перевірка що float є валідною L2-координатою ─────────────────────────────
static bool isL2Coord(float v, float lo, float hi) {
    // Відсіюємо NaN, Inf і значення поза L2 світом
    return std::isfinite(v) && v > lo && v < hi;
}

// ── blindScan: знаходить PlayerBase без координат ────────────────────────────
// Алгоритм: сканує кожні 4 байти кожного readable heap-регіону як кандидата.
// Для кожного кандидата перевіряє СТРУКТУРНУ валідність:
//   1. candidate + 0x120 → uint32 ptr → isValidPtr
//   2. candidate + 0x124 → int32 count → [1, 500]
//   3. knownListPtr[0]   → uint32 ptr → isValidPtr (перший об'єкт)
//   4. firstObj + 0x24/0x28/0x2C → float XYZ → в межах L2 світу
//   5. candidate + 0x24/0x28/0x2C → float XYZ → в межах L2 світу (сам гравець)
//   6. X,Y,Z не всі нулі і не рівні між собою
// ── blindScan: публічна обгортка з опціональним таймаутом ────────────────────
uintptr_t OffsetScanner::blindScan(int timeoutMs) {
    if (timeoutMs <= 0)
        return performBlindScan();

    // З таймаутом: запускаємо в окремому потоці через packaged_task
    m_scan_abort.store(false);
    std::packaged_task<uintptr_t()> task([this]() {
        return performBlindScan();
    });
    std::future<uintptr_t> future = task.get_future();
    std::thread t(std::move(task));
    t.detach(); // не блокуємо при завершенні за timeout

    auto status = future.wait_for(std::chrono::milliseconds(timeoutMs));
    if (status == std::future_status::ready)
        return future.get();

    m_scan_abort.store(true); // сигналізуємо performBlindScan() завершити роботу
    std::cerr << "[OffsetScanner] blindScan timeout after " << timeoutMs << "ms\n";
    return 0;
}

uintptr_t OffsetScanner::performBlindScan() {
    // L2 world bounds (Lineage 2 game world limits)
    // X/Y: Gracia Final world is roughly [-327668, 327668]
    // Z (висота): [-16384, 16384] для більшості зон
    constexpr float WORLD_XY_MIN = -327000.f;
    constexpr float WORLD_XY_MAX =  327000.f;
    constexpr float WORLD_Z_MIN  = -16000.f;
    constexpr float WORLD_Z_MAX  =  16000.f;

    auto regions = getReadableRegions();
    // Пріоритет сканування: Heap та Exe перед Misc (scanmem strategy — пункт 3)
    std::stable_partition(regions.begin(), regions.end(),
                          [](const MemRegion& r){ return r.type != MemRegionType::Misc; });
    size_t total_bytes = 0;
    for (const auto& r : regions) total_bytes += r.size;
    std::cerr << "[OffsetScanner] blindScan: " << regions.size() << " регіонів, "
              << (total_bytes / 1024 / 1024) << " MB\n";

    // Відкрити /proc/pid/mem для pread — надійніший за process_vm_readv (пункти 1+2)
    char _mempath[64];
    std::snprintf(_mempath, sizeof(_mempath), "/proc/%d/mem", (int)m_pid);
    int memfd = ::open(_mempath, O_RDONLY);
    if (memfd < 0) {
        std::cerr << "[OffsetScanner] " << _mempath
                  << " недоступний (" << std::strerror(errno)
                  << ") — fallback до process_vm_readv\n";
    }

    std::vector<uint8_t> buf;
    size_t scan_iteration = 0; // для перевірки m_scan_abort кожні 1000 ітерацій
    for (const auto& region : regions) {
        if (m_scan_abort.load(std::memory_order_relaxed)) break;
        buf.resize(region.size);
        if (memfd >= 0) {
            ssize_t nread = ::pread(memfd, buf.data(), region.size, (off_t)region.base);
            if (nread <= 0) continue;
            buf.resize((size_t)nread); // часткове читання → скорочуємо до реальних байтів
        } else {
            if (!readBytes(region.base, buf.data(), region.size)) continue;
        }

        // Helper: читає uint32 з buf[] якщо addr потрапляє в регіон, інакше rpm fallback.
        auto readU32fromBuf = [&](uintptr_t addr, uint32_t& out) -> bool {
            if (addr >= region.base && addr + 4 <= region.base + buf.size()) {
                std::memcpy(&out, buf.data() + (addr - region.base), 4);
                return true;
            }
            out = rpm<uint32_t>(addr);
            return true;
        };

        // Крок 4 байти — перевіряємо кожен вирівняний offset як candidate playerBase
        for (size_t i = 0; i + 0x130 <= buf.size(); i += 4) {
            // Перевіряємо abort кожні 1000 ітерацій (таймаут від батьківського потоку)
            if ((++scan_iteration % 1000) == 0 &&
                m_scan_abort.load(std::memory_order_relaxed)) {
                if (memfd >= 0) ::close(memfd);
                return 0;
            }
            // ── Перевірка 1: candidate + 0x120 → knownListPtr ──────────────
            uint32_t klPtr = 0;
            std::memcpy(&klPtr, buf.data() + i + 0x120, 4);
            if (!isValidPtr(klPtr)) continue;

            // ── Перевірка 2: knownListPtr[0] → перший об'єкт (valid ptr) ───
            // Примітка: +0x124 в Kamael клієнті — НЕ count, а другий ptr.
            // Тому перевірку count прибрано; замість неї — valid ptr на перший obj.
            uint32_t firstObjPtr = 0;
            readU32fromBuf(klPtr, firstObjPtr);
            if (!isValidPtr(firstObjPtr)) continue;
            // klPtr і firstObjPtr мають бути в різних діапазонах (list ≠ element)
            if (klPtr == firstObjPtr) continue;

            // ── Перевірка 3: сам кандидат (PlayerBase XYZ) ──────────────────
            float px = 0.f, py = 0.f, pz = 0.f;
            // Якщо залишились байти в буфері — беремо звідти без додаткового rpm
            if (i + 0x2C + 4 <= buf.size()) {
                std::memcpy(&px, buf.data() + i + 0x24, 4);
                std::memcpy(&py, buf.data() + i + 0x28, 4);
                std::memcpy(&pz, buf.data() + i + 0x2C, 4);
            } else {
                px = rpm<float>(region.base + i + 0x24);
                py = rpm<float>(region.base + i + 0x28);
                pz = rpm<float>(region.base + i + 0x2C);
            }
            if (!isL2Coord(px, WORLD_XY_MIN, WORLD_XY_MAX)) continue;
            if (!isL2Coord(py, WORLD_XY_MIN, WORLD_XY_MAX)) continue;
            if (!isL2Coord(pz, WORLD_Z_MIN,  WORLD_Z_MAX))  continue;
            // coordinate filter: |X|>200 AND |Y|>200 — НЕ |X|<1000.
            // ElmoreLab Kamael: зона фарму ToI має X<30000, LoA має X~83000.
            // Попередній фільтр |X|<1000 відхиляв валідні ToI координати.
            // Поточний фільтр <200 відхиляє тільки нульову Wine .data секцію. (MR32)
            // Y=0 → гравець ніколи не стоїть на осі симетрії (або стала адреса).
            // X=1098,Y=0 — false positive у Wine .data секції.
            if (std::fabs(px) < 200.f || std::fabs(py) < 200.f) continue;
            // Z не може бути точно 0 або степінь двійки (сміттєвий float)
            if (std::fabs(pz) < 10.f) continue;
            // Координати не мають бути рівними між собою
            if (px == py && py == pz) continue;

            // ── Перевірка 4: в buf[] є ще L2 XYZ крім самого гравця ──────────
            // Kamael pb+0x120 = C++ KnownList object (не масив мобів).
            // rpm(klPtr) повертає vtable ptr, а не L2Object.
            // Тому верифікуємо через наявність 2+ незалежних XYZ у тому самому buf[].
            {
                int xyz_count = 0;
                size_t scan_lo = (i >= 0x4000) ? i - 0x4000 : 0;
                size_t scan_hi = buf.size() >= 12 ? buf.size() - 12 : 0;
                if (i + 0x4000 < scan_hi) scan_hi = i + 0x4000;

                for (size_t j = scan_lo; j <= scan_hi && xyz_count < 2; j += 4) {
                    if (j + 8 >= buf.size()) break;
                    if (j == i) continue;
                    float tx, ty, tz;
                    std::memcpy(&tx, buf.data() + j,     4);
                    std::memcpy(&ty, buf.data() + j + 4, 4);
                    std::memcpy(&tz, buf.data() + j + 8, 4);
                    if (!isL2Coord(tx, WORLD_XY_MIN, WORLD_XY_MAX)) continue;
                    if (!isL2Coord(ty, WORLD_XY_MIN, WORLD_XY_MAX)) continue;
                    if (!isL2Coord(tz, WORLD_Z_MIN,  WORLD_Z_MAX))  continue;
                    if (std::fabs(tx) < 200.f || std::fabs(ty) < 200.f) continue;
                    // не рахуємо сам px/py/pz як окремий об'єкт
                    if (std::fabs(tx - px) < 1.f && std::fabs(ty - py) < 1.f) continue;
                    xyz_count++;
                }
                if (xyz_count < 2) continue; // не L2 heap → пропускаємо
            }

            uintptr_t candidate = region.base + i;
            std::cerr << "[OffsetScanner] blindScan: PlayerBase=0x" << std::hex << candidate
                      << " XYZ=(" << std::dec << (int)px << "," << (int)py << "," << (int)pz << ")\n";
            if (memfd >= 0) ::close(memfd);
            return candidate;
        }
    }

    if (memfd >= 0) ::close(memfd);
    std::cerr << "[OffsetScanner] blindScan: PlayerBase не знайдено\n";
    return 0;
}

// ── Крок 1: знайти PlayerBase ─────────────────────────────────────────────────
uintptr_t OffsetScanner::findPlayerBase(float x, float y, float z, float tolerance) {
    const auto regions = getReadableRegions();
    std::vector<uintptr_t> candidates;

    std::vector<uint8_t> buf;
    for (const auto& region : regions) {
        buf.resize(region.size);
        if (!readBytes(region.base, buf.data(), region.size)) continue;

        // Скануємо кожні 4 байти як float X, +4 → Y, +8 → Z
        for (size_t i = 0; i + 12 <= buf.size(); i += 4) {
            float vx, vy, vz;
            std::memcpy(&vx, buf.data() + i,     4);
            std::memcpy(&vy, buf.data() + i + 4, 4);
            std::memcpy(&vz, buf.data() + i + 8, 4);
            if (std::fabs(vx - x) < tolerance &&
                std::fabs(vy - y) < tolerance &&
                std::fabs(vz - z) < tolerance) {
                // X знаходиться на OFF_PLAYER_X від PlayerBase
                uintptr_t candidate = region.base + i - OFF_PLAYER_X;
                candidates.push_back(candidate);
            }
        }
    }

    std::cerr << "[OffsetScanner] findPlayerBase: " << candidates.size()
              << " кандидатів\n";

    // Верифікація: playerBase + OFF_KNOWN_LIST повинен вказувати на валідний ptr
    for (uintptr_t base : candidates) {
        uintptr_t knownPtr = rpm<uint32_t>(base + OFF_KNOWN_LIST);
        if (isValidPtr(knownPtr)) {
            std::cerr << "[OffsetScanner] PlayerBase=0x" << std::hex << base
                      << " (KnownList ptr=0x" << knownPtr << ")\n" << std::dec;
            return base;
        }
    }

    std::cerr << "[OffsetScanner] PlayerBase не знайдено. "
              << "Переконайтесь що координати точні і персонаж стоїть нерухомо.\n";
    return 0;
}

// ── Крок 2: знайти OFF_KNOWN_LIST ────────────────────────────────────────────
uintptr_t OffsetScanner::findKnownListOffset(uintptr_t playerBase,
                                              float nearbyObjX, float nearbyObjY,
                                              float tolerance) {
    if (!playerBase) return 0xFFFFFFFFu;

    std::cerr << "[OffsetScanner] Сканування KnownList offset (0x100..0x200)...\n";
    uintptr_t best = 0xFFFFFFFFu;

    for (uintptr_t off = 0x100; off <= 0x200; off += 4) {
        uintptr_t ptr = rpm<uint32_t>(playerBase + off);
        if (!isValidPtr(ptr)) continue;

        // Перший елемент масиву — ptr на L2Object
        uintptr_t objPtr = rpm<uint32_t>(ptr);
        if (!isValidPtr(objPtr)) continue;

        float fx = rpm<float>(objPtr + objXOff);
        float fy = rpm<float>(objPtr + objYOff);

        if (std::fabs(fx - nearbyObjX) < tolerance &&
            std::fabs(fy - nearbyObjY) < tolerance) {
            std::cerr << "[OffsetScanner] Знайдено: offset=0x" << std::hex << off
                      << " ptr=0x" << ptr << " obj=0x" << objPtr
                      << " X=" << std::dec << fx << " Y=" << fy << "\n";
            if (best == 0xFFFFFFFFu) {
                best = off;
                knownListOff = off;
            }
        }
    }

    if (best == 0xFFFFFFFFu)
        std::cerr << "[OffsetScanner] KnownList offset не знайдено. "
                  << "Вкажіть точні координати найближчого моба.\n";
    return best;
}

// ── CE-style reverse pointer scan ────────────────────────────────────────────
// Аналог "Find what accesses/points to this address" у Cheat Engine.
// Скануємо всі readable heap-регіони на 4-байтне значення == target (Wine 32-bit).
std::vector<uintptr_t> OffsetScanner::reversePointerScan(uintptr_t target,
                                                          size_t limit) const {
    const uint32_t target32 = static_cast<uint32_t>(target);
    std::vector<uintptr_t> result;

    for (const auto& region : getReadableRegions()) {
        std::vector<uint8_t> buf(region.size);
        if (!readBytes(region.base, buf.data(), region.size)) continue;

        for (size_t i = 0; i + 4 <= buf.size(); i += 4) {
            uint32_t v;
            std::memcpy(&v, buf.data() + i, 4);
            if (v == target32) {
                result.push_back(region.base + i);
                if (limit > 0 && result.size() >= limit) return result;
            }
        }
    }
    return result;
}

// ── Auto-discover KnownList offset (forward scan, MR42) ──────────────────────
// Алгоритм (3-рівнева розвідка + діагностика структури):
//   pb → level1 (KL C++ object) → level2 (embedded array field) → obj ptrs → XY
//   або pb → level1 (flat array) → obj ptrs → XY
//
// Покращення vs MR41i:
//   • sub діапазон 0x00..0x100 (C++ об'єкт може мати array ptr далеко)
//   • 3-й рівень: level1→level2→arrPtr (підтримка глибших C++ containers)
//   • MAX_DIST2 = 7000² (KnownList може тримати об'єкти до 7000 unit)
//   • 11 XY offset кандидатів (0x18..0x90)
//   • Діагностичний deep-dump pb+0x120 перших 32 dword
//   • Якщо nearby≥1 але <3: печатаємо для аналізу (не повертаємо)
uintptr_t OffsetScanner::autoDiscoverKnownList(uintptr_t playerBase,
                                                const std::vector<uintptr_t>& knownObjAddrs) {
    (void)knownObjAddrs; // forward scan — mob addrs не потрібні

    if (!playerBase) return 0;

    constexpr float WORLD_MIN  = -327000.f, WORLD_MAX  = 327000.f;
    constexpr float MAX_DIST2  = 7000.f * 7000.f; // до 7000 unit від гравця
    constexpr int   MIN_NEARBY = 4;   // потрібно 4+ nearby об'єктів
    constexpr int   DENSITY_AT = 80;  // після 80 checked, nearby/checked ≥ 10%

    // XY offset кандидати для L2Object: від дрібних (0x18) до великих (0x90)
    static constexpr uintptr_t XY_OFFSETS[] = {
        0x18, 0x1c, 0x24, 0x28, 0x48, 0x60, 0x64, 0x68, 0x78, 0x84, 0x90
    };

    float px = rpm<float>(playerBase + 0x24);
    float py = rpm<float>(playerBase + 0x28);
    if (!isL2Coord(px, WORLD_MIN, WORLD_MAX) || !isL2Coord(py, WORLD_MIN, WORLD_MAX)) {
        std::cerr << "[autoDiscoverKL] Невалідна позиція гравця (px=" << px << ")\n";
        return 0;
    }
    std::cerr << "[autoDiscoverKL] MR42 forward scan від pb=0x" << std::hex << playerBase
              << " XY=(" << std::dec << (int)px << "," << (int)py << ")\n";

    // testArray: скануємо arrBase як масив L2Object ptrs; повертає кількість nearby.
    // Захисти від false positives:
    //  1. Unique objects: один і той самий obj ptr не рахується двічі
    //  2. Early density abort: якщо після 20 checked < 30% nearby → false positive
    //  3. DENSITY_AT=80: якщо після 80 checked < 10% nearby → false positive
    // verbose=true → друкує перші 10 об'єктів з XY для ручної валідації.
    auto testArray = [&](uint32_t arrBase, bool verbose = false) -> int {
        int nearby = 0, checked = 0, nulls = 0;
        int printed = 0;
        std::unordered_set<uint32_t> seen_objs;
        seen_objs.reserve(64);
        for (int i = 0; i < 2000; i++) {
            uint32_t obj = rpm<uint32_t>((uintptr_t)arrBase + (uintptr_t)i * 4);
            if (!isValidPtr(obj)) {
                if (++nulls >= 8) break;
                continue;
            }
            nulls = 0;
            if (!seen_objs.insert(obj).second) continue; // дублікат → skip (false positive сигнал)
            checked++;
            bool found_xy = false;
            float found_ox = 0.f, found_oy = 0.f;
            uintptr_t found_xoff = 0;
            for (uintptr_t xoff : XY_OFFSETS) {
                float ox = rpm<float>((uintptr_t)obj + xoff);
                float oy = rpm<float>((uintptr_t)obj + xoff + 4);
                if (!isL2Coord(ox, WORLD_MIN, WORLD_MAX)) continue;
                if (!isL2Coord(oy, WORLD_MIN, WORLD_MAX)) continue;
                if (std::fabs(ox) < 200.f || std::fabs(oy) < 200.f) continue;
                found_xy = true; found_ox = ox; found_oy = oy; found_xoff = xoff;
                float dx = ox - px, dy = oy - py;
                float d2 = dx*dx + dy*dy;
                // dist > 200 units: виключаємо player's own fields (dist=0)
                // dist < 7000 units: в межах KnownList
                if (d2 > 200.f*200.f && d2 < MAX_DIST2) { nearby++; break; }
            }
            if (verbose && found_xy && printed < 10) {
                float dx = found_ox - px, dy = found_oy - py;
                float dist = std::sqrt(dx*dx + dy*dy);
                std::cerr << "    [obj " << std::dec << std::setw(2) << i
                          << "] 0x" << std::hex << obj
                          << " XY=(" << std::dec << (int)found_ox << "," << (int)found_oy
                          << ") @+0x" << std::hex << found_xoff
                          << " dist=" << std::dec << (int)dist
                          << (dx*dx+dy*dy < MAX_DIST2 ? " <NEARBY>" : "") << "\n";
                printed++;
            }
            // Early density abort #1: після 30 checked, nearby < 20% → false positive
            if (checked >= 30 && nearby * 5 < checked) break;
            // Early density abort #2: після DENSITY_AT checked, nearby < 10% → false positive
            if (checked >= DENSITY_AT && nearby * 10 < checked) break;
            if (!verbose && nearby >= MIN_NEARBY) break;
        }
        if (verbose)
            std::cerr << "    total checked=" << std::dec << checked
                      << " nearby=" << nearby << "\n";
        return nearby;
    };

    // ── Діагностичний deep-dump pb+0x120 (відомий OFF_KNOWN_LIST) ───────────────
    {
        uint32_t kl_ptr = rpm<uint32_t>(playerBase + 0x120);
        std::cerr << "[autoDiscoverKL] Deep dump pb+0x120 = 0x" << std::hex << kl_ptr << ":\n";
        if (isValidPtr(kl_ptr)) {
            for (int d = 0; d < 32; d++) {
                uint32_t v = rpm<uint32_t>((uintptr_t)kl_ptr + (uintptr_t)d * 4);
                std::cerr << "  0x" << std::hex << kl_ptr + (uintptr_t)d*4
                          << " [+" << std::setw(3) << std::hex << (uint32_t)(d*4) << "] = 0x" << v;
                if (isValidPtr(v)) std::cerr << " (ptr)";
                std::cerr << "\n";
            }
        }
        // ── Intrusive list probe via pb+0x120 sentinel ─────────────────────────────
        // L2 може використовувати intrusive list: sentinel.next → list_node (embedded у L2Object)
        // L2Object_base = node_addr - link_offset; XY = L2Object_base + 0x84 (або 0x90)
        std::cerr << "[autoDiscoverKL] Intrusive list probe via pb+0x120:\n";
        if (isValidPtr(kl_ptr)) {
            uint32_t sentinel = kl_ptr;
            // Спробуємо кілька link_offset значень (де в L2Object embedded list_link)
            for (uint32_t link_off : {0u, 4u, 8u, 0xCu, 0x10u, 0x18u, 0x20u,
                                      0x28u, 0x30u, 0x38u, 0x40u, 0x50u, 0x60u,
                                      0x70u, 0x80u, 0x84u, 0x90u, 0x98u, 0xA0u}) {
                uint32_t node = rpm<uint32_t>((uintptr_t)sentinel);  // sentinel.next
                int count = 0, nearby_lk = 0;
                while (isValidPtr(node) && node != sentinel && count < 200) {
                    if (node >= link_off) {
                        uint32_t obj_base = node - link_off;
                        for (uintptr_t xoff : XY_OFFSETS) {
                            float ox = rpm<float>((uintptr_t)obj_base + xoff);
                            float oy = rpm<float>((uintptr_t)obj_base + xoff + 4);
                            if (!isL2Coord(ox, WORLD_MIN, WORLD_MAX)) continue;
                            if (!isL2Coord(oy, WORLD_MIN, WORLD_MAX)) continue;
                            if (std::fabs(ox) < 200.f || std::fabs(oy) < 200.f) continue;
                            float dx = ox - px, dy = oy - py;
                            if (dx*dx + dy*dy < MAX_DIST2) { nearby_lk++; break; }
                            break;
                        }
                    }
                    node = rpm<uint32_t>((uintptr_t)node);  // node = node.next
                    count++;
                }
                if (nearby_lk > 0)
                    std::cerr << "  link_off=0x" << std::hex << link_off
                              << " count=" << std::dec << count
                              << " nearby=" << nearby_lk << "\n";
                if (nearby_lk >= 3) {
                    std::cerr << "[autoDiscoverKL] >>> INTRUSIVE LIST SUCCESS!\n"
                              << "  sentinel=pb+0x120, link_offset=0x" << std::hex << link_off
                              << " OFF_KNOWN_LIST=0x120\n";
                    knownListOff = 0x120;
                    return 0x120;
                }
            }
            // Також спробуємо prev ptr (sentinel.prev → last node → traverse reverse)
            uint32_t node = rpm<uint32_t>((uintptr_t)sentinel);
            std::cerr << "  sentinel=0x" << std::hex << sentinel
                      << " next=0x" << node;
            node = rpm<uint32_t>((uintptr_t)sentinel + 4);
            std::cerr << " prev=0x" << node << "\n";
        }
    }

    uintptr_t best_off = 0;
    int best_nearby    = 0;
    uint32_t best_arr  = 0;

    // ── Основний scan: pb+[0x80..0x300] ────────────────────────────────────────
    for (uintptr_t off = 0x80; off <= 0x300; off += 4) {
        uint32_t level1 = rpm<uint32_t>(playerBase + off);
        if (!isValidPtr(level1)) continue;

        // Рівень 1: level1 як пряма flat array
        {
            int n = testArray(level1);
            if (n > best_nearby) { best_nearby = n; best_off = off; best_arr = level1; }
            if (n >= MIN_NEARBY) {
                testArray(level1, /*verbose=*/true);
                std::cerr << "[autoDiscoverKL] >>> SUCCESS L1!\n"
                          << "  pb+0x" << std::hex << off << " → arr=0x" << level1
                          << " nearby=" << std::dec << n << "\n"
                          << "[autoDiscoverKL] OFF_KNOWN_LIST = 0x" << std::hex << off << "\n";
                knownListOff = off;
                return off;
            }
        }

        // Рівень 2: level1 = C++ container object, level2 = embedded array ptr
        for (uintptr_t sub = 4; sub <= 0x100; sub += 4) {
            uint32_t level2 = rpm<uint32_t>((uintptr_t)level1 + sub);
            if (!isValidPtr(level2)) continue;
            if (level2 == level1) continue; // самопосилання

            int n = testArray(level2);
            if (n > best_nearby) { best_nearby = n; best_off = off; best_arr = level2; }
            if (n >= MIN_NEARBY) {
                testArray(level2, /*verbose=*/true);
                std::cerr << "[autoDiscoverKL] >>> SUCCESS L2!\n"
                          << "  pb+0x" << std::hex << off << " → level1=0x" << level1
                          << " → sub+0x" << sub << " → arr=0x" << level2
                          << " nearby=" << std::dec << n << "\n"
                          << "[autoDiscoverKL] OFF_KNOWN_LIST = 0x" << std::hex << off << "\n";
                knownListOff = off;
                return off;
            }

            // Рівень 3: level2 = ще один C++ object
            for (uintptr_t sub2 = 4; sub2 <= 0x80; sub2 += 4) {
                uint32_t level3 = rpm<uint32_t>((uintptr_t)level2 + sub2);
                if (!isValidPtr(level3)) continue;
                if (level3 == level2 || level3 == level1) continue;

                int n3 = testArray(level3);
                if (n3 > best_nearby) { best_nearby = n3; best_off = off; best_arr = level3; }
                if (n3 >= MIN_NEARBY) {
                    testArray(level3, /*verbose=*/true);
                    std::cerr << "[autoDiscoverKL] >>> SUCCESS L3!\n"
                              << "  pb+0x" << std::hex << off
                              << " → L1=0x" << level1
                              << " →+0x" << sub << "→ L2=0x" << level2
                              << " →+0x" << sub2 << "→ arr=0x" << level3
                              << " nearby=" << std::dec << n3 << "\n"
                              << "[autoDiscoverKL] OFF_KNOWN_LIST = 0x" << std::hex << off << "\n";
                    knownListOff = off;
                    return off;
                }
            }
        }
    }

    // ── Фаза 2: пряме сканування embedded array (pb+off_direct = arr_start) ────
    // Перевіряємо чи сам pb+off є масивом L2Object ptrs (без розіменування ptr).
    // Застосовно для випадку де KnownList — fixed-size масив всередині struct гравця.
    std::cerr << "[autoDiscoverKL] Direct embedded scan pb+[0x200..0x800]...\n";
    for (uintptr_t off = 0x200; off <= 0x800; off += 4) {
        uint32_t arr_direct = static_cast<uint32_t>(playerBase + off);
        int n = testArray(arr_direct);
        if (n > best_nearby) { best_nearby = n; best_off = off; best_arr = arr_direct; }
        if (n >= MIN_NEARBY) {
            testArray(arr_direct, /*verbose=*/true);
            std::cerr << "[autoDiscoverKL] >>> SUCCESS DIRECT EMBEDDED!\n"
                      << "  pb+0x" << std::hex << off << " (direct) arr=0x" << arr_direct
                      << " nearby=" << std::dec << n << "\n"
                      << "[autoDiscoverKL] OFF_KL_EMBEDDED = 0x" << std::hex << off << "\n";
            // Зберігаємо з префіксом 0x80000000 щоб відрізнити від ptr-mode
            knownListOff = off | 0x80000000u;
            return off;
        }
    }

    // Не знайдено — виводимо найкращого кандидата та повний ptr список
    std::cerr << "[autoDiscoverKL] Не знайдено (MIN_NEARBY=" << std::dec << MIN_NEARBY << ").\n";
    if (best_nearby > 0)
        std::cerr << "  Найкращий кандидат: pb+0x" << std::hex << best_off
                  << " arr=0x" << best_arr << " nearby=" << std::dec << best_nearby << "\n";
    std::cerr << "[autoDiscoverKL] Валідні ptr у pb+[0x80..0x300]:\n";
    for (uintptr_t off = 0x80; off <= 0x300; off += 4) {
        uint32_t v = rpm<uint32_t>(playerBase + off);
        if (isValidPtr(v))
            std::cerr << "  pb+0x" << std::hex << off << " = 0x" << v << "\n";
    }
    return 0;
}

// ── Крок 3: калібрування offsets об'єкту ─────────────────────────────────────
void OffsetScanner::calibrateObjectOffsets(uintptr_t objPtr,
                                            float expectedX, float expectedY, float expectedZ,
                                            float tolerance) {
    if (!objPtr) return;
    std::cerr << "[OffsetScanner] Сканування object offsets (0x00..0x80)...\n";

    for (uintptr_t off = 0; off <= 0x80; off += 4) {
        float vx = rpm<float>(objPtr + off);
        float vy = rpm<float>(objPtr + off + 4);
        float vz = rpm<float>(objPtr + off + 8);
        if (std::fabs(vx - expectedX) < tolerance &&
            std::fabs(vy - expectedY) < tolerance &&
            std::fabs(vz - expectedZ) < tolerance) {
            std::cerr << "[OffsetScanner]  XYZ @ offset 0x" << std::hex << off
                      << " (X=0x" << off << " Y=0x" << off+4 << " Z=0x" << off+8
                      << ")\n" << std::dec;
        }
    }
}

// ── Збереження offsets у JSON ─────────────────────────────────────────────────
bool OffsetScanner::saveOffsets(const std::string& path) const {
    std::ofstream f(path);
    if (!f) return false;
    f << "{\n"
      << "  \"playerBase\":"       << playerBaseCache << ",\n"
      << "  \"OFF_KNOWN_LIST\":"   << knownListOff  << ",\n"
      << "  \"OFF_KNOWN_COUNT\":"  << knownCountOff << ",\n"
      << "  \"OFF_OBJ_TYPE\":"     << objTypeOff    << ",\n"
      << "  \"OFF_OBJ_X\":"        << objXOff       << ",\n"
      << "  \"OFF_OBJ_Y\":"        << objYOff       << ",\n"
      << "  \"OFF_OBJ_Z\":"        << objZOff       << ",\n"
      << "  \"OFF_CHAR_HP\":"      << charHpOff     << ",\n"
      << "  \"OFF_CHAR_HP_MAX\":"  << charHpMaxOff  << ",\n"
      << "  \"OFF_CHAR_IS_DEAD\":" << charIsDeadOff << "\n"
      << "}\n";
    std::cerr << "[OffsetScanner] Offsets збережено: " << path << "\n";
    return true;
}

// ── Завантаження offsets з JSON ───────────────────────────────────────────────
bool OffsetScanner::loadOffsets(const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;

    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());

    // Простий парсер: шукаємо "KEY":VALUE
    auto parseUint = [&](const std::string& key, uintptr_t& out) {
        auto pos = content.find("\"" + key + "\"");
        if (pos == std::string::npos) return;
        pos = content.find(':', pos);
        if (pos == std::string::npos) return;
        unsigned long val = 0;
        std::sscanf(content.c_str() + pos + 1, " %lu", &val);
        out = (uintptr_t)val;
    };

    parseUint("playerBase",       playerBaseCache);
    parseUint("OFF_KNOWN_LIST",   knownListOff);
    parseUint("OFF_KNOWN_COUNT",  knownCountOff);
    parseUint("OFF_OBJ_TYPE",     objTypeOff);
    parseUint("OFF_OBJ_X",        objXOff);
    parseUint("OFF_OBJ_Y",        objYOff);
    parseUint("OFF_OBJ_Z",        objZOff);
    parseUint("OFF_CHAR_HP",      charHpOff);
    parseUint("OFF_CHAR_HP_MAX",  charHpMaxOff);
    parseUint("OFF_CHAR_IS_DEAD", charIsDeadOff);

    std::cerr << "[OffsetScanner] Offsets завантажено з " << path
              << " (KnownList=0x" << std::hex << knownListOff << ")\n" << std::dec;
    return true;
}

// ── Пошук offset назви ────────────────────────────────────────────────────────
uintptr_t OffsetScanner::findNameOffset(uintptr_t playerBase,
                                         const std::string& expectedName) {
    if (!playerBase || expectedName.empty()) return 0;
    uintptr_t klPtr = rpm<uint32_t>(playerBase + knownListOff);
    if (!isValidPtr(klPtr)) return 0;

    float px = rpm<float>(playerBase + OFF_PLAYER_X);
    float py = rpm<float>(playerBase + OFF_PLAYER_Y);

    std::cerr << "[NameScan] Шукаємо \"" << expectedName << "\"...\n";

    int null_streak = 0;
    for (int i = 0; i < 200; ++i) {
        uintptr_t objPtr = rpm<uint32_t>(klPtr + (uintptr_t)i * 4);
        if (!isValidPtr(objPtr)) {
            if (++null_streak >= 8) break;
            continue;
        }
        null_streak = 0;

        float ox = rpm<float>(objPtr + objXOff);
        float oy = rpm<float>(objPtr + objYOff);
        if (!std::isfinite(ox) || !std::isfinite(oy)) continue;
        float d2 = (ox-px)*(ox-px) + (oy-py)*(oy-py);
        if (d2 > 500.f*500.f) continue; // тільки поблизу

        for (uintptr_t off = 0x60; off <= 0x80; off += 4) {
            char buf[64] = {};
            if (!readBytes(objPtr + off, buf, sizeof(buf))) continue;
            buf[63] = '\0';
            std::string found(buf);
            if (found.find(expectedName) != std::string::npos) {
                std::cerr << "[NameScan] Знайдено \"" << found
                          << "\" @ +0x" << std::hex << off << std::dec << "\n";
                return off;
            }
        }
    }
    std::cerr << "[NameScan] \"" << expectedName << "\" не знайдено.\n";
    return 0;
}

// ── Heading калібровка ─────────────────────────────────────────────────────────
// Дампає playerBase+[0x00..0x200] — ВСІ значення без фільтра.
// Запускати ДВІЧІ: до і після повороту на 90° → diff покаже змінений offset.
// L2 heading: float рад (~1.57 зміна) | float deg (~90) | int32 (~16384 зміна)
void OffsetScanner::calibrateHeadingOffset(uintptr_t playerBase) const {
    std::cerr << "[HeadingCal] PlayerBase=0x" << std::hex << playerBase << std::dec << "\n";
    std::cerr << "[HeadingCal] Full dump 0x00..0x200 (порівняти diff до/після повороту):\n";

    for (uintptr_t off = 0x00; off <= 0x200; off += 4) {
        uint32_t raw = rpm<uint32_t>(playerBase + off);
        int32_t  as_i = (int32_t)raw;
        float    as_f = 0.f;
        std::memcpy(&as_f, &raw, 4);
        std::cerr << "H 0x" << std::hex << std::setw(3) << off
                  << " " << std::setw(10) << raw << std::dec
                  << " i=" << std::setw(10) << as_i
                  << " f=" << as_f << "\n";
    }
    std::cerr << "[HeadingCal] Повернись на 90° праворуч і запусти знову, потім: diff h1.txt h2.txt\n"
              << "[HeadingCal] float рад: ~1.57 | float deg: ~90 | int32: ~16384\n";
}

// ── Heading live monitor ───────────────────────────────────────────────────────
static volatile bool g_hmon_stop = false;

struct HMonRegion {
    std::string  label;
    uintptr_t    base;
    uintptr_t    range;
    std::vector<uint32_t> baseline;
};

static void hmon_print_change(const std::string& label, uintptr_t off,
                               uint32_t bv, uint32_t cv) {
    int32_t di = (int32_t)cv - (int32_t)bv;
    float   cf = 0.f, bf = 0.f;
    std::memcpy(&cf, &cv, 4);
    std::memcpy(&bf, &bv, 4);
    float df = cf - bf;

    std::cerr << "  [" << label << "+0x" << std::hex << std::setw(3) << off << std::dec << "]"
              << "  base=" << std::setw(10) << (int32_t)bv
              << "  cur="  << std::setw(10) << (int32_t)cv
              << "  di="   << std::setw(8)  << di
              << "  df="   << df;

    bool rad_like = std::isfinite(df) && std::fabs(df) > 0.3f && std::fabs(df) < 7.f;
    bool deg_like = std::isfinite(df) && std::fabs(df) > 10.f && std::fabs(df) < 360.f;
    bool int_like = std::abs(di) > 500 && std::abs(di) < 65536;
    if (rad_like) std::cerr << " ← RAD heading?";
    if (deg_like) std::cerr << " ← DEG heading?";
    if (int_like) std::cerr << " ← INT heading?";
    std::cerr << "\n";
}

void OffsetScanner::headingMonitor(uintptr_t playerBase) const {
    std::signal(SIGINT, [](int){ g_hmon_stop = true; });

    // Регіон 1: playerBase+0x00..0x600 (розширено з 0x200)
    std::vector<HMonRegion> regions;
    regions.push_back({"PB", playerBase, 0x600, {}});

    // Регіон 2+: слідуємо ptr-подібним значенням з перших 0x60 байт playerBase
    // Типові ptr в Linux user space: 0x0800_0000 .. 0xBFFF_FFFF
    std::cerr << "[HeadingMon] Шукаю ptr в playerBase+0x00..0x60:\n";
    for (uintptr_t off = 0; off <= 0x60; off += 4) {
        uint32_t v = rpm<uint32_t>(playerBase + off);
        if (v < 0x08000000u || v > 0xBFFF0000u) continue;
        if (v == (uint32_t)playerBase) continue;  // self-ref
        // Перевіряємо що ptr читабельний
        uint32_t test = rpm<uint32_t>((uintptr_t)v);
        if (!test && rpm<uint32_t>((uintptr_t)v + 4) == 0) continue;
        char lbl[32];
        std::snprintf(lbl, sizeof(lbl), "ptr@%02x", (int)off);
        std::cerr << "  " << lbl << " → 0x" << std::hex << v << std::dec << "\n";
        regions.push_back({lbl, (uintptr_t)v, 0x200, {}});
    }

    // Знімаємо baseline для всіх регіонів
    for (auto& r : regions) {
        r.baseline.resize(r.range / 4 + 1);
        for (uintptr_t off = 0; off <= r.range; off += 4)
            r.baseline[off / 4] = rpm<uint32_t>(r.base + off);
    }

    std::cerr << "[HeadingMon] Baseline знято (" << regions.size() << " регіонів). "
              << "Крутись у грі — побачиш зміни.\n"
              << "[HeadingMon] Ctrl+C для зупинки. "
              << "0x14/0x1c = timer (ігноруємо).\n\n";

    int tick = 0;
    while (!g_hmon_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        ++tick;

        bool any = false;
        for (auto& r : regions) {
            for (uintptr_t off = 0; off <= r.range; off += 4) {
                // Ігноруємо відомі timer offsets в playerBase
                if (r.label == "PB" && (off == 0x14 || off == 0x1c)) continue;
                // Ігноруємо XYZ playerBase (вони теж змінюються від руху)
                if (r.label == "PB" && off >= 0x24 && off <= 0x2c) continue;

                uint32_t cv = rpm<uint32_t>(r.base + off);
                uint32_t bv = r.baseline[off / 4];
                if (cv == bv) continue;

                if (!any) { std::cerr << "[t=" << tick << "] Зміни:\n"; any = true; }
                hmon_print_change(r.label, off, bv, cv);
            }
        }
        if (any) {
            // Оновлюємо baseline після кожної зміни
            for (auto& r : regions)
                for (uintptr_t off = 0; off <= r.range; off += 4)
                    r.baseline[off / 4] = rpm<uint32_t>(r.base + off);
        }
    }
    std::cerr << "\n[HeadingMon] Зупинено.\n";
}
