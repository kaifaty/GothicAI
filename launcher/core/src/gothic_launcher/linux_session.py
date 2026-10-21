"""The Ryzen desktop session: one gq cgroup, cancelled when its private input pipe closes.

The terminal entry point and the window both use this runner. gq owns admission, CPU priority,
wall-clock limit and cgroup cleanup, including when this process is killed without cleanup.
"""

from __future__ import annotations

import asyncio
import ctypes
import fcntl
import json
import logging
import os
from collections import deque
from collections.abc import Callable
from pathlib import Path
from typing import Any, BinaryIO

from .commands import LaunchError, Layout
from .state import SessionFile
from .supervisor import Event

log = logging.getLogger(__name__)
NVIDIA_ICD = Path("/usr/share/vulkan/icd.d/nvidia_icd.json")
LIVE_CLEAR_ENV = (
    "ALSOFT_DRIVERS",
    "TEMPEST_WINDOW_SIZE",
    "TEMPEST_WINDOW_POS",
    "TEMPEST_BACKGROUND",
    "OPENGOTHIC_MAX_FPS",
    "GOTHIC_AI_E2E",
)
PORTAUDIO_WARNING = (
    "Voice input disabled: libportaudio2 is missing. Install it yourself on Ryzen:\n"
    "  sudo apt install libportaudio2\nGame sound remains enabled."
)


def portaudio_available() -> bool:
    try:
        ctypes.CDLL("libportaudio.so.2")
        return True
    except OSError:
        return False


def installation_lock() -> BinaryIO:
    """Keep the play clone and its environments unchanged until all session processes stop."""
    path = Path.home() / "gothic-ai/play.install.lock"
    path.parent.mkdir(parents=True, exist_ok=True)
    lock = path.open("ab")
    try:
        fcntl.flock(lock, fcntl.LOCK_SH | fcntl.LOCK_NB)
    except BlockingIOError:
        lock.close()
        raise LaunchError("Gothic AI install/update is running; try again after it finishes") from None
    return lock


def live_environment() -> dict[str, str]:
    if not os.environ.get("DISPLAY"):
        raise LaunchError("Start Gothic AI from the Ryzen GNOME desktop (DISPLAY is missing)")
    if not NVIDIA_ICD.is_file():
        raise LaunchError(f"Missing NVIDIA Vulkan ICD: {NVIDIA_ICD}")
    env = dict(os.environ)
    env.update(VK_DRIVER_FILES=str(NVIDIA_ICD), VK_ICD_FILENAMES=str(NVIDIA_ICD), GQ_OWNER="ryzen-play")
    for key in LIVE_CLEAR_ENV:
        env.pop(key, None)
    return env


class LinuxSession:
    def __init__(
        self,
        layout: Layout,
        config_dir: Path,
        log_dir: Path,
        mode: str,
        smoke: bool,
        on_message: Callable[[dict[str, Any]], None],
        on_event: Callable[[Event], None],
        session: SessionFile,
    ) -> None:
        self.env = live_environment()
        self.lock = installation_lock()
        self.layout, self.config_dir, self.log_dir = layout, config_dir, log_dir
        self.mode, self.smoke = mode, smoke
        self.on_message, self.on_event, self.session = on_message, on_event, session
        self.microphone = portaudio_available()
        self.process: asyncio.subprocess.Process | None = None
        self.stopping = False
        self.admitted = False
        self.tail: deque[str] = deque(maxlen=20)

    def command(self) -> list[str]:
        worker = [
            str(self.layout.uv),
            "run",
            "--locked",
            "--project",
            str(self.layout.repo / "launcher/core"),
            "gothic-launcher",
            "_session",
            "--mode",
            self.mode,
            "--config-dir",
            str(self.config_dir),
            "--log-dir",
            str(self.log_dir),
        ]
        if self.smoke:
            worker.append("--smoke")
        if not self.microphone:
            worker.append("--no-voice")
        return [
            str(Path.home() / "gothic-ai/bin/gq"),
            "run",
            "--class",
            "quick",
            "--interactive",
            "--threads",
            "8",
            "--mem",
            "8G",
            "--gpu-mem",
            "3G",
            "--display-exclusive",
            "--max",
            "6h",
            "--name",
            "Gothic AI play",
            "--",
            *worker,
        ]

    def stop(self) -> None:
        self.stopping = True
        if self.process is not None and self.process.stdin is not None:
            self.process.stdin.close()  # EOF is gq run's cancellation contract, also on our SIGKILL.

    async def run(self) -> int:
        pumps: list[asyncio.Task[None]] = []
        try:
            if self.stopping:
                return 0
            if not self.microphone:
                self.on_event(Event("microphone", "status", PORTAUDIO_WARNING))
            self.process = await asyncio.create_subprocess_exec(
                *self.command(),
                cwd=self.layout.repo,
                env=self.env,
                stdin=asyncio.subprocess.PIPE,
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.PIPE,
                start_new_session=True,
                limit=1 << 22,
            )
            self.session.add("queue", self.process.pid)
            if self.stopping:
                self.stop()
            pumps = [asyncio.create_task(self._stdout()), asyncio.create_task(self._stderr())]
            await asyncio.gather(*pumps)
            code = await self.process.wait()
            if code and not self.stopping:
                self.on_event(Event("queue", "failed", "\n".join(self.tail) or f"gq exited with code {code}"))
            return 0 if self.stopping else code
        finally:
            self.stop()
            try:
                for task in pumps:
                    task.cancel()
                await asyncio.gather(*pumps, return_exceptions=True)
                if self.process is not None:
                    # A cancelled pump or a failed window callback must not leave gq blocked writing
                    # its final log. Keep consuming the pipes until the cancelled job has stopped.
                    await asyncio.gather(
                        self._discard(self.process.stdout),
                        self._discard(self.process.stderr),
                        self.process.wait(),
                    )
                    self.session.remove("queue")
            finally:
                self.lock.close()

    @staticmethod
    async def _discard(stream: asyncio.StreamReader | None) -> None:
        if stream is not None:
            while await stream.read(1 << 16):
                pass

    async def _stdout(self) -> None:
        assert self.process is not None and self.process.stdout is not None
        while line := await self.process.stdout.readline():
            try:
                msg = json.loads(line)
            except ValueError:
                text = line.decode(errors="replace").rstrip()
                self.tail.append(text)
                log.info("queue: %s", text)
                continue
            if not isinstance(msg, dict) or "event" not in msg:
                continue
            if not self.admitted:
                self.admitted = True
                self.on_event(Event("queue", "ready", "Допуск gq получен"))
            self.on_message(msg)

    async def _stderr(self) -> None:
        assert self.process is not None and self.process.stderr is not None
        while line := await self.process.stderr.readline():
            text = line.decode(errors="replace").rstrip()
            self.tail.append(text)
            log.info("queue: %s", text)
            if text.startswith("gq ") and not self.admitted:
                self.on_event(Event("queue", "waiting", text))
