#include "Stats.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <filesystem>

int Stats::UptimeSec() const {
    auto now = std::chrono::steady_clock::now();
    return (int)std::chrono::duration_cast<std::chrono::seconds>(now - session_start).count();
}

std::string Stats::UptimeStr() const {
    int total = UptimeSec();
    int h = total / 3600;
    int m = (total % 3600) / 60;
    int s = total % 60;
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h, m, s);
    return buf;
}

void Stats::PrintSummary() const {
    std::cout << "\n=== Підсумок сесії ===\n";
    std::cout << "Час гри:       " << UptimeStr() << "\n";
    std::cout << "Вбивства:      " << kills << "\n";
    std::cout << "Смерті:        " << deaths << "\n";
    std::cout << "Атаки:         " << attacks << "\n";
    std::cout << "HP потіони:    " << hp_potions << "\n";
    std::cout << "MP потіони:    " << mp_potions << "\n";
    std::cout << "Прогалини таргетингу: " << targeting_failures << "\n";
    std::cout << "======================\n";
}

void Stats::SaveToFile() const {
    // Створити logs/ якщо немає
    std::filesystem::create_directories("logs");

    // Ім'я файлу: logs/stats_YYYY-MM-DD.json
    time_t now_t = time(nullptr);
    struct tm* tm_info = localtime(&now_t);
    char date_buf[32];
    strftime(date_buf, sizeof(date_buf), "%Y-%m-%d", tm_info);
    std::string filename = std::string("logs/stats_") + date_buf + ".json";

    std::ofstream f(filename, std::ios::app);
    if (!f.is_open()) {
        std::cerr << "[Stats] Не вдалося відкрити " << filename << "\n";
        return;
    }

    char time_buf[64];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%dT%H:%M:%S", tm_info);

    f << "{"
      << "\"timestamp\":\"" << time_buf << "\","
      << "\"uptime_sec\":" << UptimeSec() << ","
      << "\"kills\":" << kills << ","
      << "\"deaths\":" << deaths << ","
      << "\"attacks\":" << attacks << ","
      << "\"hp_potions\":" << hp_potions << ","
      << "\"mp_potions\":" << mp_potions << ","
      << "\"targeting_failures\":" << targeting_failures
      << "}\n";

    std::cout << "[Stats] Збережено в " << filename << "\n";

    // Live stats для QA Monitor (/tmp/rdga1bot_stats.json)
    {
        std::ofstream live("/tmp/rdga1bot_stats.json");
        if (live) {
            live << "{"
                 << "\"ts\":"                  << now_t            << ","
                 << "\"kills\":"               << kills            << ","
                 << "\"deaths\":"              << deaths           << ","
                 << "\"attacks\":"             << attacks          << ","
                 << "\"hp_potions\":"          << hp_potions       << ","
                 << "\"mp_potions\":"          << mp_potions       << ","
                 << "\"targeting_failures\":"  << targeting_failures << ","
                 << "\"uptime_sec\":"          << UptimeSec()
                 << "}\n";
        }
    }
}
