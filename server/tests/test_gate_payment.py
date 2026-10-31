"""Gate payment retries must not charge the hero twice after a lost response."""

from __future__ import annotations

import copy
from typing import Any

import pytest

from gothic_ai.game import GameError
from gothic_ai.llm import ToolCall
from tests.test_gates import CITY, GOLD, GatesFake, setup_gate


class StaleGateFake(GatesFake):
    """An installed state trigger can run again as soon as its counter is reset."""

    def __init__(self) -> None:
        super().__init__()
        self.active: dict[str, dict[str, Any]] = {}

    async def handle(self, msg: dict[str, Any]) -> None:
        if msg["type"] == "graph_patch":
            for op in msg["ops"]:
                if op.get("op") == "trigger":
                    if op.get("steps") is None:
                        self.active.pop(op["id"], None)
                    else:
                        self.active[op["id"]] = copy.deepcopy(op)
        await super().handle(msg)
        if msg["type"] == "story_effect":
            for trigger, op in self.active.items():
                if not self.vars.get(trigger, 0):
                    self.execute(op["steps"], op["npc"])
                    self.vars[trigger] = 1
                    self.executed.append(copy.deepcopy(op))
                    await self.send(
                        type="trigger_fired", id=trigger, count=1, npc=op["npc"], other="PC_HERO"
                    )


async def test_failed_trigger_cleanup_does_not_replay_old_payment(monkeypatch):
    fake = StaleGateFake()
    fake.hero_items[GOLD] = 200
    async with setup_gate(fake) as system:
        link, _, _, gates, _, _ = system
        patch_graph = link.patch_graph
        removals = 0

        async def fail_first_cleanup(ops, *args, **kwargs):
            nonlocal removals
            if any(op.get("op") == "trigger" and op.get("steps") is None for op in ops):
                removals += 1
                if removals == 2:
                    raise GameError("lost trigger cleanup response")
            await patch_graph(ops, *args, **kwargs)

        monkeypatch.setattr(link, "patch_graph", fail_first_cleanup)
        effects = [{"do": "give", "item": GOLD, "count": 100, "to": "self"}]
        await gates.bribe(CITY, effects)
        assert fake.hero_items[GOLD] == 100 and len(fake.active) == 1
        await gates.bribe(CITY, effects)
        assert removals == 4 and fake.active == {}
        assert fake.hero_items[GOLD] == 0 and fake.npc_items[CITY][GOLD] == 200
        assert len(fake.executed) == 2


async def test_lost_result_after_trigger_timeout_keeps_payment_unknown_and_blocks_retry(monkeypatch):
    fake = GatesFake()
    fake.trigger_events = False
    fake.hero_items[GOLD] = 200
    async with setup_gate(fake) as system:
        link, dialogue, chronicle, _, deals, conv = system
        args = {"hero_gives": [{"item": GOLD, "count": 100}], "let_pass": True}
        offered = await deals.propose(CITY, "Стражник", args, link.snapshots[CITY])
        assert offered.status == "accepted"
        deal = deals.open[CITY]
        story_eval = link.story_eval
        trigger = "DIR_GATE_" + CITY

        async def fail_result_read(npc, exprs, *args, **kwargs):
            if exprs == [["var", trigger], ["var", trigger + "_RESULT"]]:
                raise GameError("lost result read")
            return await story_eval(npc, exprs, *args, **kwargs)

        monkeypatch.setattr(link, "story_eval", fail_result_read)
        await deals.accept(deal.id, deal.version)
        assert fake.hero_items[GOLD] == 100 and fake.npc_items[CITY][GOLD] == 100
        assert deal.state == "unknown" and deals.open[CITY] is deal
        assert chronicle.facts("deal") == [] and deals.tools(CITY) == []
        assert "Повторно брать плату нельзя" in (deals.situation(CITY) or "")
        assert any("Исход принятой сделки" in e.content for e in conv.memory.entries)

        await deals.accept(deal.id, deal.version)
        repeated = await deals.propose(CITY, "Стражник", args, link.snapshots[CITY])
        withdrawn = await deals.handle(
            dialogue, conv, ToolCall("withdraw_deal", {}, "withdraw"), link.snapshots[CITY]
        )
        assert repeated.status == withdrawn.status == "rejected"
        deals.end(CITY)
        assert deals.open[CITY] is deal and fake.hero_items[GOLD] == 100
        assert len(fake.executed) == 1


@pytest.mark.parametrize("completes_before_removal", [False, True])
async def test_unfired_program_is_resolved_after_removing_trigger(monkeypatch, completes_before_removal):
    fake = GatesFake()
    fake.trigger_events = False
    fake.hero_items[GOLD] = 200
    async with setup_gate(fake) as system:
        link, _, chronicle, _, deals, _ = system
        patch_graph = link.patch_graph
        pending = None

        async def delay_program(ops, *args, **kwargs):
            nonlocal pending
            delayed = copy.deepcopy(ops)
            for op in delayed:
                if op.get("op") != "trigger":
                    continue
                if op.get("steps") is not None:
                    pending = copy.deepcopy(op)
                    op["when"] = ["int", 0]
                elif pending is not None:
                    if completes_before_removal:
                        fake.execute(pending["steps"], pending["npc"])
                        fake.vars[pending["id"]] = 1
                        fake.executed.append(pending)
                    pending = None
            await patch_graph(delayed, *args, **kwargs)

        monkeypatch.setattr(link, "patch_graph", delay_program)
        args = {"hero_gives": [{"item": GOLD, "count": 100}], "let_pass": True}
        offered = await deals.propose(CITY, "Стражник", args, link.snapshots[CITY])
        assert offered.status == "accepted"
        deal = deals.open[CITY]
        await deals.accept(deal.id, deal.version)
        assert deal.state == ("done" if completes_before_removal else "withdrawn")
        assert CITY not in deals.open
        assert fake.hero_items[GOLD] == (100 if completes_before_removal else 200)
        assert len(chronicle.facts("deal")) == int(completes_before_removal)
        deals.end(CITY)
        assert deals.tools(CITY)
