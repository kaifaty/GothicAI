"""CLI defaults start the director only for ordinary play."""

import asyncio
import json
from types import SimpleNamespace
from unittest.mock import AsyncMock, Mock

import pytest

from gothic_ai import cli


@pytest.mark.parametrize("scenario", [None, "m0"])
def test_scenario_start_marker_waits_for_world_and_precedes_scenario(monkeypatch, caplog, tmp_path, scenario):
    real_serve = cli.serve
    captured = []

    async def capture_args(args):
        captured.append(args)
        return 0

    flags = [
        "--llm", "fake", "--no-voice", "--no-tts", "--no-brain", "--no-quick", "--no-neural-body",
        "--no-director", "--no-knowledge", "--no-questlog", "--no-crimes", "--no-gates",
        "--no-occasions", "--no-console", "--no-trace", "--memory-db", ":memory:",
        "--game-dir", str(tmp_path), "--exit-after",
    ]
    if scenario is not None:
        flags += ["--scenario", scenario]
    monkeypatch.delenv("GOTHIC_SOAK_CONTROL", raising=False)
    monkeypatch.setattr(cli, "serve", capture_args)
    monkeypatch.setattr("sys.argv", ["gothic-ai", "serve", *flags])
    with pytest.raises(SystemExit) as exit:
        cli.main()
    assert exit.value.code == 0
    monkeypatch.setattr(cli.places.PlaceBook, "load", Mock(return_value=None))
    caplog.set_level("INFO", logger="gothic_ai")

    def markers():
        return [record.getMessage() for record in caplog.records if "checks started" in record.getMessage()]

    async def exercise():
        link = cli.GameLink(SimpleNamespace(send=AsyncMock()))
        wait_entered = asyncio.Event()
        scenario_entered = asyncio.Event()
        real_wait_world = link.wait_world

        async def wait_world():
            wait_entered.set()
            return await real_wait_world()

        link.wait_world = wait_world

        async def run_scenario(actual_link, npc):
            assert actual_link is link
            assert link.world == "NEWWORLD.ZEN"
            assert markers() == ["scenario m0: checks started"]
            scenario_entered.set()
            return []

        run_scenario_mock = AsyncMock(side_effect=run_scenario)
        monkeypatch.setattr(cli.scenarios, "m0_smoke", run_scenario_mock)

        class FakeServer:
            def __init__(self, host, port, on_link):
                self.on_link = on_link

            async def serve_forever(self):
                session = asyncio.create_task(self.on_link(link))
                try:
                    if scenario is not None:
                        await wait_entered.wait()
                    else:
                        await asyncio.sleep(0)
                    assert markers() == []
                    run_scenario_mock.assert_not_awaited()
                    if scenario is not None:
                        link.world = "NEWWORLD.ZEN"
                        link._world_ready.set()
                        await scenario_entered.wait()
                    link.closed.set()
                    await session
                finally:
                    session.cancel()
                    await asyncio.gather(session, return_exceptions=True)

        monkeypatch.setattr(cli, "GameServer", FakeServer)
        assert await asyncio.wait_for(real_serve(captured[0]), timeout=5) == 0
        assert markers() == (["scenario m0: checks started"] if scenario is not None else [])
        assert run_scenario_mock.await_count == int(scenario is not None)

    asyncio.run(exercise())


@pytest.mark.parametrize(("flags", "websocket"), [([], True), (["--no-websocket"], False)])
def test_serve_arguments_select_responses_transport(monkeypatch, flags, websocket):
    monkeypatch.setenv("OPENAI_API_KEY", "e2e-placeholder")

    async def serve(args):
        backend = cli._backend(args)
        try:
            assert backend._websocket is websocket
        finally:
            await backend.aclose()
        return 0

    monkeypatch.setattr(cli, "serve", serve)
    monkeypatch.setattr("sys.argv", ["gothic-ai", "serve", "--llm", "openai", *flags])
    with pytest.raises(SystemExit) as exit:
        cli.main()
    assert exit.value.code == 0


