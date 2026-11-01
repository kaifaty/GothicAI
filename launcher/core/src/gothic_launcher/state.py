"""One launcher session at a time, and nothing left behind by a launcher that was killed.

While a session runs, `launcher.json` in the state directory is locked (flock) and lists the process
groups the session started. The lock goes away with the launcher, even on SIGKILL. The next launcher then
finds the list of a session that never stopped its processes and stops them: only groups with a process
started before the list was last written, so a reused process id is never hit.

On Windows the lock is a byte-range lock (msvcrt.locking) far past the list, so a second launcher can still
read who holds it; it too goes away with the launcher. There is nothing to stop afterwards: every process
tree ran in a job object that Windows ended when the launcher went (winproc.py).
"""

from __future__ import annotations

import contextlib
import errno
import json
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

STATE_DIR = Path.home() / ".local" / "share" / "gothic-ai"
WINDOWS_LOCK_AT = 1 << 30  # Windows locks forbid reading the locked bytes: lock one well past the list


class AlreadyRunning(Exception):
    def __init__(self, pid: int | None) -> None:
        super().__init__(f"another launcher is running (pid {pid or '?'})")
        self.pid = pid


class SessionFile:
    def __init__(self, path: Path = STATE_DIR / "launcher.json") -> None:
        self.path = path
        self.groups: dict[str, int] = {}
        self._fd: int | None = None

    @property
    def held(self) -> bool:
        return self._fd is not None

    def acquire(self) -> dict[str, int]:
        """Locks the file; returns the groups of a killed launcher that it stopped. Raises AlreadyRunning."""
        self.path.parent.mkdir(parents=True, exist_ok=True)
        fd = os.open(self.path, os.O_RDWR | os.O_CREAT | getattr(os, "O_BINARY", 0), 0o644)
        try:
            locked = _try_lock(fd)
        except BaseException:
            os.close(fd)
            raise
        if not locked:
            pid = _read(fd).get("pid")
            os.close(fd)
            raise AlreadyRunning(pid if isinstance(pid, int) else None) from None
        self._fd = fd
        old = _read(fd)
        stopped = {
            name: pgid
            for name, pgid in (old.get("groups") or {}).items()
            if sys.platform != "win32" and _stop_group_started_before(pgid, float(old.get("written", 0)))
        }
        self._write()
        return stopped

    def add(self, name: str, pgid: int) -> None:
        self.groups[name] = pgid
        self._write()

    def remove(self, name: str) -> None:
        self.groups.pop(name, None)
        self._write()

    def release(self) -> None:
        """Every process stopped: an empty file, unlocked."""
        if self._fd is None:
            return
        self.groups.clear()
        os.ftruncate(self._fd, 0)
        _unlock(self._fd)
        os.close(self._fd)  # closing drops the lock
        self._fd = None

    def _write(self) -> None:
        if self._fd is None:
            return
        data = json.dumps({"pid": os.getpid(), "written": time.time(), "groups": self.groups}).encode()
        os.ftruncate(self._fd, 0)
        os.lseek(self._fd, 0, os.SEEK_SET)
        os.write(self._fd, data)


def _try_lock(fd: int) -> bool:
    """Exclusive and without waiting; held until the file is closed or the process ends."""
    if sys.platform == "win32":
        import msvcrt

        os.lseek(fd, WINDOWS_LOCK_AT, os.SEEK_SET)
        try:
            msvcrt.locking(fd, msvcrt.LK_NBLCK, 1)
        except OSError as err:
            if err.errno not in (errno.EACCES, getattr(errno, "EDEADLOCK", errno.EDEADLK)):
                raise
            return False
        return True
    import fcntl

    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        return False
    return True


def _unlock(fd: int) -> None:
    """Windows asks for an explicit unlock before closing; flock goes with the close."""
    if sys.platform == "win32":
        import msvcrt

        os.lseek(fd, WINDOWS_LOCK_AT, os.SEEK_SET)
        with contextlib.suppress(OSError):
            msvcrt.locking(fd, msvcrt.LK_UNLCK, 1)


def _read(fd: int) -> dict:
    os.lseek(fd, 0, os.SEEK_SET)
    raw = os.read(fd, 1 << 16)
    try:
        data = json.loads(raw) if raw.strip() else {}
    except ValueError:
        return {}
    return data if isinstance(data, dict) else {}


def _stop_group_started_before(pgid: int, written: float) -> bool:
    """SIGKILL to the group if one of its processes is older than the list (lstart has 1 s steps)."""
    try:
        out = subprocess.run(
            ["ps", "-axo", "pgid=,lstart="], capture_output=True, text=True, env={"LC_ALL": "C"}, check=True
        ).stdout
    except (OSError, subprocess.CalledProcessError):
        return False
    for line in out.splitlines():
        group, _, started = line.strip().partition(" ")
        if group != str(pgid):
            continue
        try:
            t = time.mktime(time.strptime(started.strip(), "%a %b %d %H:%M:%S %Y"))
        except ValueError:
            continue
        if t <= written + 1:
            with contextlib.suppress(ProcessLookupError, PermissionError):
                os.killpg(pgid, signal.SIGKILL)
            return True
    return False
