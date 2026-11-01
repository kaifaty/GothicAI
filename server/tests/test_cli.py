"""CLI defaults start the director only for ordinary play."""

import asyncio
import importlib.util
import json
import pkgutil
from types import ModuleType, SimpleNamespace
from unittest.mock import AsyncMock, Mock

import pytest

from gothic_ai import cli
from gothic_ai.llm.off import ModelOff, OffBackend


@pytest.mark.parametrize(("scenario", "llm", "expected"), [
    ("m0", "auto", "off"),
    *[(scenario, "auto", "off") for scenario in (
        "deal", "contract", "debt", "xardas_money", "deal_demo",
        "bandit_parley", "bandit_bounty", "road_bandits", "calm", "companion", "neural-talk",
        "rescue", "rescue_self",
    )],
    ("haggle", "auto", "scenario haggle is live: set LLM=chatgpt|openai|local"),
    ("m0", "fake", "invalid choice: 'fake'"),
    ("m0", "chatgpt", "scenario m0 requires --llm off"),
    ("deal", "chatgpt", "scenario deal requires --llm off"),
    ("haggle", "off", "scenario haggle is live: set LLM=chatgpt|openai|local"),
    *[(scenario, llm, f"scenario {scenario} is live: set LLM=chatgpt|openai|local")
      for scenario in ("live_lies", "live_deals", "live_npc", "live_director")
      for llm in ("auto", "off")],
    *[(scenario, "chatgpt", "chatgpt") for scenario in ("live_npc", "live_director")],
    *[(suite, "fake", "invalid choice: 'fake'") for suite in ("live_lies", "live_deals")],
    (None, "auto", "--llm auto requires --scenario"),
    *[
        (scenario, llm, "off" if llm == "auto" else f"scenario {scenario} requires --llm off")
        for scenario in (
            "witness", "wrath", "amends", "amends_deal", "trace", "broken", "gate", "gate_vouch", "greg",
            "crime_ally", "crime_law", "crime_talk", "trace_crime", "cover", "cover_subject",
            "cheat", "frame", "crime_meet", "crime_director", "npc_director",
            "ground", "remembers", "cave", "seen", "group", "gossip", "gossip_demo",
            "repair", "authored", "character", "chain", "own_amend", "own_repair",
            "showrunner", "vanilla_line", "feats", "feat_fame",
            "m1", "m2", "m3", "m5", "m5c", "m5e", "tts",
            "combat_barks", "mercy", "hero_yield", "humiliate",
        )
        for llm in ("auto", "chatgpt")
    ],
])
def test_scenario_llm_groups(monkeypatch, capsys, scenario, llm, expected):
    serve = AsyncMock(return_value=0)
    monkeypatch.setattr(cli, "serve", serve)
    flags = ["--scenario", scenario] if scenario else []
    monkeypatch.setattr("sys.argv", ["gothic-ai", "serve", "--llm", llm, *flags])
    with pytest.raises(SystemExit) as exit:
        cli.main()
    if expected in {"off", "chatgpt"}:
        assert exit.value.code == 0
        assert serve.await_args.args[0].llm == expected
    else:
        assert exit.value.code == 2
        assert expected in capsys.readouterr().err
        serve.assert_not_awaited()


