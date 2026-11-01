import asyncio
import json
import os
import sys
import zipfile
from argparse import Namespace
from pathlib import Path

import pytest

from gothic_launcher import cli, commands
from gothic_launcher import control as control_module
from gothic_launcher import supervisor as sv
from gothic_launcher.commands import LaunchError, Ports, Proc
from gothic_launcher.control import Control
from gothic_launcher.state import AlreadyRunning, SessionFile
from tests.test_supervisor import FAKE, gone

# the window's sessions on POSIX process groups, and the Linux install lock; Windows: test_windows.py
fcntl = pytest.importorskip("fcntl", reason="POSIX process groups and the Linux install lock")

NPC_LINE = {"kind": "npc", "npc": "NONE_100_XARDAS", "name": "Ксардас", "text": "Придержи язык."}


class Window:
    """What the window receives: answers by id, events in order."""

    def __init__(self) -> None:
        self.messages: list[dict] = []
        self._changed = asyncio.Event()

    def __call__(self, msg: dict) -> None:
        json.dumps(msg)  # everything the core writes must be JSON
        self.messages.append(msg)
        self._changed.set()

    def events(self, name: str) -> list[dict]:
        return [m["data"] for m in self.messages if m.get("event") == name]

    async def wait(self, pred, timeout: float = 20) -> None:
        async with asyncio.timeout(timeout):
            while not pred():
                self._changed.clear()
                await self._changed.wait()


async def call(c: Control, method: str, **params):
    window: Window = c.write  # type: ignore[assignment]
    req = len(window.messages) + 1000
    await c.handle({"id": req, "method": method, "params": params})
    answer = next(m for m in window.messages if m.get("id") == req)
    if "error" in answer:
        raise RuntimeError(answer["error"])
    return answer["result"]


@pytest.fixture
def setup(tmp_path, monkeypatch):
    """A config with everything find_layout wants, and fake processes instead of the real services."""
    monkeypatch.setattr(sv, "STOP_GRACE_S", 1.0)
    monkeypatch.delenv("OPENGOTHIC_BASE", raising=False)
    monkeypatch.delenv("OPENGOTHIC_BASE_FINGERPRINT", raising=False)
    repo = tmp_path / "repo"
    (repo / "server").mkdir(parents=True)
    (repo / "server" / "pyproject.toml").write_text("")
    game_dir = tmp_path / "Gothic II"
    (game_dir / "Data").mkdir(parents=True)
    (game_dir / "Data" / "Worlds.vdf").write_bytes(b"")
    binary = tmp_path / "Gothic2Notr"
    binary.write_text("#!/bin/sh\n")
    binary.chmod(0o755)
    converter = binary.with_name("content-convert")
    converter.write_text("#!/bin/sh\nexit 0\n")
    converter.chmod(0o755)
    config_dir = tmp_path / "config"
    config_dir.mkdir()
    (config_dir / "settings.toml").write_text(  # present: no import from the real repository's .env
        f'[game]\ndir = "{game_dir}"\n[paths]\nrepo = "{repo}"\nuv = "/bin/sh"\n'
        f'game_bin = "{binary}"\ngame_home = "{tmp_path / "home"}"\n'
    )
    window = Window()
    c = Control(
        window,
        config_dir=config_dir,
        state_file=tmp_path / "launcher.json",
        log_root=tmp_path / "logs",
        admitted=True,  # exercise the local supervisor; queue admission has its own Linux tests
    )

    def fake_services(config, layout, ports: Ports, cwd: Path) -> list[Proc]:
        argv = [
            sys.executable,
            FAKE,
            "service",
            "--port",
            str(ports.server),
            "--journal",
            json.dumps(NPC_LINE),
        ]
        return [Proc("server", argv, cwd, health=f"http://127.0.0.1:{ports.server}/health")]

    def fake_game(config, layout, ports: Ports, save=None, new=False) -> Proc:
        argv = [sys.executable, FAKE, "game", "--run", "1.0"]
        return Proc("game", argv, layout.game_home, restart=False)

    monkeypatch.setattr(control_module, "services", fake_services)
    monkeypatch.setattr(control_module, "game", fake_game)
    return c, window, tmp_path


