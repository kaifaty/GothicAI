"""Linux admission/lifetime boundaries with a fake queue, without windows or remote services."""

import asyncio
import contextlib
import ctypes
import fcntl
import json
import os
import signal
import sys
from argparse import Namespace
from pathlib import Path

import pytest

from gothic_launcher import cli
from gothic_launcher import control as control_module
from gothic_launcher import linux_session as linux
from gothic_launcher.commands import LaunchError, Layout
from gothic_launcher.linux_session import LinuxSession
from gothic_launcher.settings import Config
from gothic_launcher.state import SessionFile


@pytest.fixture
def desktop(tmp_path, monkeypatch):
    monkeypatch.setattr(Path, "home", classmethod(lambda cls: tmp_path))
    monkeypatch.setenv("DISPLAY", ":test")
    icd = tmp_path / "nvidia_icd.json"
    icd.touch()
    monkeypatch.setattr(linux, "NVIDIA_ICD", icd)
    monkeypatch.setattr(linux, "portaudio_available", lambda: True)
    repo = tmp_path / "repo"
    repo.mkdir()
    return Layout(repo, Path(sys.executable), Path("/game"), Path("/data"), tmp_path / "saves")


def test_live_environment_preserves_sound_and_clears_test_overrides(desktop, monkeypatch):
    for key in linux.LIVE_CLEAR_ENV:
        monkeypatch.setenv(key, "test-value")
    monkeypatch.setenv("PIPEWIRE_REMOTE", "pipewire-0")
    env = linux.live_environment()
    assert not set(linux.LIVE_CLEAR_ENV).intersection(env)
    assert env["VK_DRIVER_FILES"] == env["VK_ICD_FILENAMES"] == str(linux.NVIDIA_ICD)
    assert env["GQ_OWNER"] == "ryzen-play" and env["PIPEWIRE_REMOTE"] == "pipewire-0"
    assert os.environ["ALSOFT_DRIVERS"] == "test-value"  # the window's own environment is unchanged


def test_desktop_and_nvidia_are_required(desktop, monkeypatch):
    monkeypatch.delenv("DISPLAY")
    with pytest.raises(LaunchError, match="DISPLAY"):
        linux.live_environment()
    monkeypatch.setenv("DISPLAY", ":test")
    linux.NVIDIA_ICD.unlink()
    with pytest.raises(LaunchError, match="NVIDIA"):
        linux.live_environment()


def test_install_lock_blocks_updates_and_refuses_an_active_update(desktop):
    path = Path.home() / "gothic-ai/play.install.lock"
    with linux.installation_lock(), path.open("ab") as writer:
        with pytest.raises(BlockingIOError):
            fcntl.flock(writer, fcntl.LOCK_EX | fcntl.LOCK_NB)
    with path.open("ab") as writer:
        fcntl.flock(writer, fcntl.LOCK_EX | fcntl.LOCK_NB)
        with pytest.raises(LaunchError, match="install/update"):
            linux.installation_lock()


def test_terminal_play_waits_for_enter_after_a_busy_install_lock(desktop, tmp_path, monkeypatch, capsys):
    control = control_module.Control(
        lambda msg: None,
        config_dir=tmp_path / "config",
        state_file=tmp_path / "launcher.json",
        log_root=tmp_path / "logs",
    )
    monkeypatch.setattr(control, "_config", lambda: Config.load(tmp_path / "config"))
    monkeypatch.setattr(control_module, "Control", lambda write: control)
    monkeypatch.setattr(control_module, "find_layout", lambda config: desktop)
    monkeypatch.setattr(sys, "platform", "linux")
    monkeypatch.setattr(sys, "argv", ["gothic-launcher", "play"])
    monkeypatch.setattr(sys.stdin, "isatty", lambda: True)
    prompts = []
    monkeypatch.setattr("builtins.input", prompts.append)
    lock_path = Path.home() / "gothic-ai/play.install.lock"
    lock_path.parent.mkdir(parents=True)
    with lock_path.open("ab") as writer:
        fcntl.flock(writer, fcntl.LOCK_EX | fcntl.LOCK_NB)
        with pytest.raises(SystemExit) as result:
            cli.main()
    assert result.value.code == 2
    assert "Gothic AI install/update is running" in capsys.readouterr().err
    assert prompts == ["Press Enter to close this terminal."]
    assert control.task is None and control.supervisor is None


