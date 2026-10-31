"""Brago sells the hunt's secret, then starts leading the hero to Dexter."""

from __future__ import annotations

import asyncio
import math
from typing import TYPE_CHECKING, Any

from ..bodies import Bodies
from ..brain.reflex import Reflexes
from ..deals import GOLD
from ..game import GameError, GameLink
from ..llm import ToolCall
from ..llm.fake import FakeStep
from .bandit_parley import BRAGO, CAVALORN, MATE, PARK
from .common import ClaimScript, FixedDecider, _logged, _m5b_player_weapon, _until, _until_async

if TYPE_CHECKING:
    from ..bounty import Bounty
    from ..deals import Deals
    from ..dialogue import DialogueManager
    from ..guide import Guide
    from ..talk import TalkDesk

ASK = "Браго, кто платит за мою голову?"
PAYMENT = "Браго, дам сто пятьдесят золотых за имя заказчика."
NAME = "Браго, деньги получил. Назови заказчика."
LEAD = "Браго, отведи меня к Декстеру."
DESTINATION = "CASTLEMINE_HUT"


def _bounty_script() -> ClaimScript:
    reveal = ToolCall("tell_secret", {"secret": "client_name"})
    return ClaimScript(
        said=[
            (ASK, FakeStep("Сейчас расскажу.", [reveal])),
            (PAYMENT, FakeStep("Сто пятьдесят золотых — и скажу имя.", [ToolCall(
                "propose_deal", {
                    "npc_gives": [], "hero_gives": [{"item": GOLD, "count": 150}],
                    "secret": "client_name",
                },
            )])),
            (NAME, FakeStep("За твою голову платит Декстер.", [reveal])),
            (LEAD, FakeStep("Пойдём к Декстеру. За тебя мне заплатят.", [ToolCall(
                "propose_deal", {"npc_gives": [], "hero_gives": [], "lead": DESTINATION},
            )])),
        ],
        told=[],
        default="Жду твоего решения.",
    )


async def bandit_bounty(
    link: GameLink,
    dialogue: DialogueManager,
    talk: TalkDesk,
    deals: Deals,
    bounty: Bounty,
    guide: Guide,
) -> dict[str, str]:
    """New game, LLM=fake: no free secret, a real payment, vanilla effects and Brago's guide."""
    if dialogue.llm.name != "fake":
        raise GameError("scenario bandit_bounty requires LLM=fake and a new game without SAVE")
    await link.wait_world()
    results: dict[str, str] = {}
    applied_effects: list[dict[str, Any]] = []
    original_llm, original_reflexes = dialogue.llm, talk.reflexes
    original_effect = link.story_effect
    script = _bounty_script()

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

    dialogue.llm = script
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

        talk.hear(ASK, BRAGO, {"via": "scenario"})
        refused = await _until(lambda: any("Отклонено" in output for output in script.outputs), 20)
        if not check("secret_requires_leverage", refused and not revealed(), script.outputs):
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
        offered = await _until(lambda: BRAGO in deals.open, 20)
        offer = deals.open.get(BRAGO)
        if not check("lead_offered", bool(
            offered and offer and offer.lead and offer.lead.place.id == DESTINATION
        ), offer.terms() if offer else "no deal"):
            return _logged(results)
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
        dialogue.llm, talk.reflexes = original_llm, original_reflexes
        link.story_effect = original_effect
    return _logged(results)