async def test_state_and_settings(setup):
    c, window, tmp_path = setup
    state = await call(c, "state")
    assert state["configured"] and state["problems"] == [] and state["session"] is None
    assert state["secrets"] == {"openai_api_key": False, "inworld_api_key": False}

    state = await call(c, "settings.set", key="secrets.inworld_api_key", value="iw-secret")
    assert state["secrets"]["inworld_api_key"] is True and "iw-secret" not in json.dumps(state)
    state = await call(c, "settings.set", key="secrets.inworld_api_key", value="")
    assert state["secrets"]["inworld_api_key"] is False
    state = await call(c, "settings.set", key="tts.enabled", value=False)
    assert state["settings"]["tts"]["enabled"] is False
    with pytest.raises(RuntimeError, match="llm.backend"):
        await call(c, "settings.set", key="llm.backend", value="nope")
    with pytest.raises(RuntimeError, match="unknown method"):
        await call(c, "nope")

    schema = await call(c, "schema")
    assert schema["secrets"]["properties"]["inworld_api_key"]["secret"] is True


async def test_game_folder_check(setup):
    c, window, tmp_path = setup
    items = {i["id"]: i for i in await call(c, "check.game")}
    assert items["archives"]["ok"] and not items["addon"]["ok"] and not items["russian"]["ok"]
    items = await call(c, "check.game", dir=str(tmp_path / "nowhere"))
    assert items[0]["id"] == "folder" and not items[0]["ok"]


async def test_session_events_and_journal_reach_the_window(setup):
    c, window, tmp_path = setup
    started = await call(c, "session.start", mode="continue")
    assert started["state"] == "running" and started["save"] is None  # no saves yet: the menu

    await window.wait(lambda: any(s["state"] == "stopped" for s in window.events("session")))

    assert window.events("journal") == [NPC_LINE]
    kinds = [(e["proc"], e["kind"]) for e in window.events("service")]
    assert ("server", "ready") in kinds and ("game", "exited") in kinds
    state = await call(c, "state")
    assert state["session"]["code"] == 0 and state["journal"] == [NPC_LINE]
    assert state["session"]["services"]["server"]["state"] == "stopped"


async def test_a_second_session_is_refused_while_one_runs(setup, monkeypatch):
    c, window, tmp_path = setup
    with monkeypatch.context() as broken:
        def invalid_layout(_config):
            raise LaunchError("invalid layout")

        broken.setattr(control_module, "find_layout", invalid_layout)
        held = SessionFile(c.state_file)
        held.acquire()
        try:
            with pytest.raises(AlreadyRunning):
                await c.session_start()
        finally:
            held.release()
        with pytest.raises(LaunchError, match="invalid layout"):
            await c.session_start()
        released = SessionFile(c.state_file)
        released.acquire()
        released.release()
    await call(c, "session.start")
    with pytest.raises(RuntimeError, match="уже запущена"):
        await call(c, "session.start")
    await call(c, "session.stop")
    assert window.events("session")[-1]["state"] == "stopped"


async def test_report_has_the_logs_without_keys(setup, monkeypatch):
    c, window, tmp_path = setup
    tokens = tmp_path / "chatgpt.json"
    tokens.write_text(json.dumps({"access_token": "access-secret", "refresh_token": "refresh-secret"}))
    monkeypatch.setattr(control_module.checks, "CHATGPT_TOKENS", tokens)
    await call(c, "settings.set", key="secrets.inworld_api_key", value="iw-secret")
    log_dir = tmp_path / "logs" / "2026-10-11_18-00-00"
    log_dir.mkdir(parents=True)
    (log_dir / "tts.log").write_text(
        "key iw-secret leaked\nAuthorization: Bearer abc.DEF-123_x\n"
        "authorization: bEaReR abc.DEF-123_x\naccess_token=access-secret\nrefresh_token=refresh-secret\n"
    )

    path = Path((await call(c, "report.create"))["path"])

    with zipfile.ZipFile(path) as z:
        text = z.read("2026-10-11_18-00-00/tts.log").decode()
        settings = z.read("settings.toml").decode()
        contents = "\n".join(z.read(name).decode() for name in z.namelist())
    for secret in ("iw-secret", "abc.DEF-123_x", "access-secret", "refresh-secret"):
        assert secret not in contents
    assert text.count("<hidden>") == 5 and "Bearer <hidden>" in text and "<set>" in settings


@pytest.mark.parametrize("tokens_json", [None, "{broken", "{}"])
async def test_report_without_chatgpt_tokens(setup, monkeypatch, tokens_json):
    c, window, tmp_path = setup
    tokens = tmp_path / "chatgpt.json"
    if tokens_json is not None:
        tokens.write_text(tokens_json)
    monkeypatch.setattr(control_module.checks, "CHATGPT_TOKENS", tokens)
    log_dir = tmp_path / "logs" / "2026-10-11_18-00-00"
    log_dir.mkdir(parents=True)
    (log_dir / "server.log").write_text("Authorization: Bearer abc.DEF-123_x\n")

    path = Path((await call(c, "report.create"))["path"])

    with zipfile.ZipFile(path) as z:
        assert z.read(f"{log_dir.name}/server.log").decode() == "Authorization: Bearer <hidden>\n"


