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

    // HP/MaxHP: через global anchor pointer chain (DLL global → struct_base + offset).
    // Координати (+0x24/28/2C) та інші поля — напряму з obj_addr (playerBase).
    auto ri = [&](uintptr_t base, uintptr_t off, int& out) {
        int32_t v = 0;
        if (ReadBytes(base + off, &v, sizeof(v))) out = v;
    };
    auto rf = [&](uintptr_t off, float& out) {
        ReadBytes(obj_addr + off, &out, sizeof(out));
    };

    // HP/MaxHP: пріоритет — абсолютні адреси з full-scan AutoCalib.
    // Fallback — legacy DLL-anchor (нестабільний, залишено для сумісності).
    if (m_off.hp_abs) {
        // Абсолютні адреси — session-specific, знайдені full-process scan'ом.
        int32_t v = 0;
        if (ReadBytes(m_off.hp_abs,     &v, 4)) state.hp     = v;
        v = 0;
        if (ReadBytes(m_off.max_hp_abs, &v, 4)) state.max_hp = v;
    } else if (m_off.hp_anchor_addr) {
        // Legacy: struct_base = *(hp_anchor_addr) - hp_anchor_sub
        uintptr_t hp_base = obj_addr;
        uint32_t anchor_raw = 0;
        if (ReadBytes(m_off.hp_anchor_addr, &anchor_raw, 4) && anchor_raw > 0x10000u)
            hp_base = (uintptr_t)anchor_raw - m_off.hp_anchor_sub;
        ri(hp_base, m_off.hp_off,     state.hp);
        ri(hp_base, m_off.max_hp_off, state.max_hp);
    } else if (m_off.hp_off || m_off.max_hp_off) {
        // Legacy: game_obj через render_node+0x58
        uintptr_t hp_base = obj_addr;
        uint32_t gobj_raw = 0;
        if (ReadBytes(obj_addr + 0x58, &gobj_raw, 4) && gobj_raw > 0x10000u)
            hp_base = (uintptr_t)gobj_raw;
        if (m_off.hp_off)     ri(hp_base, m_off.hp_off,     state.hp);
        if (m_off.max_hp_off) ri(hp_base, m_off.max_hp_off, state.max_hp);
    }
    if (m_off.mp_off)     ri(obj_addr, m_off.mp_off,     state.mp);
    if (m_off.max_mp_off) ri(obj_addr, m_off.max_mp_off, state.max_mp);
    if (m_off.cp_off)     ri(obj_addr, m_off.cp_off,     state.cp);
    if (m_off.max_cp_off) ri(obj_addr, m_off.max_cp_off, state.max_cp);
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
    if (mx < 5000u || mx > 500000u) return false; // player HP завжди > 5000
    if (cur == 0u || cur > mx)      return false;
    int ratio = (int)((uint64_t)cur * 100u / mx);
    return std::abs(ratio - pct) <= tol;
}

