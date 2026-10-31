import hashlib
import json
import os
import subprocess
from pathlib import Path

import pytest

from gothic_ai.authoring import ITEMS
from gothic_ai.gamedata import base, ou, storygraph
from gothic_ai.gamedata.storygraph import is_known, load, render, variables
from tests.test_dialogue_text import _ready_base, parse_ou_bin

REPO = Path(__file__).resolve().parents[2]
TOOL = REPO / "OpenGothic" / "ai_bridge" / "storygraph" / "build" / "storygraph"
GAME_DIR = Path(os.environ.get("GOTHIC_DIR", "/Volumes/DevSSD/Dev/games/Gothic II"))
# original-format saves (ZenGin), one folder each; see docs/story-graph.md, stage 2
SAVES = Path(os.environ.get("GOTHIC_SAVES", Path.home() / ".local/share/gothic-ai/saves/original"))


@pytest.fixture
def ready_base(monkeypatch):
    if not TOOL.is_file():
        pytest.skip("needs the storygraph tool")
    try:
        selected = base.resolve(GAME_DIR)
    except RuntimeError as error:
        pytest.skip(f"needs a ready base selected by GOTHIC_DIR or OPENGOTHIC_BASE: {error}")
    with selected:
        monkeypatch.setenv("OPENGOTHIC_BASE", str(selected.path))
        yield selected


@pytest.fixture
def original_install():
    directory = os.environ.get("GOTHIC_INSTALL")
    if not directory:
        pytest.skip("needs GOTHIC_INSTALL pointing to an original installation for physical OU.BIN bytes")
    return Path(directory).expanduser()


KAPITEL_LT_3 = ["<", ["var", "KAPITEL"], ["int", 3]]
RUEBEN_RUNNING = ["==", ["var", "MIS_LOBART_RUEBEN"], ["int", 1, "LOG_RUNNING"]]
HAS_BEET = [">=", ["call", "NPC_HASITEMS", ["inst", "OTHER"], ["sym", "ITPL_BEET"]], ["int", 1]]
KNOWS_KLEIDUNG = ["call", "NPC_KNOWSINFO", ["inst", "OTHER"], ["sym", "DIA_LOBART_KLEIDUNG"]]
HAS_20 = [">=", ["call", "NPC_HASITEMS", ["inst", "OTHER"], ["sym", "ITPL_BEET"]], ["int", 20]]