@pytest.fixture
def checks_setup(setup, monkeypatch):
    """Services for the checks are fake ones; the helper commands print canned JSON lines."""
    c, window, tmp_path = setup

    def fake_services(config, layout, ports: Ports, cwd: Path) -> list[Proc]:
        procs = []
        for name, port in (("asr", ports.asr), ("tts", ports.tts)):
            pidfile = tmp_path / f"{name}.pid"
            argv = [sys.executable, FAKE, "service", "--port", str(port), "--pidfile", str(pidfile)]
            procs.append(Proc(name, argv, cwd, health=f"http://127.0.0.1:{port}/health"))
        return procs

    answers = {
        "voice-test": [
            '{"level": 0.4}',
            '{"partial": "Привет"}',
            '{"text": "Привет, Ксардас.", "provider": "voxtral"}',
        ],
        "speech-test": ['{"text": "Я ждал тебя.", "provider": "inworld", "first_chunk_s": 0.26}'],
    }

    def fake_argv(layout, command, *args):
        return [sys.executable, FAKE, "say", *answers[command]]

    monkeypatch.setattr(control_module, "services", fake_services)
    monkeypatch.setattr(Control, "_server_argv", staticmethod(fake_argv))
    return c, window, tmp_path


async def test_voice_check_streams_levels_and_stops_its_service(checks_setup):
    c, window, tmp_path = checks_setup
    result = await call(c, "test.voice", seconds=1)

    assert result == {"text": "Привет, Ксардас.", "provider": "voxtral"}
    assert window.events("voice_test") == [{"level": 0.4}, {"partial": "Привет"}]
    pid = int((tmp_path / "asr.pid").read_text().split()[0])
    assert await gone(pid)
    assert (tmp_path / "launcher.json").read_text() == ""  # the lock is free again


async def test_speech_check_answers_with_the_provider(checks_setup):
    c, window, tmp_path = checks_setup
    result = await call(c, "test.speech")
    assert result["provider"] == "inworld" and result["first_chunk_s"] == 0.26


async def test_checks_wait_for_the_game_to_end(checks_setup):
    c, window, tmp_path = checks_setup
    c.task = asyncio.get_running_loop().create_future()  # a session that runs
    with pytest.raises(RuntimeError, match="остановите игру"):
        await call(c, "test.voice")
    c.task.cancel()


async def test_checks_respect_switched_off_parts(checks_setup):
    c, window, tmp_path = checks_setup
    await call(c, "settings.set", key="voice.enabled", value=False)
    with pytest.raises(RuntimeError, match="выключен"):
        await call(c, "test.voice")


async def test_smoke_control_uses_fake_model_without_saving_overrides(setup, monkeypatch):
    c, window, tmp_path = setup
    saved = (c.config_dir / "settings.toml").read_text()
    captured = {}

    def smoke_services(config, layout, ports, cwd):
        captured.update(config.settings.model_dump())
        return []

    monkeypatch.setattr(control_module, "services", smoke_services)
    await call(c, "session.start", mode="new", smoke=True)
    await c.shutdown()
    assert captured["llm"]["backend"] == "fake"
    assert not captured["llm"]["quick"] and not captured["llm"]["body_url"]
    assert not captured["voice"]["enabled"] and not captured["tts"]["enabled"]
    assert (c.config_dir / "settings.toml").read_text() == saved


@pytest.mark.parametrize("fails", [False, True])
def test_idle_linux_core_holds_install_lock_until_serve_returns(tmp_path, monkeypatch, fails):
    monkeypatch.setattr(Path, "home", lambda: tmp_path)
    monkeypatch.setattr(sys, "platform", "linux")

    async def serve(install_lock=None):
        assert install_lock is not None and not install_lock.closed
        with (tmp_path / "gothic-ai/play.install.lock").open("ab") as updating:
            with pytest.raises(BlockingIOError):
                fcntl.flock(updating, fcntl.LOCK_EX | fcntl.LOCK_NB)
            await asyncio.sleep(0)
            with pytest.raises(BlockingIOError):
                fcntl.flock(updating, fcntl.LOCK_EX | fcntl.LOCK_NB)
        if fails:
            raise RuntimeError("serve failed")
        return 0

    monkeypatch.setattr(control_module, "serve", serve)
    if fails:
        with pytest.raises(RuntimeError, match="serve failed"):
            control_module.main()
    else:
        assert control_module.main() == 0
    with (tmp_path / "gothic-ai/play.install.lock").open("ab") as updating:
        fcntl.flock(updating, fcntl.LOCK_EX | fcntl.LOCK_NB)