def test_scenario_groups_match_main_census():
    from gothic_ai.scenarios.groups import GIVEN, LIVE, OFF, RUN

    # Preserve groups and RUN settings from origin/main at 1591a1472ad5,
    # including the intentional exclusive display settings for body scenarios.
    # Omit new scenarios to avoid shared edits when adding them.
    # Update existing entries only for intentional changes to their group or RUN settings.
    expected = {
        "amends": ("GIVEN", {"save": True, "timeout": 240}),
        "amends_deal": ("GIVEN", {"save": True, "timeout": 240}),
        "authored": ("GIVEN", {"save": True, "timeout": 240}),
        "bandit_bounty": ("GIVEN", {"save": False, "timeout": 240}),
        "bandit_hold": ("OFF", {"save": False, "timeout": 240}),
        "bandit_parley": ("GIVEN", {"save": False, "timeout": 240}),
        "broken": ("GIVEN", {
            "save": True, "timeout": 240,
            "env": {"OPENGOTHIC_FRACTURE": "~/.local/share/gothic-ai/fracture"},
        }),
        "calm": ("GIVEN", {"save": False, "timeout": 240}),
        "cast": ("OFF", {"save": True, "timeout": 240}),
        "cave": ("GIVEN", {"save": False, "timeout": 240}),
        "chain": ("GIVEN", {"save": True, "timeout": 240}),
        "character": ("GIVEN", {"save": True, "timeout": 240}),
        "cheat": ("GIVEN", {"save": True, "timeout": 720}),
        "check_walk": ("OFF", {"save": False, "timeout": 240}),
        "chests": ("GIVEN", {"save": False, "timeout": 240}),
        "chests_lobart": ("LIVE", None),
        "combat_barks": ("GIVEN", {"save": False, "timeout": 240}),
        "companion": ("GIVEN", {"save": False, "timeout": 240, "exclusive": True}),
        "contract": ("GIVEN", {"save": False, "timeout": 240}),
        "cover": ("GIVEN", {"save": True, "timeout": 720}),
        "cover_subject": ("GIVEN", {"save": True, "timeout": 720}),
        "crime_ally": ("GIVEN", {"save": False, "timeout": 240}),
        "crime_director": ("GIVEN", {"save": True, "timeout": 900}),
        "crime_law": ("GIVEN", {"save": True, "timeout": 420}),
        "crime_meet": ("GIVEN", {"save": True, "timeout": 600}),
        "crime_react": ("OFF", {"save": True, "timeout": 420}),
        "crime_talk": ("GIVEN", {"save": True, "timeout": 600}),
        "crimes": ("OFF", {"save": True, "timeout": 420}),
        "day": ("GIVEN", {"save": True, "timeout": 480}),
        "deal": ("GIVEN", {"save": True, "timeout": 240}),
        "deal_demo": ("GIVEN", {"save": False, "timeout": 240}),
        "death_load": ("OFF", {"save": False, "timeout": 240}),
        "debt": ("GIVEN", {"save": True, "timeout": 240}),
        "errand": ("GIVEN", {"save": True, "timeout": 1500}),
        "errand_far": ("GIVEN", {"save": True, "timeout": 1500}),
        "errand_fork": ("GIVEN", {"save": True, "timeout": 1500}),
        "errand_give": ("GIVEN", {"save": True, "timeout": 240}),
        "feat_fame": ("GIVEN", {"save": False, "timeout": 420}),
        "feats": ("GIVEN", {"save": False, "timeout": 240}),
        "fight_reactions_bandits": ("GIVEN", {"save": False, "timeout": 300, "exclusive": True}),
        "fight_reactions_flee": ("GIVEN", {"save": False, "timeout": 180, "exclusive": True}),
        "fight_reactions_sight": ("OFF", {"save": False, "timeout": 180, "exclusive": True}),
        "fight_reactions_wolf": ("GIVEN", {"save": False, "timeout": 180, "exclusive": True}),
        "follow_back": ("OFF", {"save": False, "timeout": 240}),
        "frame": ("GIVEN", {"save": True, "timeout": 900}),
        "gate": ("GIVEN", {"save": False, "timeout": 240}),
        "gate_vouch": ("GIVEN", {"save": False, "timeout": 240}),
        "gossip": ("GIVEN", {"save": True, "timeout": 420, "env": {"DECIDER_URL": ""}}),
        "gossip_demo": ("GIVEN", {"save": True, "timeout": 240}),
        "graph": ("OFF", {"save": False, "timeout": 240}),
        "greg": ("GIVEN", {"save": False, "timeout": 240}),
        "ground": ("GIVEN", {"save": False, "timeout": 240}),
        "group": ("GIVEN", {"save": False, "timeout": 240}),
        "haggle": ("LIVE", None),
        "hearing": ("OFF", {"save": False, "timeout": 240}),
        "hero_yield": ("GIVEN", {"save": False, "timeout": 240}),
        "humiliate": ("GIVEN", {"save": False, "timeout": 240}),
        "initiative": ("GIVEN", {"save": True, "timeout": 240}),
        "interject": ("LIVE", None),
        "knows": ("OFF", {"save": False, "timeout": 240}),
        "lead": ("GIVEN", {"save": True, "timeout": 540}),
        "lie": ("GIVEN", {"save": True, "timeout": 240}),
        "live_deals": ("LIVE", None),
        "live_director": ("LIVE", None),
        "live_lies": ("LIVE", None),
        "live_npc": ("LIVE", None),
        "load_rewind": ("OFF", {"save": False, "timeout": 240}),
        "m0": ("OFF", {"save": False, "timeout": 240}),
        "m1": ("GIVEN", {"save": False, "timeout": 240}),
        "m2": ("GIVEN", {"save": False, "timeout": 240}),
        "m3": ("GIVEN", {"save": False, "timeout": 240}),
        "m4": ("OFF", {"save": False, "timeout": 240}),
        "m5": ("GIVEN", {"save": False, "timeout": 240}),
        "m5b": ("OFF", {"save": False, "timeout": 240}),
        "m5c": ("GIVEN", {"save": False, "timeout": 240}),
        "m5e": ("GIVEN", {"save": False, "timeout": 240}),
        "m6": ("OFF", {"save": False, "timeout": 240}),
        "meet": ("GIVEN", {"save": True, "timeout": 600}),
        "meet_missed": ("GIVEN", {"save": True, "timeout": 700}),
        "memprobe": ("OFF", {
            "save": False, "timeout": 1200,
            "env": {"MEMPROBE_CYCLES": "8", "MEMPROBE_PHASES": "wave,save,load,spawn,teleport"},
        }),
        "mercy": ("GIVEN", {"save": False, "timeout": 240}),
        "neural": ("OFF", {"save": False, "timeout": 240, "exclusive": True}),
        "neural-companion": ("OFF", {"save": False, "timeout": 240, "exclusive": True}),
        "neural-group": ("OFF", {"save": False, "timeout": 240, "exclusive": True}),
        "neural-load": ("OFF", {"save": False, "timeout": 240, "exclusive": True}),
        "neural-soak": ("OFF", {
            "save": False, "timeout": 900, "exclusive": True,
            "env": {"GOTHIC_SOAK_SECONDS": "300", "BENCH_CLASS": "quiet"},
        }),
        "neural-still": ("OFF", {"save": False, "timeout": 1200, "exclusive": True}),
        "neural-talk": ("GIVEN", {"save": False, "timeout": 240, "exclusive": True}),
        "noise": ("GIVEN", {"save": True, "timeout": 240}),
        "npc_deeds": ("OFF", {"save": True, "timeout": 720}),
        "npc_director": ("GIVEN", {"save": True, "timeout": 720}),
        "npc_far": ("OFF", {"save": True, "timeout": 720}),
        "npc_will": ("OFF", {"save": True, "timeout": 240}),
        "occasions": ("OFF", {"save": True, "timeout": 240}),
        "orders": ("GIVEN", {"save": False, "timeout": 360}),
        "own": ("OFF", {"save": True, "timeout": 240}),
        "own_amend": ("GIVEN", {"save": True, "timeout": 240}),
        "own_repair": ("GIVEN", {"save": True, "timeout": 240}),
        "person": ("OFF", {"save": True, "timeout": 240}),
        "places": ("OFF", {"save": False, "timeout": 240}),
        "programs": ("OFF", {"save": False, "timeout": 240}),
        "quest_law": ("OFF", {"save": True, "timeout": 600}),
        "questlog": ("OFF", {"save": True, "timeout": 300}),
        "remembers": ("GIVEN", {"save": False, "timeout": 240}),
        "repair": ("GIVEN", {"save": False, "timeout": 240}),
        "requires": ("OFF", {"save": True, "timeout": 240}),
        "rescue": ("GIVEN", {"save": False, "timeout": 240, "exclusive": True}),
        "rescue_self": ("GIVEN", {"save": False, "timeout": 240, "exclusive": True}),
        "road_bandits": ("GIVEN", {"save": False, "timeout": 240}),
        "rumor": ("GIVEN", {"save": True, "timeout": 240}),
        "see": ("OFF", {"save": False, "timeout": 240}),
        "seen": ("GIVEN", {"save": True, "timeout": 240}),
        "showrunner": ("GIVEN", {"save": True, "timeout": 240}),
        "showrunner_live": ("LIVE", None),
        "stages": ("OFF", {"save": True, "timeout": 240}),
        "surrender_live": ("LIVE", None),
        "svm_lines": ("OFF", {"save": False, "timeout": 240}),
        "trace": ("GIVEN", {"save": True, "timeout": 240}),
        "trace_crime": ("GIVEN", {
            "save": True, "timeout": 240,
            "env": {"OPENGOTHIC_FRACTURE": "~/.local/share/gothic-ai/fracture"},
        }),
        "trigger": ("OFF", {"save": True, "timeout": 240}),
        "tts": ("GIVEN", {"save": False, "timeout": 240}),
        "vanilla_line": ("GIVEN", {"save": True, "timeout": 420}),
        "vanilla_line_live": ("LIVE", None),
        "vcheck": ("GIVEN", {"save": True, "timeout": 240}),
        "vgive": ("GIVEN", {"save": False, "timeout": 240}),
        "vlie": ("GIVEN", {"save": True, "timeout": 240}),
        "vspare": ("GIVEN", {"save": False, "timeout": 900}),
        "websocket": ("OFF", {"save": True, "timeout": 240}),
        "witness": ("GIVEN", {"save": True, "timeout": 240}),
        "wolf": ("OFF", {"save": False, "timeout": 240}),
        "wolf_load": ("OFF", {"save": False, "timeout": 240}),
        "wrath": ("GIVEN", {"save": False, "timeout": 240}),
        "xardas_money": ("GIVEN", {"save": False, "timeout": 240}),
    }
    for group, actual in (("OFF", OFF), ("GIVEN", GIVEN), ("LIVE", LIVE)):
        assert actual & expected.keys() == {
            name for name, (declared, _) in expected.items() if declared == group
        }
    assert {name: run for name, run in RUN.items() if name in expected} == {
        name: run for name, (_, run) in expected.items() if run is not None
    }


