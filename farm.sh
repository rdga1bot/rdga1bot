#!/usr/bin/env bash
# farm.sh — тривалий фарм з повним логуванням
cd "$(dirname "$0")"

SESSION_TS=$(date +%Y%m%d_%H%M%S)
LOG_FILE="logs/session_${SESSION_TS}.log"
STATS_SUMMARY="logs/summary_${SESSION_TS}.txt"

echo "=== rdga1bot farm session ===" | tee "$LOG_FILE"
echo "Старт: $(date)" | tee -a "$LOG_FILE"
echo "Лог: $LOG_FILE" | tee -a "$LOG_FILE"
echo "" | tee -a "$LOG_FILE"

# Запускаємо бот у --no-tui режимі з tee для збереження всього лога
./rdga1bot --no-tui 2>&1 | tee -a "$LOG_FILE"

EXIT_CODE=$?
echo "" | tee -a "$LOG_FILE"
echo "=== Сесія завершена ===" | tee -a "$LOG_FILE"
echo "Кінець: $(date)" | tee -a "$LOG_FILE"

# Підсумок зі stats файлу
echo "" | tee -a "$LOG_FILE"
echo "=== Stats (останній запис) ===" | tee -a "$LOG_FILE"
STATS_FILE="logs/stats_$(date +%Y-%m-%d).json"
if [ -f "$STATS_FILE" ]; then
    tail -1 "$STATS_FILE" | python3 -c "
import json, sys
d = json.loads(sys.stdin.read())
kills = d.get('kills', 0)
deaths = d.get('deaths', 0)
uptime = d.get('uptime_sec', 0)
attacks = d.get('attacks', 0)
tf = d.get('targeting_failures', 0)
h = uptime // 3600
m = (uptime % 3600) // 60
s = uptime % 60
kpm = kills / (uptime / 60) if uptime > 0 else 0
kd = kills / max(deaths, 1)
print(f'Kills: {kills} | Deaths: {deaths} | K/D: {kd:.1f}')
print(f'Uptime: {h:02d}:{m:02d}:{s:02d} | Kill/хв: {kpm:.1f}')
print(f'Attacks: {attacks} | Targeting failures: {tf}')
" 2>/dev/null | tee -a "$LOG_FILE"
    # Зберігаємо summary окремо
    cp "$LOG_FILE" "$STATS_SUMMARY" 2>/dev/null
fi

echo ""
echo "Лог збережено: $LOG_FILE"
