#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include "platform.h"

// ── HpAutoCalib ───────────────────────────────────────────────────────────────
// Диференційне авто-калібрування HP offset з пам'яті гравця.
//
// Алгоритм (3 фази):
//   Searching  — знаходить кандидатів (cur_off, max_off) де cur/max ≈ OCR HP%
//   Validating — відкидає кандидатів що не відстежують зміну OCR HP диференційно
//   Confirmed  — переможець знайдено
//
// Використання: викликати tick() кожен тік. Повертає HpOffsets рівно один раз.
struct HpAutoCalib {
    enum class State { Idle, Searching, Validating, Confirmed };

    struct HpOffsets { uintptr_t hp_off = 0, max_hp_off = 0; };

    std::optional<HpOffsets> tick(pid_t pid, uintptr_t playerBase, int ocr_hp);

    State state()      const { return m_state; }
    int   candidates() const { return (int)m_cands.size(); }
    void  reset()            { *this = {}; }

private:
    struct Candidate { uintptr_t cur_off = 0, max_off = 0; int hits = 0; };

    State                  m_state    = State::Idle;
    std::vector<Candidate> m_cands;
    std::vector<uint32_t>  m_snap;    // знімок пам'яті при попередньому delta-тіку
    int                    m_prev_ocr = -1;
    int                    m_attempts = 0;

    static bool matchPct(uint32_t cur, uint32_t mx, int pct, int tol);
};

// ── MemReader ────────────────────────────────────────────────────────────────
// Читає пам'ять процесу L2 (Wine) через process_vm_readv (Linux).
// Не потребує root — достатньо того самого UID що і L2 процес.
//
// Як знайти offsets для свого клієнта:
//   1. Запустити Cheat Engine під Wine:
//      WINEPREFIX=~/.wine wine cheatengine.exe
//   2. Приєднатись до процесу l2.exe
//   3. Шукати значення HP як 4-byte integer → "First Scan"
//   4. Отримати пошкодження → "Next Scan" (нове значення)
//   5. Знайти статичний pointer: Rights click → "Find out what writes to..."
//   6. Записати static addr + pointer offsets в [MemReader] секцію .ini
//
// Формат pointer chain в .ini:
//   PlayerPtr  = 0x019B4A28   ← статична адреса в l2.exe (base + offset)
//   PtrChain   = 0x10,0x44    ← chain offsets (порожньо якщо пряма адреса)
//   HP_Offset  = 0x5C         ← offset від кінця chain до HP int32
//
class MemReader {
public:
    struct PlayerState {
        int hp = -1, max_hp = -1;
        int mp = -1, max_mp = -1;
        int cp = -1, max_cp = -1;
        float x = 0, y = 0, z = 0; // world coordinates
        float heading = 0.f;        // кут повороту [рад] або raw int (залежить від клієнту)
        bool valid = false;
    };

    MemReader() = default;
    ~MemReader() { Close(); }

    // Знайти процес L2 і відкрити /proc/PID/mem
    bool Open(const std::string& proc_name = "l2.exe");
    void Close();
    bool IsOpen() const { return m_pid > 0; }
    pid_t GetPid() const { return m_pid; }
    uintptr_t GetBase() const { return m_base; }

    // Конфігурація offsets (з .ini)
    struct Offsets {
        bool      use_kl_base  = false; // true = приймати playerBase від KnownList напряму
        uintptr_t player_ptr   = 0;  // static addr (відносно base l2.exe)
        std::vector<uintptr_t> ptr_chain; // pointer chain offsets
        uintptr_t hp_off       = 0;
        uintptr_t max_hp_off   = 0;
        uintptr_t mp_off       = 0;
        uintptr_t max_mp_off   = 0;
        uintptr_t cp_off       = 0;
        uintptr_t max_cp_off   = 0;
        uintptr_t pos_x_off    = 0;  // float
        uintptr_t pos_y_off    = 0;
        uintptr_t pos_z_off    = 0;
        uintptr_t heading_off  = 0;  // offset heading від початку PlayerObject
        bool enabled = false;
    };
    void SetOffsets(const Offsets& off) { m_off = off; }
    const Offsets& GetOffsets() const { return m_off; }

    // Пряма передача playerBase від KnownList (режим UseKLBase).
    // Викликати щоразу коли KL знаходить/оновлює playerBase.
    void SetDirectBase(uintptr_t base) { m_direct_base = base; }

    // ── Авто-калібрування HP/MP/CP offsets ───────────────────────────────────
    // Сканує playerBase+0x00..scan_max, шукає пари (cur,max) де cur/max*100 ≈ pct.
    // hp/mp/cp_pct — поточні відсотки з OCR (0..100).
    // Повертає true якщо знайдено хоча б HP offset.
    struct AutoCalibResult {
        uintptr_t hp_off = 0,  max_hp_off = 0;
        uintptr_t mp_off = 0,  max_mp_off = 0;
        uintptr_t cp_off = 0,  max_cp_off = 0;
        bool found_hp = false, found_mp = false, found_cp = false;
    };
    static AutoCalibResult AutoCalibratePlayer(
        pid_t pid, uintptr_t playerBase,
        int hp_pct, int mp_pct, int cp_pct,
        uintptr_t scan_max = 0x300);

    // Зберегти/завантажити результат калібрування (mem_calib.json).
    void SaveCalib(const AutoCalibResult& r, const std::string& path = "mem_calib.json") const;
    bool LoadCalib(const std::string& path = "mem_calib.json");

    // Читаємо стан гравця (всі поля за один прохід)
    PlayerState ReadPlayer() const;

    // Низькорівневе читання довільного типу за абсолютною адресою
    template<typename T>
    std::optional<T> ReadAt(uintptr_t abs_addr) const {
        T val{};
        if (!ReadBytes(abs_addr, &val, sizeof(T))) return std::nullopt;
        return val;
    }

    // Читання за offset від base
    template<typename T>
    std::optional<T> ReadOffset(uintptr_t offset) const {
        return ReadAt<T>(m_base + offset);
    }

    // Слідуємо pointer chain: deref ptr → + off[0] → deref → + off[1] → ...
    // Повертає фінальну адресу або 0 при помилці
    uintptr_t ResolveChain(uintptr_t base_addr,
                           const std::vector<uintptr_t>& chain) const;

private:
    pid_t     m_pid  = 0;
    uintptr_t m_base = 0;   // base address l2.exe в адресному просторі процесу
    Offsets   m_off;

    uintptr_t m_direct_base = 0;  // playerBase від KnownList (режим UseKLBase)

    bool ReadBytes(uintptr_t abs_addr, void* buf, size_t len) const;
    static pid_t FindPid(const std::string& proc_name);
    static uintptr_t FindModuleBase(pid_t pid, const std::string& module);
};
