"""The cave gang takes a payment, keeps a truce, then reacts to a drawn weapon."""

from __future__ import annotations

import asyncio
import math
import time
from typing import TYPE_CHECKING, Any

from ..bodies import Bodies
from ..brain.reflex import Reflexes
from ..deals import GOLD
from ..game import GameError, GameLink
from ..llm import ToolCall
from ..llm.fake import FakeStep
from ..protocol import GameMessage, NpcRoster
from .common import ClaimScript, FixedDecider, _logged, _m5b_player_weapon, _until

if TYPE_CHECKING:
    from ..deals import Deals
    from ..dialogue import DialogueManager
    from ..parley import Parley
    from ..talk import TalkDesk

BRAGO, MATE = "BDT_1014_BANDIT_L", "BDT_1015_BANDIT_L"
CAVALORN, PARK = "BAU_4300_ADDON_CAVALORN", "NW_FARM1_FIELD_06"
GREETING = "Вечер в хату, бродяги."
PAYMENT = "Браго, дам сто золотых за мир с вашей шайкой."
MATE_CHALLENGE = "Бандит, а ты меня теперь тронешь?"
ARMED_CHALLENGE = "Браго, оружие наголо."


def _bandit_script() -> ClaimScript:
    attack = ToolCall("set_intent", {
        "goal": "fight", "target": "PLAYER", "attitude": "hostile", "agenda": None,
    })
    return ClaimScript(
        said=[
            (GREETING, FakeStep("Стоять. Ты кто такой?", [attack])),
            (PAYMENT, FakeStep("Сотню сюда — и четыре часа никто из наших тебя не тронет.", [ToolCall(
                "propose_deal", {
                    "npc_gives": [], "hero_gives": [{"item": GOLD, "count": 100}],
                    "truce": {"hours": 4},
                },
            )])),
            (MATE_CHALLENGE, FakeStep("А ну-ка, иди сюда!", [attack])),
            (ARMED_CHALLENGE, FakeStep("За оружие взялся? Сам напросился!", [attack])),
        ],
        told=[("Без повода", FakeStep("Сто золотых за проход. Десять секунд на ответ.", [ToolCall(
            "ultimatum", {"demand": "Заплати сто золотых за проход", "seconds": 10},
        )]))],
        default="Жду твоего решения.",
    )


