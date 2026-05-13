// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <atomic>
#include <string>
#include <chrono>

// Telegram-сповіщення через system curl (без libcurl).
// Якщо token або chat_id порожні — всі методи no-op.
class Notify {
public:
    Notify() = default;
    Notify(const std::string& token, const std::string& chat_id,
           bool on_death = true, int stats_interval = 3600);
    ~Notify();

    void NotifyDeath(int hp = -1);
    void NotifyStats(const struct Stats& stats);
    void NotifyError(const std::string& msg);
    void CheckStatsInterval(const struct Stats& stats); // call each tick

    bool IsEnabled() const { return m_enabled; }

private:
    std::string m_token;
    std::string m_chat_id;
    bool m_enabled = false;
    bool m_on_death = true;
    int m_stats_interval = 3600;
    std::chrono::steady_clock::time_point m_last_stats;

    // Обмеження одночасних curl fork'ів — захист від накопичення зомбі-процесів
    // при повільному Telegram API або відсутності інтернету.
    static constexpr int kMaxPendingForks = 3;
    std::atomic<int> m_pending{0};

    void Send(const std::string& text); // async via std::thread
    void SendSync(const std::string& text); // calls curl
    std::string Timestamp() const;
};
