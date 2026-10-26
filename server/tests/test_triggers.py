from gothic_ai import triggers
from gothic_ai.chronicle import Chronicle
from gothic_ai.memory import MemoryStore
from gothic_ai.validator import Caps, Rules, Story, WorldState, validate
from tests.fake_game import FakeGame, connected
from tests.test_dialogue import _until
from tests.test_director import HILDA, SYMBOLS, graph  # noqa: F401 - the graph fixture
from tests.test_memory import close_memory_stores as close_memory_stores

SAW = "ITMI_SAW"
WORLD = {**SYMBOLS, SAW: "item", "NPC_GETDISTTOWP": "func", "HLP_GETNPC": "func", "NPC_ISDEAD": "func"}


def test_triggers_as_the_bridge_reads_them():
    op = triggers.trigger("dir_q1_t1", triggers.DIALOG, [], npc="bau_951_hilda", node="dir_q1_give")
    assert op == {
        "op": "trigger",
        "id": "DIR_Q1_T1",
        "on": "dialog",
        "steps": [],
        "npc": HILDA,
        "node": "DIR_Q1_GIVE",
    }
    near = triggers.trigger(
        "DIR_Q1_T2", triggers.STATE, [], when=triggers.hero_near_point("nw_farm1_out_01", 500)
    )
    assert near["when"] == [
        "<",
        ["call", "NPC_GETDISTTOWP", ["inst", "HERO"], ["str", "NW_FARM1_OUT_01"]],
        ["int", 500],
    ]
    assert "repeat" not in near and triggers.trigger("X", "state", [], repeat=True)["repeat"] is True
    assert triggers.dead("DIR_NPC_2") == ["call", "NPC_ISDEAD", ["call", "HLP_GETNPC", ["sym", "DIR_NPC_2"]]]
    assert triggers.all_of(triggers.fired("DIR_Q1_T1")) == [">=", ["var", "DIR_Q1_T1"], ["int", 1]]
    assert triggers.removal("dir_q1_t1") == {"op": "trigger", "id": "DIR_Q1_T1", "steps": None}


def test_the_validator_takes_a_trigger(graph):  # noqa: F811
    state = WorldState(vars={"KAPITEL": 1}, symbols=WORLD, npcs={HILDA: "alive"})
    story, rules = Story(graph, state), Rules.of(graph)
    mark = {"do": "set", "target": ["var", "DIR_Q1"], "value": ["int", 2]}
    got = triggers.trigger("DIR_Q1_GOT", triggers.STATE, [mark], when=triggers.hero_has(SAW))
    made = [{"op": "vars", "vars": {"DIR_Q1": 0}}, got]
    assert validate(made, story, rules) == []
    after = story.then(made)
    # its count is a variable; it fires once; what its program writes may come to be, nobody's death stops it
    assert after.symbols["DIR_Q1_GOT"] == "var" and after.var_values("DIR_Q1_GOT") == frozenset({0, 1})
    assert after.var_values("DIR_Q1") == frozenset({0, 2})
    assert story.then([*made, {**got, "repeat": True}]).var_values("DIR_Q1_GOT") is None
    assert story.then([*made, triggers.removal("DIR_Q1_GOT")]).var_values("DIR_Q1") == frozenset({0})

    def why(op: dict) -> list[str]:
        return validate([{"op": "vars", "vars": {"DIR_Q1": 0}}, op], story, rules)

    assert why({**got, "id": "Q1_GOT"}) == ["id триггера начинается с DIR_: Q1_GOT"]
    assert why({**got, "on": "rain"}) == ["событие триггера — state, death, dialog, а не 'rain'"]
    assert why({**got, "npc": "DIR_NOBODY"}) == ["нет такого NPC: DIR_NOBODY"]
    assert why({**got, "node": "DIR_Q1_GIVE"}) == ["диалог (node) — только у триггера на диалог"]
    assert why({**got, "when": triggers.hero_has("ITMI_NOTHING")}) == [
        "в условии неизвестные имена: ITMI_NOTHING"
    ]
    assert why({**got, "steps": [{"do": "set", "target": ["var", "KAPITEL"], "value": ["int", 2]}]}) == [
        "KAPITEL ведёт главы сюжета, её не трогаем"
    ]
    choice = {"do": "choice", "text": "Да", "steps": []}
    assert why({**got, "steps": [choice]}) == ["вне диалога вариантов ответа нет: они — в диалогах режиссёра"]
    assert why({**got, "steps": "no"}) == [
        "программа триггера — список шагов (пустой: триггер только считает)"
    ]
    # a repair's measure holds for what a trigger gives too
    gold = {
        "do": "call",
        "call": ["call", "CREATEINVITEMS", ["inst", "HERO"], ["sym", "ITMI_GOLD"], ["int", 50]],
    }
    assert validate(
        [{**got, "steps": [gold]}],
        Story(graph, WorldState(symbols={**WORLD, "CREATEINVITEMS": "func"})),
        rules,
        caps=Caps(items={"ITMI_GOLD": 10}),
    ) == ["ITMI_GOLD ×50, а исходная концовка давала ×10"]
    # the dialog event names a dialog there is
    talk = triggers.trigger("DIR_Q1_TOLD", triggers.DIALOG, [], npc=HILDA, node="DIR_NOWHERE")
    assert why(talk) == ["нет такого диалога: DIR_NOWHERE"]


async def test_the_chronicle_hears_a_trigger_fire():
    fake = FakeGame(branch="A")
    async with connected(fake) as link:
        chronicle = Chronicle(link, await MemoryStore.open(":memory:"))
        await chronicle.initialize()
        fired = triggers.Triggers(link, chronicle)
        await _until(lambda: chronicle.branch is not None)
        await fake.send(type="trigger_fired", id="DIR_Q1_GOT", count=1, npc="PC_HERO", other="PC_HERO")
        await fake.send(
            type="trigger_fired", id="DIR_Q1_T2", count=3, npc="", other="", error="no variable X"
        )
        await _until(lambda: len(chronicle.facts("triggered")) == 2)
        assert chronicle.facts("triggered") == [
            {"fact": "triggered", "id": "DIR_Q1_GOT", "count": 1, "npc": "PC_HERO", "other": "PC_HERO"},
            {"fact": "triggered", "id": "DIR_Q1_T2", "count": 3, "error": "no variable X"},
        ]
        assert fired.fired("dir_q1_t2") == 3 and fired.fired("DIR_NONE") == 0
