"""
screen_capture.py — scrot wrapper + базовий OpenCV аналіз кадру.
Зберігає знімки в qa/frames/frame_HHMMSS.png
"""

import os
import subprocess
import base64
import logging
from datetime import datetime
from pathlib import Path

logger = logging.getLogger("qa.screen")

FRAMES_DIR = os.path.join(os.path.dirname(__file__), "frames")
_SCROT_OK: bool | None = None
_CV2_OK: bool | None = None


def _check_scrot() -> bool:
    global _SCROT_OK
    if _SCROT_OK is None:
        _SCROT_OK = subprocess.run(
            ["which", "scrot"], capture_output=True
        ).returncode == 0
    return _SCROT_OK


def _check_cv2() -> bool:
    global _CV2_OK
    if _CV2_OK is None:
        try:
            import cv2  # noqa: F401
            _CV2_OK = True
        except ImportError:
            _CV2_OK = False
    return _CV2_OK


class ScreenCapture:
    """scrot + опціональний OpenCV аналіз."""

    def __init__(self, frames_dir: str = FRAMES_DIR, max_frames: int = 50):
        self.frames_dir = frames_dir
        self.max_frames = max_frames
        os.makedirs(frames_dir, exist_ok=True)

    def snap(self) -> str | None:
        """
        Робить scrot знімок, зберігає в frames/.
        Повертає шлях до файлу або None при помилці.
        """
        if not _check_scrot():
            logger.debug("scrot не знайдено — screen capture вимкнено")
            return None

        ts = datetime.now().strftime("%H%M%S")
        fname = f"frame_{ts}.png"
        path = os.path.join(self.frames_dir, fname)

        try:
            result = subprocess.run(
                ["scrot", path],
                capture_output=True,
                timeout=10,
            )
            if result.returncode != 0:
                return None

            self._prune_old_frames()
            logger.debug(f"[SC] Знімок: {path}")
            return path
        except (subprocess.TimeoutExpired, OSError) as e:
            logger.debug(f"[SC] scrot error: {e}")
            return None

    def analyze(self, path: str) -> dict:
        """
        Базовий OpenCV аналіз кадру.
        Повертає dict з метриками або порожній dict якщо cv2 недоступний.
        """
        if not _check_cv2() or not os.path.exists(path):
            return {}

        try:
            import cv2
            import numpy as np

            img = cv2.imread(path)
            if img is None:
                return {}

            h, w = img.shape[:2]
            gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
            mean_brightness = float(np.mean(gray))

            # Перевіряємо наявність UI елементів (верхній лівий кут — статус бари)
            status_roi = img[0:80, 0:180]
            status_brightness = float(np.mean(cv2.cvtColor(status_roi, cv2.COLOR_BGR2GRAY)))

            return {
                "width": w,
                "height": h,
                "mean_brightness": round(mean_brightness, 1),
                "status_area_brightness": round(status_brightness, 1),
                "dark_screen": mean_brightness < 20,  # можливо чорний екран / дисконект
            }
        except Exception as e:
            logger.debug(f"[SC] OpenCV analyze error: {e}")
            return {}

    def snap_and_analyze(self) -> dict:
        """Знімок + аналіз в один виклик."""
        path = self.snap()
        if path is None:
            return {"available": False}
        analysis = self.analyze(path)
        analysis["available"] = True
        analysis["path"] = path
        return analysis

    def get_recent_frames(self, n: int = 6) -> list:
        """
        Повертає список останніх N знімків як base64 PNG strings.
        """
        frames = sorted(Path(self.frames_dir).glob("frame_*.png"),
                        key=os.path.getmtime)
        recent = frames[-n:] if len(frames) >= n else frames

        result = []
        for f in reversed(recent):
            try:
                with open(f, "rb") as fh:
                    b64 = base64.b64encode(fh.read()).decode("ascii")
                result.append({
                    "filename": f.name,
                    "b64": b64,
                    "path": str(f),
                })
            except OSError:
                pass
        return result

    def _prune_old_frames(self):
        """Видаляє старі знімки якщо їх > max_frames."""
        frames = sorted(Path(self.frames_dir).glob("frame_*.png"),
                        key=os.path.getmtime)
        while len(frames) > self.max_frames:
            try:
                frames[0].unlink()
                frames.pop(0)
            except OSError:
                break