def test_linux_core_refuses_to_start_while_installing(tmp_path, monkeypatch):
    monkeypatch.setattr(Path, "home", lambda: tmp_path)
    monkeypatch.setattr(sys, "platform", "linux")
    path = tmp_path / "gothic-ai/play.install.lock"
    path.parent.mkdir()

    async def serve(install_lock=None):
        pytest.fail("control must not start while install/update holds the lock")

    monkeypatch.setattr(control_module, "serve", serve)
    with path.open("ab") as updating:
        fcntl.flock(updating, fcntl.LOCK_EX | fcntl.LOCK_NB)
        with pytest.raises(control_module.LaunchError, match="install/update is running"):
            control_module.main()


def test_macos_core_does_not_take_linux_install_lock(monkeypatch):
    monkeypatch.setattr(sys, "platform", "darwin")
    monkeypatch.setattr(control_module, "installation_lock", lambda: pytest.fail("Linux-only lock"))

    async def serve(install_lock=None):
        assert install_lock is None
        return 0

    monkeypatch.setattr(control_module, "serve", serve)
    assert control_module.main() == 0


@pytest.fixture
async def update_setup(setup, monkeypatch):
    original, window, root = setup
    monkeypatch.setattr(sys, "platform", "linux")
    monkeypatch.setattr(control_module, "UPDATE_POLL_S", 0.01)
    monkeypatch.setattr(control_module, "UPDATE_CHECK_S", 0.01)
    play = root / "gothic-ai/play"
    scripts = play / "scripts"
    scripts.mkdir(parents=True)
    unit = root / ".config/systemd/user/gothic-play-update.service"
    unit.parent.mkdir(parents=True)
    unit.touch()
    lock_path = play.parent / "play.install.lock"
    held = lock_path.open("ab")
    fcntl.flock(held, fcntl.LOCK_SH | fcntl.LOCK_NB)
    os.set_inheritable(held.fileno(), True)  # helpers must close it even if the caller did not
    status = {
        "head": "old", "latest": "new", "target": "new", "good": "old", "built": "old",
        "failed": "", "current": False, "latest_failed": False, "behind": 2,
        "changes": [{"pr": 101, "title": "New game behavior"}, {"pr": None, "title": "A fix"}],
        "report": {"result": "busy", "time": "2026-11-01T00:00:00+00:00"},
    }
    (root / "status.json").write_text(json.dumps(status))
    (scripts / "ryzen-play.sh").write_text(
        "import json, os, pathlib, sys\n"
        f"root = pathlib.Path({str(root)!r})\n"
        "(root / 'status-args.json').write_text(json.dumps(sys.argv[1:]))\n"
        "try:\n"
        f"    stat = os.fstat({held.fileno()})\n"
        "except OSError:\n"
        "    pass\n"
        "else:\n"
        f"    lock = pathlib.Path({str(lock_path)!r}).stat()\n"
        "    assert (stat.st_dev, stat.st_ino) != (lock.st_dev, lock.st_ino), 'inherited install lock'\n"
        "print((root / 'status.json').read_text())\n"
    )
    binary_dir = root / "bin"
    binary_dir.mkdir()
    fake_systemd = (
        f"#!{sys.executable}\n"
        "import datetime, json, pathlib, sys\n"
        f"root = pathlib.Path({str(root)!r})\n"
        "command, args = pathlib.Path(sys.argv[0]).name, sys.argv[1:]\n"
        "with (root / 'systemd.jsonl').open('a') as trace:\n"
        "    trace.write(json.dumps({'command': command, 'args': args}) + '\\n')\n"
        "if command == 'journalctl':\n"
        "    print('Building launcher')\n"
        "elif args[1] == 'start':\n"
        "    count = root / 'starts'\n"
        "    count.write_text(str(int(count.read_text()) + 1 if count.exists() else 1))\n"
        "elif args[1] == 'show':\n"
        "    count = root / 'starts'\n"
        "    if not count.exists():\n"
        "        print('active' if (root / 'stale-busy').exists() else 'inactive')\n"
        "        sys.exit(0)\n"
        "    starts = int(count.read_text())\n"
        "    busy = (root / 'stale-busy').exists() and starts == 1\n"
        "    if busy:\n"
        "        print('inactive')\n"
        "        sys.exit(0)\n"
        "    if (root / 'release').exists():\n"
        "        when = datetime.datetime.now(datetime.timezone.utc).isoformat()\n"
        "        report = {'result': 'updated', 'time': when}\n"
        "        (root / 'gothic-ai/play-update.json').write_text(json.dumps(report))\n"
        "        print('inactive')\n"
        "    else:\n"
        "        print('active')\n"
    )
    for name in ("systemctl", "journalctl"):
        binary = binary_dir / name
        binary.write_text(fake_systemd)
        binary.chmod(0o755)
    monkeypatch.setenv("PATH", f"{binary_dir}{os.pathsep}{os.environ['PATH']}")
    c = Control(
        window, config_dir=original.config_dir, state_file=original.state_file,
        log_root=original.log_root, install_dir=play, install_lock=held,
    )
    try:
        yield c, window, root
    finally:
        (root / "release").touch()
        if c.update_task is not None:
            await asyncio.wait_for(c.update_task, 5)
        held.close()


