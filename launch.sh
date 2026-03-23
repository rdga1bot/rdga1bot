#!/usr/bin/env bash
# rdga1bot — ElmoreLab Farm Bot launcher
cd "$(dirname "$0")"

if [ ! -f rdga1bot ]; then
    echo "Бінарника не знайдено. Запускаємо build.sh..."
    ./build.sh
fi

# Якщо є збережений конфіг — пропускаємо TUI (--quick)
if [ -f rdga1bot.ini ] && [ "$1" != "--setup" ]; then
    exec ./rdga1bot --quick "$@"
fi

exec ./rdga1bot "$@"
