#pragma once
#include "MemReader.h"
#include "Eyes.h"
#include "l2_objects.h"
#include <fstream>
#include <mutex>
#include <chrono>
#include <string>
#include <atomic>

// ── ShadowLogger ─────────────────────────────────────────────────────────────
// A/B порівняння Memory Reader vs OCR у тіні.
// Записує розбіжності у JSONL без nlohmann (вручну формує рядки).
// ElmoreLab: порівнює OCR HP% з Memory hpAbs() через isAlive() флаг.
//
// Файли: memory/shadow_logs/comparison_YYYYMMDD_HHMMSS.jsonl
// Формат кожного рядка: {"type":"...","ts":...,...}
//
// Thread safety: logComparison() потокобезпечний (mutex).
// Brain тримає unique_ptr<ShadowLogger>.
class ShadowLogger {
public:
    ShadowLogger();
    ~ShadowLogger();

    // Порівняти Memory PlayerState з OCR Me (HP/MP/CP bars)
    void logPlayerComparison(const MemReader::PlayerState& mem,
                              const Eyes::Me&               ocr);

    // Порівняти Memory мобів з кількістю мобів за мінімапою/KL
    void logMobComparison(int mem_alive_count, int ocr_mob_count);

    // Записати помилку валідації Memory Reader
    void logValidationError(const std::string& error);

    // Записати подію: consecutive_failures досяг порогу
    void logFailureAlert(int count);

    // Примусовий flush буферу
    void flush();

    // Статистика (атомарні — без lock)
    uint64_t totalComparisons() const { return m_totalComparisons.load(); }
    uint64_t discrepancies()    const { return m_discrepancies.load(); }
    float    avgHpDiffPercent() const;

private:
    std::ofstream m_log;
    mutable std::mutex m_mutex;

    std::atomic<uint64_t> m_totalComparisons{0};
    std::atomic<uint64_t> m_discrepancies{0};

    // Накопичення HP diff (під mutex)
    float    m_sumHpDiff     = 0.f;
    uint64_t m_hpDiffSamples = 0;

    // Поріг значної розбіжності
    static constexpr float HP_DIFF_THRESHOLD = 0.05f; // 5% OCR HP
    static constexpr int   MOB_COUNT_DIFF_MAX = 2;

    bool openLogFile();

    // JSONL хелпери (без зовнішніх залежностей)
    static std::string jsonStr (const std::string& key, const std::string& val);
    static std::string jsonNum (const std::string& key, double val);
    static std::string jsonBool(const std::string& key, bool val);
    static std::string timestamp();

    void writeLineLocked(const std::string& json); // під m_mutex
};
