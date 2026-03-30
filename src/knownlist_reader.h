#pragma once
#include <vector>
#include <optional>
#include <string>
#include <sys/types.h>
#include "l2_objects.h"
#include "offset_scanner.h"

// ── KnownListReader ───────────────────────────────────────────────────────────
// Читає KnownList (масив об'єктів поблизу) з пам'яті Wine/L2 процесу.
// Патерн ReadBytes скопійовано з MemReader (не рефакторимо MemReader).
class KnownListReader {
public:
    KnownListReader(pid_t pid, const OffsetScanner& offsets);

    // Читати всі об'єкти KnownList.
    // playerBase — знайдений OffsetScanner::findPlayerBase().
    std::vector<L2Object>    readAll   (uintptr_t playerBase) const;
    std::vector<L2Character> readMobs  (uintptr_t playerBase) const;
    std::vector<L2Object>    readItems (uintptr_t playerBase) const;

    // Знайти найближчого живого моба в радіусі maxRange L2-юнітів.
    std::optional<L2Character> findNearestMob(
        const std::vector<L2Character>& mobs,
        float playerX, float playerY,
        float maxRange = 1200.f) const;

    // Читання назви об'єкту з пам'яті.
    // Спочатку UTF-8 (char[64] @ nameOff), потім UTF-16 (wchar_t[32] @ nameOff+4).
    // Повертає порожній рядок якщо не знайдено або назва непринтована.
    std::string readName(uintptr_t objPtr) const;

    // Пошук моба за назвою (точний або частковий збіг, case-insensitive).
    // Повертає найближчого моба з відповідною назвою в радіусі maxRange.
    std::optional<L2Character> findMobByName(
        const std::vector<L2Character>& mobs,
        const std::string& name,
        float playerX, float playerY,
        float maxRange = 1500.f) const;

    // ── Region scan (Kamael ElmoreLab) ────────────────────────────────────────
    // Читає плоский масив об'єктів напряму зі сканом пам'яті замість KnownList ptr.
    // Метод для клієнтів де pb+0x120 НЕ є масивом вказівників на об'єкти.
    // Читає увесь регіон одним readBytes(), сканує на XYZ triplets у L2 bounds,
    // фільтрує за відстанню від гравця та типом (Mob/Item).
    std::vector<L2Character> readMobsRegionScan(uintptr_t playerBase,
                                                float maxRange = 1500.f) const;
    std::vector<L2Object>    readItemsRegionScan(uintptr_t playerBase,
                                                 float maxRange = 500.f) const;

    // Читає всі об'єкти як L2Character без type filter.
    // Використовується поки objTypeOff не відкалібрований (readMobs() повертає 0).
    // Фільтр: координати в межах L2 world + hp > 0 + hpMax в розумних межах.
    std::vector<L2Character> readAllAsChars(uintptr_t playerBase) const;

    // Діагностика типів: логує offsets +0x14..+0x20 для перших 10 об'єктів.
    // Допомагає знайти правильний objTypeOff якщо readMobs() порожній.
    void diagnoseTypes(uintptr_t playerBase) const;

private:
    pid_t                m_pid;
    const OffsetScanner& m_off;

    bool readBytes(uintptr_t addr, void* buf, size_t len) const;

    template<typename T>
    T rpm(uintptr_t addr) const {
        T v{};
        readBytes(addr, &v, sizeof(T));
        return v;
    }

    static bool isValidPtr(uintptr_t v) {
        // Wine 32-bit user space reaches 0xBFFFFFFF, not 0x7FFFFFFF
        return v > 0x10000 && v < 0xBFFF0000;
    }

    // Динамічний список регіонів пам'яті для сканування (з /proc/<pid>/maps).
    // Кешується 30с — не читаємо /proc щотіку.
    struct ScanRegion { uintptr_t base = 0; size_t size = 0; };
    mutable std::vector<ScanRegion> m_scan_cache;
    mutable time_t                  m_scan_cache_time = 0;
    void refreshScanCache() const;
};
