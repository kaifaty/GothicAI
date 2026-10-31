"""The Windows port of the core.

Tests marked `windows` run only on Windows, against the real kernel: a process tree ends with its job, also
when the launcher itself is killed; launchers in separate processes exclude each other. The others run
everywhere: some flip sys.platform and stand in msvcrt where Windows is missing, to check which branch each
call takes; the cross-process lock and the signal tests run natively on every system.
"""

import asyncio
import contextlib
import ctypes
import errno
import hashlib
import json
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

import pytest

from gothic_launcher import base, checks, cli, commands, state, winproc
from gothic_launcher import supervisor as sv
from gothic_launcher.commands import LaunchError, Layout, Ports, Proc, child_env, game, services
from gothic_launcher.settings import Config
from gothic_launcher.state import AlreadyRunning, SessionFile
from gothic_launcher.supervisor import Event, Supervisor
from tests.test_checks import _ready_base

windows = pytest.mark.skipif(sys.platform != "win32", reason="real Windows job objects and locks")
FAKE = str(Path(__file__).with_name("fake_service.py"))
SLEEPER = "import time; time.sleep(600)"
# a process that starts a child and says its pid, as `uv run` starts the service
PARENT = (
    "import subprocess, sys, time; "
    f"c = subprocess.Popen([sys.executable, '-c', {SLEEPER!r}]); "
    "print(c.pid, flush=True); time.sleep(600)"
)


class FakeMsvcrt:
    """msvcrt.locking as Windows does it: byte ranges per open file, refused while another one holds them."""

    LK_UNLCK, LK_LOCK, LK_NBLCK, LK_RLCK, LK_NBRLCK = 0, 1, 2, 3, 4

    def __init__(self) -> None:
        self.held: dict[tuple[int, int], int] = {}
        self.calls: list[tuple[int, int]] = []

    def locking(self, fd: int, mode: int, nbytes: int) -> None:
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
    return fake


def alive(pid: int) -> bool:
    if sys.platform != "win32":
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return False
        except PermissionError:
            return True
        return True
    # os.kill(pid, 0) would end the process on Windows: ask whether its handle is signalled instead
    k = ctypes.WinDLL("kernel32", use_last_error=True)  # type: ignore[attr-defined]
    k.OpenProcess.restype = ctypes.c_void_p
    k.OpenProcess.argtypes = (ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32)
    k.WaitForSingleObject.restype = ctypes.c_uint32
    k.WaitForSingleObject.argtypes = (ctypes.c_void_p, ctypes.c_uint32)
    k.CloseHandle.argtypes = (ctypes.c_void_p,)
    handle = k.OpenProcess(0x00100000 | 0x1000, False, pid)  # SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION
    if not handle:
        return False
    try:
        return k.WaitForSingleObject(handle, 0) == 0x102  # WAIT_TIMEOUT: still running
    finally:
        k.CloseHandle(handle)


def wait_gone(pid: int, timeout: float = 5) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if not alive(pid):
            return True
        time.sleep(0.05)
    return False


# --- every system ----------------------------------------------------------------------------------

SIMULATED_IMPORTS = r"""
import asyncio, contextlib, ctypes, errno, functools, getpass, json, logging, pathlib, shutil, signal
import socket, subprocess, sys, sysconfig, types, typing, zipfile
import httpx, pydantic, tomli_w
# The host's own state first: sysconfig names its data module after sys.platform, and pydantic imports
# parts of itself lazily, on the first model and schema. A model like the settings' warms both up.
sysconfig.get_config_vars()
class Warm(pydantic.BaseModel):
    model_config = pydantic.ConfigDict(extra='forbid')
    path: pathlib.Path | None = pydantic.Field(None, title='t', json_schema_extra={'secret': True})
    level: float | typing.Literal['none'] | None = None
Warm.model_json_schema()
Warm.model_validate({'path': '/x', 'level': 'none'}).model_dump(mode='json')
# From here on importing fcntl fails, as on Windows: the core must not need it there.
sys.modules['fcntl'] = None
if sys.platform != 'win32':
    fake = types.ModuleType('msvcrt')
    fake.LK_UNLCK, fake.LK_NBLCK = 0, 2
    fake.locking = lambda fd, mode, nbytes: None
    sys.modules['msvcrt'] = fake
    sys.platform = 'win32'
import gothic_launcher.cli, gothic_launcher.commands, gothic_launcher.checks, gothic_launcher.control
import gothic_launcher.settings, gothic_launcher.state, gothic_launcher.supervisor, gothic_launcher.winproc
assert gothic_launcher.control.LinuxSession is None and gothic_launcher.control.installation_lock is None
assert 'gothic_launcher.linux_session' not in sys.modules
print('ok')
"""


