#!/bin/bash
# qa-start.sh — запуск QA Monitor + MemPalace екосистеми
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
VENV_PY="$PROJECT_ROOT/memory/venv/bin/python"

echo "=== rdga1bot QA + MemPalace Ecosystem ==="
cd "$PROJECT_ROOT"

# 1. Setup (ідемпотентний)
"$SCRIPT_DIR/mempalace-setup.sh"

# 2. Знайти найновіший session log
LATEST_LOG="$(ls -t logs/session_*.log 2>/dev/null | head -1 || true)"
LOG_ARG=""
if [ -n "$LATEST_LOG" ]; then
    echo "[Launch] Лог: $LATEST_LOG"
    LOG_ARG="--log $LATEST_LOG"
fi

# 3. QA Monitor у фоні
echo "[Launch] Запуск QA Monitor (--with-mempalace)..."
# shellcheck disable=SC2086
"$VENV_PY" qa/qa_monitor.py \
    $LOG_ARG \
    --duration 6h \
    --with-mempalace \
    --no-screen &
QA_PID=$!

# 4. Periodic mining (кожні 10 хв)
(
    while true; do
        sleep 600
        "$SCRIPT_DIR/mempalace-mine.sh" 2>/dev/null || true
    done
) &
MINE_PID=$!

echo ""
echo "[Launch] Запущено:"
echo "  QA Monitor PID:  $QA_PID"
echo "  Mining daemon PID: $MINE_PID"
echo "  Dashboard: $PROJECT_ROOT/qa/dashboard.html"
echo "  Anomalies: $PROJECT_ROOT/qa/anomalies.jsonl"
echo ""
echo "  Відкрий: xdg-open qa/dashboard.html"
echo "  Зупинити: kill $QA_PID $MINE_PID"
echo ""

wait $QA_PID
kill $MINE_PID 2>/dev/null || true
