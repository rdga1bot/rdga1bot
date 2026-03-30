#include "knownlist_reader.h"
#include <sys/uio.h>   // process_vm_readv
#include <algorithm>
#include <iostream>

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
