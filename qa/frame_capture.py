"""
frame_capture.py — QA демон: tail session_*.log → scrot при ключових подіях.

Відокремлений від бота: бот тільки пише лог, цей процес читає лог і знімає
скриншоти через scrot. Не змінює бота, не додає залежностей до C++ коду.

Відстежувані події (з реальних log-тегів):
  [LOOTING] Вбивство #N   → kill
  [DEAD] Фаза 0            → death
  [NAV] streak=N → WalkForward → stuck
  [ATTACKING] HP стабільний → unreachable

Запуск:
  python qa/frame_capture.py --log logs/session_latest.log
  python qa/frame_capture.py --log logs/session_*.log --max-per-min 6
"""

import argparse
import glob
import os
import re
import sys
import time
import logging
from datetime import datetime
from pathlib import Path

sys.path.insert(0, os.path.dirname(__file__))
from screen_capture import ScreenCapture

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(message)s",
    datefmt="%H:%M:%S",
)
logger = logging.getLogger("frame_capture")

# ── Патерни подій ──────────────────────────────────────────────────────────────
_EVENTS = [
    (re.compile(r'\[LOOTING\]\s+Вбивство\s+#(\d+)'),  "kill"),
    (re.compile(r'\[DEAD\]\s+Фаза 0'),                 "death"),
    (re.compile(r'\[NAV\].*streak=(\d+).*WalkForward'), "stuck"),
    (re.compile(r'\[ATTACKING\]\s+HP стабільний'),      "unreachable"),
]


def _find_latest_log(log_dir: str) -> str | None:
    candidates = glob.glob(os.path.join(log_dir, "session_*.log"))
    return max(candidates, key=os.path.getmtime) if candidates else None


def tail_and_capture(log_path: str, capture: ScreenCapture, max_per_min: int):
    min_interval = 60.0 / max(1, max_per_min)
    last_snap = 0.0

    logger.info(f"[FC] Слідкую за {log_path}")
    with open(log_path, "r", encoding="utf-8", errors="replace") as f:
        f.seek(0, 2)  # починаємо з кінця файлу
        while True:
            line = f.readline()
            if not line:
                time.sleep(0.1)
                continue
            line = line.rstrip()
            for pattern, event_name in _EVENTS:
                m = pattern.search(line)
                if m:
                    now = time.monotonic()
                    if now - last_snap < min_interval:
                        break  # rate limit
                    last_snap = now
                    extra = m.group(1) if m.lastindex else ""
                    label = f"{event_name}{extra}" if extra else event_name
                    ts = datetime.now().strftime("%Y%m%d_%H%M%S_%f")[:-3]
                    fname = f"{ts}_{label}.png"
                    dest = os.path.join(capture.frames_dir, fname)
                    path = capture.snap()
                    if path and path != dest:
                        os.rename(path, dest)
                        path = dest
                    if path:
                        logger.info(f"[FC] {label} → {os.path.basename(path)}")
                    break


def main():
    parser = argparse.ArgumentParser(description="QA frame capture daemon")
    parser.add_argument("--log", default="logs", help="Лог файл або папка з session_*.log")
    parser.add_argument("--frames-dir", default="qa/frames", help="Папка для PNG кадрів")
    parser.add_argument("--max-per-min", type=int, default=6, help="Макс кадрів/хв")
    parser.add_argument("--max-frames", type=int, default=200, help="Макс кадрів у папці")
    args = parser.parse_args()

    capture = ScreenCapture(frames_dir=args.frames_dir, max_frames=args.max_frames)

    # Якщо передана папка — знаходимо найновіший лог, потім чекаємо новий
    log_path = args.log
    if os.path.isdir(log_path):
        found = _find_latest_log(log_path)
        if not found:
            logger.info(f"[FC] Чекаю на session_*.log у {log_path}…")
            while not found:
                time.sleep(2)
                found = _find_latest_log(log_path)
        log_path = found

    # Якщо glob pattern → розкрити
    if "*" in log_path or "?" in log_path:
        candidates = sorted(glob.glob(log_path), key=os.path.getmtime)
        if not candidates:
            logger.error(f"[FC] Не знайдено логів: {log_path}")
            sys.exit(1)
        log_path = candidates[-1]

    tail_and_capture(log_path, capture, args.max_per_min)


if __name__ == "__main__":
    main()
