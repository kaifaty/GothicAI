"""Brago sells the hunt's secret, then starts leading the hero to Dexter."""

from __future__ import annotations

import asyncio
import math
from typing import TYPE_CHECKING, Any

from ..bodies import Bodies
from ..brain.reflex import Reflexes
from ..deals import GOLD
from ..game import GameLink
from ..llm import ToolCall
from ..parley import HAIL_CUE
from .bandit_parley import BRAGO, CAVALORN, MATE, PARK
from .common import FixedDecider, _logged, _m5b_player_weapon, _until, _until_async, log, model_off

if TYPE_CHECKING:
    from ..bounty import Bounty
    from ..deals import Deals
    from ..dialogue import Conversation, DialogueManager, TurnState
    from ..guide import Guide
    from ..talk import TalkDesk
    from ..turn import ToolOutcome
    from .live import LiveCase

ASK = "Браго, кто платит за мою голову?"
PAYMENT = "Браго, дам сто пятьдесят золотых за имя заказчика."
NAME = "Браго, деньги получил. Назови заказчика."
LEAD = "Браго, отведи меня к Декстеру."
DESTINATION = "CASTLEMINE_HUT"


async def bandit_bounty(
    link: GameLink,
    dialogue: DialogueManager,
    talk: TalkDesk,
    deals: Deals,
    bounty: Bounty,
    guide: Guide,
    *, live: LiveCase | None = None,
) -> dict[str, str]:
    """Given Brago's offers, check leverage, payment, vanilla effects and his guide."""
    if live is None:
        off = model_off(dialogue)
        off.given(
            "дано: главарь окликает чужака у стоянки", who=BRAGO, when=HAIL_CUE,
            line="Эй, чужак. Подойди.",
        )
        reveal = ToolCall("tell_secret", {"secret": "client_name"})
        off.given("дано: Браго пытается назвать заказчика без рычага", who=BRAGO, when=ASK, calls=[reveal])
        off.silent("дано: Браго молчит (раскрытие тайны без рычага отклонено)", who=BRAGO, when=ASK)
        off.given(
            "дано: Браго продаёт имя заказчика за сто пятьдесят золотых", who=BRAGO, when=PAYMENT,
            calls=[ToolCall("propose_deal", {
                "npc_gives": [], "hero_gives": [{"item": GOLD, "count": 150}], "secret": "client_name",
            })],
        )
        off.silent("дано: Браго молчит (тайна предложена)", who=BRAGO, when=PAYMENT)
        off.silent(
            "дано: Браго молчит (сделка состоялась)", who=BRAGO, when="Герой согласился, сделка состоялась",
        )
        off.given("дано: Браго называет оплаченного заказчика", who=BRAGO, when=NAME, calls=[reveal])
        off.silent("дано: Браго молчит (имя заказчика раскрыто)", who=BRAGO, when=NAME)
        off.given(
            "дано: Браго предлагает отвести героя к CASTLEMINE_HUT", who=BRAGO, when=LEAD,
            calls=[ToolCall("propose_deal", {"npc_gives": [], "hero_gives": [], "lead": DESTINATION})],
        )
        off.silent("дано: Браго молчит (проводы предложены)", who=BRAGO, when=LEAD)
        off.silent("дано: Браго молчит (начало проводов)", who=BRAGO, when="ты ведёшь его")
    await link.wait_world()
    results: dict[str, str] = {}
    applied_effects: list[dict[str, Any]] = []
    original_reflexes = talk.reflexes
    original_hail_talk = deals.parley.talk if deals.parley is not None else None
    if live is not None and deals.parley is not None:
        # The case starts with the hero's question, before a separate hail decision.
        deals.parley.talk = None
    original_tool = dialogue._tool
    outputs: list[tuple[str, ToolOutcome]] = []
    original_effect = link.story_effect

    def check(name: str, ok: bool, detail: object = "") -> bool:
        results[name] = f"{'done' if ok else 'failed'} ({detail})"
        return ok

    def revealed() -> list[dict[str, Any]]:
        return [fact for fact in deals.chronicle.facts("bounty_secret")
                if fact.get("npc") == BRAGO and fact.get("secret") == "client_name"]

    async def record_effect(
        npc: str, effects: list[dict[str, Any]], timeout: float = 5.0, *, check: bool = False,
    ) -> None:
        await original_effect(npc, effects, timeout, check=check)
        applied_effects.extend(dict(effect) for effect in effects)

    async def gold(npc: str) -> int:
        return int(await link.evaluate([
            "call", "NPC_HASITEMS", ["inst", npc], ["sym", GOLD],
        ]) or 0)

    async def record_tool(conv: Conversation, call: ToolCall, state: TurnState) -> ToolOutcome:
        outcome = await original_tool(conv, call, state)
        outputs.append((conv.npc, outcome))
        return outcome

    dialogue._tool = record_tool
    if live is None:
        log.info("given: дано: решающая модель выбирает слушать героя")
    talk.reflexes = Reflexes(FixedDecider({"posture": "listen"}))
    link.story_effect = record_effect
    try:
        if not check("new_game", not deals.chronicle.facts("deal") and not revealed(),
                     "no prior deals or revealed secrets"):
            return _logged(results)
        for npc in (CAVALORN, BRAGO, MATE, "BDT_1013_BANDIT_L"):
            await link.debug("skip_important", near=npc)
        await link.debug("npc_goto", npc=CAVALORN, waypoint=PARK)
        parked = await Bodies(link).act(
            CAVALORN, "start_activity", state="ZS_STAND_ARMSCROSSED", waypoint=PARK,
            minutes=120, arrive=True, radius=100, timeout=15,
        )
        await link.debug("player_equip", item="ITMW_1H_BAU_MACE")
        await _m5b_player_weapon(link, "none")
        await link.debug("player_hp", hp=1200)
        current_gold = await gold("HERO")
        if current_gold != 300:
            transfer: dict[str, Any] = {"do": "give", "item": GOLD, "count": abs(300 - current_gold)}
            if current_gold > 300:
                transfer["to"] = "self"
            await link.story_effect(BRAGO, [transfer])
        await link.debug("player_goto", to=BRAGO, distance=250)
        ready = await _until(lambda: BRAGO in talk.people and talk.people[BRAGO].distance <= 600, 20)
        brago = await link.snapshot(BRAGO, fresh=True)
        cavalorn = await link.snapshot(CAVALORN, fresh=True)
        away = math.dist(brago.npc.pos, cavalorn.npc.pos)
        hero_gold, brago_gold = await gold("HERO"), await gold(BRAGO)
        if not check("prepared", bool(
            ready and parked.status == "arrived" and away > 6000 and hero_gold == 300
            and brago.player and brago.player.distance <= 400
        ), f"near={ready}, Cavalorn={away:.0f} cm ({parked.status}), hero_gold={hero_gold}"):
            return _logged(results)

        if live is None:
            await _until(lambda: dialogue.has_spoken(BRAGO), 10)
            await _until(lambda: not dialogue.speaking(BRAGO), 10)
        prior = {row["id"] for row in live.records} if live is not None else set()
        talk.hear(ASK, BRAGO, {"via": "scenario"})
        if live is not None:
            def rows() -> list[dict[str, Any]]:
                return [row for row in live.turns(BRAGO, ASK) if row["id"] not in prior]

            def calls() -> list[dict[str, Any]]:
                return [call for row in rows() for step in row.get("steps", [])
                        for call in step.get("calls", [])]

            def offered() -> bool:
                offer = deals.open.get(BRAGO)
                return offer is not None and bool(offer.secret) and any(
                    call.get("name") == "propose_deal" and call.get("outcome") == "accepted"
                    for call in calls()
                )

            def settled() -> bool:
                accepted_reveal = any(call.get("name") == "tell_secret"
                                      and call.get("outcome") == "accepted" for call in calls())
                accepted_offer = any(call.get("name") == "propose_deal"
                                     and call.get("outcome") == "accepted" for call in calls())
                return bool(rows()) and (not accepted_reveal or bool(revealed())) and (
                    not accepted_offer or BRAGO in deals.open
                )

            effects_settled = await _until(settled, 60)
            offer = deals.open.get(BRAGO)
            success, failed = effects_settled and offered() and not revealed(), bool(revealed())
            live.finish("failure" if failed else "success" if success else "no_decision", evidence={
                "decision": "free_name" if failed else "secret_offer" if success else (
                    "pending_effect" if rows() and not effects_settled else "no_sale" if rows()
                    else "no_decision"
                ),
                "effects_settled": effects_settled, "turn_timeout_s": 60,
                "calls": calls(), "revealed": revealed(),
                "offer": offer.terms() if offer is not None else None,
                "secret": offer.secret if offer is not None else None,
                "price": sum(g.count for g in offer.hero_gives if g.item == GOLD) if offer else None,
                "words": [step.get("text", "") for row in rows() for step in row.get("steps", [])],
            })
            return _logged(results)
        refused = await _until(lambda: any(
            npc == BRAGO and outcome.status == "rejected" and "нет рычага" in outcome.output
            for npc, outcome in outputs
        ), 20)
        if not check("secret_requires_leverage", refused and not revealed(),
                     [outcome.output for _, outcome in outputs]):
            return _logged(results)
        await _until(lambda: not dialogue.speaking(BRAGO), 10)
        talk.hear(PAYMENT, BRAGO, {"via": "scenario"})
        offered = await _until(lambda: BRAGO in deals.open, 20)
        offer = deals.open.get(BRAGO)
        window = await link.deal_window() or {}
        if not check("secret_offered", bool(
            offered and offer and offer.secret == "client_name"
            and any("150" in text for text in window.get("hero_gives", []))
        ), window):
            return _logged(results)
        await asyncio.sleep(1.2)
        pressed = await link.press_deal()
        accepted = await _until(lambda: any(
            fact.get("npc") == BRAGO and fact.get("secret") == "client_name"
            for fact in deals.chronicle.facts("deal")
        ), 20)
        hero_after, brago_after = await gold("HERO"), await gold(BRAGO)
        if not check("secret_paid", bool(
            pressed == "sent" and accepted and hero_after == 150 and brago_after == brago_gold + 150
        ), f"Y={pressed}, hero_gold={hero_after}, Brago_gold={brago_gold}->{brago_after}"):
            return _logged(results)

        await _until(lambda: not dialogue.speaking(BRAGO), 10)
        talk.hear(NAME, BRAGO, {"via": "scenario"})
        told = await _until(lambda: bool(revealed()), 20)
        facts = revealed()
        state = await link.story_state(vars=["BDT13_DEXTER_VERRATEN"])
        flag = state.get("vars", {}).get("BDT13_DEXTER_VERRATEN")
        journal = bounty.data["journal"]
        written = any(
            effect.get("do") == "journal" and effect.get("topic") == journal["topic"]
            and effect.get("text") == journal["text"] for effect in applied_effects
        )
        if not check("secret_revealed", bool(
            told and len(facts) == 1 and facts[0].get("leverage") == "paid" and flag == 1 and written
        ), f"facts={facts}, BDT13_DEXTER_VERRATEN={flag}, journal={written}"):
            return _logged(results)

        await _until(lambda: not dialogue.speaking(BRAGO), 10)
        talk.hear(LEAD, BRAGO, {"via": "scenario"})
        await _until(lambda: BRAGO in deals.open, 20)
        start = await link.snapshot(BRAGO, fresh=True)
        await asyncio.sleep(1.2)
        pressed = await link.press_deal()
        started = await _until(lambda: BRAGO in guide.leads and guide.leads[BRAGO].state == "going", 30)
        moved = await _until_async(
            lambda: link.snapshot(BRAGO, fresh=True),
            lambda snap: math.dist(snap.npc.pos, start.npc.pos) > 100, 20,
        )
        check("brago_leads", bool(
            pressed == "sent" and started and moved
            and any(fact.get("npc") == BRAGO and fact.get("lead") == DESTINATION
                    for fact in deals.chronicle.facts("deal"))
        ), f"Y={pressed}, started={started}, moved={moved}")
    finally:
        if deals.parley is not None:
            deals.parley.talk = original_hail_talk
        talk.reflexes = original_reflexes
        dialogue._tool = original_tool
        link.story_effect = original_effect
    return _logged(results)