def test_scenario_groups_reject_duplicate_names(monkeypatch):
    from gothic_ai.scenarios import groups

    first = ModuleType("gothic_ai.scenarios.first")
    second = ModuleType("gothic_ai.scenarios.second")
    first.SCENARIOS = {"duplicate": ("OFF", {"save": False, "timeout": 240})}
    second.SCENARIOS = first.SCENARIOS
    modules = {module.__name__: module for module in (first, second)}
    monkeypatch.setattr(pkgutil, "iter_modules", Mock(return_value=[
        SimpleNamespace(name=name) for name in modules
    ]))
    monkeypatch.setattr(importlib, "import_module", modules.__getitem__)
    spec = importlib.util.spec_from_file_location(groups.__name__, groups.__file__)
    fresh = importlib.util.module_from_spec(spec)
    with pytest.raises(ValueError, match=(
        "scenario duplicate declared in both gothic_ai.scenarios.first and gothic_ai.scenarios.second"
    )):
        spec.loader.exec_module(fresh)


def test_every_cli_scenario_has_exactly_one_model_group(monkeypatch, capsys):
    groups = (cli.OFF, cli.GIVEN, cli.LIVE)
    parse_args = cli.argparse.ArgumentParser.parse_args
    choices = []

    def capture(parser, *args, **kwargs):
        if parser.prog == "gothic-ai":
            sub = next(action for action in parser._actions if action.dest == "command")
            serve = sub.choices["serve"]
            choices.extend(next(action.choices for action in serve._actions if action.dest == "scenario"))
        return parse_args(parser, *args, **kwargs)

    monkeypatch.setattr(cli.argparse.ArgumentParser, "parse_args", capture)
    monkeypatch.setattr("sys.argv", ["gothic-ai", "--help"])
    with pytest.raises(SystemExit) as exit:
        cli.main()
    assert exit.value.code == 0
    assert choices
    for scenario in choices:
        assert sum(scenario in group for group in groups) == 1, (
            f"scenario {scenario} must declare exactly one model group in its module"
        )
    assert set(choices) == set().union(*groups)

    monkeypatch.setattr(cli, "OFF", cli.OFF - {"m0"})
    monkeypatch.setattr("sys.argv", ["gothic-ai", "serve", "--scenario", "m0", "--llm", "auto"])
    with pytest.raises(SystemExit) as exit:
        cli.main()
    assert exit.value.code == 2
    assert "scenario m0 has no model group (docs/e2e.md)" in capsys.readouterr().err


