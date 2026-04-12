#include "knownlist_reader.h"
#include "offsets_config.h"
#include "ProcessMemory.h"
#include <algorithm>
#include <iostream>
#include <cstring>     // memcpy
#include <cmath>       // isfinite
#include <cstdio>      // snprintf, fopen
#include <ctime>       // time_t
#include <cctype>      // isprint, tolower
#include <cwchar>      // wchar_t
#include <unordered_set>

KnownListReader::KnownListReader(pid_t pid, const OffsetScanner& offsets)
    : m_pid(pid), m_off(offsets) {}

bool KnownListReader::readBytes(uintptr_t addr, void* buf, size_t len) const {
    return ProcessMemory::Read(m_pid, addr, buf, len);
}

// ── Читання всіх об'єктів KnownList ──────────────────────────────────────────
std::vector<L2Object> KnownListReader::readAll(uintptr_t playerBase) const {
    std::vector<L2Object> result;
    if (!playerBase) return result;

    uintptr_t knownListPtr = rpm<uint32_t>(playerBase + m_off.knownListOff);
    if (!isValidPtr(knownListPtr)) return result;

    // Kamael client: +0x124 — NOT a count (it's a pointer). Use sentinel iteration.
    // Sparse arrays may have null gaps → continue; only 8 consecutive nulls = end.
    int null_streak = 0;
    for (int i = 0; i < 2000; ++i) {
        uintptr_t objPtr = rpm<uint32_t>(knownListPtr + (uintptr_t)i * 4);
        if (!isValidPtr(objPtr)) {
            if (++null_streak >= 8) break;
            continue;
        }
        null_streak = 0;

        L2Object obj;
        obj.memPtr   = objPtr;
        obj.objectID = rpm<int32_t>(objPtr + OFF_OBJ_ID);
        int32_t typeRaw = rpm<int32_t>(objPtr + m_off.objTypeOff);
        switch (typeRaw) {
            case 0: obj.type = L2ObjectType::Mob;    break;
            case 1: obj.type = L2ObjectType::Player; break;
            case 2: obj.type = L2ObjectType::Item;   break;
            case 3: obj.type = L2ObjectType::Static; break;
            default: obj.type = L2ObjectType::Unknown; break;
        }
        obj.x = rpm<float>(objPtr + m_off.objXOff);
        obj.y = rpm<float>(objPtr + m_off.objYOff);
        obj.z = rpm<float>(objPtr + m_off.objZOff);

        result.push_back(obj);
    }
    return result;
}

// ── Тільки моби ──────────────────────────────────────────────────────────────
std::vector<L2Character> KnownListReader::readMobs(uintptr_t playerBase) const {
    std::vector<L2Character> result;
    if (!playerBase) return result;

    uintptr_t knownListPtr = rpm<uint32_t>(playerBase + m_off.knownListOff);
    if (!isValidPtr(knownListPtr)) return result;

    int null_streak2 = 0;
    for (int i = 0; i < 2000; ++i) {
        uintptr_t objPtr = rpm<uint32_t>(knownListPtr + (uintptr_t)i * 4);
        if (!isValidPtr(objPtr)) {
            if (++null_streak2 >= 8) break;
            continue;
        }
        null_streak2 = 0;

        int32_t typeRaw = rpm<int32_t>(objPtr + m_off.objTypeOff);
        if (typeRaw != 0) continue; // тільки Mob

        L2Character ch;
        ch.memPtr   = objPtr;
        ch.objectID = rpm<int32_t>(objPtr + OFF_OBJ_ID);
        ch.type     = L2ObjectType::Mob;
        ch.x        = rpm<float>(objPtr + m_off.objXOff);
        ch.y        = rpm<float>(objPtr + m_off.objYOff);
        ch.z        = rpm<float>(objPtr + m_off.objZOff);
        ch.hp       = rpm<float>(objPtr + m_off.charHpOff);
        ch.hpMax    = rpm<float>(objPtr + m_off.charHpMaxOff);
        ch.isDead   = rpm<int32_t>(objPtr + m_off.charIsDeadOff) != 0;

        result.push_back(ch);
    }
    return result;
}

// ── Тільки предмети (лут) ─────────────────────────────────────────────────────
std::vector<L2Object> KnownListReader::readItems(uintptr_t playerBase) const {
    const auto all = readAll(playerBase);
    std::vector<L2Object> items;
    for (const auto& obj : all)
        if (obj.type == L2ObjectType::Item)
            items.push_back(obj);
    return items;
}

