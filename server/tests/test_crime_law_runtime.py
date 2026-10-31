from __future__ import annotations

import asyncio
import copy
from collections.abc import Awaitable, Callable
from types import SimpleNamespace
from typing import Any

import pytest

from gothic_ai.chronicle import Chronicle
from gothic_ai.crime_law_graph import city_entry_program, init_ops, law_var
from gothic_ai.crime_law_runtime import CrimeLawRuntime
from gothic_ai.crimes import Crimes
from gothic_ai.game import GameError
from gothic_ai.gamedata.storygraph import Node, StoryGraph
from gothic_ai.memory import MemoryStore
from gothic_ai.protocol import DialogChosen, Loading, Session
from tests.test_crime_law_graph import GateVM
from tests.test_crimes import Link

AT = 3 * 1440 + 9 * 60 + 15
ANDRE = "MIL_311_ANDRE"
MATTEO = "VLK_406_MATTEO"
CITIZEN = "VLK_400_CITIZEN"
PAYNOW = "DIA_ANDRE_PETZMASTER_PAYNOW"


class RuntimeLink(Link):
    def __init__(self) -> None:
        super().__init__()
        self.clock = AT
        self.variables: dict[str, int] = {"ANDRE_SCHULDEN": 0}
        self.effects: list[tuple[str | None, str, list[dict[str, Any]]]] = []
        self.stores: list[tuple[str, int]] = []
        self.patches: list[list[dict[str, Any]]] = []
        self.installs: list[str | None] = []
        self.state_hook: Callable[[], Awaitable[None]] | None = None

    async def story_state(self, **kwargs: Any) -> dict[str, Any]:
        state = {
            "day": self.clock // 1440,
            "hour": self.clock % 1440 // 60,
            "minute": self.clock % 60,
            "vars": dict(self.variables),
        }
        if self.state_hook is not None:
            await self.state_hook()
        return state

    async def story_eval(self, npc: str, expressions: list[Any]) -> list[int]:
        return [3, 0]

    async def story_effect(self, npc: str, effects: list[dict[str, Any]]) -> None:
        self.effects.append((self.session.branch, npc, copy.deepcopy(effects)))
        for effect in effects:
            assert effect["do"] == "set"
            name, value = effect["var"], effect["value"]
            assert isinstance(value, int)
            self.variables[name] = value
            self.stores.append((name, value))

    async def patch_graph(self, ops: list[dict[str, Any]]) -> None:
        self.patches.append(copy.deepcopy(ops))

    def receive(self, message: Any) -> None:
        if isinstance(message, Session):
            self.session = message
        super().receive(message)


@pytest.fixture
async def runtime(monkeypatch):
    link = RuntimeLink()
    memory = MemoryStore(":memory:")
    chronicle = Chronicle(link, memory)  # type: ignore[arg-type]
    crimes = Crimes(link, chronicle)  # type: ignore[arg-type]
    node = Node(
        "DIA_ANDRE_PETZMASTER",
        "dialog",
        ANDRE,
        "",
        [],
        set(),
        condition=[">", ["fn", "B_GETGREATESTPETZCRIME", ["inst", "SELF"]], ["int", 0]],
    )
    graph = StoryGraph(nodes={node.id: node}, functions={}, instances={}, vars={}, quests={})

    async def install(service: CrimeLawRuntime) -> None:
        service.link.installs.append(service.chronicle.branch)
        service._installed = True

    native_install = CrimeLawRuntime._install
    # The graph suite validates the actual program installer. Here the VM is a recording fake so
    # tests can check the runtime's clock, branch, native-payment and store ordering behavior.
    monkeypatch.setattr(CrimeLawRuntime, "_install", install)
    service = CrimeLawRuntime(link, crimes, graph, SimpleNamespace())  # type: ignore[arg-type]
    yield SimpleNamespace(
        link=link, memory=memory, chronicle=chronicle, crimes=crimes, service=service, law=service.law,
        native_install=native_install,
    )
    await service.close()
    await crimes.close()
    if chronicle._background:
        await asyncio.gather(*chronicle._background)
    memory.close()


def case(system, *, crime="theft-1", community="city", guard=False, at=AT, witness=None):
    fact = {
        "fact": "crime",
        "id": crime,
        "kind": "theft",
        "community": community,
        "victim": MATTEO,
        "day": at // 1440,
        "hour": at % 1440 // 60,
        "minute": at % 60,
        "item": "ITFO_BREAD",
        "count": 1,
    }
    system.chronicle.record(fact)
    npc = witness or (ANDRE if guard else CITIZEN)
    system.chronicle.record(
        {
            "fact": "crime_seen",
            "crime": crime,
            "npc": npc,
            "guild": 2 if guard else (8 if community == "farm" else 3),
            "npctype": 0,
        }
    )
    return fact


