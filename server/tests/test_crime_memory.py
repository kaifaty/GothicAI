from __future__ import annotations

import asyncio
from types import SimpleNamespace
from typing import Any

import pytest

from gothic_ai.chronicle import Chronicle
from gothic_ai.crime_memory import CrimeMemoryRuntime, memory_value
from gothic_ai.crimes import Crimes
from gothic_ai.fear import knows_violence
from gothic_ai.game import GameError
from gothic_ai.memory import MemoryStore
from gothic_ai.protocol import Loading, Session
from tests.test_crimes import Link

NPC = "BAU_951_HILDA"


class MemoryLink(Link):
    def __init__(self) -> None:
        super().__init__()
        self.effects: list[tuple[str, list[dict[str, Any]]]] = []
        self.missing: set[str] = set()

    async def story_effect(self, npc: str, effects: list[dict[str, Any]]) -> None:
        if npc in self.missing:
            raise GameError("npc in another world")
        self.effects.append((npc, effects))


@pytest.fixture
async def system():
    link = MemoryLink()
    memory = MemoryStore(":memory:")
    chronicle = Chronicle(link, memory)  # type: ignore[arg-type]
    crimes = Crimes(link, chronicle)  # type: ignore[arg-type]
    runtime = CrimeMemoryRuntime(link, crimes)  # type: ignore[arg-type]
    yield SimpleNamespace(link=link, memory=memory, chronicle=chronicle, crimes=crimes, runtime=runtime)
    await runtime.close()
    await crimes.close()
    if chronicle._background:
        await asyncio.gather(*chronicle._background)
    memory.close()


def case(system, *, crime="theft-1", kind="theft", day=3, npc=NPC, degree="seen", community=None):
    system.chronicle.record({"fact": "crime", "id": crime, "kind": kind, "day": day,
                             **({"community": community} if community is not None else {})})
    fact = {"fact": "crime_seen", "npc": npc, "crime": crime}
    if degree == "heard":
        fact = {"fact": "rumor", "to": npc, "crime": crime, "from": "WITNESS"}
    elif degree == "suspects":
        fact["fact"] = "crime_suspected"
    system.chronicle.record(fact)


@pytest.mark.parametrize("kind,value", [("sheep", 1), ("attack", 2), ("theft", 3), ("murder", 4)])
@pytest.mark.parametrize("degree", ["seen", "heard"])
async def test_proven_knowledge_projects_native_level_and_day(system, kind, value, degree):
    case(system, kind=kind, degree=degree)
    await system.runtime.tick()
    # Native murder memory makes all non-bandits fight. The encounter table now owns that choice;
    # news still receives the original level through memory_value().
    assert system.link.effects == [(NPC, [{"do": "crime_memory", "value": min(value, 3), "day": 3}])]
    assert memory_value(system.crimes, NPC.lower()) == (value, 3)


async def test_without_react_only_recorded_side_removes_fear_and_native_guilt(system):
    case(system, kind="murder", degree="heard")
    system.chronicle.record({
        "fact": "rumor", "to": NPC, "crime": "theft-1", "from": "ALLY",
        "excuse": "fought", "excuse_by": "ALLY",
    })
    assert knows_violence(NPC, system.crimes)
    assert memory_value(system.crimes, NPC) == (4, 3)
    system.chronicle.record({
        "fact": "crime_side", "npc": NPC, "crime": "theft-1", "why": "fought",
    })
    assert not knows_violence(NPC, system.crimes)
    assert memory_value(system.crimes, NPC) == (0, 0)
    await system.runtime.project(NPC)
    assert system.link.effects[-1] == (NPC, [{"do": "crime_memory", "value": 0, "day": 0}])


async def test_maximum_severity_wins_then_latest_day_without_transferring_suspicion(system):
    case(system, crime="old-theft", day=2)
    case(system, crime="new-theft", day=3, degree="heard")
    case(system, crime="new-attack", kind="attack", day=4)
    case(system, crime="possible-murder", kind="murder", day=5, degree="suspects")
    assert memory_value(system.crimes, NPC) == (3, 3)
    assert memory_value(system.crimes, "UNKNOWN") == (0, 0)


@pytest.mark.parametrize("kind", ["silenced", "forgiven", "absolved", "died"])
async def test_personal_resolution_or_absolution_removes_native_memory(system, kind):
    npc = "MIL_305_JERGEL" if kind == "absolved" else NPC
    if kind == "absolved":
        system.crimes.people[npc] = {"guild": 2, "npctype": 0}
    case(system, npc=npc, community="city" if kind == "absolved" else None)
    await system.runtime.tick()
    fact = {"fact": kind, "npc": npc, "crime": "theft-1"}
    if kind == "absolved":
        fact = {"fact": kind, "community": "city", "crimes": ["theft-1"]}
    system.chronicle.record(fact)
    assert system.runtime._wake.is_set()
    await system.runtime.tick()
    assert system.link.effects[-1] == (npc, [{"do": "crime_memory", "value": 0, "day": 0}])
    assert system.crimes.known(npc)  # the fact remains; only the native encounter projection is cleared