# A small piece of what storygraph writes for Lobart's turnip quest.
GRAPH = {
    "version": 1,
    "nodes": [
        {
            "id": "DIA_LOBART_WORKNOW",
            "kind": "dialog",
            "npc": "BAU_950_LOBART",
            "description": "Я ищу работу.",
            "nr": 2,
            "important": False,
            "permanent": False,
            "trade": False,
            "condition": ["&&", KNOWS_KLEIDUNG, KAPITEL_LT_3],
            "lifted": True,
            "reads": ["KAPITEL"],
            "knows": ["DIA_LOBART_KLEIDUNG"],
            "writes": [],
            "effects": [
                {
                    "do": "line",
                    "id": "DIA_LOBART_WORKNOW_15_00",
                    "text": "Я ищу работу.",
                    "speaker": ["inst", "OTHER"],
                    "listener": ["inst", "SELF"],
                },
                {"do": "choice", "node": "DIA_LOBART_WORKNOW_OK", "text": "Хорошо..."},
            ],
        },
        {
            "id": "DIA_LOBART_WORKNOW_OK",
            "kind": "choice",
            "npc": "BAU_950_LOBART",
            "description": "Хорошо...",
            "parents": ["DIA_LOBART_WORKNOW"],
            "writes": ["MIS_LOBART_RUEBEN"],
            "effects": [
                {
                    "do": "journal_topic",
                    "topic": "TOPIC_RUEBEN",
                    "title": "Убрать поле репы",
                    "section": ["int", 0, "LOG_MISSION"],
                },
                {"do": "set", "var": "MIS_LOBART_RUEBEN", "value": ["int", 1, "LOG_RUNNING"]},
            ],
        },
        {
            "id": "DIA_LOBART_RUEBENRUNNING",
            "kind": "dialog",
            "npc": "BAU_950_LOBART",
            "description": "Вот твоя репа!",
            "nr": 1,
            "important": False,
            "permanent": True,
            "trade": False,
            "condition": ["&&", KAPITEL_LT_3, RUEBEN_RUNNING, HAS_BEET],
            "lifted": True,
            "reads": ["KAPITEL", "MIS_LOBART_RUEBEN"],
            "knows": [],
            "writes": ["MIS_LOBART_RUEBEN"],
            "effects": [
                {"do": "set", "var": "MIS_LOBART_RUEBEN", "value": ["int", 2, "LOG_SUCCESS"], "when": HAS_20},
                {"do": "xp", "amount": ["int", 50, "XP_LOBARTHOLRUEBEN"], "when": HAS_20},
            ],
        },
        {
            "id": "DIA_HILDA_RUEBEN",
            "kind": "dialog",
            "npc": "BAU_951_HILDA",
            "description": "Лобарт прислал меня за супом.",
            "nr": 1,
            "important": False,
            "permanent": False,
            "trade": False,
            "condition": RUEBEN_RUNNING,
            "lifted": True,
            "reads": ["MIS_LOBART_RUEBEN"],
            "knows": [],
            "writes": [],
            "effects": [
                {
                    "do": "give",
                    "from": ["inst", "SELF"],
                    "to": ["inst", "OTHER"],
                    "item": ["sym", "ITFO_STEW"],
                    "amount": ["int", 1],
                },
            ],
        },
    ],
    "functions": {},
    "instances": {"HILDA": "BAU_951_HILDA"},
    "vars": {
        "MIS_LOBART_RUEBEN": {
            "readers": ["DIA_LOBART_RUEBENRUNNING", "DIA_HILDA_RUEBEN"],
            "writers": ["DIA_LOBART_RUEBENRUNNING", "DIA_LOBART_WORKNOW_OK"],
            "others": [],
        },
    },
    "quests": [
        {
            "var": "MIS_LOBART_RUEBEN",
            "topics": [{"topic": "TOPIC_RUEBEN", "title": "Убрать поле репы"}],
            "running": ["DIA_LOBART_WORKNOW_OK"],
            "success": ["DIA_LOBART_RUEBENRUNNING"],
            "failed": [],
            "obsolete": [],
            "other": [],
        },
    ],
}


@pytest.fixture
def graph(tmp_path):
    path = tmp_path / "story.json"
    path.write_text(json.dumps(GRAPH, ensure_ascii=False), encoding="utf-8")
    return load(path)


def test_ensure_caches_by_base_fingerprint_and_publishes_atomically(tmp_path, monkeypatch):
    game = tmp_path / "game"
    game.mkdir()
    tool = tmp_path / "storygraph"
    tool.touch()
    cache = tmp_path / "story.json"
    cache.write_text(json.dumps(GRAPH), encoding="utf-8")
    calls = []
    before = cache.read_bytes()

    def rebuild(cmd, **kwargs):
        assert cmd == [str(tool), str(tmp_path / "base"), "--json", str(cache.with_suffix(".tmp"))]
        assert kwargs["env"]["OPENGOTHIC_BASE"] == str(tmp_path / "base")
        assert kwargs["check"] and kwargs["capture_output"] and kwargs["timeout"] == 1800
        assert cache.read_bytes() == before
        sources = {
            "program_native_sha256": "a" * 64,
            "program_source_sha256": "b" * 64,
            "ou_native_sha256": ("c" if not calls else "d") * 64,
            "ou_source_sha256": "f" * 64,
            "converter_sha256": "e" * 64,
        }
        Path(cmd[-1]).write_text(json.dumps({**GRAPH, "sources": sources}), encoding="utf-8")
        calls.append(cmd)
        return subprocess.CompletedProcess(cmd, 0)

    monkeypatch.setattr(storygraph.subprocess, "run", rebuild)
    for fingerprint in ("a", "b"):
        _ready_base(tmp_path, monkeypatch, fingerprint=fingerprint * 64)
        # A newer graph cannot hide a changed selected base.
        newer = tool.stat().st_mtime + 60
        os.utime(cache, (newer, newer))
        assert storygraph.ensure(game, cache, tool) == cache
        before = cache.read_bytes()
        assert storygraph.ensure(game, cache, tool) == cache
        assert load(cache).node("DIA_LOBART_WORKNOW").description == "Я ищу работу."
    assert len(calls) == 2
    assert json.loads(cache.read_text())["sources"]["ou_native_sha256"] == "d" * 64


