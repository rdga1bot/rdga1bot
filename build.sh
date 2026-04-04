#!/usr/bin/env bash
# Збірка rdga1bot для Linux (X11 + evdev, без cmake)
set -e

DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$DIR/src"
OUT="$DIR/rdga1bot"

echo "=========================================="
echo "rdga1bot — ElmoreLab Farm Bot"
echo "=========================================="

# Визначаємо pkg-config ім'я OpenCV
if pkg-config --exists opencv4 2>/dev/null; then
    OPENCV_PKG="opencv4"
elif pkg-config --exists opencv 2>/dev/null; then
    OPENCV_PKG="opencv"
else
    echo "ПОМИЛКА: OpenCV не знайдено. Встановіть: sudo pacman -S opencv"
    exit 1
fi

NCURSES_FLAGS="$(pkg-config --cflags ncursesw 2>/dev/null || echo '')"
NCURSES_LIBS="$(pkg-config --libs ncursesw 2>/dev/null || echo '-lncursesw')"

CFLAGS="-std=c++17 -O2 -I$SRC $(pkg-config --cflags x11 xtst xext $OPENCV_PKG) $NCURSES_FLAGS"
LDFLAGS="$(pkg-config --libs x11 xtst xext $OPENCV_PKG) $NCURSES_LIBS -lpthread"

SRCS="
  $SRC/main.cpp
  $SRC/Brain.cpp
  $SRC/Dashboard.cpp
  $SRC/Config.cpp
  $SRC/Stats.cpp
  $SRC/Notify.cpp
  $SRC/Eyes.cpp
  $SRC/Utils.cpp
  $SRC/Input.cpp
  $SRC/Window.cpp
  $SRC/Window_Linux.cpp
  $SRC/Capture.cpp
  $SRC/Capture_Linux.cpp
  $SRC/Intercept.cpp
  $SRC/Intercept_Linux.cpp
  $SRC/MemReader.cpp
  $SRC/offset_scanner.cpp
  $SRC/knownlist_reader.cpp
  $SRC/world_state.cpp
  $SRC/Geodata.cpp
  $SRC/vision_worker.cpp
  $SRC/geodata_worker.cpp
  $SRC/objective_manager.cpp
"

echo "[1/2] Компіляція..."
g++ $CFLAGS $SRCS -o "$OUT" $LDFLAGS

echo "[2/2] Готово!"
echo ""
echo "Виконуваний файл: $OUT"
echo ""
echo "Запуск:"
echo "  ./launch.sh               # звичайний запуск"
echo "  ./rdga1bot --quick        # без TUI (використати rdga1bot.ini)"
echo "  ./rdga1bot --config path  # вказати інший .ini"
echo "=========================================="