@pytest.mark.parametrize("kind", ["development", "macos", "admitted"])
async def test_update_is_only_for_the_personal_linux_core(setup, monkeypatch, kind):
    original, window, root = setup
    monkeypatch.setattr(sys, "platform", "darwin" if kind == "macos" else "linux")
    c = Control(
        window, config_dir=original.config_dir, state_file=original.state_file,
        log_root=original.log_root, admitted=kind == "admitted",
        **({"install_dir": root / "gothic-ai/play"} if kind != "development" else {}),
    )
    assert (await call(c, "state"))["update"] is None
    with pytest.raises(RuntimeError, match="Обновление из окна есть только у личной установки Ryzen"):
        await call(c, "update.start")


async def test_update_state_comes_from_the_read_only_status_command(update_setup):
    c, window, root = update_setup
    raw = json.loads((root / "status.json").read_text())
    update = (await call(c, "state"))["update"]
    assert update == {
        "current": False, "behind": 2, "built": True, "latest_failed": False,
        "head": "old", "target": "new", "changes": raw["changes"], "report": raw["report"],
        "auto": True, "updating": None,
    }
    assert json.loads((root / "status-args.json").read_text()) == ["__status"]
    raw.update(built=None, latest_failed=True, target="old", behind=0, changes=[])
    (root / "status.json").write_text(json.dumps(raw))
    await c.refresh_update()
    assert c.update["built"] is False and c.update["latest_failed"] is True
    assert c.update["target"] == "old" and c.update["behind"] == 0 and c.update["changes"] == []


async def test_update_polling_only_emits_changed_status_and_handles_failures(
    update_setup, monkeypatch, caplog,
):
    c, window, root = update_setup
    polling = asyncio.create_task(c.poll_updates())
    try:
        await window.wait(lambda: bool(window.events("update")))
        initial = len(window.events("update"))
        await asyncio.sleep(0.1)
        assert len(window.events("update")) == initial
        raw = json.loads((root / "status.json").read_text())
        raw["behind"] = 3
        (root / "status.json").write_text(json.dumps(raw))
        await window.wait(lambda: any(u and u["behind"] == 3 for u in window.events("update")))
        script = root / "gothic-ai/play/scripts/ryzen-play.sh"
        script.write_text("import sys\nprint('status failed', file=sys.stderr)\nsys.exit(4)\n")
        await window.wait(lambda: window.events("update")[-1] is None)
        assert (await call(c, "state"))["update"] is None
        assert "status failed" in caplog.text
        script.write_text("import time\ntime.sleep(60)\n")
        monkeypatch.setattr(control_module, "UPDATE_STATUS_TIMEOUT_S", 0.02)
        await c.refresh_update()
        assert c.update is None and "timeout" in caplog.text.lower()
    finally:
        polling.cancel()
        await asyncio.gather(polling, return_exceptions=True)


