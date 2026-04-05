#include "objective_manager.h"
#include "game_state.h"

void ObjectiveManager::add(std::unique_ptr<Objective> obj) {
    m_objectives.push_back(std::move(obj));
}

void ObjectiveManager::reset(GameState& gs) {
    if (m_current) {
        log("Reset: деактивовано " + m_current->name());
        m_current->deactivate(gs);
        m_current = nullptr;
    }
}

Objective* ObjectiveManager::findByName(const std::string& name) {
    for (auto& obj : m_objectives)
        if (obj->name() == name) return obj.get();
    return nullptr;
}

Objective* ObjectiveManager::findNext(GameState& gs) {
    for (auto& obj : m_objectives) {
        if (obj.get() == m_current) continue;
        if (obj->canRun(gs)) return obj.get();
    }
    return nullptr;
}

void ObjectiveManager::switchTo(Objective* next, GameState& gs) {
    if (m_current) {
        log("Exit: " + m_current->name());
        m_current->deactivate(gs);
    }
    m_current = next;
    if (m_current) {
        log("Enter: " + m_current->name());
        m_current->activate(gs);
    }
}

std::string ObjectiveManager::tick(GameState& gs) {
    // Знаходимо активний якщо немає
    if (!m_current) {
        for (auto& obj : m_objectives) {
            if (obj->canRun(gs)) { switchTo(obj.get(), gs); break; }
        }
        if (!m_current) return "None";
    }

    // Preemption: вищий пріоритет може перервати нижчий (до поточного в списку)
    for (auto& obj : m_objectives) {
        if (obj.get() == m_current) break;  // зупиняємось на поточному рівні
        if (obj->canRun(gs)) {
            log("Preempt: " + m_current->name() + " → " + obj->name());
            switchTo(obj.get(), gs);
            break;
        }
    }

    ObjectiveResult result = m_current->execute(gs);

    switch (result.type) {
        case ObjectiveResult::Type::Running:
            break;

        case ObjectiveResult::Type::Done:
            log(m_current->name() + " Done" +
                (result.reason.empty() ? "" : ": " + result.reason));
            switchTo(findNext(gs), gs);
            break;

        case ObjectiveResult::Type::Failed:
            log(m_current->name() + " Failed" +
                (result.reason.empty() ? "" : ": " + result.reason));
            switchTo(findNext(gs), gs);
            break;

        case ObjectiveResult::Type::Switch: {
            Objective* target = findByName(result.next);
            if (target && target->canRun(gs)) {
                log(m_current->name() + " Switch→" + result.next);
                switchTo(target, gs);
            } else {
                log(m_current->name() + " Switch→" + result.next +
                    " FAIL, шукаємо наступний");
                switchTo(findNext(gs), gs);
            }
            break;
        }
    }

    return currentName();
}

// ── Geo path delegation ───────────────────────────────────────────────────────

void ObjectiveManager::deliverGeoPath(
        const std::vector<std::pair<float,float>>& path, uint64_t id) {
    for (auto& obj : m_objectives)
        obj->deliverGeoPath(path, id);
}

std::optional<PathRequest> ObjectiveManager::takePendingPathRequest() {
    for (auto& obj : m_objectives) {
        auto req = obj->takePendingPathRequest();
        if (req.has_value()) return req;
    }
    return std::nullopt;
}

// ── Timer getters ─────────────────────────────────────────────────────────────

ObjectiveManager::TP ObjectiveManager::getLastKillTime() const {
    for (const auto& obj : m_objectives) {
        auto t = obj->lastKillTime();
        if (t != TP{}) return t;
    }
    return Clock::now() - std::chrono::hours(1);
}

ObjectiveManager::TP ObjectiveManager::getLastBuff() const {
    for (const auto& obj : m_objectives) {
        auto t = obj->lastBuff();
        if (t != TP{}) return t;
    }
    return Clock::now() - std::chrono::hours(1);
}

ObjectiveManager::TP ObjectiveManager::getRespawnUntil() const {
    for (const auto& obj : m_objectives) {
        auto t = obj->respawnUntil();
        if (t != TP{}) return t;
    }
    return TP{};
}

bool ObjectiveManager::isInGrace() const {
    for (const auto& obj : m_objectives)
        if (obj->inGrace()) return true;
    return false;
}

// ── Attack state notification ─────────────────────────────────────────────────

void ObjectiveManager::notifyMobUnreachable(int macro_count) {
    for (auto& obj : m_objectives) {
        obj->setAttackWasUnreachable(true);
        obj->advanceMacroIdx(macro_count);
    }
}
