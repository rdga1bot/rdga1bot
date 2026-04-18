#pragma once
#include <cstdint>
#include <cstddef>
#include "platform.h"

// Утилітарний header-only модуль для читання пам'яті Wine/L2 процесу
// через process_vm_readv (без root, Linux only).
// Замінює дубльований код в MemReader.cpp, knownlist_reader.cpp,
// offset_scanner.cpp, world_state.cpp.
namespace ProcessMemory {

inline bool Read(pid_t pid, uintptr_t addr, void* buf, size_t len) {
    if (!pid || !addr || !buf || !len) return false;
    struct iovec local  = { buf,         len };
    struct iovec remote = { (void*)addr, len };
    return process_vm_readv(pid, &local, 1, &remote, 1, 0) == (ssize_t)len;
}

template<typename T>
inline T ReadT(pid_t pid, uintptr_t addr) {
    T val{};
    Read(pid, addr, &val, sizeof(T));
    return val;
}

// Перевірка що адреса виглядає як валідний pointer в user-space
inline bool IsValidPtr(uintptr_t v) {
    return v > 0x10000u && v < 0xBFFF0000u;
}

} // namespace ProcessMemory