async def test_update_releases_the_install_lock_and_streams_the_service_result(update_setup, monkeypatch):
    c, window, root = update_setup
    await c.refresh_update()
    lock_path = root / "gothic-ai/play.install.lock"
    with lock_path.open("ab") as other:
        with pytest.raises(BlockingIOError):
            fcntl.flock(other, fcntl.LOCK_EX | fcntl.LOCK_NB)
        started = await call(c, "update.start")
        assert started["state"] == "running" and started["detail"] == "Запускаю обновление"
        assert isinstance(started["started"], float)
        assert started["result"] is None and started["error"] is None
        fcntl.flock(other, fcntl.LOCK_EX | fcntl.LOCK_NB)
        fcntl.flock(other, fcntl.LOCK_UN)
    monkeypatch.setattr(
        control_module, "find_layout", lambda _config: pytest.fail("state must not launch clone programs")
    )
    assert (await call(c, "state"))["update"]["updating"]["state"] == "running"
    changed = await call(c, "settings.set", key="tts.enabled", value=True)
    assert changed["settings"]["tts"]["enabled"] is True
    await window.wait(lambda: any(
        u and u["updating"] and u["updating"]["detail"] == "Building launcher"
        for u in window.events("update")
    ))
    for method in ("session.start", "test.voice", "test.speech", "test.llm", "login.chatgpt", "mics"):
        with pytest.raises(RuntimeError, match="Идёт обновление установки"):
            await call(c, method)
    with pytest.raises(RuntimeError, match="[Оо]бновлен"):
        await call(c, "update.start")
    (root / "status-args.json").unlink()
    await c.refresh_update()
    assert not (root / "status-args.json").exists()
    (root / "release").touch()
    await asyncio.wait_for(c.update_task, 5)
    events = [u["updating"] for u in window.events("update") if u and u["updating"]]
    assert events[0]["state"] == "running" and events[-1]["state"] == "finished"
    assert events[-1]["result"] == "updated" and events[-1]["error"] is None
    assert c.update["report"]["result"] == "updated"
    with pytest.raises(RuntimeError, match="Идёт обновление установки"):
        await call(c, "session.start")
    assert (await call(c, "state"))["update"]["updating"]["state"] == "finished"
    changed = await call(c, "settings.set", key="tts.enabled", value=False)
    assert changed["settings"]["tts"]["enabled"] is False
    assert await call(c, "schema")
    trace = [json.loads(line) for line in (root / "systemd.jsonl").read_text().splitlines()]
    assert [t["args"] for t in trace if t["command"] == "systemctl" and t["args"][1] == "start"] == [
        ["--user", "start", "--no-block", "gothic-play-update.service"]
    ]
    journal = next(t["args"] for t in trace if t["command"] == "journalctl")
    assert journal == [
        "--user", "-u", "gothic-play-update.service", "--since", f"@{started['started']}",
        "-n", "1", "-o", "cat", "--no-pager",
    ]


async def test_update_retries_a_stale_busy_run_only_once(update_setup):
    c, window, root = update_setup
    (root / "stale-busy").touch()
    report = root / "gothic-ai/play-update.json"
    busy = {"result": "busy", "time": "2000-01-01T00:00:00+00:00"}
    report.write_text(json.dumps(busy))
    os.utime(report, (946684800, 946684800))
    await call(c, "update.start")
    async with asyncio.timeout(5):
        while not (root / "starts").exists() or (root / "starts").read_text() != "2":
            await asyncio.sleep(0.01)
    assert json.loads(report.read_text()) == busy and report.stat().st_mtime == 946684800
    (root / "release").touch()
    await asyncio.wait_for(c.update_task, 5)
    assert (root / "starts").read_text() == "2"
    assert c.update["updating"]["state"] == "finished"
    assert c.update["updating"]["result"] == "updated"


async def test_update_refuses_games_checks_and_missing_auto_service(update_setup):
    c, window, root = update_setup
    for state in ("waiting", "running"):
        c.session = {"state": state}
        with pytest.raises(RuntimeError, match="Сначала остановите игру"):
            await call(c, "update.start")
    c.session = None
    async with c._testing:
        with pytest.raises(RuntimeError, match="проверк"):
            await call(c, "update.start")
    (root / ".config/systemd/user/gothic-play-update.service").unlink()
    await c.refresh_update()
    with pytest.raises(RuntimeError, match="Автообновление не установлено: выполните"):
        await call(c, "update.start")
    assert not (root / "starts").exists()