def test_cards_command_generates_from_the_connected_world(monkeypatch, capsys):
    graph = object()
    people = [{"npc": "BAU_950_LOBART", "name": "Lobart", "guild": 8, "routine": []}]
    link = SimpleNamespace(wait_world=AsyncMock(), story_people=AsyncMock(return_value=people))
    backend = SimpleNamespace(aclose=AsyncMock())
    subject = SimpleNamespace(npc="BAU_950_LOBART")
    card = object()

    class Server:
        def __init__(self, host, port):
            assert (host, port) == ("127.0.0.1", 47817)

        async def serve_forever(self):
            await asyncio.Future()

        async def wait_link(self):
            return link

    monkeypatch.setattr(cli, "GameServer", Server)
    monkeypatch.setattr(cli, "_story_graph", Mock(return_value=graph))
    monkeypatch.setattr(cli, "_backend", Mock(return_value=backend))
    monkeypatch.setattr(cli.characters, "load", Mock(return_value={subject.npc: object()}))
    subjects = Mock(return_value=[subject])
    monkeypatch.setattr(cli.characters, "subjects", subjects)
    write = AsyncMock(return_value=({subject.npc: card}, []))
    monkeypatch.setattr(cli.characters, "write", write)
    save = Mock()
    monkeypatch.setattr(cli.characters, "save", save)
    monkeypatch.setattr("sys.argv", ["gothic-ai", "cards", "--only", " bau_950_lobart, "])
    with pytest.raises(SystemExit) as exit:
        cli.main()
    assert exit.value.code == 0
    subjects.assert_called_once_with(graph, people, {subject.npc})
    write.assert_awaited_once_with(backend, [subject])
    save.assert_called_once_with({subject.npc: card})
    backend.aclose.assert_awaited_once()
    assert "1 of 1 cards written" in capsys.readouterr().out

    monkeypatch.setattr("sys.argv", ["gothic-ai", "serve", "--scenario", "cards"])
    with pytest.raises(SystemExit) as exit:
        cli.main()
    assert exit.value.code == 2
    assert "invalid choice: 'cards'" in capsys.readouterr().err
    monkeypatch.setattr("sys.argv", ["gothic-ai", "cards", "--llm", "off"])
    with pytest.raises(SystemExit) as exit:
        cli.main()
    assert exit.value.code == 2
    assert "cards requires --llm chatgpt|openai|local" in capsys.readouterr().err


