#include "world_state.h"
#include <algorithm>

WorldState::WorldState(pid_t pid, const OffsetScanner& offsets)
    : m_reader(pid, offsets) {}

void WorldState::update(uintptr_t playerBase) {
    if (!playerBase) return;

    // Координати гравця заповнює main.cpp через Brain::SetMemPlayerState().
    // Тут оновлюємо тільки списки об'єктів KnownList.
    m_mobs  = m_reader.readMobs(playerBase);
    m_items = m_reader.readItems(playerBase);

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
