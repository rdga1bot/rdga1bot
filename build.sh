#!/usr/bin/env bash
# Збірка rdga1bot — CMake + Ninja (incremental ~4с) з fallback на g++ (повна ~3хв)
set -e

DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$DIR/build"
OUT="$DIR/rdga1bot"

echo "=========================================="
echo "rdga1bot — ElmoreLab Farm Bot"
echo "=========================================="

# ── CMake + Ninja (incremental build) ────────────────────────────────────────
if command -v ninja &>/dev/null && command -v cmake &>/dev/null; then
    if [ ! -f "$BUILD_DIR/build.ninja" ]; then
        echo "[BUILD] Конфігурація CMake (перший раз)..."
        cmake -GNinja -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -S "$DIR" 2>&1 | grep -v "^--"
    fi

    echo "[1/2] Компіляція (ninja incremental)..."
    ninja -C "$BUILD_DIR"

    # Копіюємо бінарник у корінь проекту
    cp "$BUILD_DIR/rdga1bot" "$OUT"
    echo "[2/2] Готово!"

# ── Fallback: g++ повна компіляція ───────────────────────────────────────────
else
    echo "[BUILD] ninja не знайдено — повна компіляція через g++"

    SRC="$DIR/src"

    if pkg-config --exists opencv4 2>/dev/null; then OPENCV_PKG="opencv4"
    elif pkg-config --exists opencv 2>/dev/null; then OPENCV_PKG="opencv"
    else echo "ПОМИЛКА: OpenCV не знайдено"; exit 1; fi

    NCURSES_FLAGS="$(pkg-config --cflags ncursesw 2>/dev/null || echo '')"
    NCURSES_LIBS="$(pkg-config --libs ncursesw 2>/dev/null || echo '-lncursesw')"
    HARDENING="-fstack-protector-strong -D_FORTIFY_SOURCE=2 -fPIE"
    CFLAGS="-std=c++17 -O2 -I$SRC -I$DIR/third_party/eigen -DEIGEN_NO_DEBUG $(pkg-config --cflags x11 xtst xext $OPENCV_PKG) $NCURSES_FLAGS $HARDENING"
    LDFLAGS="$(pkg-config --libs x11 xtst xext $OPENCV_PKG) $NCURSES_LIBS -lpthread -pie -Wl,-z,relro,-z,now"

    SRCS="$SRC/main.cpp $SRC/Brain.cpp $SRC/Dashboard.cpp $SRC/Config.cpp
      $SRC/Stats.cpp $SRC/Notify.cpp $SRC/Eyes.cpp $SRC/Utils.cpp
      $SRC/Input.cpp $SRC/Window.cpp $SRC/Window_Linux.cpp
      $SRC/Capture.cpp $SRC/Capture_Linux.cpp
      $SRC/Intercept.cpp $SRC/Intercept_Linux.cpp
      $SRC/MemReader.cpp $SRC/offset_scanner.cpp $SRC/knownlist_reader.cpp
      $SRC/world_state.cpp $SRC/Geodata.cpp $SRC/vision_worker.cpp
      $SRC/geodata_worker.cpp $SRC/navmesh_builder.cpp $SRC/navmesh_worker.cpp
      $SRC/BehaviorTree.cpp $SRC/BotBehaviorTree.cpp
      $SRC/BotBT_Dead.cpp $SRC/BotBT_Buff.cpp $SRC/BotBT_Attack.cpp
      $SRC/BotBT_Target.cpp $SRC/BotBT_Nav.cpp $SRC/BotBT_RL.cpp
      $SRC/LinearQModel.cpp $SRC/LearningWorker.cpp
      $SRC/MemoryValidator.cpp $SRC/ShadowLogger.cpp
      $SRC/tools/diag_helpers.cpp $SRC/tools/diag_findpos.cpp
      $SRC/tools/diag_map.cpp $SRC/tools/diag_calibrate.cpp $SRC/tools/diag_dump.cpp"

    if [ -d "$SRC/recast/Detour" ]; then
        DETOUR_SRCS="$SRC/recast/Detour/Source/*.cpp"
        DETOUR_INC="-I$SRC/recast/Detour/Include -I$SRC/recast/Recast/Include"
        CFLAGS="$CFLAGS $DETOUR_INC -DHAVE_RECAST"
        echo "[BUILD] Recast/Detour знайдено → NavMesh увімкнено"
    else
        DETOUR_SRCS=""
    fi

    echo "[1/2] Компіляція..."
    g++ $CFLAGS $SRCS $DETOUR_SRCS -o "$OUT" $LDFLAGS
    echo "[2/2] Готово!"
fi

echo ""
echo "Виконуваний файл: $OUT"
echo ""
echo "Запуск:"
echo "  ./launch.sh               # звичайний запуск"
echo "  ./rdga1bot --quick        # без TUI (використати rdga1bot.ini)"
echo "=========================================="
