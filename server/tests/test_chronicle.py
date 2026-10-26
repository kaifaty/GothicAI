import asyncio
import json
from types import SimpleNamespace
from typing import Any

import pytest

from gothic_ai.chronicle import Chronicle
from gothic_ai.game import GameError
from gothic_ai.memory import MemoryStore
from gothic_ai.protocol import Session
from tests.test_memory import close_memory_stores as close_memory_stores

LOBART = "BAU_950_LOBART"


class Link:
    """Just what the chronicle uses of the game link: it starts on branch A."""

    def __init__(self) -> None:
        self.listeners: list[Any] = []
        self.session = Session(type="session", branch="A")
        self.sent: list[Any] = []

    async def send(self, msg: Any) -> None:
        self.sent.append(msg)


async def _chronicle(store: MemoryStore) -> tuple[Chronicle, list[tuple[str, int]]]:
    """A chronicle on branch A, and every (branch, seq) it asked the store for."""
    asked: list[tuple[str, int]] = []
    since = store.facts_since

    async def spy(branch: str, seq: int) -> list[tuple[int, str]]:
        asked.append((branch, seq))
        return await since(branch, seq)

    store.facts_since = spy  # type: ignore[method-assign]
    chronicle = Chronicle(Link(), store)  # type: ignore[arg-type]
    await chronicle.initialize()
    return chronicle, asked


def died(npc: str, tag: str | None = None) -> dict[str, Any]:
    return {"fact": "died", "npc": npc, "name": npc, "tag": tag, "killer": "PLAYER"}


async def test_facts_are_read_once_then_only_the_new_ones():
    store = await MemoryStore.open(":memory:")
    chronicle, asked = await _chronicle(store)
    await chronicle.record(died(LOBART))
    await chronicle.record({"fact": "quest", "id": "Q"})
    assert [f["fact"] for f in chronicle.facts()] == ["died", "quest"]
    await chronicle.initialize()
    assert asked == [("A", 0)]  # bootstrap does not reset an already prepared Session
    await chronicle.record({"fact": "paid", "quest": "Q"})
    assert [f["fact"] for f in chronicle.facts()] == ["died", "quest", "paid"]
    assert chronicle.facts() == chronicle.facts()  # nothing new: the same facts
    assert asked == [("A", 0)]  # rendering only reads the already prepared cache

    entered, release = asyncio.Event(), asyncio.Event()
    add_fact = store.add_fact
    observed = []
    chronicle.on_fact.append(observed.append)

    async def delayed(branch, content):
        seq = await add_fact(branch, content)
        entered.set()
        await release.wait()
        return seq

    store.add_fact = delayed
    pending = asyncio.create_task(chronicle.record({"fact": "paid", "quest": "NEXT"}))
    await entered.wait()
    assert len(chronicle.facts()) == 3 and observed == []
    assert chronicle.link.sent[-1].seq == 3
    pending.cancel()
    await asyncio.sleep(0)
    assert not pending.done()  # cancellation drains the same accepted completion
    release.set()
    with pytest.raises(asyncio.CancelledError):
        await pending
    assert len(chronicle.facts()) == 4 and observed == [{"fact": "paid", "quest": "NEXT"}]
    assert chronicle.link.sent[-1].seq == 4 and await store.head("A") == 4
    await store.close()


async def test_a_fact_written_straight_into_the_store_is_seen():
    store = await MemoryStore.open(":memory:")
    chronicle, _ = await _chronicle(store)
    await chronicle.record(died(LOBART))
    assert chronicle.killed("", "PLAYER") == 0 and len(chronicle.facts("died")) == 1
    await store.append("A", LOBART, "user", "не факт")  # other events take seqs too
    await store.add_fact("A", json.dumps(died("HILDA", "WOLVES")))  # as a test or another reader writes it
    assert [f["npc"] for f in chronicle.facts("died")] == [LOBART]
    session = Session(type="session", branch="A", reason="connect")
    chronicle.link.session = session
    await chronicle._on_message(session)  # an actual Session reloads externally written facts
    assert [f["npc"] for f in chronicle.facts("died")] == [LOBART, "HILDA"]
    assert chronicle.killed("WOLVES", "PLAYER") == 1
    await store.close()