async def test_macos_preparation_progress_and_order_reach_the_window(setup, monkeypatch):
    c, window, root = setup
    monkeypatch.setattr(sys, "platform", "darwin")
    monkeypatch.setattr(control_module, "services", lambda *args, **kwargs: [])
    monkeypatch.setattr(control_module, "game", commands.game)
    trace, release = root / "order", root / "release"
    (root / "Gothic2Notr").write_text(f"#!/bin/sh\necho game >> '{trace}'\n")
    (root / "content-convert").write_text(
        f"#!{sys.executable}\nimport json, pathlib, sys, time\n"
        f"pathlib.Path({str(trace)!r}).write_text(json.dumps(sys.argv[1:]) + '\\n')\n"
        "print('packing 1/2', file=sys.stderr, flush=True)\n"
        f"while not pathlib.Path({str(release)!r}).exists(): time.sleep(0.01)\n"
        "print('packing 2/2', file=sys.stderr, flush=True)\n"
        "print('/ready/base')\n"
    )
    try:
        await c.session_start()
        await window.wait(lambda: any(
            e["proc"] == "prepare" and e["detail"] == "packing 1/2" for e in window.events("service")
        ))
        state = await c.state()
        assert state["session"]["services"]["prepare"]["detail"] == "packing 1/2"
        assert "game" not in state["session"]["services"]
        release.touch()
        await asyncio.wait_for(c.task, 10)
        assert trace.read_text().splitlines() == [
            json.dumps(["resolve", str(root / "Gothic II")]), "game"
        ]
        log = Path(c.session["log_dir"]) / "prepare.log"
        assert "packing 1/2\npacking 2/2\n" in log.read_text()
        assert "/ready/base" not in log.read_text()
        assert c.session["code"] == 0 and c.session["failed"] is None
    finally:
        release.touch()
        await c.shutdown()


@pytest.mark.parametrize(("explicit", "code"), [(False, 0), (True, 13)])
def test_prepare_cli_selects_installation_without_starting_services(setup, monkeypatch, explicit, code):
    c, window, root = setup
    monkeypatch.setattr(cli, "_config", c._config)
    monkeypatch.setattr(cli, "SessionFile", lambda: SessionFile(root / "prepare-session.json"))
    monkeypatch.setattr(sv, "SessionFile", lambda: SessionFile(root / "prepare-session.json"))
    monkeypatch.setattr(cli, "new_log_dir", lambda: root / "prepare-logs")
    monkeypatch.setattr(cli, "services", lambda *args, **kwargs: pytest.fail("prepare started services"))
    trace = root / "resolve.json"
    (root / "Gothic2Notr").write_text("#!/bin/sh\nexit 77\n")
    (root / "content-convert").write_text(
        f"#!{sys.executable}\nimport json, pathlib, sys\n"
        f"pathlib.Path({str(trace)!r}).write_text(json.dumps(sys.argv[1:]))\n"
        f"print('converter exit {code}', file=sys.stderr)\nsys.exit({code})\n"
    )
    selected = root / "another install" if explicit else root / "Gothic II"
    if explicit:
        (selected / "Data").mkdir(parents=True)
    monkeypatch.setattr(sys, "argv", ["gothic-launcher", "prepare", *([str(selected)] if explicit else [])])
    with pytest.raises(SystemExit) as result:
        cli.main()
    assert result.value.code == code
    assert json.loads(trace.read_text()) == ["resolve", str(selected)]
    assert (c.config_dir / "settings.toml").read_text().find(str(root / "Gothic II")) >= 0


def test_up_prepares_before_starting_the_game(setup, monkeypatch):
    c, window, root = setup
    monkeypatch.setattr(cli, "_config", c._config)
    monkeypatch.setattr(cli, "SessionFile", lambda: SessionFile(root / "up-session.json"))
    monkeypatch.setattr(cli, "new_log_dir", lambda: root / "up-logs")
    monkeypatch.setattr(cli, "services", lambda *args, **kwargs: [])
    monkeypatch.setattr(sys, "platform", "darwin")
    trace = root / "order"
    (root / "content-convert").write_text(
        f"#!/bin/sh\nprintf '%s\\n' \"$1 $2\" >> '{trace}'\n"
    )
    (root / "Gothic2Notr").write_text(f"#!/bin/sh\necho game >> '{trace}'\n")
    monkeypatch.setattr(sys, "argv", ["gothic-launcher", "up", "--smoke"])
    with pytest.raises(SystemExit) as result:
        cli.main()
    assert result.value.code == 0
    assert trace.read_text().splitlines() == [f"resolve {root / 'Gothic II'}", "game"]


