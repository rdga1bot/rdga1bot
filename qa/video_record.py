"""
video_record.py — QA відео-запис фарм-сесії через ffmpeg x11grab.

Два режими:

  record — безперервний запис екрана:
    python qa/video_record.py record
    python qa/video_record.py record --size 1920x1080 --offset 1366,0 --fps 10

  clip  — нарізка кліпів навколо подій із session_*.log:
    python qa/video_record.py clip --video qa/videos/farm_20260418_183000.mkv \\
                                   --log logs/session_20260418_183000.log
    python qa/video_record.py clip --video qa/videos/farm_*.mkv \\
                                   --log logs/session_*.log --before 20 --after 40

Структура виводу:
  qa/videos/farm_YYYYMMDD_HHMMSS.mkv     — повний запис сесії
  qa/videos/clips/HHMMSS_kill_#N.mkv     — кліп kill
  qa/videos/clips/HHMMSS_death.mkv       — кліп смерті
  qa/videos/clips/HHMMSS_stuck.mkv       — кліп WalkForward stuck
  qa/videos/clips/HHMMSS_unreachable.mkv — кліп unreachable
"""

import argparse
import glob
import os
import re
import signal
import subprocess
import sys
import time
import logging
from datetime import datetime, timedelta
from pathlib import Path

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s", datefmt="%H:%M:%S")
logger = logging.getLogger("video_record")

VIDEOS_DIR = os.path.join(os.path.dirname(__file__), "videos")
CLIPS_DIR  = os.path.join(VIDEOS_DIR, "clips")

# ── Патерни подій (той самий формат що frame_capture.py) ─────────────────────
_EVENTS = [
    (re.compile(r'\[(\d{2}:\d{2}:\d{2})\].*\[LOOTING\]\s+Вбивство\s+#(\d+)'),  "kill"),
    (re.compile(r'\[(\d{2}:\d{2}:\d{2})\].*\[DEAD\]\s+Фаза 0'),                "death"),
    (re.compile(r'\[(\d{2}:\d{2}:\d{2})\].*\[NAV\].*streak=(\d+).*WalkForward'),"stuck"),
    (re.compile(r'\[(\d{2}:\d{2}:\d{2})\].*\[ATTACKING\]\s+HP стабільний'),     "unreachable"),
]

_SESSION_DATE_RE = re.compile(r'session_(\d{4})(\d{2})(\d{2})_(\d{2})(\d{2})(\d{2})')


# ── Helpers ───────────────────────────────────────────────────────────────────

def _check_ffmpeg() -> bool:
    return subprocess.run(["which", "ffmpeg"], capture_output=True).returncode == 0


def _find_window(title: str) -> tuple[int, int, int, int] | None:
    """Знаходить вікно за заголовком через xdotool. Повертає (x, y, w, h) або None."""
    try:
        r = subprocess.run(
            ["xdotool", "search", "--name", title],
            capture_output=True, text=True, timeout=5,
        )
        if r.returncode != 0 or not r.stdout.strip():
            return None
        wid = r.stdout.strip().splitlines()[0]
        r2 = subprocess.run(
            ["xdotool", "getwindowgeometry", "--shell", wid],
            capture_output=True, text=True, timeout=5,
        )
        vals = {}
        for line in r2.stdout.splitlines():
            if "=" in line:
                k, v = line.split("=", 1)
                vals[k.strip()] = v.strip()
        x = int(vals.get("X", 0))
        y = int(vals.get("Y", 0))
        w = int(vals.get("WIDTH",  1366))
        h = int(vals.get("HEIGHT", 768))
        return x, y, w, h
    except Exception:
        return None


def _session_start_dt(log_path: str) -> datetime | None:
    m = _SESSION_DATE_RE.search(os.path.basename(log_path))
    if not m:
        return None
    return datetime(int(m[1]), int(m[2]), int(m[3]),
                    int(m[4]), int(m[5]), int(m[6]))


