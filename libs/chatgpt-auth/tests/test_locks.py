"""The refresh lock across processes, natively on every system (flock, or msvcrt on Windows), and the
Windows branches simulated where Windows is missing."""

import errno
import os
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path

import pytest

import chatgpt_auth
from chatgpt_auth import Credentials, TokenStore

# Holds the store's lock until stdin closes; says its own pid (on Windows a venv's python.exe is a shim
# that runs the interpreter as its child).
HOLDER = (
    "import os, sys; from pathlib import Path; from chatgpt_auth import TokenStore; "
    "lock = TokenStore(Path(sys.argv[1])).lock(); lock.acquire(); print(os.getpid(), flush=True); "
    "sys.stdin.read(); lock.release()"
)


def hold(store: Path) -> tuple[subprocess.Popen[str], int]:
    process = subprocess.Popen(
        [sys.executable, "-c", HOLDER, str(store)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True
    )
    assert process.stdout is not None
    return process, int(process.stdout.readline())


def acquire_in_thread(store: Path) -> tuple[threading.Thread, threading.Event]:
    acquired = threading.Event()

    def run() -> None:
        lock = TokenStore(store).lock()
        lock.acquire()
        acquired.set()
        lock.release()

    thread = threading.Thread(target=run, daemon=True)
    thread.start()
    return thread, acquired


def test_a_refresh_in_another_process_holds_this_one_until_it_releases(tmp_path):
    store = tmp_path / "chatgpt.json"
    other, _ = hold(store)
    thread, acquired = acquire_in_thread(store)
    try:
        assert not acquired.wait(0.5)  # waits, as flock does
    finally:
        assert other.stdin is not None
        other.stdin.close()
        other.wait(timeout=10)
    assert acquired.wait(10)
    thread.join(5)


def test_a_killed_holder_lets_go_of_the_lock(tmp_path):
    store = tmp_path / "chatgpt.json"
    other, pid = hold(store)
    thread, acquired = acquire_in_thread(store)
    assert not acquired.wait(0.3)
    os.kill(pid, signal.SIGTERM if sys.platform == "win32" else signal.SIGKILL)  # no release runs
    other.wait(timeout=10)
    assert acquired.wait(10)
    thread.join(5)


class FakeMsvcrt:
    """msvcrt.locking as Windows does it: byte ranges per open file, refused while another one holds them."""

    LK_UNLCK, LK_LOCK, LK_NBLCK = 0, 1, 2

    def __init__(self) -> None:
        self.held: dict[tuple[int, int], int] = {}
        self.calls: list[tuple[int, int]] = []
        self.guard = threading.Lock()

    def locking(self, fd: int, mode: int, nbytes: int) -> None:
        with self.guard:
            at = os.lseek(fd, 0, os.SEEK_CUR)
            key = (os.fstat(fd).st_ino, at)
            self.calls.append((mode, at))
            owner = self.held.get(key)
            if mode == self.LK_UNLCK:
                if owner != fd:
                    raise OSError(errno.EACCES, "not locked")
                del self.held[key]
            elif owner is not None and owner != fd:
                raise OSError(errno.EACCES, "locked")
            else:
                self.held[key] = fd


@pytest.fixture
def simulated_windows(monkeypatch):
    fake = FakeMsvcrt()
    monkeypatch.setitem(sys.modules, "msvcrt", fake)
    monkeypatch.setitem(sys.modules, "fcntl", None)  # the Unix branch would fail to import it
    monkeypatch.setattr(sys, "platform", "win32")
    monkeypatch.setattr(chatgpt_auth, "LOCK_POLL_S", 0.01)
    return fake


def test_windows_lock_waits_by_polling_and_unlocks_its_byte(tmp_path, simulated_windows):
    first = TokenStore(tmp_path / "chatgpt.json").lock()
    first.acquire()
    thread, acquired = acquire_in_thread(tmp_path / "chatgpt.json")
    assert not acquired.wait(0.2)
    first.release()
    assert acquired.wait(5)
    thread.join(5)
    assert (simulated_windows.LK_UNLCK, 0) in simulated_windows.calls
    assert all(mode != simulated_windows.LK_LOCK for mode, _ in simulated_windows.calls)  # LK_LOCK gives up


def test_windows_lock_raises_what_is_not_contention(tmp_path, simulated_windows, monkeypatch):
    def broken(fd, mode, nbytes):
        raise OSError(errno.EBADF, "bad file descriptor")

    monkeypatch.setattr(simulated_windows, "locking", broken)
    with pytest.raises(OSError, match="bad file descriptor"):
        TokenStore(tmp_path / "chatgpt.json").lock().acquire()


def test_windows_save_waits_for_a_reader_rather_than_lose_a_rotated_token(tmp_path, monkeypatch):
    real = os.replace
    refusals = [PermissionError("open by a reader"), PermissionError("open by a reader")]

    def replace(src, dst):
        if refusals:
            raise refusals.pop()
        real(src, dst)

    monkeypatch.setattr(chatgpt_auth.os, "replace", replace)
    monkeypatch.setattr(chatgpt_auth.time, "sleep", lambda s: None)
    store = TokenStore(tmp_path / "chatgpt.json")
    monkeypatch.setattr(sys, "platform", "win32")
    store.save(Credentials("app", "fresh", time.time() + 3600, "refresh-2"))
    assert store.load().refresh_token == "refresh-2"

    refusals.append(PermissionError("refused"))
    monkeypatch.setattr(sys, "platform", "darwin")  # elsewhere a refusal is final, as before
    with pytest.raises(PermissionError):
        store.save(Credentials("app", "fresh", time.time() + 3600, "refresh-3"))


SIMULATED_IMPORT = r"""
import asyncio, base64, errno, hashlib, json, logging, secrets, sys, time, types, webbrowser
import httpx
sys.modules['fcntl'] = None  # importing it fails from here on, as on Windows
if sys.platform != 'win32':
    sys.modules['msvcrt'] = types.ModuleType('msvcrt')
    sys.platform = 'win32'
import chatgpt_auth
print('ok')
"""


def test_the_library_imports_on_windows_without_fcntl():
    argv = [sys.executable, "-c", SIMULATED_IMPORT]
    out = subprocess.run(argv, capture_output=True, text=True, timeout=60, cwd=Path(__file__).parent)
    assert out.returncode == 0, out.stderr
    assert out.stdout.strip() == "ok"
