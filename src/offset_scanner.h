#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <sys/types.h>
#include "offsets_config.h"

// ── OffsetScanner ─────────────────────────────────────────────────────────────
// Автоматичний пошук KnownList offsets у Wine/L2 процесі.
// Використовує /proc/<pid>/maps та process_vm_readv — без root, без Cheat Engine.
//
// Типовий флоу (без Cheat Engine, без координат):
//   1. scanner.loadOffsets("offsets.json")   // якщо є — пропускаємо сканування
//   2. Якщо не вдалось:
//      base = scanner.blindScan()  // STANDALONE — не потребує жодних координат
//   3. scanner.saveOffsets("offsets.json")
class OffsetScanner {
public:
    explicit OffsetScanner(pid_t pid);

    // ── ОСНОВНИЙ МЕТОД: сліпий скан без координат ────────────────────────────
    // Знаходить PlayerBase чисто структурно — перевіряє base+0x120 → масив obj
    // з координатами в межах L2 світу. Не потребує MemReader, Cheat Engine,
    // або будь-яких попередніх знань.
    // Час виконання: ~2-10с залежно від розміру heap.
    // Повертає 0 при невдачі.
    uintptr_t blindScan(int timeoutMs = 0);

    // Знайти PlayerBase якщо координати гравця вже відомі (швидший, точніший).
    // Використовується як fallback якщо blindScan() дає помилкових кандидатів.
    // Повертає 0 при невдачі.
    uintptr_t findPlayerBase(float x, float y, float z, float tolerance = 1.0f);

    // Крок 2: знайти OFF_KNOWN_LIST, скануючи playerBase+[0x100..0x200] крок 4.
    // nearbyObjX/Y — координати будь-якого видимого моба для верифікації.
    // Повертає 0xFFFFFFFF при невдачі (виводить усі кандидати в cerr).
    uintptr_t findKnownListOffset(uintptr_t playerBase,
                                  float nearbyObjX, float nearbyObjY,
                                  float tolerance = 15.0f);

    // Крок 3 (необов'язково): сканувати objPtr+[0x00..0x80] на float X/Y/Z.
    // Виводить усі кандидати в cerr для ручного аналізу.
    void calibrateObjectOffsets(uintptr_t objPtr,
                                float expectedX, float expectedY, float expectedZ,
                                float tolerance = 5.0f);

    // ── CE-style reverse pointer scan ────────────────────────────────────────────
    // Знаходить всі 4-байтові адреси в heap-пам'яті процесу де зберігається
    // pointer == target (аналог Cheat Engine "What accesses/points to this address").
    // limit=0 → без ліміту. Wine 32-bit: порівнюємо як uint32_t.
    std::vector<uintptr_t> reversePointerScan(uintptr_t target,
                                               size_t limit = 256) const;

    // ── Auto-discover KnownList offset ───────────────────────────────────────────
    // Алгоритм (CE pointer scanner техніка):
    //   1. Для кожної knownObjAddr: reversePointerScan → знаходимо "хто тримає pointer"
    //   2. Групуємо знайдені pointer-адреси в "контейнери" (суміжні адреси → array/list)
    //   3. Перевіряємо: playerBase+[0x80..0x300] → чи вказує на один із контейнерів
    //   4. Повертаємо знайдений offset або 0 при невдачі.
    // knownObjAddrs — адреси знайдених L2Character.memPtr з region scan.
    uintptr_t autoDiscoverKnownList(uintptr_t playerBase,
                                     const std::vector<uintptr_t>& knownObjAddrs);

    // Зберегти/завантажити поточні значення offsets у простий JSON.
    // Формат: {"OFF_KNOWN_LIST":288,"OFF_OBJ_TYPE":24,...}
    bool saveOffsets(const std::string& path) const;
    bool loadOffsets(const std::string& path);

    // Публічне читання для діагностики
    bool readBytesPublic(uintptr_t addr, void* buf, size_t len) const { return readBytes(addr, buf, len); }

    // Публічні обгортки для --calibrate і validity re-check
    template<typename T>
    T rpm_pub(uintptr_t addr) const { return rpm<T>(addr); }

    bool isValidPtr_pub(uintptr_t v) const { return isValidPtr(v); }

    // Автоматичний пошук offset назви через порівняння з відомою назвою моба.
    // Викликати: стояти поряд з мобом, передати його точну назву.
    // Сканує KnownList об'єкти поблизу на char[64] @ offsets 0x60..0x80.
    uintptr_t findNameOffset(uintptr_t playerBase,
                             const std::string& expectedName);

    // Виводить значення +0x28..0x80 від PlayerBase як float/int для діагностики heading.
    // Запускати двічі: до і після повороту на 90°. Порівнювати вивід.
    void calibrateHeadingOffset(uintptr_t playerBase) const;

    // Live monitor: записує baseline, потім виводить тільки змінені offsets.
    // Зупинити Ctrl+C. Крутись у грі — побачиш heading offset одразу.
    void headingMonitor(uintptr_t playerBase) const;

    // Runtime-значення offsets (перевизначаються findKnownListOffset або loadOffsets)
    uintptr_t playerBaseCache = 0;          // зберігається в offsets.json після blindScan
    uintptr_t knownListOff   = OFF_KNOWN_LIST;
    uintptr_t knownCountOff  = OFF_KNOWN_COUNT;
    uintptr_t objTypeOff     = OFF_OBJ_TYPE;
    uintptr_t objXOff        = OFF_OBJ_X;
    uintptr_t objYOff        = OFF_OBJ_Y;
    uintptr_t objZOff        = OFF_OBJ_Z;
    uintptr_t charHpOff      = OFF_CHAR_HP;
    uintptr_t charHpMaxOff   = OFF_CHAR_HP_MAX;
    uintptr_t charIsDeadOff  = OFF_CHAR_IS_DEAD;

private:
    pid_t m_pid;

    // Внутрішня реалізація blindScan без таймауту
    uintptr_t performBlindScan();

    enum class MemRegionType : uint8_t { Heap, Exe, Misc };

    struct MemRegion {
        uintptr_t     base = 0;
        size_t        size = 0;
        MemRegionType type = MemRegionType::Misc;
    };

    // Читабельні регіони з /proc/<pid>/maps (r-- або rw-), розмір ≤ 64MB
    std::vector<MemRegion> getReadableRegions() const;

    // Читання блоку пам'яті через process_vm_readv
    bool readBytes(uintptr_t addr, void* buf, size_t len) const;

    template<typename T>
    T rpm(uintptr_t addr) const {
        T v{};
        readBytes(addr, &v, sizeof(T));
        return v;
    }

    // Перевірка чи значення схоже на 32-bit pointer
    // Wine 32-bit user space reaches 0xBFFFFFFF, not 0x7FFFFFFF
    static bool isValidPtr(uintptr_t v) {
        return v > 0x10000 && v < 0xBFFF0000;
    }
};