def native_choice(system, node=PAYNOW, *, made=False):
    system.link.receive(DialogChosen(type="dialog_chosen", npc=ANDRE, node=node, made=made))


@pytest.mark.parametrize("resolution", ["forgiven", "silenced", "absolved"])
async def test_resolved_case_stops_native_news_without_erasing_knowledge(runtime, resolution):
    witness = "BAU_951_HILDA"
    crime = case(runtime, community="farm", witness=witness)
    assert runtime.service._news_levels() == {witness: 3}

    fact = {"fact": resolution, "npc": witness, "crime": crime["id"]}
    if resolution == "absolved":
        fact = {"fact": resolution, "community": "farm", "crimes": [crime["id"]]}
    runtime.chronicle.record(fact)

    assert runtime.crimes.known(witness)
    assert runtime.service._news_levels() == {}

    case(runtime, crime="unresolved-theft", community="farm", witness=witness)
    assert runtime.service._news_levels() == {witness: 3}


async def test_native_pollution_stays_locked_then_known_counters_are_written_before_opening(runtime):
    link, service = runtime.link, runtime.service
    case(runtime)
    for suffix in ("THEFT", "ATTACK", "MURDER", "SHEEPKILLER"):
        link.variables[f"PETZCOUNTER_CITY_{suffix}"] = 17
    link.variables.update({"ANDRE_LASTPETZCOUNTER": 17, "ANDRE_LASTPETZCRIME": 4})
    await service.tick()
    assert runtime.law.known("city") == []
    assert link.variables[law_var("city")] == 0
    assert link.variables["PETZCOUNTER_CITY_THEFT"] == 17
    assert not any(name.startswith("PETZCOUNTER_CITY_") for name, _ in link.stores)

    link.stores.clear()
    link.clock = AT + 1440
    await service.tick()
    assert runtime.law.debt("city") == 50
    assert link.variables["PETZCOUNTER_CITY_THEFT"] == 1
    assert all(
        link.variables[f"PETZCOUNTER_CITY_{suffix}"] == 0 for suffix in ("ATTACK", "MURDER", "SHEEPKILLER")
    )
    opening = link.stores.index((law_var("city"), 1))
    assert link.stores.index(("PETZCOUNTER_CITY_THEFT", 1)) < opening
    assert link.stores.index(("DIR_CRIME_PETZ_CITY_THEFT", 1)) < opening
    assert link.variables["ANDRE_LASTPETZCOUNTER"] == 0
    assert link.variables["ANDRE_LASTPETZCRIME"] == 0


@pytest.mark.parametrize("made,paid", [(False, True), (True, False)])
async def test_only_native_dialog_choice_can_absolve_its_cases(runtime, made, paid):
    case(runtime, guard=True)
    await runtime.service.tick()
    assert runtime.law.debt("city") == 50
    runtime.link.variables["ANDRE_SCHULDEN"] = 0  # the successful native payment has completed
    native_choice(runtime, made=made)
    await runtime.service.tick()
    assert bool(runtime.chronicle.facts("absolved")) is paid
    assert runtime.law.debt("city") == (0 if paid else 50)
    assert runtime.link.variables[law_var("city")] == (0 if paid else 1)
    assert runtime.crimes.known(ANDRE)[0].crime["id"] == "theft-1"


async def test_native_paynow_with_unsettled_script_debt_does_not_absolve(runtime):
    case(runtime, guard=True)
    await runtime.service.tick()
    runtime.link.variables["ANDRE_SCHULDEN"] = 50
    native_choice(runtime)
    await runtime.service.tick()
    assert runtime.chronicle.facts("absolved") == []
    assert runtime.law.debt("city") == 50
    assert runtime.link.variables[law_var("city")] == 1


async def test_city_entry_uses_runtime_law_and_reopens_in_the_payment_poll(runtime):
    vm = GateVM()
    entry = city_entry_program(
        "DIA_MIL_310_STADTWACHE_BRIBE", "condition", [{"do": "return", "value": ["int", 1]}]
    )
    case(runtime)
    await runtime.service.tick()
    vm.variables.update(runtime.link.variables)
    assert vm.execute(entry) == 1  # eyewitness report still travelling
    runtime.link.clock = AT + 1440
    await runtime.service.tick()
    vm.variables.update(runtime.link.variables)
    assert vm.execute(entry) == 0
    native_choice(runtime)
    await runtime.service.tick()
    vm.variables.update(runtime.link.variables)
    assert runtime.chronicle.facts("absolved")[0]["crimes"] == ["theft-1"]
    assert vm.execute(entry) == 1


