"""The cave gang takes a payment, keeps a truce, then reacts to a drawn weapon."""

from __future__ import annotations

import asyncio
import math
import time
from typing import TYPE_CHECKING, Any

from ..bodies import Bodies
from ..brain.reflex import Reflexes
from ..deals import GOLD
from ..game import GameLink
from ..llm import ToolCall
from ..parley import EXPIRY_CUE, HAIL_CUE, NO_REASON
from ..protocol import GameMessage, NpcRoster
from .common import FixedDecider, _logged, _m5b_player_weapon, _until, log, model_off

if TYPE_CHECKING:
    from ..deals import Deals
    from ..dialogue import Conversation, DialogueManager, TurnState
    from ..parley import Parley
    from ..talk import TalkDesk
    from ..turn import ToolOutcome
    from .live import LiveCase

BRAGO, MATE = "BDT_1014_BANDIT_L", "BDT_1015_BANDIT_L"
CAVALORN, PARK = "BAU_4300_ADDON_CAVALORN", "NW_FARM1_FIELD_06"
GREETING = "Вечер в хату, бродяги."
PAYMENT = "Браго, дам сто золотых за мир с вашей шайкой."
MATE_CHALLENGE = "Бандит, а ты меня теперь тронешь?"
ARMED_CHALLENGE = "Браго, оружие наголо."


