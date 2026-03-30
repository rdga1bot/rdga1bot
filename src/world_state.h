#pragma once
#include <vector>
#include <optional>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
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
    ~WorldState();

    // Запустити фоновий сканер (викликати один раз після отримання playerBase)
    void startBackground(uintptr_t playerBase,
                         float mob_range  = 2500.f,
                         float item_range = 500.f);
    void stopBackground();

    // Оновити playerBase (якщо re-scan знайшов нову адресу)
    void setPlayerBase(uintptr_t pb) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_playerBase = pb;
    }

    // Оновити стан (викликати кожен тік, якщо playerBase відомий).
    // Повний scan тепер у фоновому thread; тут тільки fast re-read таргету.
    void update(uintptr_t playerBase, float mob_range = 2500.f, float item_range = 500.f);

    // Аксесори
    const std::vector<L2Character>& mobs()  const { return m_mobs;  }
    const std::vector<L2Object>&    items() const { return m_items; }
    const std::optional<L2Character>& target() const { return m_target; }

    bool hasValidTarget() const;
    bool targetIsDead()   const;

    // Чи впав лічильник живих мобів з минулого тіку? (kill detection без objectID)
    bool anyMobDiedThisTick() const { return m_mob_died_this_tick; }

    // Чи є предмети для збору в радіусі range L2-юнітів від (px,py)?
    bool hasLootNearby(float px, float py, float range = 300.f) const;

    // Встановити таргет за objectID (викликати з Brain після таргетингу)
    void setTarget(int objectID);
    void clearTarget();

    // Координати гравця (заповнюються якщо playerBase відомий)
    float playerX = 0.f, playerY = 0.f, playerZ = 0.f;

private:
    KnownListReader          m_reader;
    pid_t                    m_pid;
    const OffsetScanner&     m_off;
    std::vector<L2Character> m_mobs;
    std::vector<L2Object>    m_items;
    std::optional<L2Character> m_target;
    int m_targetID = 0;
    int m_prev_alive_count = -1;  // -1 = не ініціалізовано
    bool m_mob_died_this_tick = false;

    // Фоновий сканер
    std::thread             m_bg_thread;
    mutable std::mutex      m_mutex;
    std::atomic<bool>       m_bg_stop{false};
    uintptr_t               m_playerBase   = 0;
    float                   m_mob_range    = 2500.f;
    float                   m_item_range   = 500.f;
    static constexpr int    kScanIntervalMs = 3000;

    void bgLoop(); // тіло фонового потоку
};
