#!/bin/bash
# mempalace-setup.sh — ідемпотентна ініціалізація MemPalace екосистеми
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
MEMORY_DIR="$PROJECT_ROOT/memory"
PALACE_PATH="$MEMORY_DIR/palace"
VENV_MP="$MEMORY_DIR/venv/bin/mempalace"

echo "=== MemPalace Setup ==="

# Перевірка venv
if [ ! -f "$VENV_MP" ]; then
    echo "[MemPalace] ERROR: venv mempalace не знайдено: $VENV_MP"
    echo "  Запусти: python3.12 -m venv memory/venv && source memory/venv/bin/activate"
    echo "           pip install mempalace scikit-learn numpy pandas joblib watchdog"
    exit 1
fi

# Директорії
mkdir -p "$MEMORY_DIR"/{weights,stats_archive}
mkdir -p "$PROJECT_ROOT/qa"/{frames,models,reports}
touch "$PROJECT_ROOT/qa/anomalies.jsonl" 2>/dev/null || true

# Ініціалізація palace (ідемпотентна — якщо chroma.sqlite3 є, пропускаємо)
if [ ! -f "$PALACE_PATH/chroma.sqlite3" ]; then
    echo "[MemPalace] Ініціалізація palace..."
    "$VENV_MP" --palace "$PALACE_PATH" init "$PROJECT_ROOT"
else
    echo "[MemPalace] Palace вже ініціалізовано."
fi

# Sentinel
touch "$MEMORY_DIR/.mempalace_initialized"

echo "[MemPalace] Статус:"
"$VENV_MP" --palace "$PALACE_PATH" status 2>/dev/null | head -15 || true

echo ""
echo "[MemPalace] Готово."
echo "  Palace:  $PALACE_PATH"
echo "  Sentinel: $MEMORY_DIR/.mempalace_initialized"
echo "  Далі:    ./scripts/mempalace-mine.sh"
