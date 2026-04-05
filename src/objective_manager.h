#pragma once
#include "objective.h"
#include <vector>
#include <memory>
#include <string>
#include <functional>

struct GameState;

// ── ObjectiveManager ──────────────────────────────────────────────────────────
// Управляє списком Objectives, вибирає і виконує активну кожен тік.
//
// Алгоритм:
//   Running → продовжуємо поточний
//   Done/Failed → шукаємо наступний де canRun()=true (по порядку реєстрації)
//   Switch(name) → переключаємось на Objective з таким іменем
//   Якщо жоден canRun() → поточний = null
class ObjectiveManager {
public:
    using LogFn = std::function<void(const std::string&)>;

    void setLogCallback(LogFn fn) { m_log_fn = std::move(fn); }

    // Зареєструвати Objective (порядок реєстрації = пріоритет)
    void add(std::unique_ptr<Objective> obj);

    // Виконати один тік. Повертає ім'я активного Objective.
    std::string tick(GameState& gs);

    // Деактивувати всі (при смерті / рестарті)
    void reset(GameState& gs);

    const Objective* current()    const { return m_current; }
    std::string      currentName() const {
        return m_current ? m_current->name() : "None";
    }

    // Пошук за іменем (публічно — для proxy методів Brain)
    Objective* findByName(const std::string& name);

private:
    std::vector<std::unique_ptr<Objective>> m_objectives;
    Objective*                              m_current = nullptr;
    LogFn                                   m_log_fn;

    void log(const std::string& msg) {
        if (m_log_fn) m_log_fn("[OBJ] " + msg);
    }

    Objective* findNext  (GameState& gs);
    void       switchTo  (Objective* next, GameState& gs);
};
