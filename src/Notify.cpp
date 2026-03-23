#include "Notify.h"
#include "Stats.h"

#include <iostream>
#include <thread>
#include <chrono>
#include <ctime>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

Notify::Notify(const std::string& token, const std::string& chat_id,
               bool on_death, int stats_interval)
    : m_token(token)
    , m_chat_id(chat_id)
    , m_on_death(on_death)
    , m_stats_interval(stats_interval)
    , m_last_stats(std::chrono::steady_clock::now())
{
    m_enabled = !token.empty() && !chat_id.empty();
    if (m_enabled) {
        std::cout << "[Notify] Telegram сповіщення увімкнено\n";
    }
}

std::string Notify::Timestamp() const {
    time_t now = time(nullptr);
    struct tm* tm_info = localtime(&now);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm_info);
    return buf;
}

void Notify::SendSync(const std::string& text) {
    if (!m_enabled) return;

    // Екранування спеціальних символів у JSON
    std::string safe_text;
    safe_text.reserve(text.size());
    for (char c : text) {
        switch (c) {
            case '"':  safe_text += "\\\""; break;
            case '\\': safe_text += "\\\\"; break;
            case '\n': safe_text += "\\n";  break;
            case '\r': safe_text += "\\r";  break;
            case '\t': safe_text += "\\t";  break;
            default:
                // Пропускаємо control characters (0x00-0x1F)
                if (static_cast<unsigned char>(c) < 0x20) continue;
                safe_text += c;
                break;
        }
    }

    std::string json = "{\"chat_id\":\"" + m_chat_id +
                       "\",\"text\":\"" + safe_text +
                       "\",\"parse_mode\":\"Markdown\"}";
    std::string url = "https://api.telegram.org/bot" + m_token + "/sendMessage";

    pid_t pid = fork();
    if (pid == 0) {
        // Дочірній процес: виконуємо curl
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        const char* args[] = {
            "/usr/bin/curl", "-s", "-X", "POST",
            "-H", "Content-Type: application/json",
            "-d", json.c_str(),
            url.c_str(),
            nullptr
        };
        execv("/usr/bin/curl", const_cast<char* const*>(args));
        _exit(1);
    } else if (pid > 0) {
        // Батьківський процес: чекаємо завершення
        waitpid(pid, nullptr, 0);
    }
}

void Notify::Send(const std::string& text) {
    if (!m_enabled) return;
    // Асинхронна відправка через окремий потік
    std::thread([this, text]() {
        SendSync(text);
    }).detach();
}

void Notify::NotifyDeath(int hp) {
    if (!m_enabled || !m_on_death) return;
    std::string msg = "☠️ *rdga1bot*: Персонаж загинув!\n";
    msg += "⏰ " + Timestamp();
    if (hp >= 0) {
        msg += "\nHP: " + std::to_string(hp) + "%";
    }
    Send(msg);
}

void Notify::NotifyStats(const struct Stats& stats) {
    if (!m_enabled) return;
    std::string msg = "📊 *rdga1bot* Статистика\n";
    msg += "⏰ " + Timestamp() + "\n";
    msg += "⚔️ Вбивства: " + std::to_string(stats.kills) + "\n";
    msg += "💀 Смерті: " + std::to_string(stats.deaths) + "\n";
    msg += "⏱ Час: " + stats.UptimeStr() + "\n";
    msg += "💊 HP поції: " + std::to_string(stats.hp_potions) + "\n";
    msg += "💧 MP поції: " + std::to_string(stats.mp_potions);
    Send(msg);
}

void Notify::NotifyError(const std::string& msg) {
    if (!m_enabled) return;
    std::string text = "⚠️ *rdga1bot* Помилка\n";
    text += "⏰ " + Timestamp() + "\n";
    text += msg;
    Send(text);
}

void Notify::CheckStatsInterval(const struct Stats& stats) {
    if (!m_enabled || m_stats_interval <= 0) return;
    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - m_last_stats).count();
    if (elapsed >= m_stats_interval) {
        m_last_stats = now;
        NotifyStats(stats);
    }
}