@pytest.mark.parametrize(
    ("flags", "running"),
    [
        ([], True),
        (["--no-showrunner"], False),
        (["--scenario", "m0"], False),
        (["--scenario", "showrunner"], False),
        (["--scenario", "showrunner_live"], False),
    ],
)
def test_serve_arguments_control_showrunner_task(monkeypatch, flags, running):
    showrunner = SimpleNamespace(run=AsyncMock())

    async def serve(args):
        task = cli._start_showrunner(args, showrunner)
        assert isinstance(task, asyncio.Task) is running
        if task is not None:
            await task
        assert showrunner.run.await_count == int(running)
        return 0

    monkeypatch.setattr(cli, "serve", serve)
    monkeypatch.setattr("sys.argv", ["gothic-ai", "serve", *flags])
    with pytest.raises(SystemExit) as exit:
        cli.main()
    assert exit.value.code == 0


def test_showrunner_opt_in_flag_is_removed(monkeypatch, capsys):
    monkeypatch.setattr("sys.argv", ["gothic-ai", "serve", "--showrunner"])
    with pytest.raises(SystemExit) as exit:
        cli.main()
    assert exit.value.code == 2
    assert "unrecognized arguments: --showrunner" in capsys.readouterr().err


@pytest.mark.parametrize("flags", [[], ["--scenario", "questlog"], ["--no-questlog"]])
def test_questlog_is_enabled_by_default_including_scenarios(monkeypatch, flags):
    async def serve(args):
        assert args.no_questlog == ("--no-questlog" in flags)
        return 0

    monkeypatch.setattr(cli, "serve", serve)
    monkeypatch.setattr("sys.argv", ["gothic-ai", "serve", *flags])
    with pytest.raises(SystemExit) as exit:
        cli.main()
    assert exit.value.code == 0


@pytest.mark.parametrize("flags", [[], ["--no-combat-barks"]])
def test_combat_barks_are_enabled_by_default(monkeypatch, flags):
    async def serve(args):
        assert args.no_combat_barks == ("--no-combat-barks" in flags)
        return 0

    monkeypatch.setattr(cli, "serve", serve)
    monkeypatch.setattr("sys.argv", ["gothic-ai", "serve", *flags])
    with pytest.raises(SystemExit) as exit:
        cli.main()
    assert exit.value.code == 0


@pytest.mark.parametrize("flags", [[], ["--no-svm-lines"]])
def test_svm_lines_are_enabled_by_default(monkeypatch, flags):
    async def serve(args):
        assert args.no_svm_lines == ("--no-svm-lines" in flags)
        return 0

    monkeypatch.setattr(cli, "serve", serve)
    monkeypatch.setattr("sys.argv", ["gothic-ai", "serve", *flags])
    with pytest.raises(SystemExit) as exit:
        cli.main()
    assert exit.value.code == 0


@pytest.mark.parametrize(
    "flags", [[], ["--scenario", "crimes"], ["--scenario", "crime_meet"], ["--no-crimes"]]
)
def test_crimes_is_enabled_by_default_including_scenarios(monkeypatch, flags):
    async def serve(args):
        assert args.no_crimes == ("--no-crimes" in flags)
        return 0

    monkeypatch.setattr(cli, "serve", serve)
    monkeypatch.setattr("sys.argv", ["gothic-ai", "serve", *flags])
    with pytest.raises(SystemExit) as exit:
        cli.main()
    assert exit.value.code == 0


@pytest.mark.parametrize("crimes", [False, True])
@pytest.mark.parametrize("planner", [False, True])
@pytest.mark.parametrize("hold_threats", [False, True])
def test_deed_control_is_independent_of_the_planner_and_threat_switch(crimes, planner, hold_threats):
    args = SimpleNamespace(no_crimes=not crimes, no_hold_threats=not hold_threats)
    command = json.loads(cli._perception_control(args, planner=planner).to_json())
    assert command["hold"] == (planner and hold_threats)
    assert command.get("deeds", False) == crimes
    assert ("deeds" in command) == crimes


@pytest.mark.parametrize("flags", [[], ["--no-cover"], ["--no-crimes"], ["--scenario", "cover"]])
def test_cover_flag_defaults_and_scenario_are_available(monkeypatch, flags):
    async def serve(args):
        assert args.no_cover == ("--no-cover" in flags)
        assert args.no_crimes == ("--no-crimes" in flags)
        return 0

    monkeypatch.setattr(cli, "serve", serve)
    monkeypatch.setattr("sys.argv", ["gothic-ai", "serve", *flags])
    with pytest.raises(SystemExit) as exit:
        cli.main()
    assert exit.value.code == 0


