"""Voice admission uses the scripts' conditions and writes the real gate state."""

from __future__ import annotations

import asyncio
import copy
from contextlib import asynccontextmanager
from typing import Any

import pytest

from gothic_ai.chronicle import Chronicle
from gothic_ai.crime_law_graph import law_var, projection_values
from gothic_ai.deals import Deals
from gothic_ai.dialogue import DialogueManager
from gothic_ai.gamedata.storygraph import Node, StoryGraph
from gothic_ai.gates import Gates
from gothic_ai.llm import ToolCall
from gothic_ai.llm.fake import FakeBackend
from gothic_ai.memory import MemoryStore
from gothic_ai.persona import PersonaProvider
from tests.fake_game import FakeGame, connected
from tests.test_dialogue import _until

CITY = "MIL_310_STADTWACHE"
PAIR = "MIL_333_STADTWACHE"
UPPER = "MIL_305_TORWACHE"
GOLD = "ITMI_GOLD"
LETTER = "ITWR_PASSIERSCHEIN"
HERBS = "ITPL_MANA_HERB_01"
HERB_QUEST = "MIS_ADDON_LESTER_PICKFORCONSTANTINO"
PASS = "DIA_MIL_310_STADTWACHE_PASSIERSCHEIN"
SMITH = "DIA_MIL_310_STADTWACHE_ZUMSCHMIED"
CONSTANTINO = "DIA_ADDON_MIL_310_STADTWACHE_CONSTANTINO"
SELF = ["inst", "SELF"]
HERO = ["inst", "OTHER"]


