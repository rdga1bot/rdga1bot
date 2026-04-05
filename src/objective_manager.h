#pragma once
#include "objective.h"
#include <vector>
#include <memory>
#include <string>
#include <functional>
#include <optional>
#include <chrono>

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
    using Clock = std::chrono::steady_clock;
    using TP    = Clock::time_point;
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

    // ── Geo path delegation ───────────────────────────────────────────────────
    void deliverGeoPath(const std::vector<std::pair<float,float>>& path,
                        uint64_t id);
    std::optional<PathRequest> takePendingPathRequest();

    // ── Timer getters (delegates to Objectives via virtual dispatch) ──────────
    TP   getLastKillTime()  const;
    TP   getLastBuff()      const;
    TP   getRespawnUntil()  const;
    bool isInGrace()        const;

private:
    std::vector<std::unique_ptr<Objective>> m_objectives;
    Objective*                              m_current = nullptr;
    LogFn                                   m_log_fn;

    // Пошук за іменем (private — внутрішнє використання)
    Objective* findByName(const std::string& name);

    void log(const std::string& msg) {
        if (m_log_fn) m_log_fn("[OBJ] " + msg);
    }

    Objective* findNext  (GameState& gs);
    void       switchTo  (Objective* next, GameState& gs,
                          const std::string& context = "");
};