def test_ensure_failed_rebuild_preserves_existing_cache(tmp_path, monkeypatch, caplog):
    game = tmp_path / "game"
    game.mkdir()
    _ready_base(tmp_path, monkeypatch)
    tool = tmp_path / "storygraph"
    tool.touch()
    cache = tmp_path / "story.json"
    cache.write_text(json.dumps(GRAPH), encoding="utf-8")
    before = cache.read_bytes()

    def fail(cmd, **kwargs):
        Path(cmd[-1]).write_text("partial output", encoding="utf-8")
        raise subprocess.CalledProcessError(1, cmd)

    monkeypatch.setattr(storygraph.subprocess, "run", fail)
    assert storygraph.ensure(game, cache, tool) is None
    assert cache.read_bytes() == before
    assert "failed" in caplog.text


def test_ensure_without_converter_reuses_only_cache_for_current_base(tmp_path, monkeypatch, caplog):
    game = tmp_path / "game"
    game.mkdir()
    _ready_base(tmp_path, monkeypatch)
    tool = tmp_path / "missing-converter"
    cache = tmp_path / "story.json"
    cache.write_text(json.dumps(GRAPH), encoding="utf-8")
    stamp = cache.with_suffix(".base.json")
    source = {
        "base_fingerprint": "a" * 64,
        "algorithm_version": storygraph.ALGORITHM_VERSION,
        "converter_sha256": "b" * 64,
    }
    stamp.write_text(json.dumps(source))
    assert storygraph.ensure(game, cache, tool) == cache
    assert "converter unavailable, converter freshness unverified" in caplog.text
    for changed in (
        {**source, "base_fingerprint": "c" * 64},
        {**source, "algorithm_version": storygraph.ALGORITHM_VERSION + 1},
    ):
        stamp.write_text(json.dumps(changed))
        assert storygraph.ensure(game, cache, tool) is None
    stamp.unlink()
    assert storygraph.ensure(game, cache, tool) is None


def test_quest_and_its_nodes(graph):
    quest = graph.quest("mis_lobart_rueben")
    assert quest.title == "Убрать поле репы"
    assert quest.running == {"DIA_LOBART_WORKNOW_OK"} and quest.success == {"DIA_LOBART_RUEBENRUNNING"}
    assert graph.quests_of("BAU_950_LOBART") == [quest]

    offer = graph.node(next(iter(quest.running)))
    assert offer.is_choice and offer.parents == ["DIA_LOBART_WORKNOW"]
    work = graph.node("DIA_LOBART_WORKNOW")
    assert work.choices() == ["DIA_LOBART_WORKNOW_OK"]
    assert work.lines() == [("other", "DIA_LOBART_WORKNOW_15_00", "Я ищу работу.")]
    by_nr = [n.id for n in graph.dialogs_of("bau_950_lobart")]
    assert by_nr == ["DIA_LOBART_RUEBENRUNNING", "DIA_LOBART_WORKNOW"]
    assert graph.npc("Hilda") == "BAU_951_HILDA" and graph.npc("BAU_950_LOBART") == "BAU_950_LOBART"


