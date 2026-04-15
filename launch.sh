#!/usr/bin/env bash
# rdga1bot — ElmoreLab Farm Bot launcher
cd "$(dirname "$0")"

if [ ! -f rdga1bot ]; then
    echo "Бінарника не знайдено. Запускаємо build.sh..."
    ./build.sh
fi

# Якщо є збережений конфіг — пропускаємо TUI (--quick)
# stderr → окремий лог для діагностики KnownList/scan без засмічення TUI
if [ -f rdga1bot.ini ] && [ "$1" != "--setup" ]; then
    exec ./rdga1bot --quick "$@" 2>> "logs/stderr_$(date +%Y%m%d).log"
fi

exec ./rdga1bot "$@" 2>> "logs/stderr_$(date +%Y%m%d).log"
