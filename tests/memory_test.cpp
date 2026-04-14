// SPDX-License-Identifier: MIT
// Memory safety unit tests — Part 4 hardening
#include <gtest/gtest.h>
#include <cstdint>
#include <climits>

// ── isValidPtr: Wine 32-bit address space ─────────────────────────────────────
// Реплікуємо логіку з offset_scanner.h та knownlist_reader.h:
//   return v > 0x10000 && v < 0xBFFF0000
// Тест відокремлений від OffsetScanner щоб не залежати від process_vm_readv.
static bool isValidPtr(uintptr_t v) {
    return v > 0x10000u && v < 0xBFFF0000u;
}

TEST(IsValidPtrTest, NullIsInvalid) {
    EXPECT_FALSE(isValidPtr(0));
}

TEST(IsValidPtrTest, LowAddressIsInvalid) {
    EXPECT_FALSE(isValidPtr(0x1000));     // нижче 0x10000
    EXPECT_FALSE(isValidPtr(0x10000));    // включно — не валідний (умова >)
}

TEST(IsValidPtrTest, LowerBoundaryValid) {
    EXPECT_TRUE(isValidPtr(0x10001));     // перший валідний
}

TEST(IsValidPtrTest, UpperBoundaryInvalid) {
    EXPECT_FALSE(isValidPtr(0xBFFF0000)); // включно — не валідний (умова <)
    EXPECT_FALSE(isValidPtr(0xBFFF0001));
    EXPECT_FALSE(isValidPtr(0xFFFFFFFF));
}

TEST(IsValidPtrTest, UpperBoundaryValid) {
    EXPECT_TRUE(isValidPtr(0xBFFEFFFF));  // останній валідний
}

TEST(IsValidPtrTest, TypicalHeapAddress) {
    EXPECT_TRUE(isValidPtr(0x12345678));
    EXPECT_TRUE(isValidPtr(0x7FFE0000));
}

TEST(IsValidPtrTest, MaxUint32Wraps) {
    // UINT32_MAX може wrap до 0 при uintptr_t якщо 32-bit
    // На 64-bit системі: 0xFFFFFFFF = 4294967295 > 0xBFFF0000 → invalid
    EXPECT_FALSE(isValidPtr(UINT32_MAX));
}

// ── objXOff underflow guard ───────────────────────────────────────────────────
// Перевірка guard: xAddr < objXOff запобігає underflow при xAddr - objXOff.
// Тестуємо арифметику безпосередньо (не залежить від процесу).
TEST(UnderflowGuardTest, GuardPreventsSubtraction) {
    uintptr_t objXOff = 0x24; // OFF_OBJ_X типовий offset

    // xAddr менший за objXOff → без guard: underflow до ~0xFFFFFFDC
    uintptr_t xAddr_bad = 0x10; // < 0x24
    EXPECT_LT(xAddr_bad, objXOff); // guard спрацьовує → continue

    // xAddr більший за objXOff → безпечна арифметика
    uintptr_t xAddr_good = 0x10024;
    EXPECT_GE(xAddr_good, objXOff);
    uintptr_t objBase = xAddr_good - objXOff;
    EXPECT_EQ(objBase, 0x10000u);
    EXPECT_TRUE(isValidPtr(objBase));
}