async def bandit_parley(
    link: GameLink,
    dialogue: DialogueManager,
    talk: TalkDesk,
    deals: Deals,
    parley: Parley,
) -> dict[str, str]:
    """New game, LLM=fake: rejected first attack, a paid gang truce and a real new cause."""
    if dialogue.llm.name != "fake":
        raise GameError("scenario bandit_parley requires LLM=fake and a new game without SAVE")
    await link.wait_world()
    results: dict[str, str] = {}
    entries: list[dict[str, Any]] = []
    native_attacks: list[tuple[float, str]] = []
    original_llm, original_reflexes = dialogue.llm, talk.reflexes
    original_sink = dialogue.journal.sink
    script = _bandit_script()

    def check(name: str, ok: bool, detail: object = "") -> bool:
        results[name] = f"{'done' if ok else 'failed'} ({detail})"
        return ok

    def keep(entry: dict[str, Any]) -> None:
        entries.append(entry)
        if original_sink is not None:
            original_sink(entry)

    def record(message: GameMessage) -> None:
        if isinstance(message, NpcRoster):
            native_attacks.extend(
                (time.monotonic(), row.id) for row in message.npcs
                if row.id in {BRAGO, MATE} and row.state == "ZS_ATTACK"
            )

    def attacks() -> list[dict[str, Any]]:
        return [entry for entry in entries
                if entry.get("kind") == "attack" and entry.get("npc") in {BRAGO, MATE}]

    async def gold(npc: str) -> int:
        return int(await link.evaluate([
            "call", "NPC_HASITEMS", ["inst", npc], ["sym", GOLD],
        ]) or 0)

    dialogue.llm = script
    talk.reflexes = Reflexes(FixedDecider({"posture": "listen"}))
    dialogue.journal.sink = keep
    link.listeners.append(record)
    try:
        if not check("new_game", not deals.chronicle.facts("deal") and not deals.chronicle.facts("truce"),
                     "no prior deals or truces"):
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
        if current_gold != 200:
            transfer: dict[str, Any] = {"do": "give", "item": GOLD, "count": abs(200 - current_gold)}
            if current_gold > 200:
                transfer["to"] = "self"
            await link.story_effect(BRAGO, [transfer])
        await link.debug("player_goto", to=BRAGO, distance=250)
        ready = await _until(lambda: all(
            npc in talk.people and talk.people[npc].distance <= 1500 for npc in (BRAGO, MATE)
        ), 20)
        brago = await link.snapshot(BRAGO, fresh=True)
        cavalorn = await link.snapshot(CAVALORN, fresh=True)
        away = math.dist(brago.npc.pos, cavalorn.npc.pos)
        hero_gold, brago_gold = await gold("HERO"), await gold(BRAGO)
        if not check("prepared", bool(
            ready and parked.status == "arrived" and away > 6000 and hero_gold == 200
            and brago.player and brago.player.distance <= 400
        ), f"near={ready}, Cavalorn={away:.0f} cm ({parked.status}), hero_gold={hero_gold}"):
            return _logged(results)

        talk.hear(GREETING, BRAGO, {"via": "scenario"})
        required = await _until(lambda: parley.ultimatum(BRAGO) is not None, 20)
        rejected = any("Без повода" in output for output in script.outputs)
        if not check("first_fight_rejected", rejected and required and not attacks() and not native_attacks,
                     f"rejected={rejected}, ultimatum={required}, attacks={len(attacks())}"):
            return _logged(results)
        expired = await _until(lambda: bool(
            (ultimatum := parley.ultimatum(BRAGO)) and ultimatum.get("status") == "expired"
        ), 20)
        if not check("ultimatum_wait", expired and not attacks() and not native_attacks,
                     f"expired={expired}, journal_attacks={len(attacks())}, "
                     f"native_attacks={len(native_attacks)}"):
            return _logged(results)
        await _until(lambda: not dialogue.speaking(BRAGO), 10)
        talk.hear(PAYMENT, BRAGO, {"via": "scenario"})
        offered = await _until(lambda: BRAGO in deals.open, 20)
        window = await link.deal_window() or {}
        if not check("truce_offered", offered and any(
            "перемир" in text.lower() or "уговор" in text.lower() for text in window.get("npc_gives", [])
        ), window):
            return _logged(results)
        await asyncio.sleep(1.2)
        pressed = await link.press_deal()
        accepted = await _until(lambda: bool(parley.truce(BRAGO) and parley.truce(MATE)), 15)
        hero_after, brago_after = await gold("HERO"), await gold(BRAGO)
        fact = next((fact for fact in deals.chronicle.facts("truce")
                     if fact.get("npc") == BRAGO and MATE in fact.get("members", [])), None)
        if not check("truce_paid", bool(
            pressed == "sent" and accepted and fact and hero_after == 100 and brago_after == brago_gold + 100
            and parley.ultimatum(BRAGO) is None
        ), f"Y={pressed}, hero_gold={hero_after}, Brago_gold={brago_gold}->{brago_after}, fact={fact}"):
            return _logged(results)

        mark = len(script.outputs)
        talk.hear(MATE_CHALLENGE, MATE, {"via": "scenario"})
        refused = await _until(lambda: any("у вас уговор" in output.lower()
                                              for output in script.outputs[mark:]), 15)
        if not check("gang_fight_rejected", refused, script.outputs[mark:]):
            return _logged(results)
        peaceful_at = time.monotonic()
        await asyncio.sleep(20)
        if not check("gang_keeps_truce", bool(
            not attacks() and not native_attacks and parley.truce(BRAGO) and parley.truce(MATE)
        ), f"wait_s={time.monotonic() - peaceful_at:.1f}, journal_attacks={len(attacks())}, "
           f"native_attacks={len(native_attacks)}"):
            return _logged(results)

        await _until(lambda: not dialogue.speaking(MATE), 10)
        await _m5b_player_weapon(link, "melee")
        # Refresh the same production perception used by fight_reason before the model's request.
        if parley.perception is not None:
            await parley.perception.fresh(BRAGO)
        await link.snapshot(BRAGO, fresh=True)
        talk.hear(ARMED_CHALLENGE, BRAGO, {"via": "scenario"})
        attacked = await _until(lambda: bool(attacks()), 20)
        conv = dialogue.conversations.get(BRAGO)
        check("weapon_breaks_truce", bool(
            attacked and conv and (conv.intent.goal, conv.intent.target) == ("fight", "PLAYER")
            and any(entry.get("npc") == BRAGO for entry in attacks())
            and parley.truce(BRAGO) is None and parley.truce(MATE) is None
        ), f"journal_attacks={attacks()}, truce_broken={deals.chronicle.facts('truce_broken')}")
    finally:
        dialogue.llm, talk.reflexes = original_llm, original_reflexes
        dialogue.journal.sink = original_sink
        link.listeners.remove(record)
    return _logged(results)