// ── Читати всі об'єкти як L2Character без type filter ────────────────────────
// Fallback коли readMobs() повертає 0 (objTypeOff не відкалібрований).
// Критерій валідності: координати в межах L2 world + hpMax в розумних межах.
std::vector<L2Character> KnownListReader::readAllAsChars(uintptr_t playerBase) const {
    std::vector<L2Character> result;
    if (!playerBase) return result;

    uintptr_t knownListPtr = rpm<uint32_t>(playerBase + m_off.knownListOff);
    if (!isValidPtr(knownListPtr)) return result;

    int null_streak = 0;
    for (int i = 0; i < 2000; ++i) {
        uintptr_t objPtr = rpm<uint32_t>(knownListPtr + (uintptr_t)i * 4);
        if (!isValidPtr(objPtr)) {
            if (++null_streak >= 8) break;
            continue;
        }
        null_streak = 0;

        float x = rpm<float>(objPtr + m_off.objXOff);
        float y = rpm<float>(objPtr + m_off.objYOff);
        float z = rpm<float>(objPtr + m_off.objZOff);

        // Фільтр: тільки об'єкти з валідними L2-координатами
        if (!std::isfinite(x) || std::fabsf(x) < 100.f || std::fabsf(x) > 327000.f) continue;
        if (!std::isfinite(y) || std::fabsf(y) < 100.f || std::fabsf(y) > 327000.f) continue;
        if (!std::isfinite(z) || std::fabsf(z) > 16000.f) continue;

        float hp    = rpm<float>(objPtr + m_off.charHpOff);
        float hpMax = rpm<float>(objPtr + m_off.charHpMaxOff);

        // Пропускаємо статичні об'єкти та items (hp=0, hpMax=0 або нереально великий)
        if (hpMax <= 0.f || hpMax > 1000000.f) continue;
        if (hp < 0.f) continue;

        L2Character ch;
        ch.memPtr   = objPtr;
        ch.objectID = rpm<int32_t>(objPtr + OFF_OBJ_ID);
        ch.type     = L2ObjectType::Unknown; // тип невідомий без відкаліброваного typeOff
        ch.x = x; ch.y = y; ch.z = z;
        ch.hp       = hp;
        ch.hpMax    = hpMax;
        ch.isDead   = (hp <= 0.f) || (rpm<int32_t>(objPtr + m_off.charIsDeadOff) != 0);

        result.push_back(ch);
    }
    return result;
}

// ── Діагностика типів об'єктів ────────────────────────────────────────────────
void KnownListReader::diagnoseTypes(uintptr_t playerBase) const {
    uintptr_t klPtr = rpm<uint32_t>(playerBase + m_off.knownListOff);
    if (!isValidPtr(klPtr)) return;

    std::cerr << "[KnownList] Type diagnosis (first 10 objects):\n";
    int null_streak = 0;
    for (int i = 0; i < 10; ++i) {
        uintptr_t objPtr = rpm<uint32_t>(klPtr + (uintptr_t)i * 4);
        if (!isValidPtr(objPtr)) {
            if (++null_streak >= 3) break;
            continue;
        }
        null_streak = 0;
        int32_t typeAt14 = rpm<int32_t>(objPtr + 0x14);
        int32_t typeAt18 = rpm<int32_t>(objPtr + 0x18);
        int32_t typeAt1C = rpm<int32_t>(objPtr + 0x1C);
        int32_t typeAt20 = rpm<int32_t>(objPtr + 0x20);
        float   x        = rpm<float>  (objPtr + m_off.objXOff);
        float   y        = rpm<float>  (objPtr + m_off.objYOff);
        std::cerr << "  obj[" << i << "] ptr=0x" << std::hex << objPtr
                  << " +0x14=" << std::dec << typeAt14
                  << " +0x18=" << typeAt18
                  << " +0x1C=" << typeAt1C
                  << " +0x20=" << typeAt20
                  << " X=" << (int)x << " Y=" << (int)y << "\n";
    }
}

