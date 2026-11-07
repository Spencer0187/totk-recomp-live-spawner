#!/usr/bin/env python3
"""X11-only J key watcher. Triggers four-part spawning only for focused TotK Recomp."""
from __future__ import annotations

import ctypes
import ctypes.util
import logging
import os
import subprocess
import sys
import time
from pathlib import Path

POLL_SECONDS = 0.04
DEBOUNCE_SECONDS = 1.5
EXPECTED_GAME = Path(os.environ.get(
    "TOTK_RECOMP_GAME_EXE", str(Path.home() / "Applications/TotK-Recomp/game/totk"))).resolve()
ROOT = Path(__file__).resolve().parent


class X11Key:
    def __init__(self, key_name: bytes = b"j") -> None:
        self.lib = ctypes.CDLL(ctypes.util.find_library("X11") or "libX11.so.6")
        lib = self.lib
        lib.XOpenDisplay.argtypes = [ctypes.c_char_p]
        lib.XOpenDisplay.restype = ctypes.c_void_p
        lib.XStringToKeysym.argtypes = [ctypes.c_char_p]
        lib.XStringToKeysym.restype = ctypes.c_ulong
        lib.XKeysymToKeycode.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
        lib.XKeysymToKeycode.restype = ctypes.c_uint
        lib.XQueryKeymap.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
        lib.XQueryKeymap.restype = ctypes.c_int
        lib.XCloseDisplay.argtypes = [ctypes.c_void_p]
        self.display = lib.XOpenDisplay(None)
        if not self.display:
            raise RuntimeError("X11 display unavailable; J shortcut requires an X11 session")
        self.keycode = lib.XKeysymToKeycode(self.display, lib.XStringToKeysym(key_name))
        if not 0 < self.keycode < 256:
            raise RuntimeError("Unable to resolve J keyboard scancode")
        self.state = ctypes.create_string_buffer(32)

    def pressed(self) -> bool:
        self.lib.XQueryKeymap(self.display, self.state)
        return bool(self.state.raw[self.keycode // 8] & (1 << (self.keycode % 8)))

    def close(self) -> None:
        if self.display:
            self.lib.XCloseDisplay(self.display)
            self.display = None


def game_is_focused() -> bool:
    """Window PID and executable must match this installation of Recomp."""
    try:
        active_id = subprocess.check_output(
            ("xdotool", "getactivewindow"), stderr=subprocess.DEVNULL,
            text=True, timeout=1).strip()
        pid = subprocess.check_output(
            ("xdotool", "getwindowpid", active_id), stderr=subprocess.DEVNULL,
            text=True, timeout=1).strip()
        return (Path(f"/proc/{int(pid)}/exe").resolve() == EXPECTED_GAME)
    except (OSError, ValueError, subprocess.CalledProcessError, subprocess.TimeoutExpired):
        return False


def watch() -> None:
    key = X11Key()
    logging.info("Monitoring J for focused %s", EXPECTED_GAME)
    prev = False
    last = 0.0
    try:
        while True:
            down = key.pressed()
            if down and not prev and time.monotonic() - last >= DEBOUNCE_SECONDS:
                if game_is_focused():
                    last = time.monotonic()
                    logging.info("J pressed: spawning four parts")
                    try:
                        result = subprocess.run(
                            (sys.executable, str(ROOT / "spawn.py"), "--four"),
                            text=True, capture_output=True, timeout=20,
                            check=False)
                        if result.stdout.strip():logging.info("%s", result.stdout.strip())
                        if result.returncode:
                            logging.warning("Spawn batch failed (exit %d): %s",
                                            result.returncode, result.stderr.strip())
                    except (OSError, subprocess.TimeoutExpired) as error:
                        logging.error("J batch error: %s", error)
            prev = down
            time.sleep(POLL_SECONDS)
    finally:
        key.close()


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO,
                        format="[TotK J hotkey] %(levelname)s: %(message)s")
    try:
        watch()
    except (KeyboardInterrupt, RuntimeError) as error:
        if not isinstance(error, KeyboardInterrupt):logging.error("%s", error)
        raise SystemExit(0 if isinstance(error, KeyboardInterrupt) else 1)