def test_story_graph_is_optional_before_the_game_publishes_its_base(tmp_path, monkeypatch, caplog):
    game = tmp_path / "game"
    game.mkdir()
    monkeypatch.delenv("OPENGOTHIC_BASE", raising=False)
    monkeypatch.setenv("OPENGOTHIC_BASE_CACHE", str(tmp_path / "cache"))
    assert cli._story_graph(SimpleNamespace(story_graph=None), game) is None
    assert f"no ready base for {game}" in caplog.text
    assert "run the game once with the installation or set OPENGOTHIC_BASE" in caplog.text


@pytest.mark.parametrize(("scenario", "backend_kind", "outcome", "code"), [
    (None, "off", "clean", 0),
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
    parley = Mock(wraps=cli.Parley)
    monkeypatch.setattr(cli, "Parley", parley)
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
        assert parley.call_args.kwargs["hail"] is (scenario is None)
        assert markers() == (["scenario m0: checks started"] if scenario is not None else [])
        assert run_scenario_mock.await_count == int(scenario is not None)
        records = [record.getMessage() for record in caplog.records]
        checks = {key: next((line for line in records if f": {key}: " in line), "")
                  for key in ("model_off", "givens_used")}
        if backend_kind == "off" and scenario is not None:
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
        (["--scenario", "showrunner", "--llm", "off"], False),
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
    "flags", [
        [], ["--scenario", "crimes", "--llm", "auto"],
        ["--scenario", "crime_meet", "--llm", "auto"], ["--no-crimes"],
    ]
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
@pytest.mark.parametrize("parley", [False, True])
def test_deed_control_is_independent_of_the_planner_and_threat_switch(crimes, planner, hold_threats, parley):
    args = SimpleNamespace(no_crimes=not crimes, no_hold_threats=not hold_threats, no_parley=not parley)
    command = json.loads(cli._perception_control(args, planner=planner).to_json())
    assert command["hold"] == (planner and hold_threats)
    assert command.get("deeds", False) == crimes
    assert ("deeds" in command) == crimes
    assert command["bandits"] == parley


@pytest.mark.parametrize("flags", [
    [], ["--no-cover"], ["--no-crimes"], ["--scenario", "cover", "--llm", "off"],
])
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


@pytest.mark.parametrize("flags", [
    [], ["--no-npc-cheat"], ["--no-crimes"], ["--scenario", "cheat", "--llm", "auto"],
])
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
        (None, "1", False, None, "off", False, "service"),
        ("neural", "0", True, None, "off", True, "engine"),
        ("neural", "0", True, None, "off", False, "service"),
        ("feats", "0", True, None, "off", True, "engine"),
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