// ── Динамічне оновлення кешу регіонів пам'яті (/proc/<pid>/maps) ─────────────
// Читає всі readable регіони Wine/l2.exe процесу.
// Фільтри: readable (r--), base >= 0x10000, base < 0x70000000 (пропускаємо DLL),
//          розмір [objXOff+12 .. 8MB].
// Кешується 30с — не читаємо /proc щотіку.
void KnownListReader::refreshScanCache() const {
    time_t now = time(nullptr);
    if (!m_scan_cache.empty() && (now - m_scan_cache_time) < 30) return;
    m_scan_cache.clear();
    m_scan_cache_time = now;

    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%d/maps", (int)m_pid);
    FILE* f = std::fopen(path, "r");
    if (!f) {
        // fallback до хардкодованого регіону якщо /proc недоступний
        m_scan_cache.push_back({OFF_REGION_SCAN_BASE,
                                OFF_REGION_SCAN_END - OFF_REGION_SCAN_BASE});
        return;
    }

    char line[512];
    while (std::fgets(line, sizeof(line), f)) {
        uintptr_t start, end;
        char perms[8] = {};
        char name[256] = {};
        std::sscanf(line, "%lx-%lx %4s %*s %*s %*s %255s", &start, &end, perms, name);
        if (perms[0] != 'r' || perms[1] != 'w') continue; // тільки read+write
        if (perms[2] == 'x') continue;            // пропускаємо executable (код)
        if (start < 0x10000u) continue;            // пропускаємо нульову сторінку
        if (start >= 0x70000000u) continue;        // пропускаємо Wine DLL space
        // НЕ пропускаємо файлові відображення! l2.exe data section (0x3F0000-0x500000)
        // це файлове відображення, але саме там живуть game objects.
        // Пропускаємо лише системні SO-бібліотеки.
        if (name[0] == '/' && (std::strstr(name, ".so") || std::strstr(name, "ld-")))
            continue;
        size_t sz = end - start;
        if (sz < (size_t)(m_off.objXOff + 12)) continue; // надто маленький
        if (sz > 32u * 1024 * 1024) continue;     // пропускаємо > 32MB (VRAM/system)
        m_scan_cache.push_back({start, sz});
    }
    std::fclose(f);

    // Додаткова діагностика: виводимо всі знайдені регіони
    size_t total_mb = 0;
    for (const auto& r : m_scan_cache) total_mb += r.size;
    std::cerr << "[KnownList] scan cache: " << m_scan_cache.size()
              << " regions, " << (total_mb / 1024 / 1024) << " MB total\n";
    for (const auto& r : m_scan_cache)
        std::cerr << "  [0x" << std::hex << r.base << " - 0x"
                  << (r.base + r.size) << "] " << std::dec
                  << (r.size / 1024) << " KB\n";
}

// ── Спільний читач типу з буферу або через readv ──────────────────────────────
namespace {
    int32_t readTypeFromBuf(pid_t pid,
                             const uint8_t* chunk, uintptr_t chunkAddr, size_t chunkSz,
                             uintptr_t typeAddr)
    {
        int32_t v = -1;
        if (typeAddr >= chunkAddr && typeAddr + 4 <= chunkAddr + chunkSz) {
            std::memcpy(&v, chunk + (typeAddr - chunkAddr), 4);
        } else {
            ProcessMemory::Read(pid, typeAddr, &v, 4);
        }
        return v;
    }
} // namespace