def test_the_core_imports_on_windows_without_unix_modules():
    argv = [sys.executable, "-c", SIMULATED_IMPORTS]
    out = subprocess.run(argv, capture_output=True, text=True, timeout=60)
    assert out.returncode == 0, out.stderr
    assert out.stdout.strip() == "ok"


def test_job_structures_have_the_documented_x64_sizes():
    if ctypes.sizeof(ctypes.c_void_p) != 8:
        pytest.skip("64-bit layouts")
    assert ctypes.sizeof(winproc.BasicLimitInformation) == 64
    assert ctypes.sizeof(winproc.IoCounters) == 48
    assert ctypes.sizeof(winproc.ExtendedLimitInformation) == 144
    assert ctypes.sizeof(winproc.BasicAccountingInformation) == 48
    assert ctypes.sizeof(winproc.ThreadEntry32) == 28


def kill(pid: int) -> None:
    """No cleanup code of the process runs (TerminateProcess on Windows)."""
    os.kill(pid, signal.SIGTERM if sys.platform == "win32" else signal.SIGKILL)


def holder(path: Path) -> tuple[subprocess.Popen[str], int]:
    """Another launcher: holds the session file until its stdin closes. Returns its own pid: on Windows a
    venv's python.exe is a shim that runs the interpreter as its child."""
    code = (
        "import os, sys; from pathlib import Path; from gothic_launcher.state import SessionFile; "
        "s = SessionFile(Path(sys.argv[1])); s.acquire(); print(os.getpid(), flush=True); sys.stdin.read(); "
        "s.release()"
    )
    process = subprocess.Popen(
        [sys.executable, "-c", code, str(path)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True
    )
    assert process.stdout is not None
    return process, int(process.stdout.readline())


def acquire_within(path: Path, timeout: float = 5) -> SessionFile:
    deadline = time.monotonic() + timeout
    while True:
        session = SessionFile(path)
        try:
            session.acquire()
            return session
        except AlreadyRunning:
            if time.monotonic() > deadline:
                raise
            time.sleep(0.05)


def test_a_launcher_in_another_process_excludes_this_one_and_says_who(tmp_path):
    path = tmp_path / "launcher.json"
    other, pid = holder(path)
    try:
        with pytest.raises(AlreadyRunning) as err:  # Windows: its pid stays readable beside the lock
            SessionFile(path).acquire()
        assert err.value.pid == pid
    finally:
        assert other.stdin is not None
        other.stdin.close()
        other.wait(timeout=10)
    acquire_within(path).release()


def test_a_killed_launcher_lets_go_of_the_session(tmp_path):
    path = tmp_path / "launcher.json"
    other, pid = holder(path)
    kill(pid)
    other.wait(timeout=10)
    acquire_within(path).release()


async def test_ctrl_c_reaches_the_loop():
    loop = asyncio.get_running_loop()
    got: list[int] = []
    done = asyncio.Event()

    def stop(sig: int) -> None:
        got.append(sig)
        done.set()

    restore = cli.on_stop_signals(loop, stop)
    try:
        signal.raise_signal(signal.SIGINT)
        await asyncio.wait_for(done.wait(), 5)
    finally:
        restore()
        if sys.platform != "win32":
            for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
                loop.remove_signal_handler(sig)
    assert got == [signal.SIGINT]


async def test_the_windows_signal_branch_hands_ctrl_c_to_the_loop_and_restores(monkeypatch):
    if sys.platform != "win32":  # no SIGBREAK here: a spare signal stands in
        monkeypatch.setattr(signal, "SIGBREAK", signal.SIGUSR1, raising=False)
    monkeypatch.setattr(sys, "platform", "win32")
    loop = asyncio.get_running_loop()
    before = signal.getsignal(signal.SIGINT)
    done = asyncio.Event()
    restore = cli.on_stop_signals(loop, lambda sig: done.set())
    try:
        assert signal.getsignal(signal.SIGINT) is not before
        signal.raise_signal(signal.SIGINT)
        await asyncio.wait_for(done.wait(), 5)
    finally:
        restore()
    assert signal.getsignal(signal.SIGINT) is before


def test_windows_session_lock_is_past_the_list_and_stops_no_groups(tmp_path, simulated_windows, monkeypatch):
    path = tmp_path / "launcher.json"
    path.write_text(json.dumps({"pid": 1, "written": time.time() - 60, "groups": {"tts": 4242}}))
    monkeypatch.setattr(state, "_stop_group_started_before", lambda *a: pytest.fail("no ps on Windows"))
    first = SessionFile(path)
    assert first.acquire() == {}  # its trees ended with their jobs
    first.add("server", 77)
    with pytest.raises(AlreadyRunning) as err:
        SessionFile(path).acquire()
    assert err.value.pid == os.getpid()
    first.release()
    assert path.read_text() == ""
    assert (simulated_windows.LK_NBLCK, state.WINDOWS_LOCK_AT) in simulated_windows.calls
    assert (simulated_windows.LK_UNLCK, state.WINDOWS_LOCK_AT) in simulated_windows.calls
    again = SessionFile(path)
    again.acquire()
    again.release()


def test_windows_paths(tmp_path, monkeypatch):
    monkeypatch.setattr(sys, "platform", "win32")
    monkeypatch.setenv("LOCALAPPDATA", str(tmp_path / "Local"))
    monkeypatch.delenv("STORYGRAPH_TOOL", raising=False)
    repo = tmp_path / "Gothic AI"
    assert commands.default_log_root() == tmp_path / "Local" / "GothicAI" / "Logs"
    assert commands.default_game_bin(repo) == repo / "OpenGothic" / "build" / "opengothic" / "Gothic2Notr.exe"
    tool = repo / "OpenGothic" / "ai_bridge" / "storygraph" / "build" / "storygraph.exe"
    assert commands.storygraph_tool(repo) == tool
    layout = Layout(repo, Path("uv.exe"), commands.default_game_bin(repo), tmp_path / "G2", tmp_path / "home")
    procs = {p.name: p for p in services(Config.load(tmp_path), layout, Ports(1, 2, 3), tmp_path)}
    assert procs["server"].env["STORYGRAPH_TOOL"] == str(tool)
    assert procs["tts"].env["STORYGRAPH_TOOL"] == str(tool)  # the original voices read OU.BIN through it
    assert "STORYGRAPH_TOOL" not in procs["asr"].env
    monkeypatch.setenv("STORYGRAPH_TOOL", "D:/own/storygraph.exe")
    assert commands.storygraph_env(layout) == {}  # the services take the explicit one as it is


def test_windows_preparation_runs_the_exe_converter_beside_the_game(tmp_path, monkeypatch):
    monkeypatch.setattr(sys, "platform", "win32")
    monkeypatch.delenv("OPENGOTHIC_BASE", raising=False)
    monkeypatch.delenv("OPENGOTHIC_BASE_FINGERPRINT", raising=False)
    build = tmp_path / "Мои игры" / "opengothic"
    build.mkdir(parents=True)
    game_bin = build / "Gothic2Notr.exe"
    folder = tmp_path / "Gothic II"
    (build / "content-convert").write_bytes(b"")  # the name of a Linux or macOS build
    with pytest.raises(LaunchError) as error:
        commands.preparation(game_bin, folder, tmp_path)
    assert str(build / "content-convert.exe") in str(error.value)
    assert "scripts/build-engine.ps1" in str(error.value)
    converter = build / "content-convert.exe"
    converter.write_bytes(b"")
    converter.chmod(0o755)  # a POSIX host running this test checks the mode
    proc = commands.preparation(game_bin, folder, tmp_path)
    assert proc is not None and proc.argv == [str(converter), "resolve", str(folder)]


def test_windows_launcher_hands_the_engines_base_cache_to_every_reader(tmp_path, monkeypatch, capsys):
    """The engine keeps its base pointer under %LOCALAPPDATA%, the Python readers default to ~/.cache: `check`
    finds the base the converter prepared there, and the children get the same folder."""
    game_dir, _, _ = _ready_base(tmp_path, monkeypatch)  # its cache and pointer, moved below
    monkeypatch.setattr(sys, "platform", "win32")
    local = tmp_path / "Пользователь" / "AppData" / "Local"
    cache = local / "gothic-ai" / "base"
    cache.parent.mkdir(parents=True)
    (tmp_path / "cache").rename(cache)
    monkeypatch.setenv("LOCALAPPDATA", str(local))
    monkeypatch.delenv("OPENGOTHIC_BASE_CACHE")
    monkeypatch.setattr(cli.sys, "argv", ["gothic-launcher", "check", str(game_dir)])
    with pytest.raises(SystemExit) as result:
        cli.main()
    assert result.value.code == 0
    assert [item["id"] for item in json.loads(capsys.readouterr().out)] == ["base", "russian"]
    assert child_env({})["OPENGOTHIC_BASE_CACHE"] == str(cache.resolve())
    monkeypatch.setenv("OPENGOTHIC_BASE_CACHE", "D:/own cache")
    assert commands.base_cache_env() == {}  # an explicit cache stays
    monkeypatch.delenv("OPENGOTHIC_BASE_CACHE")
    monkeypatch.setattr(sys, "platform", "darwin")
    assert commands.base_cache_env() == {}  # Linux and macOS keep the readers' own default


def test_windows_children_run_python_in_utf8_mode(monkeypatch):
    monkeypatch.delenv("PYTHONUTF8", raising=False)
    monkeypatch.setattr(sys, "platform", "darwin")
    assert "PYTHONUTF8" not in child_env({})
    monkeypatch.setattr(sys, "platform", "win32")
    assert child_env({})["PYTHONUTF8"] == "1"  # the services' gamedata JSON and pipes stay UTF-8
    monkeypatch.setenv("PYTHONUTF8", "0")
    assert child_env({})["PYTHONUTF8"] == "0"


def test_windows_finds_the_installed_uv(tmp_path, monkeypatch):
    monkeypatch.setattr(sys, "platform", "win32")
    for name in ("HOME", "USERPROFILE"):
        monkeypatch.setenv(name, str(tmp_path))
    monkeypatch.setenv("PATH", "")
    uv = tmp_path / ".local" / "bin" / "uv.exe"
    uv.parent.mkdir(parents=True)
    uv.write_bytes(b"")
    uv.chmod(0o755)
    assert commands.uv_path(Config.load(tmp_path)) == uv


def test_paths_with_spaces_and_cyrillic_stay_single_arguments(tmp_path):
    repo = tmp_path / "Мои игры" / "Gothic AI"
    folder = tmp_path / "Игры" / "Gothic II Gold"
    config = Config.load(tmp_path / "config")
    config.set("game.dir", str(folder))
    config.save()
    assert "Игры" in config.settings_path.read_bytes().decode("utf-8")
    again = Config.load(tmp_path / "config")
    assert again.settings.game.dir == folder
    layout = Layout(repo, tmp_path / "uv tool" / "uv.exe", repo / "Gothic2Notr.exe", folder, tmp_path / "дом")
    server = {p.name: p for p in services(again, layout, Ports(1, 2, 3), tmp_path)}["server"]
    assert server.argv[server.argv.index("--game-dir") + 1] == str(folder)
    assert server.argv[server.argv.index("--project") + 1] == str(repo / "server")
    assert game(again, layout, Ports(1, 2, 3)).argv[:3] == [str(repo / "Gothic2Notr.exe"), "-g", str(folder)]


async def test_windows_start_puts_the_process_in_its_job_before_it_runs(monkeypatch):
    calls: list[tuple] = []

    class FakeJob:
        def __init__(self) -> None:
            calls.append(("job",))

        def add(self, pid: int) -> None:
            calls.append(("add", pid))

        def close(self) -> None:
            calls.append(("close",))

    class FakeProcess:
        pid = 4242

        def kill(self) -> None:
            calls.append(("kill",))

    async def create(*argv, **kwargs):
        calls.append(("create", kwargs))
        return FakeProcess()

    monkeypatch.setattr(sys, "platform", "win32")
    monkeypatch.setattr(winproc, "Job", FakeJob)
    monkeypatch.setattr(winproc, "resume", lambda pid: calls.append(("resume", pid)))
    monkeypatch.setattr(asyncio, "create_subprocess_exec", create)
    process, job = await sv._start(["game.exe"], cwd=".")
    assert isinstance(job, FakeJob) and process.pid == 4242
    kinds = [c[0] for c in calls]
    assert kinds == ["job", "create", "add", "resume"]  # resumed only once inside its job
    kwargs = calls[1][1]
    assert kwargs["creationflags"] == winproc.CREATION_FLAGS and "start_new_session" not in kwargs

    calls.clear()
    monkeypatch.setattr(FakeJob, "add", lambda self, pid: (_ for _ in ()).throw(OSError("access denied")))
    with pytest.raises(OSError, match="access denied"):
        await sv._start(["game.exe"])
    assert [c[0] for c in calls] == ["job", "create", "close", "kill"]  # never left running outside


async def test_windows_stop_ends_the_job_without_unix_signals(monkeypatch):
    monkeypatch.setattr(sv, "_killpg", lambda *a: pytest.fail("no process groups on Windows"))
    ended: list[str] = []

    class FakeProcess:
        returncode: int | None = None

    process = FakeProcess()

    class FakeJob:
        def terminate(self) -> None:
            ended.append("terminate")
            process.returncode = 1

        def active(self) -> int:
            return 0

        def close(self) -> None:
            ended.append("close")

    await sv._end_job(process, FakeJob())  # type: ignore[arg-type]
    assert ended == ["terminate", "close"]


# --- setup: the remote-only profile ----------------------------------------------------------------


def gothic(folder: Path) -> Path:
    (folder / "Data").mkdir(parents=True)
    (folder / "Data" / "Speech_Addon.vdf").write_bytes(b"")
    (folder / "Data" / "Worlds.vdf").write_bytes(b"")
    cutscene = folder / "_work" / "Data" / "Scripts" / "Content" / "Cutscene"
    cutscene.mkdir(parents=True)
    (cutscene / "OU.BIN").write_bytes("Привет, Ксардас".encode("cp1251") * 20)
    return folder


@pytest.fixture
def setup_env(tmp_path, monkeypatch):
    config = Config.load(tmp_path / "config")
    repo = tmp_path / "repo"  # unbuilt, whatever the workspace holds (CI tests after its build)
    (repo / "server").mkdir(parents=True)
    (repo / "server" / "pyproject.toml").write_text("")
    config.set("paths.repo", str(repo))
    config.set("paths.uv", str(tmp_path / "uv.exe"))
    monkeypatch.delenv("STORYGRAPH_TOOL", raising=False)
    monkeypatch.setattr(cli, "_config", lambda: config)
    monkeypatch.setattr(cli, "CHATGPT_TOKENS", tmp_path / "chatgpt.json")
    monkeypatch.setattr(checks, "find_games", lambda: [])
    monkeypatch.setattr(cli, "login", lambda args: 1)  # no browser in tests

    async def no_network(config):
        return {"speech": {"providers": [{"title": "Inworld", "detail": "Доступен"}]}}

    monkeypatch.setattr(checks, "providers", no_network)
    return config


def setup_args(**kwargs):
    return type("Args", (), {"game_dir": None, "change_key": False, "check": False, **kwargs})()


def test_setup_sets_the_remote_profile_and_takes_the_key_hidden(setup_env, tmp_path, monkeypatch, capsys):
    config = setup_env
    config.set("voice.provider", "voxtral")  # one's own value: replaced, and said so
    config.set("llm.body_url", "tcp://gpu:47835")
    asked: list[str] = []
    monkeypatch.setattr(cli.getpass, "getpass", lambda prompt: asked.append(prompt) or "iw-test-key")
    folder = gothic(tmp_path / "Игры" / "Gothic II")

    code = cli.setup(setup_args(game_dir=str(folder)))

    s = config.settings
    assert (s.llm.backend, s.voice.provider, s.voice.fallback) == ("chatgpt", "chatgpt", "none")
    assert (s.tts.provider, s.tts.fallback, s.tts.original_voices) == ("inworld", "none", True)
    assert s.llm.quick is False and s.llm.decider_url is None and s.llm.body_url is None
    assert s.game.dir == folder and config.secrets.inworld_api_key == "iw-test-key" and len(asked) == 1
    out = capsys.readouterr().out
    assert "iw-test-key" not in out
    assert "Распознавание: было voxtral" in out and "Адрес запасного сервиса боя: было tcp://gpu:47835" in out
    assert code == 2 and "Нет входа в ChatGPT" in out and "Игра ещё не собрана" in out

    again = Config.load(tmp_path / "config")
    assert again.secrets.inworld_api_key == "iw-test-key" and again.settings.voice.provider == "chatgpt"
    monkeypatch.setattr(cli.getpass, "getpass", lambda prompt: pytest.fail("the key is set already"))
    cli.setup(setup_args(game_dir=str(folder)))


def test_setup_without_a_game_folder_asks_for_one(setup_env, tmp_path, capsys):
    assert cli.setup(setup_args()) == cli.NO_GAME_FOLDER
    assert "не найдена" in capsys.readouterr().out
    english = tmp_path / "Gothic II EN"
    (english / "Data").mkdir(parents=True)
    assert cli.setup(setup_args(game_dir=str(english))) == cli.NO_GAME_FOLDER
    assert "не подходит" in capsys.readouterr().out


def test_preflight_names_the_windows_setup(setup_env, monkeypatch, capsys):
    monkeypatch.setattr(sys, "platform", "win32")
    assert cli.setup(setup_args(check=True)) == 2
    out = capsys.readouterr().out
    assert "Папка игры не выбрана" in out and "Setup-GothicAI.cmd" in out


def test_preflight_wants_the_converter_beside_the_game(setup_env, tmp_path, monkeypatch, capsys):
    monkeypatch.setattr(sys, "platform", "win32")
    monkeypatch.delenv("OPENGOTHIC_BASE", raising=False)
    build = tmp_path / "repo" / "OpenGothic" / "build" / "opengothic"
    build.mkdir(parents=True)
    (build / "Gothic2Notr.exe").write_bytes(b"")
    cli.setup(setup_args(check=True))
    assert "Игра ещё не собрана" in capsys.readouterr().out  # `up` would stop before the game
    (build / "content-convert.exe").write_bytes(b"")
    cli.setup(setup_args(check=True))
    assert "Игра ещё не собрана" not in capsys.readouterr().out


# --- Windows itself --------------------------------------------------------------------------------


@windows
async def test_ending_the_job_ends_the_whole_tree():
    process, job = await sv._start([sys.executable, "-c", PARENT], stdout=asyncio.subprocess.PIPE)
    assert job is not None and process.stdout is not None
    child = int((await process.stdout.readline()).decode())
    assert alive(process.pid) and alive(child) and job.active() >= 2  # venv shims add their interpreters
    await sv._end_job(process, job)
    assert await asyncio.to_thread(wait_gone, process.pid)
    assert await asyncio.to_thread(wait_gone, child)


LAUNCHER = """
import asyncio, os, sys
from gothic_launcher import supervisor
async def main():
    process, job = await supervisor._start([sys.executable, '-c', PARENT], stdout=asyncio.subprocess.PIPE)
    child = int((await process.stdout.readline()).decode())
    print(os.getpid(), process.pid, child, flush=True)
    await asyncio.sleep(600)
asyncio.run(main())
""".replace("PARENT", repr(PARENT))


@windows
def test_a_killed_launcher_leaves_no_process_behind():
    launcher = subprocess.Popen([sys.executable, "-c", LAUNCHER], stdout=subprocess.PIPE, text=True)
    assert launcher.stdout is not None
    own, service, child = map(int, launcher.stdout.readline().split())
    try:
        assert alive(service) and alive(child)
    finally:
        kill(own)  # like the Task Manager: none of the launcher's cleanup code runs
        launcher.wait(timeout=10)
    assert wait_gone(service) and wait_gone(child)  # the job's last handle went with the launcher


def fake_service(name: str, port: int, tmp_path: Path, *extra: str) -> Proc:
    argv = [sys.executable, FAKE, "service", "--port", str(port), "--pidfile", str(tmp_path / f"{name}.pid")]
    return Proc(name, [*argv, *extra], tmp_path, health=f"http://127.0.0.1:{port}/health")


def pids(path: Path) -> list[int]:
    return [int(p) for p in path.read_text().split()]


@windows
async def test_the_supervisor_stops_and_restarts_whole_trees_on_windows(tmp_path, monkeypatch):
    monkeypatch.setattr(sv, "RESTART_DELAYS_S", (0.1,))
    monkeypatch.setattr(sv, "STOP_GRACE_S", 1.0)
    port = Ports.free().tts
    events: list[Event] = []
    ready = asyncio.Event()

    def record(e: Event) -> None:
        events.append(e)
        if e.kind == "ready":
            ready.set()

    sup = Supervisor(
        [fake_service("tts", port, tmp_path, "--child")],
        None,
        tmp_path / "logs",
        record,
        session=SessionFile(tmp_path / "launcher.json"),
    )
    run = asyncio.create_task(sup.run())
    await asyncio.wait_for(ready.wait(), 30)
    leader, child = pids(tmp_path / "tts.pid")
    ready.clear()
    os.kill(leader, signal.SIGTERM)  # TerminateProcess of `uv run` alone: its child would keep the port
    await asyncio.wait_for(ready.wait(), 30)
    assert await asyncio.to_thread(wait_gone, child)
    new_leader, new_child = pids(tmp_path / "tts.pid")
    assert new_leader != leader
    sup.stop()
    assert await asyncio.wait_for(run, 30) == 0
    for pid in (new_leader, new_child):
        assert await asyncio.to_thread(wait_gone, pid)
    assert (tmp_path / "launcher.json").read_text() == ""


# the converter: diagnostics on stderr, nothing for the launcher on stdout
PREPARING = (
    "import os, sys, time; print('stdout is not read', flush=True); "
    "print('base: importing installation', os.getpid(), file=sys.stderr, flush=True); time.sleep(600)"
)


@windows
async def test_windows_preparation_reports_stderr_and_stopping_ends_its_job(tmp_path):
    lines: list[str] = []
    reported = asyncio.Event()

    def record(e: Event) -> None:
        if e.proc == "prepare" and e.kind == "status":
            lines.append(e.detail)
            reported.set()

    prepare = Proc("prepare", [sys.executable, "-c", PREPARING], tmp_path, restart=False)
    session = SessionFile(tmp_path / "launcher.json")
    sup = Supervisor([], None, tmp_path / "logs", record, session=session, preparation=prepare)
    run = asyncio.create_task(sup.run())
    await asyncio.wait_for(reported.wait(), 30)
    shim = sup.running["prepare"].process.pid  # a venv's python.exe runs the interpreter as its child
    sup.stop()
    assert await asyncio.wait_for(run, 30) == 0
    [line] = lines
    assert line.startswith("base: importing installation")
    for pid in (shim, int(line.split()[-1])):
        assert await asyncio.to_thread(wait_gone, pid)


@windows
async def test_windows_preparation_failure_keeps_the_converters_reason(tmp_path):
    events: list[Event] = []
    reason = "import sys; print('no ready base for C:/Игры/Gothic II', file=sys.stderr); sys.exit(3)"
    prepare = Proc("prepare", [sys.executable, "-c", reason], tmp_path, restart=False)
    session = SessionFile(tmp_path / "launcher.json")
    sup = Supervisor([], None, tmp_path / "logs", events.append, session=session, preparation=prepare)
    assert await asyncio.wait_for(sup.run(), 30) == 3
    [failed] = [e for e in events if e.proc == "prepare" and e.kind == "failed"]
    assert "exited with code 3" in failed.detail and "no ready base for C:/Игры/Gothic II" in failed.detail


def _try_exclusive(path: Path) -> bool:
    """What the engine's cleanup takes before it removes a cached base (BaseStore Lock::TryExclusive)."""
    import msvcrt

    k = ctypes.WinDLL("kernel32", use_last_error=True)  # type: ignore[attr-defined]
    k.LockFileEx.restype = ctypes.c_int
    k.LockFileEx.argtypes = (ctypes.c_void_p, *(ctypes.c_uint32,) * 4, ctypes.c_void_p)
    with path.open("a+b") as file:
        overlapped = ctypes.create_string_buffer(32)  # OVERLAPPED, offset 0
        handle = msvcrt.get_osfhandle(file.fileno())
        # LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY; closing the file lets go of it
        return bool(k.LockFileEx(handle, 0x2 | 0x1, 0, 0xFFFFFFFF, 0xFFFFFFFF, ctypes.byref(overlapped)))


@windows
def test_a_base_read_here_holds_off_the_engines_cleanup(tmp_path):
    cache = (tmp_path / "cache").resolve()
    fingerprint = "a" * 64
    directory = cache / fingerprint
    directory.mkdir(parents=True)
    manifest = json.dumps({"format": "gothic-base-files", "version": 1, "files": []}).encode()
    (directory / "base-files.json").write_bytes(manifest)
    binding = {"path": "base-files.json", "size": len(manifest), "sha256": hashlib.sha256(manifest).hexdigest()}
    (directory / "base.json").write_text(json.dumps({
        "format": "gothic-base", "version": 2, "fingerprint": fingerprint, "manifest": binding,
    }))
    lease = cache / f"{fingerprint}.use"
    with base.Base(directory, cache, fingerprint):
        assert not _try_exclusive(lease)
        with base.Base(directory, cache, fingerprint):  # readers share it, as the game does
            assert not _try_exclusive(lease)
    assert _try_exclusive(lease)


@windows
def test_real_windows_lock_refuses_a_second_handle(tmp_path):
    import msvcrt

    first = SessionFile(tmp_path / "launcher.json")
    first.acquire()
    try:
        fd = os.open(tmp_path / "launcher.json", os.O_RDWR | os.O_BINARY)  # type: ignore[attr-defined]
        try:
            os.lseek(fd, state.WINDOWS_LOCK_AT, os.SEEK_SET)
            with pytest.raises(OSError) as err:
                msvcrt.locking(fd, msvcrt.LK_NBLCK, 1)
            assert err.value.errno in (errno.EACCES, errno.EDEADLOCK)
            os.lseek(fd, 0, os.SEEK_SET)
            assert json.loads(os.read(fd, 1 << 16))["pid"] == os.getpid()  # the list stays readable
        finally:
            os.close(fd)
    finally:
        first.release()
    with contextlib.suppress(FileNotFoundError):
        (tmp_path / "launcher.json").unlink()