def test_the_things_of_an_npcs_story(graph):
    # his own dialogs, and the dialogs of others in his quest: the stew is named only in Hilda's
    assert graph.things_of("bau_950_lobart") == {"ITPL_BEET", "ITFO_STEW"}
    assert graph.things_of("BAU_951_HILDA") == {"ITFO_STEW", "ITPL_BEET"}  # she reads his quest
    assert graph.things_of("NONE_100_XARDAS") == set()


def test_all_script_things_include_nested_conditions_effects_and_choices(graph):
    graph.node("DIA_LOBART_WORKNOW").condition = [
        "&&",
        KNOWS_KLEIDUNG,
        ["call", "NPC_HASITEMS", ["inst", "OTHER"], ["sym", "ITMI_SAW"]],
    ]
    graph.node("DIA_LOBART_WORKNOW_OK").condition_effects = [
        {"do": "call", "call": ["fn", "CHECK_ITEM", ["sym", "ITMI_RAKE"]]}
    ]
    graph.node("DIA_HILDA_RUEBEN").effects.append(
        {"do": "xp", "amount": ["int", 5], "when": ["!=", ["sym", "ITMI_HAMMER"], ["int", 0]]}
    )
    assert graph.things() == {"ITPL_BEET", "ITFO_STEW", "ITMI_SAW", "ITMI_RAKE", "ITMI_HAMMER"}


def test_expressions(graph):
    done = graph.node("DIA_LOBART_RUEBENRUNNING")
    assert render(done.condition) == (
        "(KAPITEL < 3) && (MIS_LOBART_RUEBEN == LOG_RUNNING) && (NPC_HASITEMS(other, ITPL_BEET) >= 1)"
    )
    assert variables(done.condition) == {"KAPITEL", "MIS_LOBART_RUEBEN"}
    assert is_known(done.condition) and not is_known(["&&", KAPITEL_LT_3, ["unknown", "guard too complex"]])
    assert render(done.effects[1]["when"]) == "NPC_HASITEMS(other, ITPL_BEET) >= 20"
    defeated = ["field", ["inst", "ROD"], "AIVAR", 47, "AIV_DEFEATEDBYPLAYER"]
    cases = ["cases", ["==", defeated, ["int", 1, "TRUE"]], ["int", 1]]
    assert render(cases) == "case (ROD.aivar[AIV_DEFEATEDBYPLAYER] == TRUE) -> 1; else 0"


def test_other_versions_are_refused(tmp_path):
    path = tmp_path / "story.json"
    path.write_text(json.dumps({**GRAPH, "version": 2}), encoding="utf-8")
    with pytest.raises(ValueError, match="version 2"):
        load(path)


