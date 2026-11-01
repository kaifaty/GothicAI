"""CLI defaults start the director only for ordinary play."""

import asyncio
import json
from types import SimpleNamespace
from unittest.mock import AsyncMock, Mock

import pytest

from gothic_ai import cli
from gothic_ai.llm.off import ModelOff, OffBackend


@pytest.mark.parametrize(("scenario", "llm", "expected"), [
    ("m0", "auto", "off"),
    *[(scenario, "auto", "off") for scenario in (
        "deal", "contract", "debt", "xardas_money", "deal_demo",
        "bandit_parley", "bandit_bounty", "calm", "companion", "neural-talk", "rescue", "rescue_self",
    )],
    ("m1", "auto", "fake"),
    ("haggle", "auto", "scenario haggle is live: set LLM=chatgpt|openai|local"),
    ("m0", "fake", "scenario m0 requires --llm off"),
    ("m0", "chatgpt", "scenario m0 requires --llm off"),
    ("deal", "chatgpt", "scenario deal requires --llm off"),
    ("haggle", "off", "scenario haggle is live: set LLM=chatgpt|openai|local"),
    ("haggle", "fake", "scenario haggle is live: set LLM=chatgpt|openai|local"),
    (None, "auto", "--llm auto requires --scenario"),
])
def test_scenario_llm_groups(monkeypatch, capsys, scenario, llm, expected):
    serve = AsyncMock(return_value=0)
    monkeypatch.setattr(cli, "serve", serve)
    flags = ["--scenario", scenario] if scenario else []
    monkeypatch.setattr("sys.argv", ["gothic-ai", "serve", "--llm", llm, *flags])
    with pytest.raises(SystemExit) as exit:
        cli.main()
    if expected in {"off", "fake"}:
        assert exit.value.code == 0
        assert serve.await_args.args[0].llm == expected
    else:
        assert exit.value.code == 2
        assert expected in capsys.readouterr().err
        serve.assert_not_awaited()


def test_story_graph_is_optional_before_the_game_publishes_its_base(tmp_path, monkeypatch, caplog):
    game = tmp_path / "game"
    game.mkdir()
    monkeypatch.delenv("OPENGOTHIC_BASE", raising=False)
    monkeypatch.setenv("OPENGOTHIC_BASE_CACHE", str(tmp_path / "cache"))
    assert cli._story_graph(SimpleNamespace(story_graph=None), game) is None
    assert f"no ready base for {game}" in caplog.text
    assert "run the game once with the installation or set OPENGOTHIC_BASE" in caplog.text


