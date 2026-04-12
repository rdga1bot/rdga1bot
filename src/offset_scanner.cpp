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
#include <csignal>

static constexpr size_t MAX_REGION_SIZE = 64 * 1024 * 1024; // 64 MB

OffsetScanner::OffsetScanner(pid_t pid) : m_pid(pid) {}

// ── Читання регіонів з /proc/<pid>/maps ──────────────────────────────────────
std::vector<OffsetScanner::MemRegion> OffsetScanner::getReadableRegions() const {
    std::vector<MemRegion> result;
    std::ifstream maps("/proc/" + std::to_string(m_pid) + "/maps");
    if (!maps) return result;

    std::string line;
    while (std::getline(maps, line)) {
        if (line.size() < 20) continue;

        // Формат: addr_start-addr_end perms offset dev inode [path]
        uintptr_t addr_start = 0, addr_end = 0;
        char perms[8] = {};
        if (std::sscanf(line.c_str(), "%lx-%lx %7s", &addr_start, &addr_end, perms) < 3)
            continue;

        // Тільки readable
        if (perms[0] != 'r') continue;

        // Пропускаємо vdso, vsyscall, stack
        if (line.find("[vdso]")     != std::string::npos) continue;
        if (line.find("[vsyscall]") != std::string::npos) continue;
        if (line.find("[stack]")    != std::string::npos) continue;

        size_t sz = addr_end - addr_start;
        if (sz == 0 || sz > MAX_REGION_SIZE) continue;

        result.push_back({addr_start, sz});
    }
    return result;
}

