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

cleanup() {
    echo ""
    echo "[QA] Зупинка всіх процесів..."
    for pid in "${PIDS[@]}"; do
        kill "$pid" 2>/dev/null || true
    done
    wait 2>/dev/null || true
    echo "[QA] Готово."
}
trap cleanup INT TERM EXIT

# ── Запуск бота ───────────────────────────────────────────────────────────────
echo "[QA] Запуск бота..."
if [ -f rdga1bot.ini ]; then
    ./rdga1bot --quick 2>> "logs/stderr_$(date +%Y%m%d).log" &
else
    ./rdga1bot 2>> "logs/stderr_$(date +%Y%m%d).log" &
fi
BOT_PID=$!
PIDS+=("$BOT_PID")
echo "[QA] Бот PID=$BOT_PID"

# Чекаємо поки з'явиться session_*.log (бот стартував і почав писати лог)
echo "[QA] Чекаємо на session_*.log..."
for i in $(seq 1 30); do
    LOG=$(ls -t logs/session_*.log 2>/dev/null | head -1 || true)
    [ -n "$LOG" ] && break
    sleep 1
done

if [ -z "${LOG:-}" ]; then
    echo "[QA] ПОМИЛКА: лог не з'явився за 30с — бот не стартував?"
    exit 1
fi
echo "[QA] Лог: $LOG"

# ── Frame capture ─────────────────────────────────────────────────────────────
if [ "$FRAME_CAPTURE" = true ]; then
    $PYTHON qa/frame_capture.py --log "$LOG" --max-per-min 6 &
    FC_PID=$!
    PIDS+=("$FC_PID")
    echo "[QA] frame_capture PID=$FC_PID"
fi

# ── Video record ──────────────────────────────────────────────────────────────
if [ "$RECORD_VIDEO" = true ]; then
    $PYTHON qa/video_record.py record --auto &
    VR_PID=$!
    PIDS+=("$VR_PID")
    echo "[QA] video_record PID=$VR_PID"
fi

echo ""
echo "[QA] Всі процеси запущені. Ctrl+C для зупинки."
echo ""

# Чекаємо поки бот завершиться (ScrollLock або crash)
wait "$BOT_PID" || true
echo "[QA] Бот завершився."