@pytest.mark.parametrize("flags", [[], ["--no-npc-cheat"], ["--no-crimes"], ["--scenario", "cheat"]])
def test_npc_cheat_flag_and_scenario_are_available(monkeypatch, flags):
    async def serve(args):
        assert args.no_npc_cheat == ("--no-npc-cheat" in flags)
        assert args.no_crimes == ("--no-crimes" in flags)
        return 0

    monkeypatch.setattr(cli, "serve", serve)
    monkeypatch.setattr("sys.argv", ["gothic-ai", "serve", *flags])
    with pytest.raises(SystemExit) as exit:
        cli.main()
    assert exit.value.code == 0


async def test_no_crimes_starts_no_encounter_worker_or_approach(monkeypatch):
    meeting, approach = Mock(), Mock()
    monkeypatch.setattr(cli, "CrimeMeet", meeting)
    monkeypatch.setattr(cli, "ImportantApproach", approach)
    state = cli.ServerState()
    assert cli._start_crime_meet(state, Mock(), {}) is None
    assert state.crime_meet is None
    meeting.assert_not_called()
    approach.assert_not_called()


@pytest.mark.parametrize("flags", [[], ["--no-npc-crimes"], ["--no-crimes"], ["--scenario", "npc_deeds"]])
def test_npc_crimes_switch_and_scenario(monkeypatch, flags):
    async def serve(args):
        assert args.no_npc_crimes == ("--no-npc-crimes" in flags)
        assert args.no_crimes == ("--no-crimes" in flags)
        return 0

    monkeypatch.setattr(cli, "serve", serve)
    monkeypatch.setattr("sys.argv", ["gothic-ai", "serve", *flags])
    with pytest.raises(SystemExit) as exit:
        cli.main()
    assert exit.value.code == 0


async def test_encounter_worker_starts_without_a_behavioral_planner(monkeypatch):
    meeting = Mock(return_value=SimpleNamespace(
        run=AsyncMock(), refuses=Mock(return_value=False), refusal=Mock(return_value=None)
    ))
    monkeypatch.setattr(cli, "CrimeMeet", meeting)
    monkeypatch.setattr(cli, "ImportantApproach", Mock())
    state = cli.ServerState()
    state.crimes = state.crime_react = state.crime_memory = Mock()
    state.bodies = state.talk = state.chronicle = Mock()
    assert state.brain is None
    task = cli._start_crime_meet(state, Mock(), {})
    assert task is not None
    await task
    assert state.crime_meet is meeting.return_value
    assert state.talk.refuses is state.crime_meet.refuses
    assert state.talk.refusal is state.crime_meet.refusal
    state.crime_meet.run.assert_awaited_once()


@pytest.mark.parametrize(
    ("scenario", "control", "enabled", "url", "llm", "valid"),
    [
        ("neural-soak", "1", False, None, "fake", True),
        ("neural-soak", "1", False, "tcp://policy:47835", "fake", True),
        ("neural-soak", "1", True, "tcp://policy:47835", "fake", False),
        ("neural-soak", "0", False, None, "fake", False),
        ("neural-soak", "0", True, None, "fake", False),
        ("neural-soak", "0", True, "tcp://policy:47835", "fake", True),
        ("neural-soak", "1", False, None, "chatgpt", False),
        ("neural-load", "1", False, None, "fake", False),
        (None, "1", False, None, "fake", False),
    ],
)
async def test_soak_control_requires_explicit_pilot_disable_and_only_skips_body_url(
    monkeypatch, scenario, control, enabled, url, llm, valid
):
    monkeypatch.setenv("GOTHIC_SOAK_CONTROL", control)
    args = SimpleNamespace(scenario=scenario, neural_body=enabled, body_url=url, llm=llm)
    backend = Mock(side_effect=RuntimeError("passed launch validation"))
    monkeypatch.setattr(cli, "_backend", backend)
    if valid:
        with pytest.raises(RuntimeError, match="passed launch validation"):
            await cli.serve(args)
        backend.assert_called_once_with(args)
    else:
        assert await cli.serve(args) == 2
        backend.assert_not_called()


