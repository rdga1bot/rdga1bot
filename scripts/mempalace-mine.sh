#!/bin/bash
# mempalace-mine.sh — індексація логів і аномалій у MemPalace
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
PALACE_PATH="$PROJECT_ROOT/memory/palace"
VENV_MP="$PROJECT_ROOT/memory/venv/bin/mempalace"
QA_DIR="$PROJECT_ROOT/qa"

if [ ! -f "$VENV_MP" ]; then
    echo "[MemPalace] ERROR: venv mempalace не знайдено" >&2
    exit 1
fi

echo "[MemPalace] Mining розпочато: $(date)"

# 1. Logs stats_*.json як сесії
if ls "$PROJECT_ROOT/logs/stats_"*.json 1>/dev/null 2>&1; then
    echo "[MemPalace] Mining logs/ → wing rdga1bot-sessions..."
    "$VENV_MP" --palace "$PALACE_PATH" mine "$PROJECT_ROOT/logs" \
        --wing rdga1bot-sessions --mode convos
fi

# 2. qa/anomalies.jsonl
if [ -s "$QA_DIR/anomalies.jsonl" ]; then
    echo "[MemPalace] Mining anomalies.jsonl → wing rdga1bot-anomalies..."
    "$VENV_MP" --palace "$PALACE_PATH" mine "$QA_DIR" \
        --wing rdga1bot-anomalies --mode convos
fi

# 3. Документація проекту (CLAUDE*.md)
echo "[MemPalace] Mining project docs → wing rdga1bot-docs..."
"$VENV_MP" --palace "$PALACE_PATH" mine "$PROJECT_ROOT" \
    --wing rdga1bot-docs

echo "[MemPalace] Mining завершено: $(date)"
"$VENV_MP" --palace "$PALACE_PATH" status 2>/dev/null | head -10 || true