@pytest.mark.parametrize(("code", "tty"), [(7, True), (7, False), (0, True)])
def test_terminal_play_pauses_only_after_an_error_on_a_tty(code, tty, monkeypatch, capsys):
    async def finished_session(control, mode, smoke):
        return code

    monkeypatch.setattr(cli, "_controlled_session", finished_session)
    monkeypatch.setattr(sys, "argv", ["gothic-launcher", "play"])
    monkeypatch.setattr(sys.stdin, "isatty", lambda: tty)
    prompts = []
    monkeypatch.setattr("builtins.input", prompts.append)
    with pytest.raises(SystemExit) as result:
        cli.main()
    assert result.value.code == code
    assert prompts == (["Press Enter to close this terminal."] if code and tty else [])
    if code:
        assert "Gothic AI exited with code 7" in capsys.readouterr().err


def test_terminal_play_does_not_pause_on_keyboard_interrupt(monkeypatch):
    def interrupted(args):
        raise KeyboardInterrupt

    monkeypatch.setattr(cli, "play", interrupted)
    monkeypatch.setattr(sys, "argv", ["gothic-launcher", "play"])
    monkeypatch.setattr(sys.stdin, "isatty", lambda: True)
    prompts = []
    monkeypatch.setattr("builtins.input", prompts.append)
    with pytest.raises(SystemExit) as result:
        cli.main()
    assert result.value.code == 130 and not prompts


@pytest.mark.parametrize(("smoke", "mode"), [(False, "continue"), (True, "new")])
def test_terminal_play_selects_continue_unless_smoke(smoke, mode, monkeypatch):
    calls = []

    async def controlled_session(control, selected_mode, selected_smoke):
        calls.append((selected_mode, selected_smoke))
        return 0

    monkeypatch.setattr(cli, "_controlled_session", controlled_session)
    assert cli.play(Namespace(smoke=smoke)) == 0
    assert calls == [(mode, smoke)]


def test_portaudio_requires_a_loadable_library(monkeypatch):
    calls = []
    monkeypatch.setattr(ctypes, "CDLL", calls.append)
    assert linux.portaudio_available()
    assert calls == ["libportaudio.so.2"]

    def missing(name):
        raise OSError(name)

    monkeypatch.setattr(ctypes, "CDLL", missing)
    assert not linux.portaudio_available()


def test_play_request_retains_exclusive_live_limits(desktop, tmp_path, monkeypatch):
    monkeypatch.setattr(linux, "portaudio_available", lambda: False)
    state = SessionFile(tmp_path / "launcher.json")
    runner = LinuxSession(
        desktop, tmp_path, tmp_path / "logs", "new", True, lambda m: None, lambda e: None, state
    )
    try:
        cmd = runner.command()
        options, worker = cmd[: cmd.index("--")], cmd[cmd.index("--") + 1 :]
        assert "--interactive" in options and "--display-exclusive" in options
        for flag, value in (
            ("--class", "quick"),
            ("--threads", "8"),
            ("--mem", "8G"),
            ("--gpu-mem", "3G"),
            ("--max", "6h"),
        ):
            assert options[options.index(flag) + 1] == value
        assert worker[worker.index("--mode") + 1] == "new"
        assert "--smoke" in worker and "--no-voice" in worker and "_session" in worker
    finally:
        runner.lock.close()


async def test_missing_portaudio_prints_the_install_command(desktop, tmp_path, monkeypatch, capsys):
    monkeypatch.setattr(linux, "portaudio_available", lambda: False)
    state = SessionFile(tmp_path / "launcher.json")
    state.acquire()
    runner = LinuxSession(
        desktop, tmp_path, tmp_path / "logs", "continue", False, lambda msg: None, cli.print_event, state
    )
    monkeypatch.setattr(runner, "command", lambda: [sys.executable, "-c", "pass"])
    try:
        assert await asyncio.wait_for(runner.run(), 10) == 0
        assert "sudo apt install libportaudio2" in capsys.readouterr().out
    finally:
        runner.stop()
        state.release()


