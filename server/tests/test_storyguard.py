import json
from dataclasses import replace
from pathlib import Path

import pytest

from gothic_ai.gamedata.storygraph import StoryGraph, load
from gothic_ai.protocol import RosterEntry
from gothic_ai.storyguard import StoryGuard, asleep
from tests.test_director import _node

GUARD, LARES, VINO, LOBART = "MIL_310_STADTWACHE", "VLK_449_LARES", "BAU_952_VINO", "BAU_950_LOBART"
MESSENGER = "VLK_1_BOTE"  # not a plot prefix, but a dialog of his changes the chapter
SELF, OTHER = ["inst", "SELF"], ["inst", "OTHER"]
AT_POST = ["call", "HLP_STRCMP", ["call", "NPC_GETNEARESTWP", SELF], ["field", SELF, "WP"]]
HERO_AT_GATE = ["<", ["call", "NPC_GETDISTTOWP", OTHER, ["str", "NW_CITY_ENTRANCE_MAIN"]], ["int", 700]]
AWAY = [">=", ["call", "NPC_GETDISTTOWP", SELF, ["str", "NW_MONASTERY_PLACE_07"]], ["int", 4000]]
POST = "стоит на посту"
PLOT = "сюжетный: только слова"
ESCORT = "ведёт или сопровождает героя по скриптам"


@pytest.fixture
def graph(tmp_path: Path) -> StoryGraph:
    """Cut down from the exported graph: the city guard's passage, Lares' guide dialog, Vino's healing (reads
    where he stands, not a post), Lobart's (reads where the hero stands) and a chapter change."""
    nodes = [
        _node("DIA_MIL_310_STADTWACHE_FIRSTWARN", GUARD, [], condition=["&&", AT_POST, HERO_AT_GATE]),
        _node("DIA_MIL_310_STADTWACHE_SECONDWARN", GUARD, [], condition=AT_POST),
        _node("DIA_MIL_310_STADTWACHE_PERM", GUARD, [], condition=None),
        _node("DIA_LARES_GUIDE", LARES, [], condition=["call", "NPC_GETNEARESTWP", SELF]),
        _node("DIA_VINO_HEILUNG", VINO, [], condition=AWAY),
        _node("DIA_LOBART_HALLO", LOBART, [], condition=["call", "NPC_GETNEARESTWP", OTHER]),
        _node("DIA_BOTE_KAPITEL2", MESSENGER, [{"do": "chapter", "value": ["int", 2]}], condition=None),
    ]
    data = {"version": 1, "nodes": nodes, "functions": {}, "instances": {}, "vars": {}, "quests": []}
    path = tmp_path / "story.json"
    path.write_text(json.dumps(data, ensure_ascii=False), encoding="utf-8")
    return load(path)


def test_plot_with_and_without_graph(graph):
    bare = StoryGuard(None)
    assert bare.plot("NONE_100_XARDAS") and bare.plot("pc_fighter")
    assert not bare.plot(MESSENGER) and not bare.plot(VINO)
    assert bare.body(MESSENGER) is None
    assert bare.posts == set() and bare.placed() == {}

    guard = StoryGuard(graph)
    assert guard.plot(MESSENGER)  # changes the chapter: the director's rules know him
    assert guard.plot("NONE_100_XARDAS") and not guard.plot(VINO)
    assert guard.body(MESSENGER) == PLOT
    assert guard.body(LARES) == PLOT  # a plot npc at a post: the plot is the reason


def test_guard_passage_post_from_graph(graph):
    guard = StoryGuard(graph)
    assert guard.posts == {GUARD, LARES}
    assert guard.body(GUARD) == POST
    assert guard.body(GUARD.lower(), "ZS_STAND_WP") == POST  # off its passage state, still the post
    assert guard.body(VINO) is None  # reads where it stands, yet may walk
    assert guard.body(LOBART) is None  # reads where the hero stands
    assert guard.placed() == {
        GUARD: ["DIA_MIL_310_STADTWACHE_FIRSTWARN", "DIA_MIL_310_STADTWACHE_SECONDWARN"],
        VINO: ["DIA_VINO_HEILUNG"],
        LARES: ["DIA_LARES_GUIDE"],
    }
    assert list(guard.placed()) == sorted(guard.placed())


def test_guard_and_escort_states_from_roster():
    guard = StoryGuard(None)

    def state(s: str) -> str:
        return RosterEntry(id=LOBART, name="Лобарт", distance=300.0, state=s).state

    assert guard.body(LOBART, state("ZS_GUARD_PASSAGE")) == POST
    assert guard.body(LOBART, state("ZS_GUIDE_PLAYER")) == ESCORT
    assert guard.body(LOBART, state("ZS_FOLLOW_PLAYER")) == ESCORT
    assert guard.body(LOBART, "zs_follow_player") == ESCORT
    for s in ("", "ZS_SIT_BENCH", "ZS_STAND_GUARDING", "ZS_TALK", "ZS_SLEEP"):
        assert guard.body(LOBART, state(s)) is None, s
    assert guard.body("NONE_100_XARDAS", "ZS_GUIDE_PLAYER") == PLOT


def test_on_duty_is_posts_and_escorts_only(graph):
    guard = StoryGuard(graph)
    assert guard.on_duty(GUARD) == POST  # its dialogs read where it stands
    assert guard.on_duty(VINO, "ZS_GUARD_PASSAGE") == POST
    assert guard.on_duty(VINO, "zs_guide_player") == ESCORT
    assert guard.on_duty(VINO, "ZS_FOLLOW_PLAYER") == ESCORT
    assert guard.on_duty(VINO, "ZS_STAND_GUARDING") is None
    # a plot npc may be held to speak, unless the scripts have it at a post or leading the hero
    assert guard.body(MESSENGER) == PLOT and guard.on_duty(MESSENGER) is None
    assert guard.on_duty(LARES) == POST and guard.on_duty("NONE_100_XARDAS", "ZS_GUIDE_PLAYER") == ESCORT


def test_asleep():
    for s in ("ZS_GOTOBED", "zs_gotobed", "ZS_SLEEP", "ZS_SLEEP_GROUND"):
        assert asleep(s), s
    for s in ("", "ZS_STAND_GUARDING", "ZS_SIT_BENCH", "ZS_PRAY_SLEEPER"):
        assert not asleep(s), s


def test_harm_protects_exact_story_holder_and_mission_item(graph):
    first = next(iter(graph.nodes.values()))
    graph.nodes["DIA_LOBART_ITEM"] = replace(
        first, id="DIA_LOBART_ITEM", npc=LOBART,
        condition=["call", "NPC_HASITEMS", SELF, ["sym", "ITMI_QUEST_OBJECT"]],
    )
    guard = StoryGuard(graph)
    assert "сюжетного владельца" in guard.harm(VINO, target=LOBART, item="ITMI_QUEST_OBJECT")
    assert guard.harm(VINO, target="BAU_OTHER", item="ITMI_QUEST_OBJECT") is None
    assert guard.harm(VINO, target=LOBART, item="ITFO_BREAD") is None
    assert "вещь для задания" in guard.harm(VINO, target=LOBART, item="ITFO_BREAD", mission=True)
