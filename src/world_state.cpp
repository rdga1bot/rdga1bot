#include "world_state.h"
#include "offsets_config.h"
#include <algorithm>
#include <iostream>
#include <sys/uio.h>

WorldState::WorldState(pid_t pid, const OffsetScanner& offsets)
    : m_reader(pid, offsets), m_pid(pid), m_off(offsets) {}

WorldState::~WorldState() {
    stopBackground();
}

// Швидке пряме читання з пам'яті (без KnownListReader)
static bool fastRead(pid_t pid, uintptr_t addr, void* buf, size_t len) {
    if (!pid || !addr || !buf) return false;
    struct iovec loc = { buf, len };
    struct iovec rem = { (void*)addr, len };
    return process_vm_readv(pid, &loc, 1, &rem, 1, 0) == (ssize_t)len;
}

// ── Фоновий потік: сканує кожні kScanIntervalMs мс ───────────────────────────
void WorldState::bgLoop() {
    while (!m_bg_stop.load(std::memory_order_relaxed)) {
        uintptr_t pb;
        float mob_r, item_r;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            pb     = m_playerBase;
            mob_r  = m_mob_range;
            item_r = m_item_range;
        }

        if (pb) {
            auto mobs  = m_reader.readMobsRegionScan(pb, mob_r);
            // Fallback: якщо region scan порожній → спробувати без type filter
            // (objTypeOff може бути не відкаліброваним для цього клієнту)
            if (mobs.empty()) {
                mobs = m_reader.readAllAsChars(pb);
            }
            auto items = m_reader.readItemsRegionScan(pb, item_r);

            int alive = 0;
            for (const auto& mob : mobs)
                if (!mob.isDead && mob.hp > 0.f) alive++;

            {
                std::lock_guard<std::mutex> lk(m_mutex);
                bool died = (m_prev_alive_count > 0 && alive < m_prev_alive_count);
                m_mobs  = std::move(mobs);
                m_items = std::move(items);
                m_mob_died_this_tick = died;
                m_prev_alive_count   = alive;

                // Оновлюємо поточний таргет якщо він є
                if (m_targetID != 0) {
                    auto it = std::find_if(m_mobs.begin(), m_mobs.end(),
                        [&](const L2Character& c){ return c.objectID == m_targetID; });
                    if (it != m_mobs.end()) m_target = *it;
                    else                    m_target = std::nullopt;
                }
            }

            // Логуємо тільки при зміні кількості мобів (не кожні 3с)
            static size_t prev_log_mobs = SIZE_MAX;
            if (m_mobs.size() != prev_log_mobs) {
                std::cerr << "[KnownList] mobs=" << m_mobs.size()
                          << " alive=" << alive
                          << " items=" << m_items.size() << "\n";
                prev_log_mobs = m_mobs.size();
            }

            // Діагностика при першому порожньому скані
            static bool diagnosed = false;
            if (!diagnosed && m_mobs.empty() && m_items.empty()) {
                m_reader.diagnoseTypes(pb);
                diagnosed = true;
            }
        }

        // Чекаємо kScanIntervalMs мс, але перевіряємо m_bg_stop кожні 100мс
        for (int i = 0; i < kScanIntervalMs / 100 && !m_bg_stop; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void WorldState::startBackground(uintptr_t playerBase, float mob_range, float item_range) {
    stopBackground();
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_playerBase  = playerBase;
        m_mob_range   = mob_range;
        m_item_range  = item_range;
    }
    m_bg_stop = false;
    m_bg_thread = std::thread(&WorldState::bgLoop, this);
}

void WorldState::stopBackground() {
    m_bg_stop = true;
    if (m_bg_thread.joinable())
        m_bg_thread.join();
}

// ── update(): викликається кожен тік з main loop ──────────────────────────────
// Фоновий потік вже оновлює m_mobs/m_items.
// Тут: оновлюємо playerBase + fast re-read таргету між скануваннями.
void WorldState::update(uintptr_t playerBase, float mob_range, float item_range) {
    if (!playerBase) return;

    // Оновлюємо playerBase/range якщо змінились
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_playerBase = playerBase;
        m_mob_range  = mob_range;
        m_item_range = item_range;
    }

    // Якщо фоновий потік ще не запущено — запустити
    if (!m_bg_thread.joinable() && !m_bg_stop) {
        startBackground(playerBase, mob_range, item_range);
        return;
    }

    // Fast re-read: перечитуємо hp/isDead поточного таргету між скануваннями
    std::lock_guard<std::mutex> lk(m_mutex);
    m_mob_died_this_tick = false;
    if (m_target.has_value() && m_target->memPtr) {
        float hp = 0.f;
        int32_t dead = 0;
        fastRead(m_pid, m_target->memPtr + m_off.charHpOff,     &hp,   4);
        fastRead(m_pid, m_target->memPtr + m_off.charIsDeadOff, &dead, 4);
        m_target->hp     = hp;
        m_target->isDead = (dead != 0);
        if (m_target->isDead || hp <= 0.f) {
            m_mob_died_this_tick = true;
        }
    }
}

bool WorldState::hasValidTarget() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_target.has_value() && !m_target->isDead && m_target->hp > 0.f;
}

bool WorldState::targetIsDead() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (!m_target.has_value()) return false;
    return m_target->isDead || m_target->hp <= 0.f;
}

bool WorldState::hasLootNearby(float px, float py, float range) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    for (const auto& item : m_items)
        if (item.distanceTo(px, py) <= range)
            return true;
    return false;
}

void WorldState::setTarget(int objectID) {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_targetID = objectID;
    auto it = std::find_if(m_mobs.begin(), m_mobs.end(),
        [&](const L2Character& c) { return c.objectID == objectID; });
    if (it != m_mobs.end()) m_target = *it;
    else                    m_target = std::nullopt;
}

void WorldState::clearTarget() {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_targetID = 0;
    m_target   = std::nullopt;
}

std::optional<L2Character> WorldState::findNearestMob(
        const std::vector<L2Character>& mobs,
        float playerX, float playerY,
        float maxRange) const {
    return m_reader.findNearestMob(mobs, playerX, playerY, maxRange);
}

std::optional<L2Character> WorldState::findMobByName(
        const std::vector<L2Character>& mobs,
        const std::string& name,
        float playerX, float playerY,
        float maxRange) const {
    return m_reader.findMobByName(mobs, name, playerX, playerY, maxRange);
}

