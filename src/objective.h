#pragma once
#include <string>
#include <memory>
#include <chrono>
#include <optional>
#include <vector>
#include <utility>

struct GameState;
#include "geodata_worker.h"  // PathRequest

// ── ObjectiveResult ───────────────────────────────────────────────────────────
// Повертається з Objective::execute() кожен тік.
struct ObjectiveResult {
    enum class Type { Running, Done, Failed, Switch };

    Type        type   = Type::Running;
    std::string next;    // для Switch: ім'я наступного Objective
    std::string reason;  // для логу

    static ObjectiveResult running()                         { return {Type::Running, "", ""}; }
    static ObjectiveResult done(const std::string& r = "")   { return {Type::Done,    "", r}; }
    static ObjectiveResult failed(const std::string& r = "") { return {Type::Failed,  "", r}; }
    static ObjectiveResult switchTo(const std::string& name) { return {Type::Switch, name, ""}; }
};

// ── Objective ─────────────────────────────────────────────────────────────────
// Базовий клас для всіх цілей бота.
// Кожен підклас відповідає за одну логічну задачу (таргетинг, атака тощо).
// Має повний доступ до GameState: gs.hands, gs.eyes, gs.cfg, gs.stats.
class Objective {
public:
    using Clock = std::chrono::steady_clock;
    using TP    = Clock::time_point;

    explicit Objective(std::string name) : m_name(std::move(name)) {}
    virtual ~Objective() = default;

    const std::string& name() const { return m_name; }
    bool isActive()           const { return m_active; }

    // Чи можна запустити прямо зараз?
    virtual bool canRun(const GameState& gs) const = 0;

    // Один тік виконання.
    virtual ObjectiveResult execute(GameState& gs) = 0;

    // Callbacks при активації/деактивації (скидання внутрішнього стану)
    virtual void onEnter(GameState& gs) { (void)gs; }
    virtual void onExit (GameState& gs) { (void)gs; }

    void activate  (GameState& gs) { m_active = true;  onEnter(gs); }
    void deactivate(GameState& gs) { m_active = false;  onExit(gs); }

    // ── Virtual getters — для читання стану через ObjectiveManager ────────────
    // Дефолт: "порожній" стан (не впливає на логіку).
    // Реалізується тільки у відповідному Objective.
    virtual TP   lastKillTime()  const { return TP{}; }
    virtual TP   lastBuff()      const { return TP{}; }
    virtual TP   respawnUntil()  const { return TP{}; }
    virtual bool inGrace()       const { return false; }

    // ── Geo path virtual dispatch ─────────────────────────────────────────────
    // Реалізується в TargetObjective.
    virtual void deliverGeoPath(
        const std::vector<std::pair<float,float>>& /*path*/,
        uint64_t /*id*/) {}

    virtual std::optional<PathRequest> takePendingPathRequest() {
        return std::nullopt;
    }

    // ── Attack state virtual setters ──────────────────────────────────────────
    // Реалізується в TargetObjective. AttackObjective сигналізує через callback.
    virtual void setAttackWasUnreachable(bool /*v*/) {}
    virtual void advanceMacroIdx(int /*total*/) {}

protected:
    std::string m_name;
    bool        m_active = false;
};
