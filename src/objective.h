#pragma once
#include <string>
#include <memory>

struct GameState;

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

protected:
    std::string m_name;
    bool        m_active = false;
};
