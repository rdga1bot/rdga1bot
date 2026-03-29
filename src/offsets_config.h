#pragma once
#include <cstdint>

// ── KnownList offsets (HF client, calibrate per server via OffsetScanner) ────
// Всі значення — дефолти для HF клієнта.
// Перевизначаються runtime через OffsetScanner::loadOffsets("offsets.json").

// PlayerBase offsets
constexpr uintptr_t OFF_PLAYER_X     = 0x24;   // float — world X
constexpr uintptr_t OFF_PLAYER_Y     = 0x28;   // float — world Y
constexpr uintptr_t OFF_PLAYER_Z     = 0x2C;   // float — world Z
constexpr uintptr_t OFF_KNOWN_LIST   = 0x120;  // ptr → array of obj ptrs
constexpr uintptr_t OFF_KNOWN_COUNT  = 0x124;  // int32 — кількість об'єктів

// L2Object offsets (від початку об'єкту)
constexpr uintptr_t OFF_OBJ_ID       = 0x00;   // int32  — objectID
constexpr uintptr_t OFF_OBJ_TYPE     = 0x18;   // int32  — 0=mob,1=player,2=item,3=static
constexpr uintptr_t OFF_OBJ_X        = 0x24;   // float  — world X
constexpr uintptr_t OFF_OBJ_Y        = 0x28;   // float  — world Y
constexpr uintptr_t OFF_OBJ_Z        = 0x2C;   // float  — world Z

// L2Character extra offsets (мобі/гравці)
constexpr uintptr_t OFF_CHAR_HP      = 0x1F4;  // float  — поточне HP
constexpr uintptr_t OFF_CHAR_HP_MAX  = 0x1F8;  // float  — максимальне HP
constexpr uintptr_t OFF_CHAR_IS_DEAD = 0x210;  // int32  — 1 = мертвий
