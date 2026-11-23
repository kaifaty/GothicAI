import json
from types import SimpleNamespace
from typing import Any

from gothic_ai.chronicle import Chronicle
from gothic_ai.memory import MemoryStore
from gothic_ai.protocol import HeroKill, NpcDied, Session

LOBART = "BAU_950_LOBART"


class Link:
    """Just what the chronicle uses of the game link: it starts on branch A."""

    def __init__(self) -> None:
        self.listeners: list[Any] = []
        self.session = Session(type="session", branch="A")
        self.sent: list[Any] = []

    async def send(self, msg: Any) -> None:
        self.sent.append(msg)


def _chronicle(store: MemoryStore) -> tuple[Chronicle, list[tuple[str, int]]]:
    """A chronicle on branch A, and every (branch, seq) it asked the store for."""
    asked: list[tuple[str, int]] = []
    since = store.facts_since

    def spy(branch: str, seq: int) -> list[tuple[int, str]]:
        asked.append((branch, seq))
        return since(branch, seq)

    store.facts_since = spy  # type: ignore[method-assign]
    return Chronicle(Link(), store), asked  # type: ignore[arg-type]


def died(npc: str, tag: str | None = None) -> dict[str, Any]:
    return {"fact": "died", "npc": npc, "name": npc, "tag": tag, "killer": "PLAYER"}


async def test_facts_are_read_once_then_only_the_new_ones():
    store = MemoryStore(":memory:")
    chronicle, asked = _chronicle(store)
    chronicle.record(died(LOBART))
    chronicle.record({"fact": "quest", "id": "Q"})
    assert [f["fact"] for f in chronicle.facts()] == ["died", "quest"]
    assert asked == [("A", 0)]
    chronicle.record({"fact": "paid", "quest": "Q"})
    assert [f["fact"] for f in chronicle.facts()] == ["died", "quest", "paid"]
    assert chronicle.facts() == chronicle.facts()  # nothing new: the same facts
    assert asked == [("A", 0), ("A", 2), ("A", 3), ("A", 3)]


async def test_a_fact_written_straight_into_the_store_is_seen():
    store = MemoryStore(":memory:")
    chronicle, _ = _chronicle(store)
    chronicle.record(died(LOBART))
    assert chronicle.killed("", "PLAYER") == 0 and len(chronicle.facts("died")) == 1
    store.append("A", LOBART, "user", "не факт")  # other events take seqs too
    store.add_fact("A", json.dumps(died("HILDA", "WOLVES")))  # as a test or another reader writes it
    assert [f["npc"] for f in chronicle.facts("died")] == [LOBART, "HILDA"]
    assert chronicle.killed("WOLVES", "PLAYER") == 1


async def test_kind_filters():
    store = MemoryStore(":memory:")
    chronicle, _ = _chronicle(store)
    chronicle._on_message(HeroKill(type="hero_kill", npc=LOBART, guild=12, monster=False))
    chronicle._on_message(NpcDied(type="npc_died", npc=LOBART, killer="PLAYER"))
    chronicle.record({"fact": "quest", "id": "Q"})
    chronicle._on_message(NpcDied(type="npc_died", npc="HILDA", killer="OTHER"))
    chronicle.record({"note": "no kind"})
    deaths = chronicle.facts("died")
    assert [f["npc"] for f in deaths] == [LOBART, "HILDA"]
    assert deaths[0]["guild"] == 12 and deaths[0]["monster"] is False
    assert "guild" not in deaths[1] and "monster" not in deaths[1]
    assert [f["id"] for f in chronicle.facts("quest")] == ["Q"]
    assert chronicle.facts("rumor") == []
    assert len(chronicle.facts()) == 4


async def test_a_branch_switch_resets_the_cache():
    store = MemoryStore(":memory:")
    chronicle, asked = _chronicle(store)
    chronicle.record(died(LOBART))
    saved = store.head("A")
    chronicle.record(died("HILDA"))
    assert len(chronicle.facts("died")) == 2

    # a load of the save made after Lobart died: Hilda lives on the new branch
    chronicle._on_message(Session(type="session", branch="B", parent="A", parent_seq=saved, reason="load"))
    assert [f["npc"] for f in chronicle.facts("died")] == [LOBART]
    assert asked[-1] == ("B", 0)
    chronicle.record({"fact": "quest", "id": "Q"})
    assert [f["fact"] for f in chronicle.facts()] == ["died", "quest"]

    # and back: branch A is read anew, from its start
    chronicle._on_message(Session(type="session", branch="A", reason="connect"))
    assert [f["npc"] for f in chronicle.facts("died")] == [LOBART, "HILDA"]
    assert chronicle.facts("quest") == []
    chronicle.branch = None  # no session: nothing
    assert chronicle.facts() == []


async def test_the_lists_are_the_callers_own():
    store = MemoryStore(":memory:")
    chronicle, _ = _chronicle(store)
    fact = died(LOBART)
    chronicle.record(fact)
    every, deaths = chronicle.facts(), chronicle.facts("died")
    every.clear()
    deaths.append(died("HILDA"))
    assert len(chronicle.facts()) == 1 and len(chronicle.facts("died")) == 1
    # the dicts come from the store, not from whoever recorded them: changing one's own does no harm
    fact["npc"] = "HILDA"
    assert chronicle.facts("died")[0]["npc"] == LOBART
    # but they are shared between readers (read-only by agreement): no copy per call
    assert chronicle.facts()[0] is chronicle.facts("died")[0]


async def test_no_session_no_facts():
    store = MemoryStore(":memory:")
    link = SimpleNamespace(listeners=[], session=None)
    chronicle = Chronicle(link, store)  # type: ignore[arg-type]
    chronicle.record(died(LOBART))  # not kept
    assert chronicle.facts() == [] and store.branches() == []