async def test_other_npcs_forgiveness_and_other_cases_absolution_do_not_erase_memory(system):
    case(system)
    system.chronicle.record({"fact": "forgiven", "npc": "OTHER", "crime": "theft-1"})
    system.chronicle.record({"fact": "absolved", "community": "city", "crimes": ["elsewhere"]})
    assert memory_value(system.crimes, NPC) == (3, 3)


async def test_resolution_reveals_next_unresolved_case(system):
    case(system)
    case(system, crime="assault", kind="attack", day=4)
    system.chronicle.record({"fact": "silenced", "npc": NPC, "crime": "theft-1"})
    assert memory_value(system.crimes, NPC) == (2, 4)


async def test_unchanged_projection_keeps_native_charm_clearing_until_load(system):
    case(system)
    await system.runtime.project(NPC)
    await system.runtime.tick()
    assert len(system.link.effects) == 1
    system.link.receive(Loading(type="loading"))
    await system.runtime.tick()
    assert len(system.link.effects) == 2


async def test_new_same_day_theft_reinstates_memory_after_native_charm(system):
    case(system)
    await system.runtime.project(NPC)
    case(system, crime="another-theft")
    await system.runtime.project(NPC)
    assert len(system.link.effects) == 2


async def test_missing_npc_is_retried_without_blocking_other_witness(system):
    case(system)
    case(system, crime="other", npc="VLK_OTHER")
    system.link.missing.add(NPC)
    await system.runtime.tick()
    assert system.link.effects[0][0] == "VLK_OTHER"
    system.link.missing.clear()
    await system.runtime.tick()
    assert system.link.effects[-1][0] == NPC


async def test_loading_new_branch_forgets_process_projection_and_uses_current_facts(system):
    case(system)
    await system.runtime.tick()
    system.link.receive(Session(type="session", branch="B"))
    assert system.runtime._projection == {}
    await system.runtime.tick()
    assert len(system.link.effects) == 1
    case(system, crime="new", kind="attack", day=7)
    await system.runtime.tick()
    assert system.link.effects[-1] == (NPC, [{"do": "crime_memory", "value": 2, "day": 7}])


async def test_law_payment_does_not_release_a_private_victims_home_or_personal_claim(system):
    case(system)
    system.chronicle.record({"fact": "absolved", "community": "city", "crimes": ["theft-1"]})
    await system.runtime.project(NPC)
    assert system.link.effects[-1] == (NPC, [{"do": "crime_memory", "value": 3, "day": 3}])
    assert memory_value(system.crimes, NPC) == (0, 0)  # preserve the layer-3 news contract


async def test_suspend_allows_important_talk_without_changing_raw_news_knowledge(system):
    case(system, kind="murder")
    await system.runtime.project(NPC)
    async with system.runtime.suspend(NPC):
        assert system.link.effects[-1] == (NPC, [{"do": "crime_memory", "value": 0, "day": 0}])
        assert memory_value(system.crimes, NPC) == (4, 3)
        await system.runtime.tick()
        assert system.link.effects[-1][1][0]["value"] == 0
    assert system.link.effects[-1] == (NPC, [{"do": "crime_memory", "value": 3, "day": 3}])


async def test_nested_suspend_and_payment_restore_current_knowledge_only(system):
    case(system)
    async with system.runtime.suspend(NPC):
        async with system.runtime.suspend(NPC):
            system.chronicle.record({"fact": "forgiven", "npc": NPC, "crime": "theft-1", "paid": 50})
        await system.runtime.project(NPC)
        assert system.link.effects[-1][1][0]["value"] == 0
    assert system.runtime._suspended == {}
    assert system.link.effects[-1] == (NPC, [{"do": "crime_memory", "value": 0, "day": 0}])


async def test_cancelled_suspend_restores_home_memory(system):
    case(system)
    started = asyncio.Event()

    async def walk():
        async with system.runtime.suspend(NPC):
            started.set()
            await asyncio.Event().wait()

    task = asyncio.create_task(walk())
    await started.wait()
    task.cancel()
    await asyncio.gather(task, return_exceptions=True)
    assert system.runtime._suspended == {}
    assert system.link.effects[-1][1][0]["value"] == 3


async def test_old_suspension_does_not_write_or_decrement_new_world_state(system):
    case(system)
    async with system.runtime.suspend(NPC):
        system.link.receive(Loading(type="loading"))
        system.link.receive(Session(type="session", branch="B"))
        case(system, kind="attack", day=7)
        await system.runtime.project(NPC)
        effect_count = len(system.link.effects)
    assert system.runtime._suspended == {}
    assert len(system.link.effects) == effect_count
    assert system.link.effects[-1] == (NPC, [{"do": "crime_memory", "value": 2, "day": 7}])


async def test_payment_during_projection_is_cleared_before_the_same_flush_returns(system, monkeypatch):
    case(system)
    send = system.link.story_effect

    async def paying(npc, effects):
        await send(npc, effects)
        if effects[0]["value"] == 3:
            system.chronicle.record({"fact": "forgiven", "npc": NPC, "crime": "theft-1", "paid": 50})

    monkeypatch.setattr(system.link, "story_effect", paying)
    await system.runtime.project(NPC)
    assert [effects[0]["value"] for _, effects in system.link.effects] == [3, 0]
