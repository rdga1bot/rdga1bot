#pragma once
#include <vector>
#include <optional>
#include <sys/types.h>
#include "l2_objects.h"
#include "offset_scanner.h"

// ── KnownListReader ───────────────────────────────────────────────────────────
// Читає KnownList (масив об'єктів поблизу) з пам'яті Wine/L2 процесу.
// Патерн ReadBytes скопійовано з MemReader (не рефакторимо MemReader).
class KnownListReader {
public:
    KnownListReader(pid_t pid, const OffsetScanner& offsets);

    // Читати всі об'єкти KnownList.
    // playerBase — знайдений OffsetScanner::findPlayerBase().
    std::vector<L2Object>    readAll   (uintptr_t playerBase) const;
    std::vector<L2Character> readMobs  (uintptr_t playerBase) const;
    std::vector<L2Object>    readItems (uintptr_t playerBase) const;

    // Знайти найближчого живого моба в радіусі maxRange L2-юнітів.
    std::optional<L2Character> findNearestMob(
        const std::vector<L2Character>& mobs,
        float playerX, float playerY,
        float maxRange = 1200.f) const;

private:
    pid_t                m_pid;
    const OffsetScanner& m_off;

    bool readBytes(uintptr_t addr, void* buf, size_t len) const;

    template<typename T>
    T rpm(uintptr_t addr) const {
        T v{};
        readBytes(addr, &v, sizeof(T));
        return v;
    }

    static bool isValidPtr(uintptr_t v) {
        return v > 0x10000 && v < 0x7FFF0000;
    }
};
