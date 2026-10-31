"""Live shots use the production pilot, native fights and real player input."""
from __future__ import annotations

import asyncio
import configparser
import importlib.util
import json
import logging
from pathlib import Path
import sys
from types import ModuleType, SimpleNamespace

import pytest


SCRIPT = Path(__file__).resolve().parents[1] / "body_live_capture.py"


@pytest.fixture
def capture():
    spec = importlib.util.spec_from_file_location("body_live_capture", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@pytest.mark.parametrize("shot,count", [("live-takeover", 1), ("live-group", 3), ("live-passive", 1)])
def test_scene_waits_for_preparation_and_records_every_native_fight(capture, tmp_path, shot, count):
    lines = capture.scene(shot, tmp_path, 25).splitlines()
    npcs = capture.actors(shot)
    assert len(npcs) == count and len(set(npcs)) == count
    gate = lines.index(f"bodylive wait {tmp_path / 'ready'} 120000")
    recording = lines.index("rec shot")
    assert lines.index("clear 3000") < lines.index("bodylive on") < gate < recording
    assert gate < lines.index("immortal player on") < recording
    assert gate < lines.index("stats hpmax 100000 hp 100000") < recording
    assert gate < lines.index(f"track {','.join(npcs)} head") < recording
    assert any(line.startswith(f"cam two {npcs[0]} ") for line in lines[gate:recording])
    assert [line for line in lines if line.startswith("fight ")] == [f"fight {npc} player" for npc in npcs]
    assert all(recording < lines.index(f"fight {npc} player") < lines.index("stop") for npc in npcs)
    assert not any(line.startswith(("bodyarena", "bodyreplay")) for line in lines)
    assert lines[-4:] == ["release all", "stop", "wait 3000", "bodylive off"]


@pytest.mark.parametrize("shot", ["live-takeover", "live-group", "live-passive"])
@pytest.mark.parametrize("duration", [20, 25, 30])
def test_scene_runs_the_full_recording_duration(capture, tmp_path, shot, duration):
    lines = capture.scene(shot, tmp_path, duration).splitlines()
    recording = lines[lines.index("rec shot") + 1:lines.index("stop")]
    # `hold` schedules a release without waiting: only `wait` advances the scene.
    assert sum(int(line.split()[1]) for line in recording if line.startswith("wait ")) == duration * 1000
    if shot == "live-passive":
        assert not any(line.startswith(("hold ", "press ", "tap ")) for line in recording)
    else:
        holds = [index for index, line in enumerate(recording) if line.startswith("hold action ")]
        assert len(holds) >= 10
        assert all(recording[index + 1] == f"wait {recording[index].split()[2]}" for index in holds)


@pytest.mark.parametrize("duration", [0, 19, 31, 120])
def test_scene_rejects_duration_outside_the_requested_range(capture, tmp_path, duration):
    with pytest.raises(ValueError, match="20..30"):
        capture.scene("live-takeover", tmp_path, duration)


def test_unknown_shot_is_rejected(capture):
    with pytest.raises(ValueError, match="unknown shot"):
        capture.actors("arena-replay")


@pytest.mark.parametrize("returncode,stdout,exact_code,error", [
    (1, "", 1, None),
    (0, "123 Gothic2Notr\n", 0, "another Gothic2Notr"),
    (0, "123 cmake --build build --target Gothic2Notr\n", 1, None),
    (2, "", None, "pgrep failed: 2"),
    (1, "", 2, "pgrep failed: 2"),
])
def test_game_guard_checks_the_mac_and_rejects_both_a_game_and_query_failure(
    capture, monkeypatch, capsys, returncode, stdout, exact_code, error,
):
    calls = []

    def run(argv, *, capture_output, text, env):
        calls.append(argv)
        assert capture_output and text
        assert env["LC_ALL"] == "C"
        if len(calls) == 1:
            assert argv == ["pgrep", "-fl", "Gothic2Notr"]
            return SimpleNamespace(returncode=returncode, stdout=stdout)
        assert len(calls) == 2 and argv == ["pgrep", "-x", "Gothic2Notr"]
        assert exact_code is not None
        return SimpleNamespace(returncode=exact_code, stdout="123\n" if exact_code == 0 else "")

    monkeypatch.setattr(capture.subprocess, "run", run)
    if error:
        with pytest.raises(RuntimeError, match=error):
            capture.game_guard()
    else:
        capture.game_guard()
    assert len(calls) == (1 if exact_code is None else 2)
    assert capsys.readouterr().out.strip() == (stdout.strip() or "no game")


def install_runtime_stubs(monkeypatch, **attributes):
    """The body-service test environment does not install the server's dependencies."""
    for name, exports in {
        "body_replay_film": {"macos_window_size": lambda size: "960x540"},
        "gothic_ai.bodypilot": {"BodyPilot": attributes.get("BodyPilot", object)},
        "gothic_ai.game": {"GameServer": attributes.get("GameServer", object)},
    }.items():
        module = ModuleType(name)
        module.__dict__.update(exports)
        monkeypatch.setitem(sys.modules, name, module)


def arguments(tmp_path):
    settings = tmp_path / "settings.toml"
    settings.write_text('[llm]\nbody_url = "tcp://policy.invalid:9000"\n')
    save = tmp_path / "save.sav"
    save.write_bytes(b"fixture save")
    converter = tmp_path / "content-convert"
    converter.write_text("#!/bin/sh\nexit 0\n")
    converter.chmod(0o755)
    return SimpleNamespace(shot="live-group", output=tmp_path / "take", settings=settings,
                           duration=25, save=save, game=tmp_path / "Gothic2Notr", assets=tmp_path / "assets")


@pytest.mark.parametrize("old_name", ["shot.mp4", "shot.wav", "actions.json", "events.jsonl", "server.log", "ready"])
def test_capture_refuses_existing_take_before_launch(capture, tmp_path, monkeypatch, old_name):
    install_runtime_stubs(monkeypatch)
    monkeypatch.setattr(capture.sys, "platform", "darwin")
    for name in ("LLM", "ASR", "TTS"):
        monkeypatch.setenv(name, "fake")
    args = arguments(tmp_path)
    args.output.mkdir()
    (args.output / old_name).write_bytes(b"old take")
    with pytest.raises(RuntimeError, match="fresh directory"):
        asyncio.run(capture.capture(args))
    assert (args.output / old_name).read_bytes() == b"old take"
    assert not (args.output / "scene.bench").exists()


def test_runtime_uses_settings_policy_native_sound_and_prepared_scene(capture, tmp_path, monkeypatch):
    args = arguments(tmp_path)
    order = []
    state = {}

    class Pilot:
        def __init__(self, link, url):
            assert url == "tcp://policy.invalid:9000"
            assert (args.output / "ready").exists()
            assert any(event.get("kind") == "rec" and event.get("seg") == "shot"
                       for event in capture.read_events(args.output / "events.jsonl"))
            order.append("pilot")
            self.stopped = asyncio.Event()
            state["pilot"] = self

        async def run(self):
            await self.stopped.wait()

        async def aclose(self):
            self.stopped.set()

    class Server:
        def __init__(self, *, port, on_link):
            assert port == 0
            self.port = 49717
            self.started = asyncio.Event()
            self.on_link = on_link
            state["server"] = self

        async def serve_forever(self):
            order.append("server")
            self.started.set()
            await asyncio.Event().wait()

    class Game:
        returncode = None

        async def wait(self):
            session = asyncio.create_task(state["server"].on_link(SimpleNamespace(closed=asyncio.Event())))
            await asyncio.sleep(0)
            # Reaching the preparation gate must not start a policy lease. Native
            # fight setup runs after this gate and could revoke an earlier lease.
            assert (args.output / "ready").exists() and "pilot" not in state
            (args.output / "events.jsonl").write_text("".join(json.dumps(event) + "\n" for event in [
                {"kind": "rec", "seg": "shot", "fps": 30},
                {"kind": "rec_stop", "seg": "shot", "frames": args.duration * 30},
            ]))
            (args.output / "shot.mp4").write_bytes(b"completed video")
            (args.output / "shot.wav").write_bytes(b"completed audio")
            order.append("recorded")
            await session
            self.returncode = 0

    async def prepared(link, shot):
        assert shot == "live-group" and not (args.output / "ready").exists()
        order.append("prepared")

    async def launch(*argv, cwd, env, stdout, stderr):
        order.append("launch")
        assert order[-2:] == ["guard", "launch"]
        assert argv == (str(args.game), "-g", str(args.assets), "-g2", "-window", "-devmode", "-save", "1")
        assert cwd == args.output / "run"
        assert (cwd / "save_slot_1.sav").read_bytes() == b"fixture save"
        assert env["GOTHIC_AI"] == "1" and env["GOTHIC_AI_URL"] == "ws://127.0.0.1:49717"
        assert env["TEMPEST_WINDOW_SIZE"] == "960x540"
        assert env["TEMPEST_AUDIO_LOOPBACK"] == "48000"
        assert env["OPENGOTHIC_BENCH_REALTIME"] == "1" and env["OPENGOTHIC_PHYSICS_THREADS"] == "0"
        assert env["OPENGOTHIC_BENCH"] == str(args.output / "scene.bench")
        assert env["OPENGOTHIC_BENCH_OUT"] == str(args.output)
        assert all(env[name] == "fake" for name in ("LLM", "ASR", "TTS"))
        assert not any(name in env for name in ("ALSOFT_DRIVERS", "VK_DRIVER_FILES", "VK_ICD_FILENAMES"))
        config = configparser.ConfigParser()
        config.read(cwd / "Gothic.ini")
        assert config.getint("GAME", "USEGOTHIC1CONTROLS") == 0  # action alone swings the sword
        assert config.getint("INTERNAL", "vidResIndex") == 0
        assert {key: config.getfloat("SOUND", key) for key in
                ("soundVolume", "musicVolume", "musicEnabled", "worldSounds")} == {
                    "soundVolume": 1, "musicVolume": 0, "musicEnabled": 0, "worldSounds": 0}
        return Game()

    install_runtime_stubs(monkeypatch, BodyPilot=Pilot, GameServer=Server)
    monkeypatch.setattr(capture.sys, "platform", "darwin")
    for name in ("LLM", "ASR", "TTS"):
        monkeypatch.setenv(name, "fake")
    for name in ("ALSOFT_DRIVERS", "VK_DRIVER_FILES", "VK_ICD_FILENAMES"):
        monkeypatch.setenv(name, "inherited-linux-setting")
    monkeypatch.setattr(capture, "prepare", prepared)
    monkeypatch.setattr(capture, "game_guard", lambda: order.append("guard"))
    monkeypatch.setattr(capture.asyncio, "create_subprocess_exec", launch)
    # Keep the host pytest log configuration and avoid retaining temporary handlers.
    monkeypatch.setattr(capture.logging, "basicConfig", lambda **kw: [handler.close() for handler in kw["handlers"]])
    assert asyncio.run(capture.capture(args)) == 0
    assert order == ["server", "guard", "launch", "prepared", "recorded", "pilot"]
    assert (args.output / "ready").exists() and state["pilot"].stopped.is_set()


@pytest.mark.parametrize("transport", ["stub", "websocket"])
@pytest.mark.parametrize("phase", ["before-rec", "after-rec", "after-rec-stop"])
def test_disconnect_requires_finalized_recording_and_cleans_up_pilot(
    capture, tmp_path, monkeypatch, caplog, transport, phase,
):
    """A zero game exit cannot turn GameServer's session cancellation into success."""
    server_class = connect = None
    if transport == "websocket":
        if sys.version_info < (3, 12):
            pytest.skip("the production server requires Python 3.12+")
        pytest.importorskip("pydantic")
        connect = pytest.importorskip("websockets.asyncio.client").connect
        server_class = pytest.importorskip("gothic_ai.game").GameServer

    args = arguments(tmp_path)
    state = {}

    class Pilot:
        def __init__(self, link, url):
            self.stopped = asyncio.Event()
            state["pilot"] = self
            state["pilot_started"].set()

        async def run(self):
            try:
                await self.stopped.wait()
            finally:
                state["pilot_finished"] = True

        async def aclose(self):
            # Cleanup must be awaited even when GameServer cancels the session.
            await asyncio.sleep(0)
            self.stopped.set()
            state["pilot_closed"] = True

    class Server:
        def __init__(self, *, port, on_link):
            self.port = 49717
            self.started = asyncio.Event()
            self.on_link = on_link
            state["server"] = self

        async def serve_forever(self):
            self.started.set()
            await asyncio.Event().wait()

    async def record_and_disconnect(disconnect):
        await state["prepared"].wait()
        # The real session touches ready immediately after prepare returns.
        assert (args.output / "ready").exists()
        if phase != "before-rec":
            events = [{"kind": "rec", "seg": "shot", "fps": 30}]
            event_log = args.output / "events.jsonl"
            event_log.write_text(json.dumps(events[0]) + "\n")
            await state["pilot_started"].wait()
            if phase == "after-rec-stop":
                with event_log.open("a") as stream:
                    stream.write(json.dumps({"kind": "rec_stop", "seg": "shot",
                                             "frames": args.duration * 30}) + "\n")
                (args.output / "shot.mp4").write_bytes(b"completed video")
                (args.output / "shot.wav").write_bytes(b"completed audio")
        # In the positive case the stop is already flushed, but cancellation can
        # still arrive before the session's next rec_stop poll.
        await disconnect()

    class Game:
        returncode = None

        async def wait(self):
            if connect is not None:
                async with connect(state["url"]) as websocket:
                    await record_and_disconnect(websocket.close)
            else:
                link = SimpleNamespace(closed=asyncio.Event())
                session = asyncio.create_task(state["server"].on_link(link))

                async def disconnect():
                    link.closed.set()
                    session.cancel()
                    await asyncio.gather(session, return_exceptions=True)

                await record_and_disconnect(disconnect)
            self.returncode = 0

    async def prepared(link, shot):
        state["prepared"].set()

    async def launch(*argv, **kwargs):
        state["url"] = kwargs["env"]["GOTHIC_AI_URL"]
        return Game()

    async def run():
        state["prepared"] = asyncio.Event()
        state["pilot_started"] = asyncio.Event()
        return await asyncio.wait_for(capture.capture(args), timeout=5)

    install_runtime_stubs(monkeypatch, BodyPilot=Pilot, GameServer=server_class or Server)
    monkeypatch.setattr(capture.sys, "platform", "darwin")
    for name in ("LLM", "ASR", "TTS"):
        monkeypatch.setenv(name, "fake")
    monkeypatch.setattr(capture, "prepare", prepared)
    monkeypatch.setattr(capture, "game_guard", lambda: None)
    monkeypatch.setattr(capture.asyncio, "create_subprocess_exec", launch)
    monkeypatch.setattr(capture.logging, "basicConfig", lambda **kw: [handler.close() for handler in kw["handlers"]])
    caplog.set_level(logging.INFO, logger="body_live_capture")
    if phase == "after-rec-stop":
        assert asyncio.run(run()) == 0
        assert "capture complete" in caplog.text
    else:
        with pytest.raises(RuntimeError):
            asyncio.run(run())
        assert "capture complete" not in caplog.text
        assert not (args.output / "shot.mp4").exists()
    if phase != "before-rec":
        assert state["pilot_closed"] and state["pilot_finished"]


def test_events_accept_complete_flushed_rows_and_ignore_a_partial_tail(capture, tmp_path):
    path = tmp_path / "events.jsonl"
    assert capture.read_events(path) == []
    path.write_text('{"kind":"rec"}\n{"kind":"rec_stop"}\n{"kind":')
    assert capture.read_events(path) == [{"kind": "rec"}, {"kind": "rec_stop"}]
