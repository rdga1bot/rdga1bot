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
// Типовий флоу:
//   1. scanner.loadOffsets("offsets.json")   // якщо є — пропускаємо сканування
//   2. Якщо не вдалось:
//      base = scanner.findPlayerBase(x, y, z)  // стоячи нерухомо в грі
//      scanner.findKnownListOffset(base, mob_x, mob_y)
//   3. scanner.saveOffsets("offsets.json")
class OffsetScanner {
public:
    explicit OffsetScanner(pid_t pid);

    // Крок 1: знайти PlayerBase, скануючи всю пам'ять на triplet float X/Y/Z.
    // x,y,z — поточні координати гравця (з MemReader або з логу клієнту).
    // tolerance — допуск у L2 units (1.0 = дуже точно; 5.0 = якщо є дрейф).
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

    // Зберегти/завантажити поточні значення offsets у простий JSON.
    // Формат: {"OFF_KNOWN_LIST":288,"OFF_OBJ_TYPE":24,...}
    bool saveOffsets(const std::string& path) const;
    bool loadOffsets(const std::string& path);

    // Runtime-значення offsets (перевизначаються findKnownListOffset або loadOffsets)
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

    struct MemRegion {
        uintptr_t base = 0;
        size_t    size = 0;
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
    static bool isValidPtr(uintptr_t v) {
        return v > 0x10000 && v < 0x7FFF0000;
    }
};
