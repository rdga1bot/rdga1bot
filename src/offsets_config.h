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

// L2Character extra offsets — ще не відкалібровано для цього клієнту
// Потребують Cheat Engine або dump живого моба (TODO)
constexpr uintptr_t OFF_CHAR_HP      = 0x1F4;  // float  — HP (HF default, може бути неправильно)
constexpr uintptr_t OFF_CHAR_HP_MAX  = 0x1F8;  // float  — MaxHP
constexpr uintptr_t OFF_CHAR_IS_DEAD = 0x210;  // int32  — 1=мертвий

// Region scan параметри (Kamael ElmoreLab, підтверджено calibrate 2026-03-30)
// Об'єкти гри знаходяться у фіксованому регіоні l2.exe flat object array
constexpr uintptr_t OFF_REGION_SCAN_BASE = 0x3F0000; // початок сканування
constexpr uintptr_t OFF_REGION_SCAN_END  = 0x440000; // кінець (320KB)