async def test_kind_filters():
    store = await MemoryStore.open(":memory:")
    chronicle, _ = await _chronicle(store)
    for fact in (died(LOBART), {"fact": "quest", "id": "Q"}, died("HILDA"), {"note": "no kind"}):
        await chronicle.record(fact)
    assert [f["npc"] for f in chronicle.facts("died")] == [LOBART, "HILDA"]
    assert [f["id"] for f in chronicle.facts("quest")] == ["Q"]
    assert chronicle.facts("rumor") == []
    assert len(chronicle.facts()) == 4
    await store.close()


async def test_a_branch_switch_resets_the_cache():
    store = await MemoryStore.open(":memory:")
    chronicle, asked = await _chronicle(store)
    await chronicle.record(died(LOBART))
    saved = await store.head("A")
    await chronicle.record(died("HILDA"))
    assert len(chronicle.facts("died")) == 2

    # a load of the save made after Lobart died: Hilda lives on the new branch
    session = Session(type="session", branch="B", parent="A", parent_seq=saved, reason="load")
    chronicle.link.session = session
    await chronicle._on_message(session)
    assert [f["npc"] for f in chronicle.facts("died")] == [LOBART]
    assert asked[-1] == ("B", 0)
    await chronicle.record({"fact": "quest", "id": "Q"})
    assert [f["fact"] for f in chronicle.facts()] == ["died", "quest"]

    # and back: branch A is read anew, from its start
    session = Session(type="session", branch="A", reason="connect")
    chronicle.link.session = session
    await chronicle._on_message(session)
    assert [f["npc"] for f in chronicle.facts("died")] == [LOBART, "HILDA"]
    assert chronicle.facts("quest") == []

    entered, release = asyncio.Event(), asyncio.Event()
    add_fact = store.add_fact

    async def delayed(branch, content):
        seq = await add_fact(branch, content)
        entered.set()
        await release.wait()
        return seq

    store.add_fact = delayed
    pending = asyncio.create_task(chronicle.record({"fact": "quest", "id": "OLD"}))
    await entered.wait()
    heads = len(chronicle.link.sent)
    # The reader has switched Session before the ordered Chronicle listener gets its turn.
    session = Session(type="session", branch="C", parent="A", parent_seq=saved, reason="load")
    chronicle.link.session = session
    loading = asyncio.create_task(chronicle._on_message(session))
    await asyncio.sleep(0)
    release.set()
    with pytest.raises(GameError, match="session changed"):
        await pending
    await loading
    assert chronicle.branch == "C" and [f["npc"] for f in chronicle.facts("died")] == [LOBART]
    assert chronicle.facts("quest") == [] and len(chronicle.link.sent) == heads
    chronicle.branch = None  # no session: nothing
    assert chronicle.facts() == []
    await store.close()


async def test_the_lists_are_the_callers_own():
    store = await MemoryStore.open(":memory:")
    chronicle, _ = await _chronicle(store)
    fact = died(LOBART)
    await chronicle.record(fact)
    every, deaths = chronicle.facts(), chronicle.facts("died")
    every.clear()
    deaths.append(died("HILDA"))
    assert len(chronicle.facts()) == 1 and len(chronicle.facts("died")) == 1
    # the dicts come from the store, not from whoever recorded them: changing one's own does no harm
    fact["npc"] = "HILDA"
    assert chronicle.facts("died")[0]["npc"] == LOBART
    # but they are shared between readers (read-only by agreement): no copy per call
    assert chronicle.facts()[0] is chronicle.facts("died")[0]
    await store.close()


async def test_no_session_no_facts():
    store = await MemoryStore.open(":memory:")
    link = SimpleNamespace(listeners=[], session=None)
    chronicle = Chronicle(link, store)  # type: ignore[arg-type]
    await chronicle.record(died(LOBART))  # not kept
    assert chronicle.facts() == [] and (await store.branches()) == []
    await store.close()