class GatesFake(FakeGame):
    """The existing protocol's one-shot gate trigger, with real stores and transfers.

    Kept local to this suite: the general FakeGame deliberately does not execute story programs.
    """

    def __init__(self, **kwargs: Any) -> None:
        super().__init__(**kwargs)
        self.fields: dict[tuple, Any] = {}
        self.armor = ""
        self.posts = {npc: "GATE_POST" for npc in (CITY, PAIR, UPPER)}
        self.instances.update({"STADTWACHE_310": CITY, "STADTWACHE_333": PAIR})
        self.guard_state = {npc: True for npc in self.posts}
        self.fields.update({(npc, "WP"): "GATE_POST" for npc in self.posts})
        self.checklog: list[str | None] = []
        self.executed: list[dict[str, Any]] = []
        self.trigger_events = True
        self.vars.update({law_var("city"): 0, **projection_values("city", [])})

    def evaluate(self, expr: Any, npc: str | None = None) -> Any:
        match expr:
            case ["&&" | "||", *terms]:
                assert len(terms) >= 2, "the bridge requires at least two logical terms"
            case ["inst", "SELF"]:
                return npc
            case ["inst", "OTHER" | "HERO"]:
                return "PC_HERO"
            case ["inst", name]:
                return self.instances.get(name, name)
            case ["str" | "sym", name, *_]:
                return name
            case ["field", owner, field, *index]:
                key = (self.evaluate(owner, npc), field, *index[:1])
                return self.fields.get(key, 0)
            case ["+", first, second]:
                return self.evaluate(first, npc) + self.evaluate(second, npc)
            case ["call", "HLP_GETNPC", symbol]:
                return self.evaluate(symbol, npc)
            case ["call", "HLP_STRCMP" | "HLP_ISITEM", first, second]:
                return int(self.evaluate(first, npc) == self.evaluate(second, npc))
            case ["call", "NPC_GETNEARESTWP", owner]:
                return self.posts.get(self.evaluate(owner, npc), self.nearest_waypoint)
            case ["call", "NPC_ISINSTATE", owner, _]:
                return int(self.guard_state.get(self.evaluate(owner, npc), False))
            case ["call", "NPC_GETEQUIPPEDARMOR", _]:
                return self.armor
            case ["call", "NPC_HASEQUIPPEDARMOR", _]:
                return int(bool(self.armor))
            case ["call", "NPC_GETDISTTOWP", *_]:
                return 300
            case ["call", "NPC_REFUSETALK", *_]:
                return 0
            case ["fn", "B_GETCURRENTABSOLUTIONLEVEL", *_]:
                return self.vars.get("ABSOLUTIONLEVEL_CITY", 0)
            case ["fn", "B_GIVEINVITEMS", giver, taker, item, count]:
                who, to = self.evaluate(giver, npc), self.evaluate(taker, npc)
                inventory = self.hero_items if who == "PC_HERO" else self.npc_items.setdefault(who, {})
                other = self.hero_items if to == "PC_HERO" else self.npc_items.setdefault(to, {})
                name, amount = self.evaluate(item, npc), self.evaluate(count, npc)
                if inventory.get(name, 0) < amount:
                    return 0
                inventory[name] -= amount
                other[name] = other.get(name, 0) + amount
                return 1
            case ["fn", "B_GIVEPLAYERXP", amount]:
                self.hero_xp += self.evaluate(amount, npc)
                return 0
            case ["fn", "B_CHECKLOG"]:
                self.checklog.append(npc)
                return 0
            case ["call", "LOG_ADDENTRY" | "LOG_SETTOPICSTATUS" | "LOG_CREATETOPIC", *_]:
                return 0
        return super().evaluate(expr, npc)

    def execute(self, steps: list[dict[str, Any]], npc: str | None) -> Any:
        class Returned(Exception):
            pass

        result = None

        def run(body: list[dict[str, Any]]) -> None:
            nonlocal result
            for step in body:
                match step["do"]:
                    case "set":
                        target, value = step["target"], self.evaluate(step["value"], npc)
                        if target[0] == "var":
                            self.vars[target[1]] = value
                        else:
                            assert target[0] == "field", step
                            self.fields[(self.evaluate(target[1], npc), target[2], *target[3:4])] = value
                    case "if":
                        run(step.get("then" if self.evaluate(step["cond"], npc) else "else", []))
                    case "call":
                        self.evaluate(step["call"], npc)
                    case "return":
                        result = self.evaluate(step["value"], npc) if "value" in step else None
                        raise Returned
                    case _:
                        raise AssertionError(f"unsupported gate step: {step}")

        try:
            run(steps)
        except Returned:
            pass
        return result

    async def handle(self, msg: dict[str, Any]) -> None:
        fired = []
        if msg["type"] == "graph_patch":
            for op in msg["ops"]:
                if op.get("op") == "trigger" and op.get("steps") is not None:
                    assert op["on"] == "state" and op["id"].startswith("DIR_GATE_"), op
                    npc = op.get("npc", "PC_HERO")
                    if not self.vars.get(op["id"], 0) and self.evaluate(op.get("when", ["int", 1]), npc):
                        self.execute(op["steps"], npc)
                        self.vars[op["id"]] = 1
                        self.executed.append(copy.deepcopy(op))
                        fired.append((op["id"], npc))
        elif msg["type"] == "story_effect":
            for effect in msg["effects"]:
                if effect["do"] == "set":
                    self.vars[effect["var"]] = effect["value"]
        await super().handle(msg)
        if self.trigger_events:
            for id, npc in fired:
                await self.send(type="trigger_fired", id=id, count=1, npc=npc, other="PC_HERO")


def _has(item: str, count: int = 1) -> list:
    return [">=", ["call", "NPC_HASITEMS", HERO, ["sym", item]], ["int", count]]


def _admission(npc: str, *, when: list | None = None) -> list[dict[str, Any]]:
    effects = [{
        "do": "set_field", "of": SELF, "field": "AIVAR", "index": ["int", 14, "AIV_PASSGATE"],
        "value": ["int", 1],
    }]
    if npc in (CITY, PAIR):
        effects += [{
            "do": "set_field", "of": ["inst", PAIR if npc == CITY else CITY], "field": "AIVAR",
            "index": ["int", 14, "AIV_PASSGATE"], "value": ["int", 1],
        }, {"do": "set", "var": "MIL_310_SCHONMALREINGELASSEN", "value": ["int", 1]}]
    effects.append({"do": "call", "fn": "B_CHECKLOG", "args": []})
    return [{**e, **({"when": when} if when is not None else {})} for e in effects]