async def test_queue_reports_waiting_events_and_cancels_by_eof(desktop, tmp_path, monkeypatch):
    messages, events = [], []
    state = SessionFile(tmp_path / "launcher.json")
    state.acquire()
    runner = LinuxSession(
        desktop, tmp_path, tmp_path / "logs", "continue", True, messages.append, events.append, state
    )
    marker = tmp_path / "cancelled"
    ready = {"event": "service", "data": {"proc": "server", "kind": "ready", "data": {"game": True}}}
    script = (
        "import sys, pathlib; "
        "print('gq own: ждём игры: existing (another-owner, another-game)', file=sys.stderr, flush=True); "
        f"print({json.dumps(ready)!r}, flush=True); "
        f"sys.stdin.buffer.read(); pathlib.Path({str(marker)!r}).touch()"
    )
    monkeypatch.setattr(runner, "command", lambda: [sys.executable, "-c", script])
    task = asyncio.create_task(runner.run())
    try:
        async with asyncio.timeout(10):
            while not messages:
                await asyncio.sleep(0.01)
        assert messages == [ready]
        assert any(e.kind == "ready" and e.proc == "queue" for e in events)
        with (Path.home() / "gothic-ai/play.install.lock").open("ab") as lock:
            with pytest.raises(BlockingIOError):
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        runner.stop()
        assert await asyncio.wait_for(task, 10) == 0
        assert marker.exists() and not state.groups
        with (Path.home() / "gothic-ai/play.install.lock").open("ab") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    finally:
        runner.stop()
        await task
        state.release()


async def test_queue_waiting_detail_reaches_the_window(desktop, tmp_path, monkeypatch):
    messages = []
    waiting, running = asyncio.Event(), asyncio.Event()

    def write(msg):
        messages.append(json.loads(json.dumps(msg)))
        if msg["event"] == "service" and msg["data"]["proc"] == "queue" and msg["data"]["kind"] == "waiting":
            waiting.set()
        if msg["event"] == "session" and msg["data"]["state"] == "running":
            running.set()

    control = control_module.Control(
        write,
        config_dir=tmp_path / "config",
        state_file=tmp_path / "launcher.json",
        log_root=tmp_path / "logs",
    )
    monkeypatch.setattr(control, "_config", lambda: Config.load(tmp_path / "config"))
    monkeypatch.setattr(control_module, "find_layout", lambda config: desktop)
    monkeypatch.setattr(sys, "platform", "linux")
    detail = "gq own: ждём игры: other (another-owner, another-game)"
    worker_session = {
        "state": "running", "mode": "new", "code": None,
        "services": {"server": {"state": "ready"}},
        "health_url": "http://127.0.0.1:12345/health",
    }
    script = (
        "import sys; "
        f"print({detail!r}, file=sys.stderr, flush=True); "
        "sys.stdin.buffer.read(1); "  # Admit only after the window has received the waiting event.
        f"print({json.dumps({'event': 'session', 'data': worker_session})!r}, flush=True); "
        "sys.stdin.buffer.read()"
    )
    monkeypatch.setattr(LinuxSession, "command", lambda self: [sys.executable, "-c", script])
    try:
        await control.session_start(mode="new", smoke=True)
        assert isinstance(control.supervisor, LinuxSession)
        assert not control.admitted
        await asyncio.wait_for(waiting.wait(), 10)
        assert control.session["state"] == "waiting"
        assert control.session["services"]["queue"] == {"state": "waiting", "detail": detail}
        assert messages[-1] == {
            "event": "service",
            "data": {"proc": "queue", "kind": "waiting", "detail": detail, "data": None},
        }
        assert messages[0]["event"] == "session" and messages[0]["data"]["state"] == "waiting"
        runner = control.supervisor
        runner.process.stdin.write(b"\n")
        await runner.process.stdin.drain()
        await asyncio.wait_for(running.wait(), 10)
        assert control.session["state"] == "running"
        assert control.session["health_url"] == worker_session["health_url"]
        assert control.session["services"]["server"] == {"state": "ready"}
        assert control.session["services"]["queue"] == {"state": "ready", "detail": "Допуск gq получен"}
        assert messages[-1] == {"event": "session", "data": control.session}
        assert messages[0]["data"]["state"] == "waiting"  # Stored window messages are independent snapshots.
    finally:
        await asyncio.wait_for(control.shutdown(), 10)


