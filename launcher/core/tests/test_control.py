import asyncio
import fcntl
import json
import sys
import zipfile
from pathlib import Path

import pytest

from gothic_launcher import control as control_module
from gothic_launcher import supervisor as sv
from gothic_launcher.commands import Ports, Proc
from gothic_launcher.control import Control
from tests.test_supervisor import FAKE, gone

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
    repo = tmp_path / "repo"
    (repo / "server").mkdir(parents=True)
    (repo / "server" / "pyproject.toml").write_text("")
    game_dir = tmp_path / "Gothic II"
    (game_dir / "Data").mkdir(parents=True)
    (game_dir / "Data" / "Worlds.vdf").write_bytes(b"")
    binary = tmp_path / "Gothic2Notr"
    binary.write_text("#!/bin/sh\n")
    binary.chmod(0o755)
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


async def test_a_second_session_is_refused_while_one_runs(setup):
    c, window, tmp_path = setup
    await call(c, "session.start")
    with pytest.raises(RuntimeError, match="уже запущена"):
        await call(c, "session.start")
    await call(c, "session.stop")
    assert window.events("session")[-1]["state"] == "stopped"


async def test_report_has_the_logs_without_keys(setup):
    c, window, tmp_path = setup
    await call(c, "settings.set", key="secrets.inworld_api_key", value="iw-secret")
    log_dir = tmp_path / "logs" / "2026-10-11_18-00-00"
    log_dir.mkdir(parents=True)
    (log_dir / "tts.log").write_text("key iw-secret leaked\n")

    path = Path((await call(c, "report.create"))["path"])

    with zipfile.ZipFile(path) as z:
        text = z.read("2026-10-11_18-00-00/tts.log").decode()
        settings = z.read("settings.toml").decode()
    assert "iw-secret" not in text and "<hidden>" in text and "<set>" in settings


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

    async def serve():
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

    async def serve():
        pytest.fail("control must not start while install/update holds the lock")

    monkeypatch.setattr(control_module, "serve", serve)
    with path.open("ab") as updating:
        fcntl.flock(updating, fcntl.LOCK_EX | fcntl.LOCK_NB)
        with pytest.raises(control_module.LaunchError, match="install/update is running"):
            control_module.main()


def test_macos_core_does_not_take_linux_install_lock(monkeypatch):
    monkeypatch.setattr(sys, "platform", "darwin")
    monkeypatch.setattr(control_module, "installation_lock", lambda: pytest.fail("Linux-only lock"))

    async def serve():
        return 0

    monkeypatch.setattr(control_module, "serve", serve)
    assert control_module.main() == 0