async def test_city_entry_rebuilds_from_the_loaded_branch_law(runtime):
    case(runtime)
    await runtime.service.tick()
    saved_head = runtime.memory.head("A")
    runtime.link.clock = AT + 1440
    await runtime.service.tick()
    vm = GateVM("MIL_333_STADTWACHE")
    entry = city_entry_program(
        "DIA_MIL_333_STADTWACHE_BRIBE", "condition", [{"do": "return", "value": ["int", 1]}]
    )
    vm.variables.update(runtime.link.variables)
    assert vm.execute(entry) == 0
    runtime.link.receive(Session(type="session", branch="B", parent="A", parent_seq=saved_head))
    runtime.link.clock = AT
    await runtime.service.tick()
    vm.variables.update(runtime.link.variables)
    assert vm.execute(entry) == 1


async def test_installer_patches_both_parts_of_entry_that_never_read_native_petz(runtime, monkeypatch):
    service, link = runtime.service, runtime.link
    entry = "DIA_MIL_310_STADTWACHE_BRIBE"
    service.graph.nodes[entry] = Node(
        entry, "dialog", "MIL_310_STADTWACHE", "", [], set(), condition=["int", 1]
    )
    service.graph.nodes[PAYNOW] = Node(
        PAYNOW, "choice", ANDRE, "", [], set(), parents=["DIA_ANDRE_PETZMASTER"]
    )
    service.patches = SimpleNamespace(static=init_ops(service.graph), _background=[])
    requested = []

    async def program(node, part, *, own):
        assert own
        requested.append((node, part))
        if node.endswith("_PAYNOW"):
            return {"steps": [{"do": "call", "call": ["fn", "B_GRANTABSOLUTION", ["int", 2]]}]}
        if part == "condition":
            return {"steps": [{"do": "return", "value": ["int", 1]}]}
        return {"steps": [{
            "do": "set", "target": ["field", ["inst", "SELF"], "AIVAR", 14], "value": ["int", 1]
        }]}

    state = link.story_state

    async def symbols(**kwargs):
        result = await state(**kwargs)
        result["symbols"] = {name: "var" for name in kwargs.get("symbols", [])}
        return result

    monkeypatch.setattr(link, "story_program", program, raising=False)
    monkeypatch.setattr(link, "story_state", symbols)
    await runtime.native_install(service)
    assert {(entry, "condition"), (entry, "information")} <= set(requested)
    programs = {op["part"]: op["steps"] for op in link.patches[-1] if op["node"] == entry}
    vm = GateVM()
    vm.variables.update({law_var("city"): 1, "DIR_CRIME_PETZ_CITY_THEFT": 1})
    assert vm.execute(programs["condition"]) == 0
    vm.execute(programs["information"])
    assert vm.fields.get((vm.npc, "AIVAR", 14), 0) == 0
    vm.variables.update({law_var("city"): 0, "DIR_CRIME_PETZ_CITY_THEFT": 0})
    assert vm.execute(programs["condition"]) == 1
    vm.execute(programs["information"])
    assert vm.fields[(vm.npc, "AIVAR", 14)] == 1


async def test_payment_covers_event_snapshot_while_report_arriving_on_tick_stays_owed(runtime):
    case(runtime, crime="already-known", guard=True)
    case(runtime, crime="still-travelling")
    await runtime.service.tick()
    assert [f["id"] for f in runtime.law.unpaid("city")] == ["already-known"]

    runtime.link.clock = AT + 1440
    runtime.link.variables["ANDRE_SCHULDEN"] = 0
    native_choice(runtime)
    await runtime.service.tick()
    (payment,) = runtime.chronicle.facts("absolved")
    assert payment["crimes"] == ["already-known"]
    assert [f["id"] for f in runtime.law.unpaid("city")] == ["still-travelling"]
    assert runtime.law.debt("city") == 50
    assert runtime.link.variables["PETZCOUNTER_CITY_THEFT"] == 1
    assert runtime.link.variables[law_var("city")] == 1