def gate_graph() -> StoryGraph:
    """Small lifted graph: native ids, conditions and guarded PASSGATE effects."""
    nodes: dict[str, Node] = {}

    def add(id: str, npc: str, condition: list, *, when: list | None = None) -> None:
        nodes[id] = Node(id, "dialog", npc, id, _admission(npc, when=when), set(), condition=condition)

    for npc in (CITY, PAIR):
        prefix = f"DIA_{npc}"
        nodes[f"{prefix}_FIRSTWARN"] = Node(
            f"{prefix}_FIRSTWARN", "dialog", npc, "Стой!", [], set(),
            condition=["call", "HLP_STRCMP", ["call", "NPC_GETNEARESTWP", SELF],
                       ["field", SELF, "WP"]],
        )
        add(f"{prefix}_PASSIERSCHEIN", npc, _has(LETTER))
        add(f"{prefix}_ZUMSCHMIED", npc, ["&&",
            ["==", ["call", "NPC_GETEQUIPPEDARMOR", HERO], ["sym", "ITAR_BAU_L"]],
            ["call", "NPC_KNOWSINFO", HERO, ["sym", "DIA_MALETH_TOTHECITY"]],
        ])
        add(f"DIA_ADDON_{npc}_CONSTANTINO", npc,
            ["==", ["var", HERB_QUEST], ["int", 1]], when=_has(HERBS, 10))
        add(f"{prefix}_BRIBE", npc, _has(GOLD, 100))
    nodes[f"DIA_{UPPER}_FIRSTWARN"] = Node(
        f"DIA_{UPPER}_FIRSTWARN", "dialog", UPPER, "Стой!", [], set(),
        condition=["call", "HLP_STRCMP", ["call", "NPC_GETNEARESTWP", SELF], ["field", SELF, "WP"]],
    )
    for suffix, condition in {
        "PASSASCITIZEN": ["==", ["var", "PLAYER_ISAPPRENTICE"], ["int", 1]],
        "PASSASMIL": ["==", ["field", HERO, "GUILD"], ["int", 2]],
        "PASSASMAGE": ["==", ["field", HERO, "GUILD"], ["int", 5]],
        "PASSASSLD": ["==", ["field", HERO, "GUILD"], ["int", 7]],
    }.items():
        add(f"DIA_{UPPER}_{suffix}", UPPER, condition)
    return StoryGraph(nodes, {}, {}, {}, {})


@asynccontextmanager
async def setup_gate(fake: GatesFake, *, npc: str = CITY):
    store = MemoryStore(":memory:")
    fake.symbols.update({GOLD: "item", LETTER: "item", HERBS: "item"})
    fake.item_names.update({GOLD: "Золото", LETTER: "Пропуск", HERBS: "Травы"})
    fake.symbols["VLK_417_CONSTANTINO"] = "npc"
    graph = gate_graph()
    for node in graph.nodes.values():
        steps = []
        for effect in node.effects:
            if effect["do"] == "set_field":
                step = {"do": "set", "target": ["field", effect["of"], "AIVAR", 14],
                        "value": effect["value"]}
            elif effect["do"] == "set":
                step = {"do": "set", "target": ["var", effect["var"]], "value": effect["value"]}
            else:
                assert effect["do"] == "call"
                step = {"do": "call", "call": ["fn", effect["fn"], *effect.get("args", [])]}
            steps.append({"do": "if", "cond": effect["when"], "then": [step]}
                         if "when" in effect else step)
        fake.programs[(node.id, "information")] = steps
    async with connected(fake) as link:
        dialogue = DialogueManager(
            link, FakeBackend("Проходи.", delay=0), PersonaProvider(None), memory=store,
        )
        chronicle = Chronicle(link, store)
        gates = Gates(link, dialogue, chronicle, graph)
        deals = Deals(link, dialogue, chronicle, gates=gates)
        dialogue.extensions.append(deals)
        await _until(lambda: chronicle.branch is not None)
        await link.snapshot(npc)
        conv = dialogue.open(npc, "Стражник")
        await gates.prepare(npc)
        yield link, dialogue, chronicle, gates, deals, conv
        if conv.reply is not None:
            await conv.reply
        if conv.actions:
            await asyncio.gather(*conv.actions)
        if chronicle._background:
            await asyncio.gather(*chronicle._background)
    store.close()


async def _call(system, args: dict[str, Any]):
    link, dialogue, _, gates, _, conv = system
    return await gates.handle(dialogue, conv, ToolCall("let_pass", args, "g1"), link.snapshots.get(conv.npc))