@pytest.mark.usefixtures("ready_base")
def test_the_real_game(tmp_path):
    out = tmp_path / "story.json"
    subprocess.run([str(TOOL), str(GAME_DIR), "--json", str(out)], check=True, capture_output=True)
    graph = load(out)

    dialogs = [n for n in graph.nodes.values() if not n.is_choice]
    assert len(dialogs) > 4000
    assert sum(n.lifted for n in dialogs) / len(dialogs) >= 0.95
    # The broad authored supply list must remain outside every script dialog and choice.
    assert 30 <= len(ITEMS) == len(set(ITEMS)) <= 40
    assert set(ITEMS).isdisjoint(graph.things()), sorted(set(ITEMS) & graph.things())

    quest = graph.quest("MIS_LOBART_RUEBEN")
    assert quest.topics == {"TOPIC_RUEBEN": "Убрать поле репы"}
    assert quest.running == {"DIA_LOBART_WORKNOW_OK", "DIA_LOBART_WORKNOW_WANNAFOOLME"}
    assert quest.success == {"DIA_LOBART_RUEBENRUNNING"}
    done = graph.node("DIA_LOBART_RUEBENRUNNING")
    assert render(done.condition) == (
        "(KAPITEL < 3) && (MIS_LOBART_RUEBEN == LOG_RUNNING) && (NPC_HASITEMS(other, ITPL_BEET) >= 1)"
    )
    success = next(e for e in done.effects if e["do"] == "set" and e["var"] == "MIS_LOBART_RUEBEN")
    assert render(success["when"]) == "NPC_HASITEMS(other, ITPL_BEET) >= 20"
    hilda = next(e for e in done.effects if e["do"] == "set" and e["var"] == "MIS_LOBART_RUEBENTOHILDA")
    assert render(hilda["when"]) == "!NPC_ISDEAD(HILDA) && (NPC_HASITEMS(other, ITPL_BEET) >= 20)"
    assert graph.npc("HILDA") == "BAU_951_HILDA"
    # Cavalorn's stolen letter is named only in Vatras's dialog, of the quest Cavalorn's move
    assert "ITWR_SATURASFIRSTMESSAGE_ADDON_SEALED" in graph.things_of("BAU_4300_ADDON_CAVALORN")
    # Franco's death makes two quests obsolete in ZS_Dead, outside any dialog
    assert "ZS_DEAD" in graph.vars["MIS_HLPLOGAN"].others
    # the gate guard stores AIV_PASSGATE and then checks it: the check sees the store (found by the VM check)
    assert "PAL_205_SCHONMALREINGELASSEN" in variables(graph.node("DIA_PAL_205_TORWACHE_FIRSTWARN").condition)


def test_native_ou_matches_the_complete_legacy_server_view(ready_base, original_install, monkeypatch):
    index = ready_base.json("outputunits/outputunits.json")
    source = index["sources"][index["server"]["ou_py"]]
    monkeypatch.setenv("STORYGRAPH_TOOL", str(TOOL))
    messages = ou.native_messages(GAME_DIR)
    assert messages is not None
    assert ou.lines_from_messages(messages) == parse_ou_bin((original_install / source["path"]).read_bytes())


def test_native_ou_metadata_and_overrides_preserve_the_graph(ready_base, tmp_path):
    index = ready_base.json("outputunits/outputunits.json")
    provenance = ready_base.json("sources/sources.json")["server"]
    result = subprocess.run(
        [str(TOOL), str(GAME_DIR), "--ou-messages"], check=True, capture_output=True, timeout=1800
    )
    exported = json.loads(result.stdout)
    source = index["sources"][index["server"]["ou_py"]]
    record = ready_base.path / "outputunits" / f"{source['record']}.json"
    assert exported["format"] == "gothic-ou-messages" and exported["version"] == 1
    assert exported["source"] == {"path": source["path"], "sha256": source["sha256"]}
    assert exported["source"] == {
        "path": provenance["ou_py"]["path"], "sha256": provenance["ou_py"]["sha256"]
    }
    assert exported["native_sha256"] == hashlib.sha256(record.read_bytes()).hexdigest()

    def graph_with(*args):
        out = tmp_path / "story.json"
        subprocess.run(
            [str(TOOL), str(GAME_DIR), "--json", str(out), *args],
            check=True, capture_output=True, timeout=1800,
        )
        return json.loads(out.read_text())

    graph = graph_with()
    source = index["sources"][index["host"][provenance["storygraph_ou"]["path"]]]
    record = ready_base.path / "outputunits" / f"{source['record']}.json"
    programs = ready_base.json("programs/programs.json")
    program = programs["sources"][programs["server"]["storygraph_dat"]]
    program_file = ready_base.path / "programs" / program["program"]
    assert program["sha256"] == provenance["storygraph_dat"]["sha256"]
    assert source["sha256"] == provenance["storygraph_ou"]["sha256"]
    sources = graph.pop("sources")
    assert sources == {
        "program_native_sha256": hashlib.sha256(
            program_file.with_suffix(".json").read_bytes() + program_file.with_suffix(".bin").read_bytes()
        ).hexdigest(),
        "program_source_sha256": program["sha256"],
        "ou_native_sha256": hashlib.sha256(record.read_bytes()).hexdigest(),
        "ou_source_sha256": source["sha256"],
        "converter_sha256": hashlib.sha256(TOOL.read_bytes()).hexdigest(),
    }
    arbitrary = tmp_path / "own-ou.json"
    arbitrary.write_bytes(record.read_bytes())
    own = graph_with("--ou", str(arbitrary))
    assert own.pop("sources") == {**sources, "ou_source_sha256": None}
    assert own == graph
    empty_game = tmp_path / "game-without-installation"
    empty_game.mkdir()
    out = tmp_path / "without-installation.json"
    subprocess.run(
        [str(TOOL), str(empty_game), "--json", str(out)], check=True, capture_output=True, timeout=1800
    )
    without_installation = json.loads(out.read_text())
    assert without_installation.pop("sources") == sources
    assert without_installation == graph
    exported_without_installation = subprocess.run(
        [str(TOOL), str(empty_game), "--ou-messages"], check=True, capture_output=True, timeout=1800
    )
    assert json.loads(exported_without_installation.stdout) == exported


