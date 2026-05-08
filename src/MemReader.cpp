#include "MemReader.h"
#include "ProcessMemory.h"
#include <fstream>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <vector>
#include <sstream>
#include <string>
#include <cstring>
#include <cerrno>
#include <iostream>
#include <iomanip>
#ifndef _WIN32
#include <dirent.h>
#include <sys/types.h>
#include <unistd.h>
#endif

// ── Знайти PID за іменем процесу ─────────────────────────────────────────────
#ifdef _WIN32
pid_t MemReader::FindPid(const std::string& proc_name) {
    auto lower = [](std::string s) {
        for (auto& c : s) c = (char)tolower((unsigned char)c);
        return s;
    };
    const std::string target = lower(proc_name);

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32 pe = {};
    pe.dwSize = sizeof(pe);
    pid_t found = 0;
    if (Process32First(snap, &pe)) {
        do {
            std::string exe(pe.szExeFile);
            if (lower(exe) == target) {
                found = pe.th32ProcessID;
                break;
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}
#else
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
#endif

// ── Знайти base address модуля ────────────────────────────────────────────────
#ifdef _WIN32
uintptr_t MemReader::FindModuleBase(pid_t pid, const std::string& module) {
    auto lower = [](std::string s) {
        for (auto& c : s) c = (char)tolower((unsigned char)c);
        return s;
    };
    const std::string target = lower(module);

    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!h) return 0;

    HMODULE mods[1024];
    DWORD needed = 0;
    uintptr_t base = 0;
    if (EnumProcessModules(h, mods, sizeof(mods), &needed)) {
        DWORD count = needed / sizeof(HMODULE);
        char name[MAX_PATH];
        for (DWORD i = 0; i < count; ++i) {
            if (GetModuleFileNameExA(h, mods[i], name, sizeof(name))) {
                std::string fname(name);
                auto slash = fname.rfind('\\');
                if (slash == std::string::npos) slash = fname.rfind('/');
                std::string basename = (slash != std::string::npos) ? fname.substr(slash + 1) : fname;
                if (lower(basename) == target) {
                    MODULEINFO mi = {};
                    GetModuleInformation(h, mods[i], &mi, sizeof(mi));
                    base = (uintptr_t)mi.lpBaseOfDll;
                    break;
                }
            }
        }
    }
    CloseHandle(h);
    return base;
}
#else
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
#endif

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

    // Отримуємо адресу об'єкту гравця
    uintptr_t obj_addr = 0;
    if (m_off.use_kl_base) {
        // Режим UseKLBase: playerBase передається від KnownList напряму (SetDirectBase)
        obj_addr = m_direct_base;
    } else {
        obj_addr = m_base + m_off.player_ptr;
        if (!m_off.ptr_chain.empty())
            obj_addr = ResolveChain(obj_addr, m_off.ptr_chain);
    }
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

    // valid: HP+MaxHP прочитані → повний режим (замінює OCR HP)
    //        XY координати прочитані (UseKLBase режим) → coords_valid, HP з OCR
    const bool hp_ok    = (state.hp >= 0 && state.max_hp > 0);
    const bool coords_ok = std::isfinite(state.x) && std::isfinite(state.y)
                           && (state.x != 0.f || state.y != 0.f);
    state.valid = hp_ok || (m_off.use_kl_base && coords_ok);
    return state;
}

// ── HpAutoCalib ───────────────────────────────────────────────────────────────
bool HpAutoCalib::matchPct(uint32_t cur, uint32_t mx, int pct, int tol) {
    if (mx < 50u || mx > 500000u) return false;
    if (cur == 0u || cur > mx)    return false;
    int ratio = (int)((uint64_t)cur * 100u / mx);
    return std::abs(ratio - pct) <= tol;
}

std::optional<HpAutoCalib::HpOffsets> HpAutoCalib::tick(
        pid_t pid, uintptr_t playerBase, int ocr_hp)
{
    if (m_state == State::Confirmed)  return std::nullopt;
    if (!pid || !playerBase)          return std::nullopt;
    if (ocr_hp < 1 || ocr_hp > 100)  return std::nullopt;

    constexpr size_t kN      = 0x500 / 4; // 320 uint32 = 0x500 байт
    constexpr size_t kWindow = 64;        // пара (cur,max) в межах 256 байт
    constexpr int    kTol    = 5;         // допуск ±5% при match
    constexpr int    kDelta  = 3;         // мінімальна зміна OCR% для диф.валідації
    constexpr int    kNeed   = 3;         // підтверджень для перемоги
    constexpr int    kMaxTry = 20;        // максимум диф.спроб

    std::vector<uint32_t> buf(kN, 0);
    ProcessMemory::Read(pid, playerBase, buf.data(), kN * 4);

    // ── Фаза 1: Searching ────────────────────────────────────────────────────
    if (m_state == State::Idle || m_state == State::Searching) {
        if (ocr_hp >= 95) return std::nullopt; // зачекати поки HP < 95%

        m_cands.clear();
        for (size_t i = 0; i < kN; ++i) {
            size_t jend = std::min(i + kWindow, kN);
            for (size_t j = i + 1; j < jend; ++j)
                if (matchPct(buf[i], buf[j], ocr_hp, kTol))
                    m_cands.push_back({i * 4u, j * 4u, 0});
        }
        m_snap     = buf;
        m_prev_ocr = ocr_hp;
        m_attempts = 0;

        if (!m_cands.empty()) {
            m_state = State::Validating;
            std::cerr << "[AutoCalib] Фаза 1: " << m_cands.size()
                      << " кандидатів (HP=" << ocr_hp << "%). Валідуємо...\n";
        } else {
            // Повний діагностичний дамп — ВСІ значення 0x00..kN*4
            std::cerr << "[AutoCalib] 0 кандидатів (HP=" << ocr_hp
                      << "%). Дамп 0x00..0x" << std::hex << kN*4 << " (значення [1..500000]):\n" << std::dec;
            for (size_t i = 0; i < kN; ++i) {
                uint32_t v = buf[i];
                if (v >= 1 && v <= 500000)
                    std::cerr << "  +0x" << std::hex << std::setw(3) << std::setfill('0') << i*4
                              << " = " << std::dec << v << "\n";
            }
            // Пари що відповідають HP% ±10% (розширений допуск для діагностики)
            std::cerr << "[AutoCalib] Пошук пар cur/max ≈ " << ocr_hp << "% (±10%, max до 500000):\n";
            for (size_t i = 0; i < kN; ++i) {
                uint32_t cur = buf[i];
                if (cur < 1 || cur > 500000) continue;
                for (size_t j = i+1; j < std::min(kN, i+64); ++j) {
                    uint32_t mx = buf[j];
                    if (mx < cur || mx > 500000) continue;
                    int ratio = (int)((uint64_t)cur * 100u / mx);
                    if (std::abs(ratio - ocr_hp) <= 10)
                        std::cerr << "  cur=+0x" << std::hex << i*4 << "(" << std::dec << cur
                                  << ") max=+0x" << std::hex << j*4 << "(" << std::dec << mx
                                  << ") ratio=" << ratio << "%\n";
                }
            }
        }
        return std::nullopt;
    }

    // ── Фаза 2: Validating ───────────────────────────────────────────────────
    int delta = std::abs(ocr_hp - m_prev_ocr);
    if (delta < kDelta) return std::nullopt; // OCR не змінився достатньо

    ++m_attempts;
    std::vector<Candidate> survivors;

    for (auto& c : m_cands) {
        size_t ci = c.cur_off / 4, mi = c.max_off / 4;
        if (ci >= kN || mi >= kN || mi >= m_snap.size()) continue;

        uint32_t old_cur = m_snap[ci], old_max = m_snap[mi];
        uint32_t new_cur = buf[ci],    new_max  = buf[mi];

        // Перевірка 1: нові значення відповідають новому OCR%
        if (!matchPct(new_cur, new_max, ocr_hp, kTol)) continue;

        // Перевірка 2: диференціал — зміна в пам'яті ≈ зміна OCR
        if (old_max > 0u && new_max > 0u) {
            int old_pct = (int)((uint64_t)old_cur * 100u / old_max);
            int new_pct = (int)((uint64_t)new_cur * 100u / new_max);
            int d_mem   = std::abs(new_pct - old_pct);
            if (std::abs(d_mem - delta) > kTol + 2) continue; // диф.розбіжність
        }

        ++c.hits;
        survivors.push_back(c);
    }

    m_cands    = survivors;
    m_snap     = buf;
    m_prev_ocr = ocr_hp;

    if (m_cands.empty()) {
        std::cerr << "[AutoCalib] Всі кандидати відхилені. Перезапуск пошуку.\n";
        m_state = State::Searching;
        return std::nullopt;
    }

    auto best = std::max_element(m_cands.begin(), m_cands.end(),
        [](const Candidate& a, const Candidate& b){ return a.hits < b.hits; });

    std::cerr << "[AutoCalib] Спроба " << m_attempts
              << "/" << kMaxTry << ": " << m_cands.size() << " кандидатів"
              << ", best hits=" << best->hits
              << " cur=+0x" << std::hex << best->cur_off
              << " max=+0x" << best->max_off << std::dec << "\n";

    if (best->hits >= kNeed || m_attempts >= kMaxTry) {
        m_state = State::Confirmed;
        std::cerr << "[AutoCalib] ПІДТВЕРДЖЕНО: HP cur=+0x"
                  << std::hex << best->cur_off
                  << " max=+0x" << best->max_off << std::dec
                  << " (hits=" << best->hits << ")\n";
        return HpOffsets{best->cur_off, best->max_off};
    }
    return std::nullopt;
}

// ── AutoCalibratePlayer ───────────────────────────────────────────────────────
MemReader::AutoCalibResult MemReader::AutoCalibratePlayer(
        pid_t pid, uintptr_t playerBase,
        int hp_pct, int mp_pct, int cp_pct,
        uintptr_t scan_max)
{
    AutoCalibResult result;
    if (!pid || !playerBase || scan_max < 8) return result;

    // Читаємо весь блок за один виклик
    const size_t n = scan_max / 4;
    std::vector<uint32_t> buf(n, 0);
    ProcessMemory::Read(pid, playerBase, buf.data(), n * 4);

    // Повертає true якщо cur/max*100 ≈ pct (±tol%) і значення виглядають як HP
    auto match = [](uint32_t cur, uint32_t mx, int pct, int tol = 4) -> bool {
        if (mx < 100u || mx > 500000u) return false;
        if (cur == 0u || cur > mx)     return false;
        int ratio = (int)((uint64_t)cur * 100u / mx);
        return std::abs(ratio - pct) <= tol;
    };

    // Якщо всі три статси = 100% — калібрування ненадійне (будь-яка рівна пара підійде)
    const bool degenerate = (hp_pct >= 98 && mp_pct >= 98 && cp_pct >= 98);

    // Шукаємо пари (cur_idx, max_idx) в межах вікна 64 байти (16 uint32)
    constexpr size_t kWindow = 16;
    for (size_t i = 0; i < n && i < n; ++i) {
        uint32_t cur = buf[i];
        if (cur < 10u || cur > 500000u) continue;

        size_t jend = std::min(i + kWindow, n);
        for (size_t j = i + 1; j < jend; ++j) {
            uint32_t mx = buf[j];

            if (!result.found_hp && match(cur, mx, hp_pct)) {
                // Перевірка: пара не підходить одночасно до mp/cp (якщо ті відрізняються)
                bool ambig = (!degenerate)
                    && (std::abs(hp_pct - mp_pct) < 3 || match(cur, mx, mp_pct))
                    && (std::abs(hp_pct - cp_pct) < 3 || match(cur, mx, cp_pct));
                if (!ambig) {
                    result.hp_off = i * 4; result.max_hp_off = j * 4;
                    result.found_hp = true;
                }
            }
            if (!result.found_mp && match(cur, mx, mp_pct)) {
                bool ambig = result.found_hp && (i * 4 == result.hp_off);
                if (!ambig) {
                    result.mp_off = i * 4; result.max_mp_off = j * 4;
                    result.found_mp = true;
                }
            }
            if (!result.found_cp && match(cur, mx, cp_pct)) {
                bool ambig = (result.found_hp && i * 4 == result.hp_off)
                          || (result.found_mp && i * 4 == result.mp_off);
                if (!ambig) {
                    result.cp_off = i * 4; result.max_cp_off = j * 4;
                    result.found_cp = true;
                }
            }
        }
        if (result.found_hp && result.found_mp && result.found_cp) break;
    }

    std::cerr << "[MemCalib] playerBase=0x" << std::hex << playerBase << std::dec
              << " hp=" << hp_pct << "% mp=" << mp_pct << "% cp=" << cp_pct << "%\n";
    if (result.found_hp)
        std::cerr << "[MemCalib] HP  cur=+0x" << std::hex << result.hp_off
                  << " max=+0x" << result.max_hp_off << std::dec << "\n";
    if (result.found_mp)
        std::cerr << "[MemCalib] MP  cur=+0x" << std::hex << result.mp_off
                  << " max=+0x" << result.max_mp_off << std::dec << "\n";
    if (result.found_cp)
        std::cerr << "[MemCalib] CP  cur=+0x" << std::hex << result.cp_off
                  << " max=+0x" << result.max_cp_off << std::dec << "\n";
    if (!result.found_hp && !result.found_mp && !result.found_cp)
        std::cerr << "[MemCalib] Нічого не знайдено — спробуй при HP/MP < 90%\n";

    return result;
}

// ── SaveCalib / LoadCalib ─────────────────────────────────────────────────────
void MemReader::SaveCalib(const AutoCalibResult& r, const std::string& path) const {
    std::ofstream f(path);
    if (!f) return;
    f << "{\n"
      << "  \"hp_off\":"     << r.hp_off     << ",\n"
      << "  \"max_hp_off\":" << r.max_hp_off << ",\n"
      << "  \"mp_off\":"     << r.mp_off     << ",\n"
      << "  \"max_mp_off\":" << r.max_mp_off << ",\n"
      << "  \"cp_off\":"     << r.cp_off     << ",\n"
      << "  \"max_cp_off\":" << r.max_cp_off << "\n"
      << "}\n";
    std::cerr << "[MemCalib] Збережено: " << path << "\n";
}

bool MemReader::LoadCalib(const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;

    auto parseUint = [&](const std::string& key, uintptr_t& out) {
        std::string line;
        f.clear(); f.seekg(0);
        while (std::getline(f, line)) {
            auto pos = line.find("\"" + key + "\"");
            if (pos == std::string::npos) continue;
            auto colon = line.find(':', pos);
            if (colon == std::string::npos) continue;
            try { out = (uintptr_t)std::stoull(line.substr(colon + 1)); } catch (...) {}
            return;
        }
    };

    AutoCalibResult r;
    parseUint("hp_off",     r.hp_off);
    parseUint("max_hp_off", r.max_hp_off);
    parseUint("mp_off",     r.mp_off);
    parseUint("max_mp_off", r.max_mp_off);
    parseUint("cp_off",     r.cp_off);
    parseUint("max_cp_off", r.max_cp_off);

    r.found_hp = (r.hp_off > 0 && r.max_hp_off > 0);
    r.found_mp = (r.mp_off > 0 && r.max_mp_off > 0);
    r.found_cp = (r.cp_off > 0 && r.max_cp_off > 0);

    if (!r.found_hp && !r.found_mp) return false;

    if (r.found_hp) { m_off.hp_off = r.hp_off; m_off.max_hp_off = r.max_hp_off; }
    if (r.found_mp) { m_off.mp_off = r.mp_off; m_off.max_mp_off = r.max_mp_off; }
    if (r.found_cp) { m_off.cp_off = r.cp_off; m_off.max_cp_off = r.max_cp_off; }

    std::cerr << "[MemCalib] Завантажено з " << path << "\n";
    return true;
}