async def test_loc_all_cancels_existing_pending_reports_but_not_a_later_crime(runtime):
    case(runtime, crime="city-before", guard=True)
    case(runtime, crime="farm-pending", community="farm")
    await runtime.service.tick()
    native_choice(runtime, "DIA_PARLAN_WELCOME")
    case(runtime, crime="city-after", guard=True, at=AT + 1)
    runtime.link.clock = AT + 2
    await runtime.service.tick()
    assert {f["community"] for f in runtime.chronicle.facts("absolved")} == {"city", "farm"}
    paid = {crime for fact in runtime.chronicle.facts("absolved") for crime in fact["crimes"]}
    assert paid == {"city-before", "farm-pending"}
    assert [f["id"] for f in runtime.law.unpaid("city")] == ["city-after"]
    runtime.link.clock = AT + 2000
    await runtime.service.tick()
    assert runtime.law.known("farm") == [] and runtime.law.debt("farm") == 0
    assert runtime.link.variables[law_var("farm")] == 0
    assert runtime.law.debt("city") == 50


async def test_loading_a_branch_before_arrival_resets_runtime_projection_and_drops_old_payment(runtime):
    case(runtime)
    await runtime.service.tick()
    saved_head = runtime.memory.head("A")
    runtime.link.clock = AT + 1440
    await runtime.service.tick()
    assert runtime.link.variables[law_var("city")] == 1
    native_choice(runtime)
    assert runtime.service._events

    runtime.link.receive(Session(type="session", branch="B", parent="A", parent_seq=saved_head))
    assert not runtime.service._installed
    assert runtime.service._projection == {}
    assert runtime.service._levels is None
    assert runtime.service._events == []
    runtime.link.clock = AT
    await runtime.service.tick()
    assert runtime.link.installs == ["A", "B"]
    assert runtime.chronicle.facts("crime_known") == []
    assert runtime.chronicle.facts("absolved") == []
    assert runtime.link.variables[law_var("city")] == 0
    assert runtime.link.variables["DIR_CRIME_PETZ_CITY_THEFT"] == 0

    runtime.link.clock = AT + 1440
    await runtime.service.tick()
    assert len(runtime.chronicle.facts("crime_known")) == 1
    assert runtime.law.debt("city") == 50
    assert runtime.link.variables[law_var("city")] == 1


async def test_loading_notification_discards_a_queued_payment(runtime):
    case(runtime, guard=True)
    await runtime.service.tick()
    native_choice(runtime)
    runtime.link.receive(Loading(type="loading"))
    await runtime.service.tick()
    assert runtime.chronicle.facts("absolved") == []
    assert runtime.law.debt("city") == 50


async def test_a_clock_reply_from_previous_branch_cannot_publish_law_state(runtime):
    case(runtime, guard=True)
    started, resume = asyncio.Event(), asyncio.Event()

    async def pause_state():
        started.set()
        await resume.wait()

    runtime.link.state_hook = pause_state
    task = asyncio.create_task(runtime.service.tick())
    await started.wait()
    runtime.link.receive(Session(type="session", branch="B"))
    runtime.link.clock = AT - 1440
    resume.set()
    await task
    assert runtime.link.effects == []
    assert runtime.chronicle.facts("crime_known") == []
    assert runtime.service._projection == {}
    runtime.link.state_hook = None
    await runtime.service.tick()
    assert runtime.link.variables[law_var("city")] == 0
    assert runtime.law.known("city") == []


async def test_unavailable_legacy_witness_does_not_block_a_current_local_guard_report(runtime, monkeypatch):
    remote = "MIL_OLDWORLD_WITNESS"
    runtime.chronicle.record(
        {
            "fact": "crime",
            "id": "legacy-oldcamp",
            "kind": "theft",
            "community": "oldcamp",
            "day": AT // 1440,
            "hour": AT % 1440 // 60,
            "minute": AT % 60,
        }
    )
    runtime.chronicle.record({"fact": "crime_seen", "crime": "legacy-oldcamp", "npc": remote})
    current = case(runtime, crime="current-city", guard=True)
    requested = []

    async def unavailable(npc, expressions):
        requested.append(npc)
        assert npc == remote  # the current deed already carries its guard's metadata
        raise GameError("npc is in another world")

    monkeypatch.setattr(runtime.link, "story_eval", unavailable)
    await runtime.service.tick()
    assert requested == [remote]
    assert runtime.law.unpaid("city") == [current]
    assert runtime.law.known("oldcamp") == []
    assert runtime.link.variables["PETZCOUNTER_CITY_THEFT"] == 1
    assert runtime.link.variables[law_var("city")] == 1