std::optional<HpAutoCalib::HpOffsets> HpAutoCalib::tick(
        pid_t pid, uintptr_t /*playerBase*/, int ocr_hp)
{
    if (m_state == State::Confirmed)  return std::nullopt;
    if (!pid)                         return std::nullopt;
    if (ocr_hp < 1 || ocr_hp > 100)  return std::nullopt;

    constexpr int    kTol    = 4;   // ±4% OCR допуск
    constexpr int    kDelta  = 3;   // мінімальна зміна OCR% для диф.валідації
    constexpr int    kNeed   = 3;   // підтверджень для перемоги
    constexpr int    kMaxTry = 20;  // максимум диф.спроб
    // kWindow: пара (cur,max) в межах 32 байт (реальний layout: max=+0, cur=+8)
    constexpr size_t kWindow = 8;

    // ── Фаза 1: повне сканування процесу (full-process scan) ─────────────────
    // Аналог --scan-hp: читаємо всі r-w регіони і шукаємо пари (cur,max)
    // де matchPct(cur, max, ocr_hp).
    if (m_state == State::Idle || m_state == State::Searching) {
        if (ocr_hp >= 95) return std::nullopt; // зачекати поки HP < 95%

        m_cands.clear();

        std::ifstream maps("/proc/" + std::to_string(pid) + "/maps");
        if (!maps) return std::nullopt;

        std::string line;
        size_t total_cands = 0;
        while (std::getline(maps, line)) {
            // Тільки r-w регіони (heap, data) — не r-x (code) і не r-- (read-only)
            if (line.size() < 5) continue;
            char perms[5] = {};
            uintptr_t rstart = 0, rend = 0;
            std::sscanf(line.c_str(), "%lx-%lx %4s", &rstart, &rend, perms);
            if (perms[0] != 'r' || perms[1] != 'w') continue;
            if (rend <= rstart) continue;
            size_t rlen = rend - rstart;
            if (rlen > 64u * 1024u * 1024u) continue; // пропускаємо регіони > 64MB

            size_t n = rlen / 4;
            std::vector<uint32_t> buf(n, 0);
            if (!ProcessMemory::Read(pid, rstart, buf.data(), rlen)) continue;

            for (size_t i = 0; i < n; ++i) {
                uint32_t cur = buf[i];
                if (cur < 5000u || cur > 200000u) continue; // filter: player HP range
                size_t jend = std::min(i + kWindow, n);
                for (size_t j = i + 1; j < jend; ++j) {
                    if (matchPct(cur, buf[j], ocr_hp, kTol)) {
                        m_cands.push_back({rstart + i * 4, rstart + j * 4,
                                           0, cur, buf[j]});
                        ++total_cands;
                    }
                }
            }
        }

        m_prev_ocr = ocr_hp;
        m_attempts = 0;

        if (!m_cands.empty()) {
            m_state = State::Validating;
            std::cerr << "[AutoCalib] Фаза 1: " << m_cands.size()
                      << " кандидатів (HP=" << ocr_hp << "%). Валідуємо...\n";
        } else {
            std::cerr << "[AutoCalib] 0 кандидатів (HP=" << ocr_hp
                      << "%). Чекаємо зміни HP або HP<95%...\n";
            m_state = State::Searching;
        }
        return std::nullopt;
    }

    // ── Фаза 2: Validating ───────────────────────────────────────────────────
    int delta = std::abs(ocr_hp - m_prev_ocr);
    if (delta < kDelta) return std::nullopt;

    ++m_attempts;
    std::vector<Candidate> survivors;

    for (auto& c : m_cands) {
        uint32_t new_cur = 0, new_max = 0;
        if (!ProcessMemory::Read(pid, c.cur_off, &new_cur, 4)) continue;
        if (!ProcessMemory::Read(pid, c.max_off, &new_max, 4)) continue;

        if (!matchPct(new_cur, new_max, ocr_hp, kTol)) continue;

        // Диференціальна перевірка: зміна в пам'яті ≈ зміна OCR
        if (c.prev_max > 0u && new_max > 0u) {
            int old_pct = (int)((uint64_t)c.prev_cur * 100u / c.prev_max);
            int new_pct = (int)((uint64_t)new_cur    * 100u / new_max);
            int d_mem   = std::abs(new_pct - old_pct);
            if (std::abs(d_mem - delta) > kTol + 2) continue;
        }

        c.hits++;
        c.prev_cur = new_cur;
        c.prev_max = new_max;
        survivors.push_back(c);
    }

    m_cands    = survivors;
    m_prev_ocr = ocr_hp;

    if (m_cands.empty()) {
        std::cerr << "[AutoCalib] Всі кандидати відхилені. Перезапуск.\n";
        m_state = State::Searching;
        return std::nullopt;
    }

    auto best = std::max_element(m_cands.begin(), m_cands.end(),
        [](const Candidate& a, const Candidate& b){ return a.hits < b.hits; });

    std::cerr << "[AutoCalib] Спроба " << m_attempts
              << "/" << kMaxTry << ": " << m_cands.size() << " кандидатів"
              << ", best hits=" << best->hits
              << " cur_abs=0x" << std::hex << best->cur_off
              << " max_abs=0x" << best->max_off << std::dec << "\n";

    if (best->hits >= kNeed || m_attempts >= kMaxTry) {
        m_state = State::Confirmed;
        std::cerr << "[AutoCalib] ПІДТВЕРДЖЕНО: cur_abs=0x"
                  << std::hex << best->cur_off
                  << " max_abs=0x" << best->max_off << std::dec
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
      << "  \"hp_abs\":"      << r.hp_abs      << ",\n"
      << "  \"max_hp_abs\":"  << r.max_hp_abs  << ",\n"
      << "  \"mp_off\":"      << r.mp_off      << ",\n"
      << "  \"max_mp_off\":"  << r.max_mp_off  << ",\n"
      << "  \"cp_off\":"      << r.cp_off      << ",\n"
      << "  \"max_cp_off\":"  << r.max_cp_off  << "\n"
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
    parseUint("hp_abs",        r.hp_abs);
    parseUint("max_hp_abs",    r.max_hp_abs);
    parseUint("hp_anchor_addr", r.hp_anchor_addr);
    parseUint("hp_anchor_sub",  r.hp_anchor_sub);
    parseUint("hp_off",        r.hp_off);
    parseUint("max_hp_off",    r.max_hp_off);
    parseUint("mp_off",        r.mp_off);
    parseUint("max_mp_off",    r.max_mp_off);
    parseUint("cp_off",        r.cp_off);
    parseUint("max_cp_off",    r.max_cp_off);

    // Пріоритет: abs > anchor > game_obj offset
    r.found_hp = (r.hp_abs > 0) || (r.hp_anchor_addr > 0)
                 || (r.hp_off > 0 && r.max_hp_off > 0);
    r.found_mp = (r.mp_off > 0 && r.max_mp_off > 0);
    r.found_cp = (r.cp_off > 0 && r.max_cp_off > 0);

    if (!r.found_hp && !r.found_mp) return false;

    if (r.hp_abs) {
        m_off.hp_abs         = r.hp_abs;
        m_off.max_hp_abs     = r.max_hp_abs;
    } else if (r.hp_anchor_addr) {
        m_off.hp_anchor_addr = r.hp_anchor_addr;
        m_off.hp_anchor_sub  = r.hp_anchor_sub;
        m_off.hp_off         = r.hp_off;
        m_off.max_hp_off     = r.max_hp_off;
    } else if (r.found_hp) {
        m_off.hp_off         = r.hp_off;
        m_off.max_hp_off     = r.max_hp_off;
    }
    if (r.found_mp) { m_off.mp_off = r.mp_off; m_off.max_mp_off = r.max_mp_off; }
    if (r.found_cp) { m_off.cp_off = r.cp_off; m_off.max_cp_off = r.max_cp_off; }

    std::cerr << "[MemCalib] Завантажено з " << path << "\n";
    return true;
}