def _has_pass(fake: GatesFake, npc: str) -> bool:
    return fake.fields.get((npc, "AIVAR", 14)) == 1


@pytest.mark.parametrize("basis,item", [(PASS, LETTER), (SMITH, "armor"), (CONSTANTINO, HERBS)])
async def test_native_basis_uses_story_eval_for_current_truth(basis, item):
    fake = GatesFake()
    if item == "armor":
        fake.armor = "ITAR_BAU_L"
        fake.known.add("DIA_MALETH_TOTHECITY")
    else:
        fake.hero_items[item] = 10 if item == HERBS else 1
    fake.vars[HERB_QUEST] = 1
    async with setup_gate(fake) as system:
        _, _, _, gates, _, _ = system
        assert basis in (gates.situation(CITY) or "")
        evaluated = [e for m in fake.story if m["type"] == "story_eval" for e in m["exprs"]]
        node = gate_graph().nodes[basis]
        assert ["&&", node.condition, node.effects[0].get("when", ["int", 1])] in evaluated
        outcome = await _call(system, {"basis": basis, "claim": "", "about": ""})
        assert outcome.status == "accepted"
        await _until(lambda: _has_pass(fake, CITY) and basis in fake.known)
        assert _has_pass(fake, PAIR) and fake.checklog
        assert basis in fake.known


@pytest.mark.parametrize("basis", [PASS, SMITH, CONSTANTINO])
async def test_false_native_basis_cannot_open_gate(basis):
    fake = GatesFake()
    fake.vars[HERB_QUEST] = 1  # the herb quest is running, but no herbs: its guarded admission is false
    async with setup_gate(fake) as system:
        outcome = await _call(system, {"basis": basis, "claim": "", "about": ""})
        assert outcome.status == "rejected"
        assert not _has_pass(fake, CITY) and fake.executed == []


@pytest.mark.parametrize("trigger_events", [True, False])
async def test_claim_opens_both_guards_and_records_belief_with_game_time(monkeypatch, trigger_events):
    fake = GatesFake()
    fake.trigger_events = trigger_events
    fake.day, fake.hour, fake.minute = 4, 13, 27
    async with setup_gate(fake) as system:
        _, dialogue, chronicle, gates, _, _ = system
        jobs = []
        monkeypatch.setattr(dialogue, "after_speech", lambda conv, job: jobs.append(job))
        claim = "Константино ждёт эти травы."
        outcome = await _call(system, {"basis": "claim", "claim": claim, "about": "VLK_417_CONSTANTINO"})
        assert outcome.status == "accepted" and len(jobs) == 1
        assert not _has_pass(fake, CITY) and chronicle.facts("gate_claim") == []
        _, ok = await jobs.pop()()
        assert ok and _has_pass(fake, CITY) and _has_pass(fake, PAIR)
        fact = chronicle.facts("gate_claim")[0]
        assert {key: fact[key] for key in ("npc", "claim", "about", "day", "hour", "minute", "believed")} == {
            "npc": CITY, "claim": claim, "about": "VLK_417_CONSTANTINO",
            "day": 4, "hour": 13, "minute": 27, "believed": True,
        }
        assert "Ворота" in (gates.situation(CITY) or "")


async def test_known_unpaid_theft_blocks_native_claim_and_bribe():
    fake = GatesFake()
    fake.vars.update({law_var("city"): 1, **projection_values("city", [{"kind": "theft"}])})
    fake.hero_items.update({LETTER: 1, GOLD: 100})
    async with setup_gate(fake) as system:
        _, _, chronicle, gates, deals, _ = system
        for args in ({"basis": PASS}, {"basis": "claim", "claim": "Я к кузнецу.", "about": ""}):
            outcome = await _call(system, args)
            assert outcome.status == "rejected"
        assert await gates.bribe_error(CITY, 100)
        outcome = await deals.propose(CITY, "Стражник", {
            "hero_gives": [{"item": GOLD, "count": 100}], "let_pass": True,
        }, system[0].snapshots[CITY])
        assert outcome.status == "rejected"
        assert chronicle.facts("gate_claim") == [] and fake.executed == []
        assert fake.hero_items[GOLD] == 100 and not _has_pass(fake, CITY)