// ── ReadBytes через process_vm_readv ─────────────────────────────────────────
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
    std::packaged_task<uintptr_t()> task([this]() {
        return performBlindScan();
    });
    std::future<uintptr_t> future = task.get_future();
    std::thread t(std::move(task));
    t.detach(); // не блокуємо при завершенні за timeout

    auto status = future.wait_for(std::chrono::milliseconds(timeoutMs));
    if (status == std::future_status::ready)
        return future.get();

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

    const auto regions = getReadableRegions();
    size_t total_bytes = 0;
    for (const auto& r : regions) total_bytes += r.size;
    std::cerr << "[OffsetScanner] blindScan: " << regions.size() << " регіонів, "
              << (total_bytes / 1024 / 1024) << " MB\n";

    std::vector<uint8_t> buf;
    for (const auto& region : regions) {
        buf.resize(region.size);
        if (!readBytes(region.base, buf.data(), region.size)) continue;

        // Крок 4 байти — перевіряємо кожен вирівняний offset як candidate playerBase
        for (size_t i = 0; i + 0x130 <= buf.size(); i += 4) {
            // ── Перевірка 1: candidate + 0x120 → knownListPtr ──────────────
            uint32_t klPtr = 0;
            std::memcpy(&klPtr, buf.data() + i + 0x120, 4);
            if (!isValidPtr(klPtr)) continue;

            // ── Перевірка 2: knownListPtr[0] → перший об'єкт (valid ptr) ───
            // Примітка: +0x124 в Kamael клієнті — НЕ count, а другий ptr.
            // Тому перевірку count прибрано; замість неї — valid ptr на перший obj.
            uint32_t firstObjPtr = rpm<uint32_t>(klPtr);
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
            // Обидві координати X та Y мають бути ненульовими.
            // Y=0 → гравець ніколи не стоїть на осі симетрії (або стала адреса).
            // X=1098,Y=0 — false positive у Wine .data секції.
            if (std::fabsf(px) < 200.f || std::fabsf(py) < 200.f) continue;
            // Z не може бути точно 0 або степінь двійки (сміттєвий float)
            if (std::fabsf(pz) < 10.f) continue;
            // Координати не мають бути рівними між собою
            if (px == py && py == pz) continue;

            // ── Перевірка 4: перший KL об'єкт має бути ПОРЯД з кандидатом ─────
            // Реальний гравець: KL містить локальних мобів (в радіусі MaxRange ~2500 L2u).
            // False positive: KL "об'єкт" → випадкова пам'ять → рандомні XYZ.
            // Перевіряємо offset +0x24 і +0x90 (два можливих XYZ поля в L2 об'єкті).
            {
                bool kl_nearby = false;
                static constexpr float KL_MAX_DIST2 = 4000.f * 4000.f; // 4000 L2u
                for (uint32_t xoff : {0x24u, 0x90u}) {
                    float ox = rpm<float>(firstObjPtr + xoff);
                    float oy = rpm<float>(firstObjPtr + xoff + 4);
                    if (!isL2Coord(ox, WORLD_XY_MIN, WORLD_XY_MAX)) continue;
                    if (!isL2Coord(oy, WORLD_XY_MIN, WORLD_XY_MAX)) continue;
                    float ddx = ox - px, ddy = oy - py;
                    if (ddx*ddx + ddy*ddy < KL_MAX_DIST2) { kl_nearby = true; break; }
                }
                if (!kl_nearby) continue; // KL об'єкти далеко → не гравець
            }

            uintptr_t candidate = region.base + i;
            std::cerr << "[OffsetScanner] blindScan: PlayerBase=0x" << std::hex << candidate
                      << " XYZ=(" << std::dec << (int)px << "," << (int)py << "," << (int)pz << ")\n";
            return candidate;
        }
    }

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
            if (std::fabsf(vx - x) < tolerance &&
                std::fabsf(vy - y) < tolerance &&
                std::fabsf(vz - z) < tolerance) {
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

        if (std::fabsf(fx - nearbyObjX) < tolerance &&
            std::fabsf(fy - nearbyObjY) < tolerance) {
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

// ── Auto-discover KnownList offset ───────────────────────────────────────────
// Алгоритм (CE pointer scanner техніка):
//   1. Для кожного moб addr: reversePointerScan → набір адрес що тримають pointer на моба
//   2. "Контейнер" = адреса X така що X-N*4..X+N*4 тримають pointers на різних мобів
//      (тобто це масив/список покажчиків)
//   3. Перевіряємо playerBase+[0x80..0x300] крок 4:
//      чи ptr @ offset вказує всередину якогось контейнера
//   4. Виводимо дамп для аналізу + повертаємо перший знайдений offset.
uintptr_t OffsetScanner::autoDiscoverKnownList(uintptr_t playerBase,
                                                const std::vector<uintptr_t>& knownObjAddrs) {
    if (knownObjAddrs.empty()) {
        std::cerr << "[discover-klist] Немає mob addresses — стань поряд з мобами\n";
        return 0;
    }

    std::cerr << "[discover-klist] Reverse pointer scan для " << knownObjAddrs.size()
              << " мобів...\n";

    // Крок 1: для кожного моба — знайти всі покажчики на нього
    // ptr_containers[container_addr] = кількість мобів що мають pointer в цьому контейнері
    // container = адреса вирівняна на 0x40 (64 bytes) → групуємо близькі покажчики
    std::map<uintptr_t, int> container_score;

    for (uintptr_t mobAddr : knownObjAddrs) {
        auto ptrs = reversePointerScan(mobAddr, 64);
        std::cerr << "  mob=0x" << std::hex << mobAddr << " → " << std::dec
                  << ptrs.size() << " покажчиків\n";
        for (uintptr_t ptrAddr : ptrs) {
            // Округляємо до блоку 0x40 — отримуємо "контейнер"
            uintptr_t container = ptrAddr & ~(uintptr_t)0x3F;
            container_score[container]++;
        }
    }

    // Крок 2: відбираємо контейнери де score >= 2 (тримають >= 2 різних мобів)
    // (якщо mob'ів < 2 — беремо score >= 1)
    const int min_score = (knownObjAddrs.size() >= 2) ? 2 : 1;
    std::vector<uintptr_t> candidates;
    std::cerr << "[discover-klist] Контейнери (score >= " << min_score << "):\n";
    for (const auto& [addr, score] : container_score) {
        if (score >= min_score) {
            std::cerr << "  container=0x" << std::hex << addr
                      << " score=" << std::dec << score << "\n";
            candidates.push_back(addr);
        }
    }

    if (candidates.empty()) {
        std::cerr << "[discover-klist] Жодного контейнера не знайдено\n";
        return 0;
    }

    // Крок 3: скануємо playerBase + [0x80..0x300] на pointer до container (або всередину)
    std::cerr << "[discover-klist] Пошук offsets від PlayerBase=0x"
              << std::hex << playerBase << ":\n";
    uintptr_t best = 0;

    for (uintptr_t off = 0x80; off <= 0x300; off += 4) {
        uintptr_t val = static_cast<uintptr_t>(rpm<uint32_t>(playerBase + off));
        if (!isValidPtr(val)) continue;
        for (uintptr_t cand : candidates) {
            // val вказує в межах контейнера [cand .. cand+0xFF]
            if (val >= cand && val < cand + 0x100) {
                std::cerr << "  >>> offset=0x" << std::hex << off
                          << " pb[off]=0x" << val
                          << " → container=0x" << cand << std::dec << "\n";
                if (!best) {
                    best = off;
                    knownListOff = off;
                }
            }
        }
    }

    if (!best)
        std::cerr << "[discover-klist] Offset не знайдено. "
                  << "Спробуй стати ближче до більшої кількості мобів.\n";
    else
        std::cerr << "[discover-klist] Знайдено OFF_KNOWN_LIST=0x"
                  << std::hex << best << " → оновіть offsets_config.h\n" << std::dec;

    return best;
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
        if (std::fabsf(vx - expectedX) < tolerance &&
            std::fabsf(vy - expectedY) < tolerance &&
            std::fabsf(vz - expectedZ) < tolerance) {
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

    bool rad_like = std::isfinite(df) && std::fabsf(df) > 0.3f && std::fabsf(df) < 7.f;
    bool deg_like = std::isfinite(df) && std::fabsf(df) > 10.f && std::fabsf(df) < 360.f;
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
