#pragma once
#include <vector>
#include <optional>
#include <sys/types.h>
#include "l2_objects.h"
#include "knownlist_reader.h"
#include "offset_scanner.h"

// ── WorldState ────────────────────────────────────────────────────────────────
// Агрегує KnownList дані для Brain. Оновлюється кожен тік.
// Brain використовує WorldState як ДОДАТОК до OpenCV — не заміну.
class WorldState {
public:
    WorldState(pid_t pid, const OffsetScanner& offsets);

    // Оновити стан (викликати кожен тік, якщо playerBase відомий)
    void update(uintptr_t playerBase);

    // Аксесори
    const std::vector<L2Character>& mobs()  const { return m_mobs;  }
    const std::vector<L2Object>&    items() const { return m_items; }
    const std::optional<L2Character>& target() const { return m_target; }

    bool hasValidTarget() const;
    bool targetIsDead()   const;

    // Чи є предмети для збору в радіусі range L2-юнітів від (px,py)?
    bool hasLootNearby(float px, float py, float range = 300.f) const;

    // Встановити таргет за objectID (викликати з Brain після таргетингу)
    void setTarget(int objectID);
    void clearTarget();

    // Координати гравця (заповнюються якщо playerBase відомий)
    float playerX = 0.f, playerY = 0.f, playerZ = 0.f;

private:
    KnownListReader          m_reader;
    std::vector<L2Character> m_mobs;
    std::vector<L2Object>    m_items;
    std::optional<L2Character> m_target;
    int m_targetID = 0;
};
