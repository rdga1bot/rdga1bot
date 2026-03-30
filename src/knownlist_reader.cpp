#include "knownlist_reader.h"
#include "offsets_config.h"
#include <sys/uio.h>   // process_vm_readv
#include <algorithm>
#include <iostream>
#include <cstring>     // memcpy
#include <cmath>       // isfinite
#include <unordered_set>

KnownListReader::KnownListReader(pid_t pid, const OffsetScanner& offsets)
    : m_pid(pid), m_off(offsets) {}

bool KnownListReader::readBytes(uintptr_t addr, void* buf, size_t len) const {
    if (!m_pid || !addr || !buf || !len) return false;
    struct iovec local  = { buf,         len };
    struct iovec remote = { (void*)addr, len };
    ssize_t n = process_vm_readv(m_pid, &local, 1, &remote, 1, 0);
    return n == (ssize_t)len;
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

// ── Region scan: пряме сканування плоского масиву об'єктів ──────────────────
// Для Kamael ElmoreLab: KnownList pointer (pb+0x120) веде в DLL/code space,
// а не до масиву вказівників на об'єкти. Замість цього сканується регіон
// де підтверджено знаходяться об'єкти гри (OFF_REGION_SCAN_BASE..END).
//
// Алгоритм:
// 1. Читаємо регіон чанками по 64KB (process_vm_readv один раз на чанк)
// 2. Скануємо кожен 4-байтний offset на float X: L2 world bounds + dist від гравця
// 3. objBase = xAddr - OFF_OBJ_X (0x90); type at objBase + OFF_OBJ_TYPE (0x5C)
// 4. Дедуплікуємо по objBase
namespace {
    // Зчитати тип об'єкта: спочатку з буфера, якщо не потрапляє — через readv
    int32_t readTypeFromBuf(pid_t pid,
                             const uint8_t* chunk, uintptr_t chunkAddr, size_t chunkSz,
                             uintptr_t typeAddr)
    {
        int32_t v = -1;
        if (typeAddr >= chunkAddr && typeAddr + 4 <= chunkAddr + chunkSz) {
            std::memcpy(&v, chunk + (typeAddr - chunkAddr), 4);
        } else {
            struct iovec loc = { &v, 4 };
            struct iovec rem = { (void*)typeAddr, 4 };
            process_vm_readv(pid, &loc, 1, &rem, 1, 0);
        }
        return v;
    }
} // namespace

std::vector<L2Character> KnownListReader::readMobsRegionScan(
        uintptr_t playerBase, float maxRange) const
{
    float px = rpm<float>(playerBase + OFF_PLAYER_X);
    float py = rpm<float>(playerBase + OFF_PLAYER_Y);
    if (!std::isfinite(px) || !std::isfinite(py)) return {};

    const uintptr_t kBase  = OFF_REGION_SCAN_BASE;
    const uintptr_t kEnd   = OFF_REGION_SCAN_END;
    const size_t    kChunk = 0x10000; // 64KB
    const float     kMaxR2 = maxRange * maxRange;
    const float kXYmin = -327680.f, kXYmax = 327680.f;
    const float kZmin  =  -16384.f, kZmax  =  16384.f;

    std::vector<uint8_t> chunk(kChunk);
    std::vector<L2Character> result;
    std::unordered_set<uintptr_t> seen;
    seen.reserve(64);

    for (uintptr_t addr = kBase; addr < kEnd; addr += kChunk) {
        size_t sz = (size_t)std::min((uintptr_t)kChunk, kEnd - addr);
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
            if (typeRaw != 0) continue; // тільки Mob

            L2Character ch;
            ch.memPtr = objBase;
            ch.type   = L2ObjectType::Mob;
            ch.x = x; ch.y = y; ch.z = z;
            // HP offsets ще не відкалібровані — best-effort
            ch.hp     = rpm<float>  (objBase + m_off.charHpOff);
            ch.hpMax  = rpm<float>  (objBase + m_off.charHpMaxOff);
            ch.isDead = rpm<int32_t>(objBase + m_off.charIsDeadOff) != 0;
            result.push_back(ch);
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

    const uintptr_t kBase  = OFF_REGION_SCAN_BASE;
    const uintptr_t kEnd   = OFF_REGION_SCAN_END;
    const size_t    kChunk = 0x10000;
    const float     kMaxR2 = maxRange * maxRange;
    const float kXYmin = -327680.f, kXYmax = 327680.f;
    const float kZmin  = -16384.f,  kZmax  = 16384.f;

    std::vector<uint8_t> chunk(kChunk);
    std::vector<L2Object> result;
    std::unordered_set<uintptr_t> seen;
    seen.reserve(32);

    for (uintptr_t addr = kBase; addr < kEnd; addr += kChunk) {
        size_t sz = (size_t)std::min((uintptr_t)kChunk, kEnd - addr);
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
