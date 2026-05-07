#!/usr/bin/env bash
# launch_qa.sh — запуск бота + QA демонів одною командою
#
# Використання:
#   ./launch_qa.sh           # бот + frame_capture + video_record
#   ./launch_qa.sh --no-video  # без відеозапису
#   ./launch_qa.sh --no-frame  # без скриншотів
#
# Зупинка: Ctrl+C — зупиняє всі процеси разом

set -euo pipefail
cd "$(dirname "$0")"

# ── Аргументи ─────────────────────────────────────────────────────────────────
RECORD_VIDEO=true
FRAME_CAPTURE=true
for arg in "$@"; do
    case "$arg" in
        --no-video) RECORD_VIDEO=false ;;
        --no-frame) FRAME_CAPTURE=false ;;
    esac
done

# ── Перевірки ─────────────────────────────────────────────────────────────────
if [ ! -f rdga1bot ]; then
    echo "[QA] Бінарника не знайдено — збираємо..."
    ./build.sh
fi

mkdir -p logs qa/frames qa/videos qa/videos/clips

PYTHON="${PYTHON:-python3}"

# ── PIDs дочірніх процесів ────────────────────────────────────────────────────
PIDS=()
_CLEANED=0

cleanup() {
    [ "$_CLEANED" -eq 1 ] && return
    _CLEANED=1
    echo ""
    echo "[QA] Зупинка всіх процесів..."
    for pid in "${PIDS[@]}"; do
        kill -INT "$pid" 2>/dev/null || true
    done
    # Даємо ffmpeg час на коректне завершення (flush + moov atom)
    sleep 3
    for pid in "${PIDS[@]}"; do
        kill -KILL "$pid" 2>/dev/null || true
    done
    wait 2>/dev/null || true
    echo "[QA] Готово."
}
trap cleanup EXIT
trap 'cleanup; exit 0' INT TERM HUP

# ── Запуск бота ───────────────────────────────────────────────────────────────
SESSION_LOG="logs/session_$(date +%Y%m%d_%H%M%S).log"
# stdout до старту ncurses → у лог, не в термінал (щоб не забруднювати TUI)
LAUNCH_LOG="logs/launch_$(date +%Y%m%d).log"
echo "[QA] Запуск бота → $SESSION_LOG" >> "$LAUNCH_LOG"
if [ -f rdga1bot.ini ]; then
    ./rdga1bot --quick 2>> "logs/stderr_$(date +%Y%m%d).log" | tee "$SESSION_LOG" &
else
    ./rdga1bot 2>> "logs/stderr_$(date +%Y%m%d).log" | tee "$SESSION_LOG" &
fi
BOT_PID=$!
PIDS+=("$BOT_PID")
echo "[QA] Бот PID=$BOT_PID" >> "$LAUNCH_LOG"

# Чекаємо поки session_*.log з'явиться і стане непорожнім
for i in $(seq 1 30); do
    [ -s "$SESSION_LOG" ] && break
    sleep 1
done

if [ ! -s "$SESSION_LOG" ]; then
    echo "[QA] ПОМИЛКА: лог порожній за 30с — бот не стартував?" >> "$LAUNCH_LOG"
    exit 1
fi
LOG="$SESSION_LOG"
echo "[QA] Лог: $LOG" >> "$LAUNCH_LOG"

# ── Frame capture ─────────────────────────────────────────────────────────────
if [ "$FRAME_CAPTURE" = true ]; then
    $PYTHON qa/frame_capture.py --log "$LOG" --max-per-min 6 \
        >> "logs/frame_capture_$(date +%Y%m%d).log" 2>&1 &
    FC_PID=$!
    PIDS+=("$FC_PID")
    echo "[QA] frame_capture PID=$FC_PID" >> "$LAUNCH_LOG"
fi

# ── Video record ──────────────────────────────────────────────────────────────
if [ "$RECORD_VIDEO" = true ]; then
    $PYTHON qa/video_record.py record --auto \
        >> "logs/video_record_$(date +%Y%m%d).log" 2>&1 &
    VR_PID=$!
    PIDS+=("$VR_PID")
    echo "[QA] video_record PID=$VR_PID" >> "$LAUNCH_LOG"
fi

echo "[QA] Запущено. PIDs → $LAUNCH_LOG" >> "$LAUNCH_LOG"

# Чекаємо поки бот завершиться (ScrollLock або crash)
wait "$BOT_PID" || true