@pytest.fixture
def linux_preparation(setup, monkeypatch):
    from gothic_launcher import linux_session as linux

    c, window, root = setup
    c.admitted = False
    monkeypatch.setattr(sys, "platform", "linux")
    monkeypatch.setattr(Path, "home", classmethod(lambda cls: root))
    monkeypatch.setenv("DISPLAY", ":test")
    icd = root / "nvidia.json"
    icd.touch()
    monkeypatch.setattr(linux, "NVIDIA_ICD", icd)
    monkeypatch.setattr(linux, "portaudio_available", lambda: True)
    trace, release = root / "order", root / "release"
    (root / "Gothic2Notr").write_text(f"#!/bin/sh\necho game >> '{trace}'\n")
    (root / "content-convert").write_text(
        f"#!{sys.executable}\nimport json, os, pathlib, sys, time\n"
        f"pathlib.Path({str(trace)!r}).write_text(json.dumps(sys.argv[1:]) + '\\n')\n"
        f"pathlib.Path({str(root / 'converter.pid')!r}).write_text(str(os.getpid()))\n"
        "print('packing 1/2', file=sys.stderr, flush=True)\n"
        f"while not pathlib.Path({str(release)!r}).exists(): time.sleep(0.01)\n"
        "print('packing 2/2', file=sys.stderr, flush=True)\n"
    )

    def fake_queue(runner):
        # The real admitted CLI worker runs under a queue stand-in with the same EOF cancellation.
        worker = (
            "import pathlib, sys\nfrom argparse import Namespace\n"
            "from gothic_launcher import cli, control\n"
            f"pathlib.Path.home = classmethod(lambda cls: pathlib.Path({str(root)!r}))\n"
            "sys.platform = 'linux'\ncontrol.services = lambda *args, **kwargs: []\n"
            f"sys.exit(cli.session_worker(Namespace(config_dir=pathlib.Path({str(c.config_dir)!r}), "
            f"log_dir=pathlib.Path({str(runner.log_dir)!r}), mode='new', smoke=True, no_voice=True)))\n"
        )
        queue = (
            "import contextlib, os, signal, subprocess, sys, threading\n"
            f"worker = subprocess.Popen([sys.executable, '-c', {worker!r}])\n"
            "def cancel():\n"
            "    os.read(sys.stdin.fileno(), 1)\n"
            "    with contextlib.suppress(ProcessLookupError): worker.send_signal(signal.SIGTERM)\n"
            "threading.Thread(target=cancel, daemon=True).start()\n"
            "sys.exit(worker.wait())\n"
        )
        return [sys.executable, "-c", queue]

    monkeypatch.setattr(linux.LinuxSession, "command", fake_queue)
    return c, window, root


@pytest.mark.parametrize("outcome", ["success", "stop", "failure"])
async def test_linux_session_prepares_inside_admitted_worker(linux_preparation, outcome):
    from gothic_launcher.linux_session import LinuxSession

    c, window, root = linux_preparation
    release = root / "release"
    if outcome == "failure":
        converter = root / "content-convert"
        with converter.open("a") as script:
            script.write("print('cannot import base', file=sys.stderr)\nsys.exit(17)\n")
    try:
        await c.session_start(mode="new", smoke=True)
        assert isinstance(c.supervisor, LinuxSession)
        await window.wait(lambda: any(
            e["proc"] == "prepare" and e["detail"] == "packing 1/2" for e in window.events("service")
        ))
        assert c.session["services"]["prepare"]["detail"] == "packing 1/2"
        assert "game" not in c.session["services"]
        pid = int((root / "converter.pid").read_text())
        if outcome == "stop":
            await asyncio.wait_for(c.session_stop(), 10)
        else:
            release.touch()
            await asyncio.wait_for(c.task, 10)
        assert await gone(pid)
        expected = [json.dumps(["resolve", str(root / "Gothic II")])]
        assert (root / "order").read_text().splitlines() == expected + (
            ["game"] if outcome == "success" else []
        )
        assert "packing 1/2" in (Path(c.session["log_dir"]) / "prepare.log").read_text()
        assert c.session["code"] == (17 if outcome == "failure" else 0)
        if outcome == "failure":
            assert c.session["failed"]["proc"] == "prepare"
            assert "cannot import base" in c.session["failed"]["detail"]
    finally:
        release.touch()
        await asyncio.wait_for(c.shutdown(), 10)


def test_terminal_play_prepares_before_starting_the_game(linux_preparation, monkeypatch):
    c, window, root = linux_preparation
    (root / "release").touch()

    def make_control(write):
        def emit(msg):
            window(msg)
            write(msg)

        c.write = emit
        return c

    monkeypatch.setattr(control_module, "Control", make_control)
    assert cli.play(Namespace(smoke=True)) == 0
    assert (root / "order").read_text().splitlines() == [
        json.dumps(["resolve", str(root / "Gothic II")]), "game"
    ]
    assert any(e["proc"] == "prepare" and e["detail"] == "packing 1/2" for e in window.events("service"))