def _parse_events(log_path: str, session_dt: datetime) -> list[dict]:
    """Повертає список {'dt': datetime, 'label': str} відсортований за часом."""
    events = []
    with open(log_path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            for pattern, event_name in _EVENTS:
                m = pattern.search(line)
                if m:
                    ts_str = m.group(1)       # HH:MM:SS
                    extra  = m.group(2) if m.lastindex >= 2 else ""
                    h, mi, s = map(int, ts_str.split(":"))
                    dt = session_dt.replace(hour=h, minute=mi, second=s)
                    # Перехід через північ
                    if dt < session_dt:
                        dt += timedelta(days=1)
                    label = f"{event_name}_{extra}" if extra else event_name
                    events.append({"dt": dt, "label": label, "ts": ts_str})
                    break
    events.sort(key=lambda e: e["dt"])
    return events


def _video_start_dt(video_path: str) -> datetime | None:
    """Час старту відео — з назви farm_YYYYMMDD_HHMMSS.mkv або mtime."""
    m = re.search(r'farm_(\d{4})(\d{2})(\d{2})_(\d{2})(\d{2})(\d{2})', video_path)
    if m:
        return datetime(int(m[1]), int(m[2]), int(m[3]),
                        int(m[4]), int(m[5]), int(m[6]))
    # Fallback: mtime мінус тривалість — неточно, але краще нічого
    return None


# ── Режим record ──────────────────────────────────────────────────────────────

def cmd_record(args):
    if not _check_ffmpeg():
        logger.error("ffmpeg не знайдено")
        sys.exit(1)

    os.makedirs(VIDEOS_DIR, exist_ok=True)
    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    out = os.path.join(VIDEOS_DIR, f"farm_{ts}.mkv")

    display = args.display
    fps     = str(args.fps)

    # Авто-детект вікна L2 через xdotool
    size   = args.size
    offset = args.offset
    if args.auto or (not args.size and not args.offset):
        geo = _find_window(args.window_title)
        if geo:
            x, y, w, h = geo
            # libx264 потребує парних розмірів
            w = w - (w % 2)
            h = h - (h % 2)
            size   = f"{w}x{h}"
            offset = f"{x},{y}"
            logger.info(f"[REC] Авто-детект вікна '{args.window_title}': "
                        f"{size} offset={offset}")
        else:
            logger.warning(f"[REC] Вікно '{args.window_title}' не знайдено — "
                           f"використовую --size {size} --offset {offset}")

    input_url = f"{display}+{offset}" if offset else display

    cmd = [
        "ffmpeg", "-loglevel", "warning",
        "-f", "x11grab",
        "-r", fps,
        "-video_size", size,
        "-i", input_url,
        "-c:v", "libx264",
        "-crf", str(args.crf),
        "-preset", "ultrafast",
        "-pix_fmt", "yuv420p",
        out,
    ]

    logger.info(f"[REC] Запис → {out}")
    logger.info(f"[REC] {size} @ {fps}fps  display={input_url}  crf={args.crf}")
    logger.info("[REC] Ctrl+C або ScrollLock-сигнал для зупинки")

    proc = subprocess.Popen(cmd)

    def _stop(sig, frame):
        logger.info("[REC] Зупинка…")
        proc.send_signal(signal.SIGINT)
        proc.wait()
        logger.info(f"[REC] Збережено: {out}")
        sys.exit(0)

    signal.signal(signal.SIGINT,  _stop)
    signal.signal(signal.SIGTERM, _stop)
    proc.wait()
    logger.info(f"[REC] Готово: {out}")


# ── Режим clip ────────────────────────────────────────────────────────────────

def cmd_clip(args):
    if not _check_ffmpeg():
        logger.error("ffmpeg не знайдено")
        sys.exit(1)

    # Розкриваємо glob
    videos = sorted(glob.glob(args.video), key=os.path.getmtime) if args.video else []
    logs   = sorted(glob.glob(args.log),   key=os.path.getmtime) if args.log   else []

    if not videos:
        logger.error(f"Відео не знайдено: {args.video}")
        sys.exit(1)
    if not logs:
        logger.error(f"Лог не знайдено: {args.log}")
        sys.exit(1)

    video_path = videos[-1]
    log_path   = logs[-1]

    session_dt = _session_start_dt(log_path)
    if not session_dt:
        logger.error(f"Не вдалось визначити дату сесії з {log_path}")
        sys.exit(1)

    video_dt = _video_start_dt(video_path)
    if not video_dt:
        logger.error(f"Не вдалось визначити час старту відео з {video_path}")
        sys.exit(1)

    events = _parse_events(log_path, session_dt)
    if not events:
        logger.info("[CLIP] Подій не знайдено у логу")
        return

    logger.info(f"[CLIP] Відео: {video_path}")
    logger.info(f"[CLIP] Лог:   {log_path} ({len(events)} подій)")

    os.makedirs(CLIPS_DIR, exist_ok=True)
    before = args.before  # секунд до події
    after  = args.after   # секунд після події

    # Де-дублікуємо: пропускаємо події ближчі ніж (before+after)/2 до попередньої
    min_gap = (before + after) / 2
    filtered, last_dt = [], None
    for ev in events:
        if last_dt and (ev["dt"] - last_dt).total_seconds() < min_gap:
            continue
        filtered.append(ev)
        last_dt = ev["dt"]

    cut_count = 0
    for ev in filtered:
        offset_s = (ev["dt"] - video_dt).total_seconds() - before
        if offset_s < 0:
            offset_s = 0
        duration = before + after

        safe_label = re.sub(r'[^\w\-]', '_', ev["label"])
        out_name   = f"{ev['ts'].replace(':', '')}_{safe_label}.mkv"
        out_path   = os.path.join(CLIPS_DIR, out_name)

        cmd = [
            "ffmpeg", "-loglevel", "warning", "-y",
            "-ss", str(int(offset_s)),
            "-i", video_path,
            "-t", str(int(duration)),
            "-c", "copy",
            out_path,
        ]
        result = subprocess.run(cmd, capture_output=True)
        if result.returncode == 0:
            logger.info(f"[CLIP] {ev['ts']} {ev['label']} → {out_name}")
            cut_count += 1
        else:
            logger.warning(f"[CLIP] Помилка для {ev['ts']} {ev['label']}: "
                           f"{result.stderr.decode(errors='replace')[-80:]}")

    logger.info(f"[CLIP] Готово: {cut_count} кліпів у {CLIPS_DIR}")


# ── main ──────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="QA video record & clip")
    sub = parser.add_subparsers(dest="cmd")

    # record
    rec = sub.add_parser("record", help="Безперервний запис екрана")
    rec.add_argument("--display",      default=":0.0",     help="X11 display (default :0.0)")
    rec.add_argument("--size",         default="1366x768", help="Розмір захвату WxH (fallback)")
    rec.add_argument("--offset",       default="",         help="Зсув X,Y (fallback)")
    rec.add_argument("--fps",          type=int, default=10, help="Кадрів/сек (default 10)")
    rec.add_argument("--crf",          type=int, default=28, help="libx264 CRF (28=баланс)")
    rec.add_argument("--auto",         action="store_true", help="Авто-детект вікна L2 (xdotool)")
    rec.add_argument("--window-title", default="Lineage II", help="Заголовок вікна для авто-детекту")

    # clip
    cl = sub.add_parser("clip", help="Нарізка кліпів навколо подій з логу")
    cl.add_argument("--video",  default="qa/videos/farm_*.mkv", help="Відео (glob)")
    cl.add_argument("--log",    default="logs/session_*.log",   help="Лог (glob)")
    cl.add_argument("--before", type=int, default=20,  help="Секунд ДО події (default 20)")
    cl.add_argument("--after",  type=int, default=40,  help="Секунд ПІСЛЯ події (default 40)")

    args = parser.parse_args()
    if args.cmd == "record":
        cmd_record(args)
    elif args.cmd == "clip":
        cmd_clip(args)
    else:
        parser.print_help()


if __name__ == "__main__":
    main()
