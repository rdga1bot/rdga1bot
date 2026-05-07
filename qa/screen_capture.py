"""
screen_capture.py — мінімальний wrapper навколо scrot для frame_capture.py
"""
import os
import subprocess
import time
from pathlib import Path


class ScreenCapture:
    def __init__(self, frames_dir: str = "qa/frames", max_frames: int = 500):
        self.frames_dir = frames_dir
        self.max_frames = max_frames
        Path(frames_dir).mkdir(parents=True, exist_ok=True)

    def snap(self) -> str | None:
        ts = time.strftime("%Y%m%d_%H%M%S")
        path = os.path.join(self.frames_dir, f"frame_{ts}.png")
        try:
            subprocess.run(["scrot", path], check=True,
                           capture_output=True, timeout=3)
            self._trim()
            return path
        except (subprocess.CalledProcessError, FileNotFoundError, subprocess.TimeoutExpired):
            return None

    def _trim(self):
        frames = sorted(Path(self.frames_dir).glob("*.png"))
        for old in frames[:-self.max_frames]:
            old.unlink(missing_ok=True)
