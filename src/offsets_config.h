#pragma once
#include <cstdint>

// ── KnownList offsets (ElmoreLab Kamael client, calibrated 2026-03-30) ────────
// Дефолти відкалібровані для ElmoreLab Kamael через --calibrate + float scan.
// Перевизначаються runtime через OffsetScanner::loadOffsets("offsets.json").

// PlayerBase offsets (структура гравця — інша від загальних об'єктів!)
constexpr uintptr_t OFF_PLAYER_X     = 0x24;   // float — world X (підтверджено blindScan)
constexpr uintptr_t OFF_PLAYER_Y     = 0x28;   // float — world Y
constexpr uintptr_t OFF_PLAYER_Z     = 0x2C;   // float — world Z
constexpr uintptr_t OFF_KNOWN_LIST   = 0x120;  // ptr → (для Kamael — НЕ масив об'єктів!)
constexpr uintptr_t OFF_KNOWN_COUNT  = 0x124;  // (не використовується в region scan)

// L2Object offsets (від початку об'єкту, stride=0x5C0 між об'єктами)
// Калібровано через float scan: stride=0x5C0 підтверджено на 6+ об'єктах
constexpr uintptr_t OFF_OBJ_ID       = 0x00;   // int32  — objectID (не відкалібровано)
constexpr uintptr_t OFF_OBJ_TYPE     = 0x5C;   // int32  — 0=mob,1=player,2=item,3=static
constexpr uintptr_t OFF_OBJ_X        = 0x90;   // float  — world X (підтверджено float scan)
constexpr uintptr_t OFF_OBJ_Y        = 0x94;   // float  — world Y
constexpr uintptr_t OFF_OBJ_Z        = 0x98;   // float  — world Z

// L2Character extra offsets (відкалібровано ElmoreLab Kamael 2026-04-02)
constexpr uintptr_t OFF_CHAR_HP      = 0x100;  // float  — HP (підтверджено: 87.22→0.00 після kill)
constexpr uintptr_t OFF_CHAR_HP_MAX  = 0x000;  // N/A    — ElmoreLab не зберігає MaxHP в KnownList
constexpr uintptr_t OFF_CHAR_IS_DEAD = 0x180;  // int32  — 1=мертвий (підтверджено: 0→1 після kill)

// L2Character extended offsets (потребують калібровки через --calibrate)
// MP гравця/моба — типові HF значення, перевір через HP scan в --calibrate
constexpr uintptr_t OFF_CHAR_MP      = 0x1FC;  // float  — MP
constexpr uintptr_t OFF_CHAR_MP_MAX  = 0x200;  // float  — MaxMP
constexpr uintptr_t OFF_CHAR_LEVEL   = 0x214;  // int32  — рівень

// Heading персонажа. Калібрувати: стоячи нерухомо запустити --calibrate,
// потім повернутись на 90° і знову. Значення що змінилось — це heading.
// Одиниці: float радіани [-pi..pi] або int16 [0..65535] (залежить від клієнту).
constexpr uintptr_t OFF_PLAYER_HEADING = 0x30;

// Назва об'єкту. Калібрувати через --calibrate --name "Назва моба поряд".
// Формат: char[64] UTF-8 або wchar_t[32] UTF-16 (залежить від клієнту).
// Перевір обидва через --calibrate dump.
constexpr uintptr_t OFF_OBJ_NAME = 0x68;

// Region scan параметри (Kamael ElmoreLab, підтверджено calibrate 2026-03-30)
// Об'єкти гри знаходяться у фіксованому регіоні l2.exe flat object array.
// 0x3F0000-0x500000 = 704KB покриває підтверджений діапазон + буфер для далеких мобів.
// stride=0x5C0 (1472 bytes) → 704KB / 1472 ≈ 490 object slots.
constexpr uintptr_t OFF_REGION_SCAN_BASE = 0x3F0000; // початок сканування
constexpr uintptr_t OFF_REGION_SCAN_END  = 0x500000; // кінець (704KB, розширено з 320KB)
