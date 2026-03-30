#include "offset_scanner.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <iomanip>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <sys/uio.h>   // process_vm_readv

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
    if (!m_pid || !addr || !buf || !len) return false;
    struct iovec local  = { buf,          len };
    struct iovec remote = { (void*)addr,  len };
    ssize_t n = process_vm_readv(m_pid, &local, 1, &remote, 1, 0);
    return n == (ssize_t)len;
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
uintptr_t OffsetScanner::blindScan() {
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
            // X і Y мають бути значущими (справжній гравець ніколи не X=0 або Y=0)
            if (std::fabsf(px) < 500.f || std::fabsf(py) < 500.f) continue;
            // Z не може бути точно 0 або степінь двійки (сміттєвий float)
            if (std::fabsf(pz) < 10.f) continue;
            // Координати не мають бути рівними між собою
            if (px == py && py == pz) continue;

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
// Виводить playerBase+[0x28..0x80] floats що схожі на кут ([-pi..pi] або [0..360]).
// Запускати двічі: стоячи і після повороту на 90° → offset що змінився = heading.
void OffsetScanner::calibrateHeadingOffset(uintptr_t playerBase) const {
    std::cerr << "[HeadingCal] PlayerBase+0x28..0x80:\n";
    std::cerr << "  offset | float_val  | deg(if_rad) | note\n";
    std::cerr << "  -------|------------|-------------|-----\n";
    for (uintptr_t off = 0x28; off <= 0x80; off += 4) {
        float v = rpm<float>(playerBase + off);
        if (!std::isfinite(v)) continue;
        // Якщо в [-pi, pi] → може бути heading в радіанах
        bool maybe_rad = (v >= -3.15f && v <= 3.15f);
        // Якщо в [0, 360] → може бути heading в градусах
        bool maybe_deg = (v >= 0.f && v <= 360.f);
        if (!maybe_rad && !maybe_deg) continue;
        float deg_from_rad = maybe_rad ? (v * 180.f / 3.14159f) : v;
        std::cerr << "  0x" << std::hex << std::setw(4) << off << std::dec
                  << " | " << std::setw(10) << v
                  << " | " << std::setw(11) << (int)deg_from_rad
                  << " | " << (maybe_rad ? "rad?" : "deg?") << "\n";
    }
    std::cerr << "[HeadingCal] Повернись на 90° і запусти знову → знайди змінений offset.\n";
}