// ── Region scan: динамічне сканування всіх heap-регіонів процесу ─────────────
// Читає /proc/<pid>/maps (кеш 30с), ітерує readable heap-регіони,
// сканує кожен 4-байтний offset на XYZ float triplet у межах L2 світу,
// фільтрує за відстанню від гравця та типом об'єкту.
std::vector<L2Character> KnownListReader::readMobsRegionScan(
        uintptr_t playerBase, float maxRange) const
{
    float px = rpm<float>(playerBase + OFF_PLAYER_X);
    float py = rpm<float>(playerBase + OFF_PLAYER_Y);
    if (!std::isfinite(px) || !std::isfinite(py)) return {};
    // Захист від застарілого PlayerBase: гравець ніколи не стоїть на (0,0)
    if (std::fabsf(px) < 500.f && std::fabsf(py) < 500.f) return {};

    refreshScanCache();

    const size_t kChunk = 0x10000; // 64KB
    const float  kMaxR2 = maxRange * maxRange;
    const float kXYmin = -327680.f, kXYmax = 327680.f;
    const float kZmin  =  -16384.f, kZmax  =  16384.f;
    // Мінімальна координата об'єкту: нульова пам'ять (0,0,0) — не реальний об'єкт
    const float kObjMinCoord = 100.f;

    std::vector<uint8_t> chunk(kChunk);
    std::vector<L2Character> result;
    std::unordered_set<uintptr_t> seen;
    seen.reserve(64);

    for (const auto& reg : m_scan_cache) {
        const uintptr_t rEnd = reg.base + reg.size;
        for (uintptr_t addr = reg.base; addr < rEnd; addr += kChunk) {
            size_t sz = (size_t)std::min((uintptr_t)kChunk, rEnd - addr);
            if (!readBytes(addr, chunk.data(), sz)) continue;

            for (size_t o = 0; o + 12 <= sz; o += 4) {
                float x, y, z;
                std::memcpy(&x, chunk.data() + o,     4);
                std::memcpy(&y, chunk.data() + o + 4, 4);
                std::memcpy(&z, chunk.data() + o + 8, 4);

                if (!std::isfinite(x) || x < kXYmin || x > kXYmax) continue;
                if (!std::isfinite(y) || y < kXYmin || y > kXYmax) continue;
                if (!std::isfinite(z) || z < kZmin  || z > kZmax)  continue;
                // Відхиляємо нульову/близьку до нуля пам'ять — не реальні L2 об'єкти
                if (std::fabsf(x) < kObjMinCoord && std::fabsf(y) < kObjMinCoord) continue;

                float dx = x - px, dy = y - py;
                if (dx*dx + dy*dy > kMaxR2) continue;

                uintptr_t xAddr = addr + (uintptr_t)o;
                if (xAddr < m_off.objXOff) continue;
                uintptr_t objBase = xAddr - m_off.objXOff;
                if (!seen.insert(objBase).second) continue;

                int32_t typeRaw = readTypeFromBuf(
                    m_pid, chunk.data(), addr, sz, objBase + m_off.objTypeOff);
                if (typeRaw != 0) continue; // тільки Mob

                L2Character ch;
                ch.memPtr = objBase;
                ch.type   = L2ObjectType::Mob;
                ch.x = x; ch.y = y; ch.z = z;
                ch.hp     = rpm<float>  (objBase + m_off.charHpOff);
                ch.hpMax  = rpm<float>  (objBase + m_off.charHpMaxOff);
                ch.isDead = rpm<int32_t>(objBase + m_off.charIsDeadOff) != 0;

                // Читаємо назву якщо OFF_OBJ_NAME відкалібровано
                if (OFF_OBJ_NAME != 0) {
                    ch.name = readName(objBase);
                }
                // Читаємо level і MP (якщо offsets відкалібровані)
                if (OFF_CHAR_LEVEL != 0) {
                    ch.level = rpm<int32_t>(objBase + OFF_CHAR_LEVEL);
                    if (ch.level < 1 || ch.level > 85) ch.level = 0;
                }
                if (OFF_CHAR_MP != 0 && OFF_CHAR_MP_MAX != 0) {
                    ch.mp    = rpm<float>(objBase + OFF_CHAR_MP);
                    ch.mpMax = rpm<float>(objBase + OFF_CHAR_MP_MAX);
                    if (!std::isfinite(ch.mp)    || ch.mp    < 0.f) ch.mp    = 0.f;
                    if (!std::isfinite(ch.mpMax) || ch.mpMax < 0.f) ch.mpMax = 0.f;
                }

                result.push_back(ch);
            }
        }
    }
    return result;
}

std::vector<L2Object> KnownListReader::readItemsRegionScan(
        uintptr_t playerBase, float maxRange) const
{
    float px = rpm<float>(playerBase + OFF_PLAYER_X);
    float py = rpm<float>(playerBase + OFF_PLAYER_Y);
    if (!std::isfinite(px) || !std::isfinite(py)) return {};

    refreshScanCache();

    const size_t kChunk = 0x10000;
    const float  kMaxR2 = maxRange * maxRange;
    const float kXYmin = -327680.f, kXYmax = 327680.f;
    const float kZmin  = -16384.f,  kZmax  = 16384.f;

    std::vector<uint8_t> chunk(kChunk);
    std::vector<L2Object> result;
    std::unordered_set<uintptr_t> seen;
    seen.reserve(32);

    for (const auto& reg : m_scan_cache) {
        const uintptr_t rEnd = reg.base + reg.size;
        for (uintptr_t addr = reg.base; addr < rEnd; addr += kChunk) {
            size_t sz = (size_t)std::min((uintptr_t)kChunk, rEnd - addr);
            if (!readBytes(addr, chunk.data(), sz)) continue;

            for (size_t o = 0; o + 12 <= sz; o += 4) {
                float x, y, z;
                std::memcpy(&x, chunk.data() + o,     4);
                std::memcpy(&y, chunk.data() + o + 4, 4);
                std::memcpy(&z, chunk.data() + o + 8, 4);

                if (!std::isfinite(x) || x < kXYmin || x > kXYmax) continue;
                if (!std::isfinite(y) || y < kXYmin || y > kXYmax) continue;
                if (!std::isfinite(z) || z < kZmin  || z > kZmax)  continue;

                float dx = x - px, dy = y - py;
                if (dx*dx + dy*dy > kMaxR2) continue;

                uintptr_t xAddr = addr + (uintptr_t)o;
                if (xAddr < m_off.objXOff) continue;
                uintptr_t objBase = xAddr - m_off.objXOff;
                if (!seen.insert(objBase).second) continue;

                int32_t typeRaw = readTypeFromBuf(
                    m_pid, chunk.data(), addr, sz, objBase + m_off.objTypeOff);
                if (typeRaw != 2) continue; // тільки Item

                L2Object obj;
                obj.memPtr = objBase;
                obj.type   = L2ObjectType::Item;
                obj.x = x; obj.y = y; obj.z = z;
                result.push_back(obj);
            }
        }
    }
    return result;
}