async def bandit_parley(
    link: GameLink,
    dialogue: DialogueManager,
    talk: TalkDesk,
    deals: Deals,
    parley: Parley,
    *, live: LiveCase | None = None,
) -> dict[str, str]:
    """Given Brago's demands and attacks, check rejection, payment and the gang's truce."""
    attack = ToolCall("set_intent", {
        "goal": "fight", "target": "PLAYER", "attitude": "hostile", "agenda": None,
    })
    if live is None:
        off = model_off(dialogue)
        off.given(
            "дано: главарь окликает чужака у стоянки", who=BRAGO, when=HAIL_CUE,
            line="Стоять. Ты кто такой?",
        )
        off.given("дано: Браго хочет напасть при встрече", who=BRAGO, when=GREETING, calls=[attack])
        off.given(
            "дано: Браго требует сто золотых за тридцать секунд после отказа нападать без повода",
            who=BRAGO, when=GREETING,
            calls=[ToolCall("ultimatum", {"demand": "Заплати сто золотых за проход", "seconds": 30})],
        )
        off.silent("дано: Браго молчит (ультиматум поставлен)", who=BRAGO, when=GREETING)
        off.given(
            "дано: Браго нападает по истечении неотвеченного ультиматума",
            who=BRAGO, when=EXPIRY_CUE, calls=[attack],
        )
        off.silent("дано: Браго молчит (нападение после срока принято)", who=BRAGO, when=EXPIRY_CUE)
        off.given(
            "дано: Браго предлагает четыре часа перемирия за сто золотых", who=BRAGO, when=PAYMENT,
            calls=[ToolCall("propose_deal", {
                "npc_gives": [], "hero_gives": [{"item": GOLD, "count": 100}], "truce": {"hours": 4},
            })],
        )
        off.silent("дано: Браго молчит (перемирие предложено)", who=BRAGO, when=PAYMENT)
        off.silent(
            "дано: Браго молчит (сделка состоялась)", who=BRAGO, when="Герой согласился, сделка состоялась",
        )
        off.given("дано: товарищ Браго хочет напасть во время перемирия",
                  who=MATE, when=MATE_CHALLENGE, calls=[attack])
        off.silent("дано: товарищ Браго молчит (нападение отклонено из-за перемирия)",
                   who=MATE, when=MATE_CHALLENGE)
        off.given("дано: Браго нападает при обнажённом оружии героя",
                  who=BRAGO, when=ARMED_CHALLENGE, calls=[attack])
        off.silent("дано: Браго молчит (нападение принято)", who=BRAGO, when=ARMED_CHALLENGE)
    await link.wait_world()
    results: dict[str, str] = {}
    entries: list[dict[str, Any]] = []
    native_attacks: list[tuple[int, str]] = []
    original_reflexes = talk.reflexes
    original_hail_talk = parley.talk
    if live is not None:
        # Start at the rejected attack, before an autonomous hail can set an ultimatum.
        parley.talk = None
    original_tool = dialogue._tool
    outputs: list[tuple[str, ToolOutcome]] = []
    original_sink = dialogue.journal.sink

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
                (message.t, row.id) for row in message.npcs
                if row.id in {BRAGO, MATE} and row.state == "ZS_ATTACK"
            )

    def attacks() -> list[dict[str, Any]]:
        return [entry for entry in entries
                if entry.get("kind") == "attack" and entry.get("npc") in {BRAGO, MATE}]

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

        if live is not None:
            conv = dialogue.open(BRAGO, brago.npc.name, warm=False)
            rejection = dialogue._set_intent(conv, attack, brago)
            if not check("first_fight_rejected", rejection.status == "rejected"
                         and rejection.output == f"Отклонено: {NO_REASON}"
                         and not attacks() and not native_attacks, rejection.output):
                return _logged(results)
            if not check("partner", talk.begin(BRAGO, brago.npc.name)):
                return _logged(results)
            prior = {row["id"] for row in live.records}
            dialogue.event_turn(
                BRAGO, f"Герой подошёл и сказал: «{GREETING}» Ты попытался напасть. {rejection.output}",
                tools={"ultimatum", "propose_deal", "set_intent", "recall"}, kind="parley_rejected",
            )

            def rows() -> list[dict[str, Any]]:
                return [row for row in live.turns(BRAGO) if row["id"] not in prior]

            def calls() -> list[dict[str, Any]]:
                return [call for row in rows() for step in row.get("steps", [])
                        for call in step.get("calls", [])]

            def repeated_attack() -> bool:
                return any(call.get("name") == "set_intent" and call.get("outcome") == "rejected"
                           and call.get("result") == f"Отклонено: {NO_REASON}" for call in calls())

            def terms() -> bool:
                return any(call.get("outcome") == "accepted" and (
                    call.get("name") == "ultimatum" and parley.ultimatum(BRAGO) is not None
                    or call.get("name") == "propose_deal" and BRAGO in deals.open
                ) for call in calls())

            await _until(lambda: bool(rows()) and (repeated_attack() or terms()), 60)
            success, failed = terms(), repeated_attack()
            live.finish("failure" if failed else "success" if success else "no_decision", evidence={
                "decision": "repeated_unprovoked_attack" if failed else "terms" if success else "no_decision",
                "initial_rejection": {
                    "name": attack.name, "arguments": attack.arguments,
                    "outcome": rejection.status, "result": rejection.output,
                },
                "calls": calls(),
                "ultimatum": parley.ultimatum(BRAGO),
                "offer": deals.open[BRAGO].terms() if BRAGO in deals.open else None,
                "attacks": attacks(), "native_attacks": native_attacks,
                "words": [step.get("text", "") for row in rows() for step in row.get("steps", [])],
            })
            return _logged(results)

        await _until(lambda: dialogue.has_spoken(BRAGO), 10)
        await _until(lambda: not dialogue.speaking(BRAGO), 10)
        talk.hear(GREETING, BRAGO, {"via": "scenario"})
        rejected = await _until(lambda: any(
            npc == BRAGO and outcome.status == "rejected" and "Без повода" in outcome.output
            for npc, outcome in outputs
        ), 20)
        if not check("first_fight_rejected", rejected and not attacks() and not native_attacks,
                     f"rejected={rejected}, attacks={len(attacks())}"):
            return _logged(results)
        await asyncio.sleep(5)
        ultimatum = parley.ultimatum(BRAGO)
        waiting = bool(ultimatum and ultimatum.get("status") == "active")
        if not check("ultimatum_wait", waiting and not attacks() and not native_attacks,
                     f"active={waiting}, wait_s=5, journal_attacks={len(attacks())}, "
                     f"native_attacks={len(native_attacks)}"):
            return _logged(results)
        expired = await _until(lambda: bool(
            (threat := parley.ultimatum(BRAGO)) and threat.get("status") == "expired"
        ), 35)
        attacked = await _until(lambda: any(npc == BRAGO for _, npc in native_attacks), 5)
        attacking, target_hero = await link.story_eval(BRAGO, [
            ["call", "NPC_ISINSTATE", ["inst", "SELF"], ["sym", "ZS_ATTACK"]],
            ["&&", ["call", "NPC_GETTARGET", ["inst", "SELF"]],
             ["call", "NPC_ISPLAYER", ["inst", "OTHER"]]],
        ])
        threat = parley.ultimatum(BRAGO)
        if not check("ultimatum_expires", bool(
            expired and attacked and attacking and target_hero and threat
            and not threat.get("answered")
            and all(at >= threat["deadline"] for at, _ in native_attacks)
        ), f"expired={expired}, answered={threat.get('answered') if threat else None}, "
           f"target_PLAYER={target_hero}, "
           f"attack_after_ms={[at - threat['deadline'] for at, _ in native_attacks] if threat else []}"):
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
        calmed = await _until(lambda: all(
            npc in talk.people and talk.people[npc].state != "ZS_ATTACK" for npc in (BRAGO, MATE)
        ), 10)
        hero_after, brago_after = await gold("HERO"), await gold(BRAGO)
        fact = next((fact for fact in deals.chronicle.facts("truce")
                     if fact.get("npc") == BRAGO and MATE in fact.get("members", [])), None)
        if not check("truce_paid", bool(
            pressed == "sent" and accepted and calmed and fact
            and hero_after == 100 and brago_after == brago_gold + 100
            and parley.ultimatum(BRAGO) is None
        ), f"Y={pressed}, calmed={calmed}, hero_gold={hero_after}, "
           f"Brago_gold={brago_gold}->{brago_after}, fact={fact}"):
            return _logged(results)

        journal_mark, native_mark = len(attacks()), len(native_attacks)
        mark = len(outputs)
        talk.hear(MATE_CHALLENGE, MATE, {"via": "scenario"})
        refused = await _until(lambda: any(
            npc == MATE and outcome.status == "rejected" and "у вас уговор" in outcome.output.lower()
            for npc, outcome in outputs[mark:]
        ), 15)
        if not check("gang_fight_rejected", refused, [outcome.output for _, outcome in outputs[mark:]]):
            return _logged(results)
        peaceful_at = time.monotonic()
        await asyncio.sleep(20)
        if not check("gang_keeps_truce", bool(
            len(attacks()) == journal_mark and len(native_attacks) == native_mark
            and parley.truce(BRAGO) and parley.truce(MATE)
        ), f"wait_s={time.monotonic() - peaceful_at:.1f}, "
           f"journal_attacks={len(attacks()) - journal_mark}, "
           f"native_attacks={len(native_attacks) - native_mark}"):
            return _logged(results)

        await _until(lambda: not dialogue.speaking(MATE), 10)
        await _m5b_player_weapon(link, "melee")
        # Refresh the same production perception used by fight_reason before the model's request.
        if parley.perception is not None:
            await parley.perception.fresh(BRAGO)
        await link.snapshot(BRAGO, fresh=True)
        talk.hear(ARMED_CHALLENGE, BRAGO, {"via": "scenario"})
        attacked = await _until(lambda: len(attacks()) > journal_mark, 20)
        conv = dialogue.conversations.get(BRAGO)
        check("weapon_breaks_truce", bool(
            attacked and conv and (conv.intent.goal, conv.intent.target) == ("fight", "PLAYER")
            and any(entry.get("npc") == BRAGO for entry in attacks()[journal_mark:])
            and parley.truce(BRAGO) is None and parley.truce(MATE) is None
        ), f"journal_attacks={attacks()[journal_mark:]}, "
           f"truce_broken={deals.chronicle.facts('truce_broken')}")
    finally:
        parley.talk = original_hail_talk
        talk.reflexes = original_reflexes
        dialogue._tool = original_tool
        dialogue.journal.sink = original_sink
        link.listeners.remove(record)
    return _logged(results)