def test_storygraph_rejects_raw_ou(ready_base, original_install, tmp_path):
    source = ready_base.json("sources/sources.json")["server"]["storygraph_ou"]["path"]
    selected = original_install / source
    rejected = subprocess.run(
        [str(TOOL), str(GAME_DIR), "--ou", str(selected)], capture_output=True, timeout=1800
    )
    assert rejected.returncode != 0 and b"--ou requires a native" in rejected.stderr
    raw = tmp_path / "external-ou.bin"
    raw.write_bytes(selected.read_bytes())
    failed = subprocess.run(
        [str(TOOL), str(GAME_DIR), "--ou", str(raw)], capture_output=True, timeout=1800
    )
    assert failed.returncode != 0 and b"invoke content-convert outputunits-file" in failed.stderr


@pytest.mark.usefixtures("ready_base")
def test_native_storygraph_matches_the_complete_p13_graph(tmp_path):
    baseline = os.environ.get("STORYGRAPH_P13_JSON")
    if not baseline:
        pytest.skip("needs the independently generated P13 graph in STORYGRAPH_P13_JSON")
    out = tmp_path / "story.json"
    subprocess.run(
        [str(TOOL), str(GAME_DIR), "--json", str(out)], check=True, capture_output=True, timeout=1800
    )
    old, new = json.loads(Path(baseline).read_text()), json.loads(out.read_text())
    old.pop("sources")
    new.pop("sources")
    assert new == old


def _run(*args):
    return subprocess.run([str(TOOL), str(GAME_DIR), *args], capture_output=True, text=True, errors="replace")


@pytest.mark.usefixtures("ready_base")
def test_conditions_match_the_vm_in_random_states():
    r = _run("--fuzz", "200")
    assert r.returncode == 0, r.stdout[-3000:]
    assert "DIFFER in 0 dialogs" in r.stdout


def _saves():
    return sorted(p for p in SAVES.iterdir() if (p / "SAVEDAT.SAV").exists()) if SAVES.is_dir() else []


@pytest.mark.usefixtures("ready_base")
@pytest.mark.skipif(not _saves(), reason="needs original-format saves in GOTHIC_SAVES")
def test_conditions_match_the_vm_on_original_saves():
    r = _run(*[a for s in _saves() for a in ("--check", str(s))])
    assert r.returncode == 0, r.stdout[-3000:]
    assert ", DIFFER 0;" in r.stdout.splitlines()[-1]


@pytest.mark.usefixtures("ready_base")
def test_effects_match_the_vm_in_random_states():
    # what dialogs and choices do (calls in order with their arguments, globals left), 20 states each
    r = _run("--fuzz", "20", "--effects")
    assert r.returncode == 0, r.stdout[-3000:]
    assert "DIFFER 0;" in r.stdout and "VM failed 0" in r.stdout


