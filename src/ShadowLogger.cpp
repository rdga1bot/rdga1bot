#include "ShadowLogger.h"
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <cmath>
#include <iostream>

ShadowLogger::ShadowLogger() {
    if (!openLogFile())
        std::cerr << "[ShadowLogger] Не вдалося відкрити лог файл\n";
}

ShadowLogger::~ShadowLogger() {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_log.is_open()) {
        std::ostringstream oss;
        oss << "{"
            << jsonStr("type","session_end") << ","
            << jsonNum("ts", std::stod(timestamp())) << ","
            << jsonNum("total_comparisons", (double)m_totalComparisons.load()) << ","
            << jsonNum("discrepancies", (double)m_discrepancies.load()) << ","
            << jsonNum("avg_hp_diff_pct", (double)avgHpDiffPercent())
            << "}";
        m_log << oss.str() << "\n";
        m_log.close();
    }
}

bool ShadowLogger::openLogFile() {
    namespace fs = std::filesystem;
    fs::create_directories("memory/shadow_logs");

    auto now   = std::chrono::system_clock::now();
    auto now_t = std::chrono::system_clock::to_time_t(now);
    struct tm* tm_info = std::localtime(&now_t);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", tm_info);

    std::string path = std::string("memory/shadow_logs/comparison_") + buf + ".jsonl";
    m_log.open(path, std::ios::app);
    if (!m_log.is_open()) return false;

    std::ostringstream oss;
    oss << "{"
        << jsonStr("type","session_start") << ","
        << jsonNum("ts", std::stod(timestamp()))
        << "}";
    m_log << oss.str() << "\n";
    m_log.flush();
    return true;
}

void ShadowLogger::logPlayerComparison(const MemReader::PlayerState& mem,
                                        const Eyes::Me&               ocr) {
    m_totalComparisons++;

    // OCR: HP як int 0-100 (відсотки)
    // Memory: якщо max_hp > 0 → відсоток; ElmoreLab (max_hp=0) → пропускаємо % diff
    float mem_hp_pct = -1.f;
    if (mem.valid && mem.max_hp > 0)
        mem_hp_pct = (float)mem.hp / (float)mem.max_hp * 100.f;

    float ocr_hp_pct = (float)ocr.hp; // вже у відсотках 0-100

    bool  significant = false;
    float hp_diff     = 0.f;

    if (mem_hp_pct >= 0.f && ocr_hp_pct >= 0.f) {
        hp_diff   = std::fabs(mem_hp_pct - ocr_hp_pct);
        significant = hp_diff > (HP_DIFF_THRESHOLD * 100.f); // >5 з шкали 0-100
    }

    float mem_mp_pct = (mem.valid && mem.max_mp > 0)
                       ? (float)mem.mp / (float)mem.max_mp * 100.f : -1.f;
    float ocr_mp_pct = (float)ocr.mp;
    float mp_diff = (mem_mp_pct >= 0.f && ocr_mp_pct >= 0.f)
                    ? std::fabs(mem_mp_pct - ocr_mp_pct) : 0.f;

    if (mp_diff > HP_DIFF_THRESHOLD * 100.f) significant = true;

    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (hp_diff > 0.f) {
            m_sumHpDiff += hp_diff;
            m_hpDiffSamples++;
        }
        if (!significant) return;

        m_discrepancies++;

        std::ostringstream oss;
        oss << "{"
            << jsonStr("type","player_discrepancy") << ","
            << jsonNum("ts", std::stod(timestamp())) << ","
            << "\"mem\":{"
              << jsonNum("hp_pct", mem_hp_pct) << ","
              << jsonNum("mp_pct", mem_mp_pct) << ","
              << jsonNum("x", mem.x) << ","
              << jsonNum("y", mem.y) << ","
              << jsonBool("valid", mem.valid)
            << "},"
            << "\"ocr\":{"
              << jsonNum("hp_pct", ocr_hp_pct) << ","
              << jsonNum("mp_pct", ocr_mp_pct)
            << "},"
            << "\"diff\":{"
              << jsonNum("hp", hp_diff) << ","
              << jsonNum("mp", mp_diff)
            << "}"
            << "}";
        writeLineLocked(oss.str());
    }
}

void ShadowLogger::logMobComparison(int mem_alive_count, int ocr_mob_count) {
    int diff = std::abs(mem_alive_count - ocr_mob_count);
    if (diff <= MOB_COUNT_DIFF_MAX) return;

    std::lock_guard<std::mutex> lk(m_mutex);
    m_discrepancies++;
    std::ostringstream oss;
    oss << "{"
        << jsonStr("type","mob_discrepancy") << ","
        << jsonNum("ts", std::stod(timestamp())) << ","
        << jsonNum("mem_alive", mem_alive_count) << ","
        << jsonNum("ocr_dots", ocr_mob_count) << ","
        << jsonNum("diff", diff)
        << "}";
    writeLineLocked(oss.str());
}

void ShadowLogger::logValidationError(const std::string& error) {
    std::lock_guard<std::mutex> lk(m_mutex);
    std::ostringstream oss;
    oss << "{"
        << jsonStr("type","validation_error") << ","
        << jsonNum("ts", std::stod(timestamp())) << ","
        << jsonStr("error", error)
        << "}";
    writeLineLocked(oss.str());
}

void ShadowLogger::logFailureAlert(int count) {
    std::lock_guard<std::mutex> lk(m_mutex);
    std::ostringstream oss;
    oss << "{"
        << jsonStr("type","failure_alert") << ","
        << jsonNum("ts", std::stod(timestamp())) << ","
        << jsonNum("consecutive_failures", count)
        << "}";
    writeLineLocked(oss.str());
}

void ShadowLogger::flush() {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_log.is_open()) m_log.flush();
}

float ShadowLogger::avgHpDiffPercent() const {
    if (m_hpDiffSamples == 0) return 0.f;
    return m_sumHpDiff / (float)m_hpDiffSamples;
}

// ── Private helpers ──────────────────────────────────────────────────────────

void ShadowLogger::writeLineLocked(const std::string& json) {
    if (m_log.is_open())
        m_log << json << "\n";
}

std::string ShadowLogger::timestamp() {
    auto now = std::chrono::system_clock::now();
    auto sec = std::chrono::duration_cast<std::chrono::seconds>(
               now.time_since_epoch()).count();
    return std::to_string(sec);
}

static std::string escapeJsonString(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        if      (c == '"')  out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else                out += c;
    }
    return out;
}

std::string ShadowLogger::jsonStr(const std::string& key, const std::string& val) {
    return "\"" + key + "\":\"" + escapeJsonString(val) + "\"";
}

std::string ShadowLogger::jsonNum(const std::string& key, double val) {
    if (!std::isfinite(val))
        return "\"" + key + "\":null";
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);
    oss << "\"" << key << "\":" << val;
    return oss.str();
}

std::string ShadowLogger::jsonBool(const std::string& key, bool val) {
    return "\"" + key + "\":" + (val ? "true" : "false");
}