// ── Знайти найближчого живого моба ────────────────────────────────────────────
std::optional<L2Character> KnownListReader::findNearestMob(
        const std::vector<L2Character>& mobs,
        float playerX, float playerY,
        float maxRange) const {
    std::optional<L2Character> best;
    float bestDist = maxRange;

    for (const auto& mob : mobs) {
        if (mob.isDead || mob.hp <= 0.f) continue;
        float d = mob.distanceTo(playerX, playerY);
        if (d < bestDist) {
            bestDist = d;
            best = mob;
        }
    }
    return best;
}

// ── Читання назви об'єкту з пам'яті ──────────────────────────────────────────
std::string KnownListReader::readName(uintptr_t objPtr) const {
    if (!objPtr) return "";

    // Спроба 1: char[64] UTF-8 @ OFF_OBJ_NAME
    char buf8[64] = {};
    if (readBytes(objPtr + OFF_OBJ_NAME, buf8, sizeof(buf8))) {
        buf8[63] = '\0';
        bool ok = false;
        for (int i = 0; i < 63 && buf8[i]; ++i) {
            if (std::isprint((unsigned char)buf8[i])) { ok = true; break; }
        }
        if (ok) return std::string(buf8);
    }

    // Спроба 2: wchar_t[32] UTF-16LE @ OFF_OBJ_NAME + 4
    wchar_t buf16[32] = {};
    if (readBytes(objPtr + OFF_OBJ_NAME + 4, buf16, sizeof(buf16))) {
        buf16[31] = L'\0';
        if (buf16[0] > 0x1F && buf16[0] < 0x8000) {
            std::string result;
            result.reserve(32);
            for (int i = 0; i < 32 && buf16[i]; ++i) {
                wchar_t wc = buf16[i];
                if (wc < 0x80) {
                    result += (char)wc;
                } else if (wc < 0x800) {
                    result += (char)(0xC0 | (wc >> 6));
                    result += (char)(0x80 | (wc & 0x3F));
                } else {
                    result += (char)(0xE0 | (wc >> 12));
                    result += (char)(0x80 | ((wc >> 6) & 0x3F));
                    result += (char)(0x80 | (wc & 0x3F));
                }
            }
            if (!result.empty()) return result;
        }
    }
    return "";
}

// ── Пошук моба за назвою ──────────────────────────────────────────────────────
std::optional<L2Character> KnownListReader::findMobByName(
        const std::vector<L2Character>& mobs,
        const std::string& name,
        float playerX, float playerY,
        float maxRange) const {
    if (name.empty()) return std::nullopt;

    std::string needle = name;
    std::transform(needle.begin(), needle.end(), needle.begin(),
                   [](unsigned char c){ return (char)std::tolower(c); });

    std::optional<L2Character> best;
    float bestDist = maxRange;

    for (const auto& mob : mobs) {
        if (mob.isDead || mob.hp <= 0.f) continue;
        if (mob.name.empty()) continue;

        std::string haystack = mob.name;
        std::transform(haystack.begin(), haystack.end(), haystack.begin(),
                       [](unsigned char c){ return (char)std::tolower(c); });

        if (haystack.find(needle) == std::string::npos) continue;

        float d = mob.distanceTo(playerX, playerY);
        if (d < bestDist) {
            bestDist = d;
            best = mob;
        }
    }
    return best;
}
