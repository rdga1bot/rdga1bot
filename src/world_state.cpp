#include "world_state.h"
#include <algorithm>
#include <iostream>

WorldState::WorldState(pid_t pid, const OffsetScanner& offsets)
    : m_reader(pid, offsets) {}

void WorldState::update(uintptr_t playerBase) {
    if (!playerBase) return;

    // Region scan: пряме сканування пам'яті замість KnownList pointer chain.
    // Для ElmoreLab Kamael: pb+0x120 → DLL/code space, не масив об'єктів.
    // readMobsRegionScan() сканує 0x3F0000-0x440000 по 64KB чанках.
    m_mobs  = m_reader.readMobsRegionScan(playerBase, 1500.f);
    m_items = m_reader.readItemsRegionScan(playerBase, 500.f);

    // Kill detection: порівнюємо кількість живих мобів з попереднім тіком
    int alive = 0;
    for (const auto& mob : m_mobs)
        if (!mob.isDead && mob.hp > 0.f) alive++;
    m_mob_died_this_tick = (m_prev_alive_count > 0 && alive < m_prev_alive_count);

    // DEBUG: раз на 5с виводимо кількість мобів (тимчасово)
    static int dbg_tick = 0;
    if (++dbg_tick % 50 == 0)
        std::cerr << "[KnownList] mobs=" << m_mobs.size()
                  << " alive=" << alive
                  << " prev_alive=" << m_prev_alive_count
                  << " died=" << m_mob_died_this_tick << "\n";

    // Fix 3: якщо після 50 тіків мобів і предметів досі 0 — один раз виводимо діагностику
    static bool diagnosed = false;
    if (!diagnosed && dbg_tick >= 50 && m_mobs.empty() && m_items.empty()) {
        m_reader.diagnoseTypes(playerBase);
        diagnosed = true;
    }

    m_prev_alive_count = alive;

    // Оновити поточний таргет якщо він встановлений
    if (m_targetID != 0) {
        auto it = std::find_if(m_mobs.begin(), m_mobs.end(),
            [&](const L2Character& c) { return c.objectID == m_targetID; });
        if (it != m_mobs.end())
            m_target = *it;
        else
            m_target = std::nullopt;
    }
}

bool WorldState::hasValidTarget() const {
    return m_target.has_value() && !m_target->isDead && m_target->hp > 0.f;
}

bool WorldState::targetIsDead() const {
    if (!m_target.has_value()) return false;
    return m_target->isDead || m_target->hp <= 0.f;
}

bool WorldState::hasLootNearby(float px, float py, float range) const {
    for (const auto& item : m_items)
        if (item.distanceTo(px, py) <= range)
            return true;
    return false;
}

void WorldState::setTarget(int objectID) {
    m_targetID = objectID;
    // Одразу шукаємо в поточному списку
    auto it = std::find_if(m_mobs.begin(), m_mobs.end(),
        [&](const L2Character& c) { return c.objectID == objectID; });
    if (it != m_mobs.end())
        m_target = *it;
    else
        m_target = std::nullopt;
}

void WorldState::clearTarget() {
    m_targetID = 0;
    m_target   = std::nullopt;
}