@pytest.mark.parametrize(("scenario", "backend_kind", "outcome", "code"), [
    (None, "fake", "clean", 0),
    ("m0", "off", "clean", 0),
    ("m0", "off", "caught", 1),
    ("m0", "off", "uncaught", 1),
    ("m0", "off", "unused", 1),
    ("m0", "off", "used", 0),
])
def test_scenario_start_marker_waits_for_world_and_precedes_scenario(
    monkeypatch, caplog, tmp_path, scenario, backend_kind, outcome, code,
):
    real_serve = cli.serve
    captured = []

    async def capture_args(args):
        captured.append(args)
        return 0

    flags = [
        "--llm", backend_kind, "--no-voice", "--no-tts", "--no-brain", "--no-quick", "--no-neural-body",
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
    backend = cli._backend(captured[0])
    monkeypatch.setattr(cli, "_backend", lambda args: backend)
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
            if isinstance(backend, OffBackend):
                if outcome in {"unused", "used"}:
                    backend.silent("Cavalorn stays quiet", who="CAVALORN")
                if outcome in {"caught", "uncaught", "used"}:
                    try:
                        async for _ in backend.stream(
                            instructions="", messages=[], cache_key="CAVALORN",
                        ):
                            pass
                    except ModelOff:
                        if outcome == "uncaught":
                            raise
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
        assert await asyncio.wait_for(real_serve(captured[0]), timeout=5) == code
        assert markers() == (["scenario m0: checks started"] if scenario is not None else [])
        assert run_scenario_mock.await_count == int(scenario is not None)
        records = [record.getMessage() for record in caplog.records]
        checks = {key: next((line for line in records if f": {key}: " in line), "")
                  for key in ("model_off", "givens_used")}
        if backend_kind == "off":
            assert ("failed" in checks["model_off"]) == (outcome in {"caught", "uncaught"})
            assert ("failed" in checks["givens_used"]) == (outcome == "unused")
            assert all(checks.values())
            if outcome == "used":
                assert "scenario m0: given: Cavalorn stays quiet" in records
        else:
            assert not any(checks.values())

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
        (["--scenario", "m0", "--llm", "off"], False),
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


@pytest.mark.parametrize("flags", [[], ["--no-feat-challenges"], ["--no-feats"]])
def test_feat_challenge_flags(monkeypatch, flags):
    async def serve(args):
        assert args.no_feat_challenges == ("--no-feat-challenges" in flags)
        assert args.no_feats == ("--no-feats" in flags)
        return 0

    monkeypatch.setattr(cli, "serve", serve)
    monkeypatch.setattr("sys.argv", ["gothic-ai", "serve", *flags])
    with pytest.raises(SystemExit) as exit:
        cli.main()
    assert exit.value.code == 0


@pytest.mark.parametrize("flags", [[], ["--scenario", "questlog", "--llm", "auto"], ["--no-questlog"]])
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
    "flags", [[], ["--scenario", "crimes", "--llm", "auto"], ["--scenario", "crime_meet"], ["--no-crimes"]]
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


@pytest.mark.parametrize("flags", [
    [], ["--no-npc-crimes"], ["--no-crimes"], ["--scenario", "npc_deeds", "--llm", "auto"],
])
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
    ("scenario", "control", "enabled", "url", "llm", "valid", "policy"),
    [
        ("neural-soak", "1", False, None, "off", True, "service"),
        ("neural-soak", "1", False, "tcp://policy:47835", "off", True, "service"),
        ("neural-soak", "1", True, "tcp://policy:47835", "off", False, "service"),
        ("neural-soak", "0", False, None, "off", False, "service"),
        ("neural-soak", "0", True, None, "off", True, "engine"),
        ("neural-soak", "0", True, None, "off", False, "service"),
        ("neural-soak", "0", True, "tcp://policy:47835", "off", True, "service"),
        ("neural-soak", "0", True, "tcp://policy:47835", "off", True, "engine"),
        ("neural-load", "1", False, None, "off", False, "service"),
        (None, "1", False, None, "fake", False, "service"),
        ("neural", "0", True, None, "off", True, "engine"),
        ("neural", "0", True, None, "off", False, "service"),
        ("feats", "0", True, None, "fake", True, "engine"),
        ("feats", "0", True, None, "chatgpt", True, "engine"),
        ("feats", "0", True, None, "local", False, "engine"),
    ],
)
async def test_serve_validates_scenario_backend_and_soak_control(
    monkeypatch, scenario, control, enabled, url, llm, valid, policy
):
    monkeypatch.setenv("GOTHIC_SOAK_CONTROL", control)
    args = SimpleNamespace(
        scenario=scenario, neural_body=enabled, body_url=url, llm=llm, body_policy=policy,
    )
    backend = Mock(side_effect=RuntimeError("passed launch validation"))
    monkeypatch.setattr(cli, "_backend", backend)
    if valid:
        with pytest.raises(RuntimeError, match="passed launch validation"):
            await cli.serve(args)
        backend.assert_called_once_with(args)
        assert args.body_policy == policy
    else:
        assert await cli.serve(args) == 2
        backend.assert_not_called()


@pytest.mark.parametrize(
    ("flags", "env_url", "enabled", "expected_url", "created"),
    [
        ([], None, True, None, True),
        ([], "tcp://policy:47835", True, "tcp://policy:47835", True),
        (["--no-neural-body"], "tcp://policy:47835", False, "tcp://policy:47835", False),
        (["--neural-body", "--body-url", "tcp://explicit:47835"], None, True, "tcp://explicit:47835", True),
        (["--body-url", "tcp://explicit:47835"], "tcp://policy:47835", True, "tcp://explicit:47835", True),
        (["--body-url", ""], "tcp://policy:47835", True, "", True),
        (["--body-policy", "service"], None, True, None, False),
        (["--body-policy", "service"], "tcp://policy:47835", True, "tcp://policy:47835", True),
    ],
)
def test_serve_arguments_create_engine_pilot_without_url_and_service_with_url(
    monkeypatch, flags, env_url, enabled, expected_url, created
):
    monkeypatch.delenv("GOTHIC_BODY_POLICY", raising=False)
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
            assert factory.call_args.kwargs["body_policy"] == args.body_policy
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


@pytest.mark.parametrize("policy", ["engine", "service"])
def test_body_policy_environment_selects_default(monkeypatch, policy):
    monkeypatch.setenv("GOTHIC_BODY_POLICY", policy)

    async def serve(args):
        assert args.body_policy == policy
        return 0

    monkeypatch.setattr(cli, "serve", serve)
    monkeypatch.setattr("sys.argv", ["gothic-ai", "serve"])
    with pytest.raises(SystemExit) as exit:
        cli.main()
    assert exit.value.code == 0


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