async def test_parent_crash_closes_the_queue_lifetime_pipe(desktop, tmp_path):
    """An uncatchable launcher crash must close the gq pipe even without stop()/finally."""
    marker = tmp_path / "cancelled"
    queue = tmp_path / "queue.py"
    queue.write_text(
        "import sys, pathlib\n"
        f"pathlib.Path({str(tmp_path / 'queue.pid')!r}).write_text(str(__import__('os').getpid()))\n"
        "sys.stdin.buffer.read()\n"
        f"pathlib.Path({str(marker)!r}).touch()\n"
    )
    parent = tmp_path / "parent.py"
    parent.write_text(
        "import asyncio, pathlib, sys, os\n"
        "from gothic_launcher.linux_session import LinuxSession\n"
        "from gothic_launcher import linux_session as linux\n"
        "from gothic_launcher.commands import Layout\n"
        "from gothic_launcher.state import SessionFile\n"
        f"pathlib.Path.home = classmethod(lambda cls: pathlib.Path({str(tmp_path)!r}))\n"
        f"linux.NVIDIA_ICD = pathlib.Path({str(linux.NVIDIA_ICD)!r})\n"
        "linux.portaudio_available = lambda: True\n"
        f"root = pathlib.Path({str(tmp_path)!r})\n"
        "layout = Layout(root, pathlib.Path(sys.executable), root, root, root)\n"
        "state = SessionFile(root / 'child-state.json'); state.acquire()\n"
        "runner = LinuxSession(layout, root, root, 'menu', False, lambda m: None, lambda e: None, state)\n"
        f"runner.command = lambda: [sys.executable, {str(queue)!r}]\n"
        "asyncio.run(runner.run())\n"
    )
    process = await asyncio.create_subprocess_exec(sys.executable, str(parent))
    try:
        async with asyncio.timeout(10):
            while not (tmp_path / "queue.pid").exists():
                await asyncio.sleep(0.01)
        process.kill()
        await process.wait()
        async with asyncio.timeout(10):
            while not marker.exists():
                await asyncio.sleep(0.01)
    finally:
        if process.returncode is None:
            process.kill()
            await process.wait()
        if (tmp_path / "queue.pid").exists() and not marker.exists():
            with contextlib.suppress(ProcessLookupError):
                os.kill(int((tmp_path / "queue.pid").read_text()), signal.SIGKILL)


async def test_cancelled_forwarder_drains_final_queue_output(desktop, tmp_path, monkeypatch):
    """Even if forwarding fails, gq can print more than a pipe buffer while acknowledging EOF."""
    state = SessionFile(tmp_path / "launcher.json")
    state.acquire()
    marker = tmp_path / "drained"

    def broken_window(msg):
        raise BrokenPipeError("window exited")

    runner = LinuxSession(
        desktop, tmp_path, tmp_path / "logs", "menu", False, broken_window, lambda e: None, state
    )
    script = (
        "import sys, pathlib; "
        'print(\'{"event":"session","data":{}}\', flush=True); '
        "sys.stdin.buffer.read(); sys.stdout.write('x' * 1000000); sys.stdout.flush(); "
        f"pathlib.Path({str(marker)!r}).touch()"
    )
    monkeypatch.setattr(runner, "command", lambda: [sys.executable, "-c", script])
    try:
        with pytest.raises(BrokenPipeError, match="window exited"):
            await asyncio.wait_for(runner.run(), 10)
        assert marker.exists() and not state.groups
    finally:
        runner.stop()
        state.release()