@pytest.mark.usefixtures("ready_base")
@pytest.mark.skipif(not _saves(), reason="needs original-format saves in GOTHIC_SAVES")
def test_effects_match_the_vm_on_original_saves():
    r = _run(*[a for s in _saves() for a in ("--check", str(s))], "--effects")
    assert r.returncode == 0, r.stdout[-3000:]
    total = next(line for line in r.stdout.splitlines() if "effects (all saves)" in line)
    assert "DIFFER 0;" in total and "VM failed 0" in total


@pytest.mark.usefixtures("ready_base")
def test_a_read_after_a_call_that_writes_it_is_not_known():
    # Vatras raises the hero's max mana through B_RaiseAttribute, then sets his mana to it: found by the
    # effects check; the value after the call is not the one before it
    r = _run("--node", "DIA_ADDON_VATRAS_FREE")
    assert "set other.ATTRIBUTE[ATR_MANA] = ?other.attribute[ATR_MANA_MAX] after B_RAISEATTRIBUTE" in r.stdout


# programs: the graph runs a function itself (docs/story-graph.md, «Исполнение без VM»)


@pytest.mark.usefixtures("ready_base")
def test_programs_match_the_vm_in_random_states():
    # every condition, dialog and choice run by the Interpreter against the VM, 5 states each; the engine
    # remembers items given and taken, so reading the inventory after changing it is checked too
    r = _run("--fuzz", "5", "--exec")
    assert r.returncode == 0, r.stdout[-3000:]
    assert "DIFFER 0;" in r.stdout and "VM failed 0" in r.stdout and "not a program 0 " in r.stdout


@pytest.mark.usefixtures("ready_base")
def test_a_program_read_back_from_json_runs_as_the_vm():
    # programs reach the game as JSON (story_program, graph_patch "program"): each one written and read back,
    # then run against the VM; one that reads back otherwise would not be a program
    r = _run("--fuzz", "2", "--exec", "--via-json")
    assert r.returncode == 0, r.stdout[-3000:]
    assert "DIFFER 0;" in r.stdout and "VM failed 0" in r.stdout and "not a program 0 " in r.stdout


@pytest.mark.usefixtures("ready_base")
@pytest.mark.skipif(not _saves(), reason="needs original-format saves in GOTHIC_SAVES")
def test_programs_match_the_vm_on_original_saves():
    r = _run(*[a for s in _saves() for a in ("--check", str(s))], "--exec")
    assert r.returncode == 0, r.stdout[-3000:]
    total = next(line for line in r.stdout.splitlines() if "programs (all saves)" in line)
    assert "DIFFER 0;" in total and "not a program 0 " in total


@pytest.mark.usefixtures("ready_base")
def test_a_program_keeps_the_order_of_the_script():
    # Alrik takes 50 gold, then removes all his gold: the count comes after the gift, where the VM makes it
    r = _run("--program", "DIA_ALRIK_WANNAFIGHT_GOLD")
    lines = r.stdout.splitlines()
    give = lines.index("B_GIVEINVITEMS(other, self, ITMI_GOLD, 50)")
    assert lines[give + 3] == "NPC_REMOVEINVITEMS(self, ITMI_GOLD, NPC_HASITEMS(self, ITMI_GOLD))"
    # `Bullco_Leave_Day == B_GetDayPlus();`, a typo for `=` in the original: nobody takes the comparison,
    # but the call in it still runs
    r = _run("--program", "DIA_BULLCO_DAILYCHECK")
    assert "BULLCO_LEAVE_DAY == B_GETDAYPLUS()" in r.stdout


@pytest.mark.usefixtures("ready_base")
def test_a_returned_local_is_read_when_it_is_taken():
    # B_GetLearnCostTalent returns its static local: the VM reads it after all three calls, so the original
    # prices "all thief talents" at three times the last one; the program does the same
    r = _run("--fuzz", "20", "--exec", "--node", "DIA_CH_DIEB_ALLE")
    assert r.returncode == 0, r.stdout[-3000:]
    assert "DIFFER 0;" in r.stdout