@pytest.mark.parametrize(
    ("flags", "env_url", "enabled", "expected_url", "created"),
    [
        ([], None, True, None, False),
        ([], "tcp://policy:47835", True, "tcp://policy:47835", True),
        (["--no-neural-body"], "tcp://policy:47835", False, "tcp://policy:47835", False),
        (["--neural-body", "--body-url", "tcp://explicit:47835"], None, True, "tcp://explicit:47835", True),
        (["--body-url", "tcp://explicit:47835"], "tcp://policy:47835", True, "tcp://explicit:47835", True),
        (["--body-url", ""], "tcp://policy:47835", True, "", False),
    ],
)
def test_serve_arguments_create_bodypilot_only_with_flag_and_url(
    monkeypatch, flags, env_url, enabled, expected_url, created
):
    if env_url is None:
        monkeypatch.delenv("GOTHIC_BODY_URL", raising=False)
    else:
        monkeypatch.setenv("GOTHIC_BODY_URL", env_url)
    factory = Mock()
    monkeypatch.setattr(cli, "BodyPilot", factory)
    link = SimpleNamespace(body_watch=AsyncMock(), body_acquire=AsyncMock())
    talk = SimpleNamespace(orders=SimpleNamespace(companion=Mock()), attached=set(), partner=None)

    async def serve(args):
        assert args.neural_body is enabled
        assert args.body_url == expected_url
        pilot = cli._start_bodypilot(args, link, talk)
        if created:
            assert pilot is factory.return_value
            assert factory.call_args.args == (link, expected_url)
            assert factory.call_args.kwargs["companion"] is talk.orders.companion
        else:
            assert pilot is None
            factory.assert_not_called()
        link.body_watch.assert_not_called()
        link.body_acquire.assert_not_called()
        return 0

    monkeypatch.setattr(cli, "serve", serve)
    monkeypatch.setattr("sys.argv", ["gothic-ai", "serve", *flags])
    with pytest.raises(SystemExit) as exit:
        cli.main()
    assert exit.value.code == 0


def test_bodypilot_distinguishes_conversation_partner_from_attached_npcs(monkeypatch):
    factory = Mock()
    monkeypatch.setattr(cli, "BodyPilot", factory)
    talk = SimpleNamespace(orders=SimpleNamespace(companion=Mock()), attached=set(), partner=None)
    cli._start_bodypilot(
        SimpleNamespace(neural_body=True, body_url="tcp://policy:47835", neural_body_max=4), Mock(), talk,
    )
    unavailable = factory.call_args.kwargs["unavailable"]
    talking = factory.call_args.kwargs["talking"]
    assert not unavailable("npc")
    assert not talking("npc")
    talk.partner = SimpleNamespace(npc="NPC")
    assert talking("npc")
    assert not unavailable("npc")
    talk.partner = None
    talk.attached.add("NPC")
    assert unavailable("npc")
    assert not talking("npc")
    talk.attached.clear()
    assert not unavailable("NPC")
    assert not talking("NPC")


@pytest.mark.parametrize("maximum", [1, 4])
def test_neural_body_max_passes_supported_limit_to_pilot(monkeypatch, maximum):
    factory = Mock()
    monkeypatch.setattr(cli, "BodyPilot", factory)
    talk = SimpleNamespace(orders=SimpleNamespace(companion=Mock()), attached=set(), partner=None)

    async def serve(args):
        assert args.neural_body_max == maximum
        cli._start_bodypilot(args, Mock(), talk)
        assert factory.call_args.kwargs["max_fights"] == maximum
        return 0

    monkeypatch.setattr(cli, "serve", serve)
    monkeypatch.setattr("sys.argv", [
        "gothic-ai", "serve", "--body-url", "tcp://policy:47835", "--neural-body-max", str(maximum),
    ])
    with pytest.raises(SystemExit) as exit:
        cli.main()
    assert exit.value.code == 0


@pytest.mark.parametrize("maximum", [0, 5])
def test_neural_body_max_rejects_values_outside_engine_watch_limit(monkeypatch, maximum):
    monkeypatch.setattr("sys.argv", ["gothic-ai", "serve", "--neural-body-max", str(maximum)])
    with pytest.raises(SystemExit) as exit:
        cli.main()
    assert exit.value.code == 2