@pytest.mark.parametrize("trigger_events", [True, False])
async def test_accepted_gold_deal_writes_native_pass_and_bribe_absolution(trigger_events):
    fake = GatesFake()
    fake.trigger_events = trigger_events
    fake.hero_items[GOLD] = 100
    fake.vars["ABSOLUTIONLEVEL_CITY"] = 3
    async with setup_gate(fake) as system:
        link, _, chronicle, _, deals, _ = system
        outcome = await deals.propose(CITY, "Стражник", {
            "hero_gives": [{"item": GOLD, "count": 100}], "let_pass": True,
        }, link.snapshots[CITY])
        assert outcome.status == "accepted"
        await _until(lambda: any(m["type"] == "deal_show" for m in fake.deals))
        shown = next(m for m in fake.deals if m["type"] == "deal_show")
        deal = deals.open[CITY]
        assert fake.hero_items[GOLD] == 100 and not _has_pass(fake, CITY)
        await fake.send(type="deal_accept", deal=shown["deal"], version=shown["version"])
        await _until(lambda: chronicle.facts("deal") != [], timeout=8)
        assert deal.state == "done"
        assert fake.hero_items[GOLD] == 0 and fake.npc_items[CITY][GOLD] == 100
        assert _has_pass(fake, CITY) and _has_pass(fake, PAIR)
        assert fake.vars["MIL_310_SCHONMALREINGELASSEN"] == 1
        assert fake.vars["MIL_310_PERSONAL_ABSOLUTIONLEVEL"] == 4
        assert fake.executed and fake.checklog
        assert chronicle.facts("deal")[0]["let_pass"] is True


async def test_repeated_admissions_reuse_and_reset_the_same_two_globals():
    fake = GatesFake()
    fake.hero_items[GOLD] = 200
    async with setup_gate(fake) as system:
        _, _, _, gates, _, _ = system
        effects = [{"do": "give", "item": GOLD, "count": 100, "to": "self"}]
        await gates.bribe(CITY, effects)
        variables = {name for name in fake.vars if name.startswith("DIR_GATE_")}
        assert len(variables) == 2
        await gates.bribe(CITY, effects)
        assert {name for name in fake.vars if name.startswith("DIR_GATE_")} == variables
        assert all(fake.vars[name] == 1 for name in variables)
        assert len(fake.executed) == 2 and fake.executed[0]["id"] == fake.executed[1]["id"]
        assert fake.hero_items[GOLD] == 0 and fake.npc_items[CITY][GOLD] == 200
        assert _has_pass(fake, CITY) and _has_pass(fake, PAIR)


@pytest.mark.parametrize("npc,amount,reason", [(CITY, 40, "60"), (UPPER, 100, "bribes")])
async def test_guard_minimum_and_no_bribes_for_upper_quarter(npc, amount, reason):
    fake = GatesFake()
    fake.hero_items[GOLD] = 100
    async with setup_gate(fake, npc=npc) as system:
        link, _, _, gates, deals, _ = system
        assert reason in (await gates.bribe_error(npc, amount)).lower()
        outcome = await deals.propose(npc, "Стражник", {
            "hero_gives": [{"item": GOLD, "count": amount}], "let_pass": True,
        }, link.snapshots[npc])
        assert outcome.status == "rejected" and reason in outcome.output.lower()
        assert fake.deals == [] and fake.hero_items[GOLD] == 100


async def test_guard_must_still_be_at_his_post_when_admission_runs():
    fake = GatesFake()
    fake.hero_items[LETTER] = 1
    async with setup_gate(fake) as system:
        _, _, chronicle, gates, _, _ = system
        assert "let_pass" in [t.name for t in gates.tools(CITY)]
        fake.posts[CITY] = "TAVERN"
        outcome = await _call(system, {"basis": "claim", "claim": "Я к кузнецу.", "about": ""})
        assert outcome.status == "rejected" and not _has_pass(fake, CITY)
        assert await gates.bribe_error(CITY, 100)
        await gates.prepare(CITY)
        assert gates.tools(CITY) == [] and gates.situation(CITY) is None
        assert chronicle.facts("gate_claim") == []
