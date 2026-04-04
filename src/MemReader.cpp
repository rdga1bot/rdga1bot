#include "MemReader.h"
#include "ProcessMemory.h"
#include <fstream>
#include <sstream>
#include <string>
#include <cstring>
#include <dirent.h>
#include <sys/types.h>
#include <unistd.h>
#include <cerrno>
#include <iostream>

// ── Знайти PID за іменем процесу ─────────────────────────────────────────────
pid_t MemReader::FindPid(const std::string& proc_name) {
    DIR* dir = opendir("/proc");
    if (!dir) return 0;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        // Тільки числові директорії (PID)
        bool is_num = true;
        for (char c : std::string(entry->d_name))
            if (!isdigit(c)) { is_num = false; break; }
        if (!is_num) continue;

        pid_t pid = (pid_t)std::stoi(entry->d_name);
        std::string comm_path = "/proc/" + std::string(entry->d_name) + "/comm";
        std::ifstream comm(comm_path);
        if (!comm) continue;
        std::string name;
        std::getline(comm, name);
        // comm обрізає до 15 символів — перевіряємо і повне cmdline
        if (name == proc_name || name == proc_name.substr(0, 15)) {
            closedir(dir);
            return pid;
        }
        // Перевіряємо cmdline (Wine запускає l2.exe як окремий процес)
        std::ifstream cmdline("/proc/" + std::string(entry->d_name) + "/cmdline");
        if (!cmdline) continue;
        std::string cmd;
        std::getline(cmdline, cmd, '\0'); // перший аргумент (exe path)
        // Шукаємо ім'я файлу в кінці шляху
        auto pos = cmd.rfind('/');
        std::string exe = (pos != std::string::npos) ? cmd.substr(pos + 1) : cmd;
        // case-insensitive для Wine (l2.exe / L2.exe)
        auto lower = [](std::string s) {
            for (auto& c : s) c = (char)tolower((unsigned char)c);
            return s;
        };
        if (lower(exe) == lower(proc_name)) {
            closedir(dir);
            return pid;
        }
    }
    closedir(dir);
    return 0;
}

// ── Знайти base address модуля з /proc/PID/maps ──────────────────────────────
uintptr_t MemReader::FindModuleBase(pid_t pid, const std::string& module) {
    std::ifstream maps("/proc/" + std::to_string(pid) + "/maps");
    if (!maps) return 0;

    auto lower = [](std::string s) {
        for (auto& c : s) c = (char)tolower((unsigned char)c);
        return s;
    };
    const std::string mod_lower = lower(module);

    std::string line;
    while (std::getline(maps, line)) {
        // Формат: addr_start-addr_end perms offset dev inode [path]
        if (lower(line).find(mod_lower) == std::string::npos) continue;
        // Перший запис = base (text section)
        if (line.find('x') == std::string::npos) continue; // шукаємо executable
        uintptr_t addr_start = 0;
        std::sscanf(line.c_str(), "%lx", &addr_start);
        if (addr_start) return addr_start;
    }
    return 0;
}

// ── Open ─────────────────────────────────────────────────────────────────────
bool MemReader::Open(const std::string& proc_name) {
    Close();
    m_pid = FindPid(proc_name);
    if (!m_pid) {
        std::cerr << "[MemReader] Процес '" << proc_name << "' не знайдено\n";
        return false;
    }
    m_base = FindModuleBase(m_pid, proc_name);
    if (!m_base) {
        // Для Wine процес може називатись інакше в maps — спробуємо l2.exe без розширення
        m_base = FindModuleBase(m_pid, "l2");
        if (!m_base) {
            std::cerr << "[MemReader] Base address для '" << proc_name
                      << "' не знайдено в /proc/" << m_pid << "/maps\n";
            m_pid = 0;
            return false;
        }
    }
    std::cerr << "[MemReader] Знайдено PID=" << m_pid
              << " base=0x" << std::hex << m_base << std::dec << "\n";
    return true;
}

void MemReader::Close() {
    m_pid  = 0;
    m_base = 0;
}

// ── ReadBytes через process_vm_readv ─────────────────────────────────────────
bool MemReader::ReadBytes(uintptr_t abs_addr, void* buf, size_t len) const {
    return ProcessMemory::Read(m_pid, abs_addr, buf, len);
}

// ── ResolveChain ─────────────────────────────────────────────────────────────
// base_addr → dereference → +chain[0] → dereference → +chain[1] → ... → фінальна адреса
uintptr_t MemReader::ResolveChain(uintptr_t base_addr,
                                  const std::vector<uintptr_t>& chain) const {
    uintptr_t addr = base_addr;
    for (uintptr_t off : chain) {
        // Wine — 32-bit процес: pointer = uint32_t
        uint32_t ptr = 0;
        if (!ReadBytes(addr, &ptr, sizeof(ptr))) return 0;
        addr = (uintptr_t)ptr + off;
    }
    return addr;
}

// ── ReadPlayer ───────────────────────────────────────────────────────────────
MemReader::PlayerState MemReader::ReadPlayer() const {
    PlayerState state;
    if (!m_off.enabled || !IsOpen()) return state;

    // Отримуємо адресу об'єкту гравця через pointer chain
    uintptr_t obj_addr = m_base + m_off.player_ptr;
    if (!m_off.ptr_chain.empty())
        obj_addr = ResolveChain(obj_addr, m_off.ptr_chain);
    if (!obj_addr) return state;

    // Читаємо поля (int32 для HP/MP/CP, float для позиції)
    auto ri = [&](uintptr_t off, int& out) {
        int32_t v = 0;
        if (ReadBytes(obj_addr + off, &v, sizeof(v))) out = v;
    };
    auto rf = [&](uintptr_t off, float& out) {
        ReadBytes(obj_addr + off, &out, sizeof(out));
    };

    if (m_off.hp_off)     ri(m_off.hp_off,     state.hp);
    if (m_off.max_hp_off) ri(m_off.max_hp_off, state.max_hp);
    if (m_off.mp_off)     ri(m_off.mp_off,     state.mp);
    if (m_off.max_mp_off) ri(m_off.max_mp_off, state.max_mp);
    if (m_off.cp_off)     ri(m_off.cp_off,     state.cp);
    if (m_off.max_cp_off) ri(m_off.max_cp_off, state.max_cp);
    if (m_off.pos_x_off)  rf(m_off.pos_x_off,  state.x);
    if (m_off.pos_y_off)  rf(m_off.pos_y_off,  state.y);
    if (m_off.pos_z_off)  rf(m_off.pos_z_off,  state.z);

    // Heading (якщо offset задано)
    if (m_off.heading_off) {
        float hval = 0.f;
        if (ReadBytes(obj_addr + m_off.heading_off, &hval, sizeof(hval)))
            state.heading = hval;
    }

    state.valid = (state.hp >= 0 && state.max_hp > 0);
    return state;
}
