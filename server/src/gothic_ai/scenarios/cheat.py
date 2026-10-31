"""A dishonest witness sells silence and an NPC keeps the price but breaks a later payment."""

from __future__ import annotations

import asyncio
import logging
import math
from collections.abc import AsyncIterator
from dataclasses import replace
from pathlib import Path
from typing import TYPE_CHECKING, Any

from ..brain.reflex import Reflexes
from ..deals import GOLD
from ..game import GameError, GameLink
from ..llm import Done, LLMEvent, Message, ToolCall
from ..llm.fake import FakeBackend, FakeStep
from ..protocol import Session
from ..trace import read as read_trace
from .common import (
    HILDA,
    LOBART,
    VINO,
    ClaimScript,
    FixedDecider,
    WitnessReaction,
    _logged,
    _save,
    _until,
    _until_async,
    prepare_theft_witness,
    wait_message,
)

if TYPE_CHECKING:
    from ..crimes import Crimes
    from ..deals import Deal, Deals
    from ..dialogue import DialogueManager
    from ..llm import LLMBackend
    from ..people_knowledge import PeopleKnowledge
    from ..social import Rumors
    from ..talk import TalkDesk

log = logging.getLogger(__name__)
HOUSE = "NW_FARM1_INHOUSE_01"
ASK_PAY = "Лобарт, я пришёл за обещанными семью золотыми. Срок уже настал, отдавай."


async def cheat_scenario(
    link: GameLink,
    service: Crimes,
    dialogue: DialogueManager,
    talk: TalkDesk,
    deals: Deals,
    rumors: Rumors,
    people: PeopleKnowledge,
    log_dir: str,
    slot: int = 7,
) -> dict[str, str]:
    """Fake: native theft, paid silence betrayed by rumor, unpaid npc_later, honest control and load.

    Live runs only the later-payment scene. Its proposal and acceptance are scripted, then exactly
    one real model request receives the ordinary situation and the hero's demand. The response is
    recorded verbatim. Character fixtures and the real model's prompt are never adjusted after it.
    """
    results: dict[str, str] = {}
    original_llm, original_reflexes = dialogue.llm, talk.reflexes
    original_gate, original_stage = rumors.gate, rumors.stage
    original_cards, original_personas = deals.cards, dialogue.personas.cards
    fake = isinstance(original_llm, FakeBackend)
    chronicle, directory = service.chronicle, Path(log_dir)
    facts = chronicle.facts
    participants = {HILDA, LOBART, VINO}

    def check(name: str, ok: bool, detail: object = "") -> bool:
        results[name] = f"{'done' if ok else 'failed'} ({detail})"
        log.info("cheat %-24s %s", name, results[name])
        return ok

    def cards(honesty: int) -> None:
        fixture = {
            **original_cards,
            HILDA: replace(original_cards[HILDA], honesty=1),
            LOBART: replace(original_cards[LOBART], honesty=honesty),
        }
        deals.cards = dialogue.personas.cards = fixture
        # Personas are cached by NPC; this scenario alone changes the cards between its scenes.
        dialogue.personas._get.cache_clear()

    def records(kind: str, deal: str) -> list[dict[str, Any]]:
        return [f for f in facts(kind) if f.get("deal") == deal]

    def turns() -> list[dict[str, Any]]:
        return [
            row
            for path in sorted((directory / "traces").glob("*.jsonl"))
            for row in read_trace(path)
            if row.get("type") == "turn" and row.get("npc") == LOBART
        ]

    async def stand(npc: str, point: str) -> None:
        await link.debug("skip_important", near=npc)
        await link.debug("npc_goto", npc=npc, waypoint=point)
        await link.npc_act(
            npc, "start_activity", state="ZS_STAND_WP", waypoint=point, minutes=120, place=False
        )

    async def settled() -> bool:
        return await _until(
            lambda: all(
                not dialogue.speaking(npc)
                and not (dialogue.conversations.get(npc) and dialogue.conversations[npc].actions)
                for npc in participants
            ),
            60,
        )

    async def release() -> None:
        await settled()
        talk.end("gone")
        await _until(lambda: not participants.intersection(talk.attached), 20)

    async def gold(npc: str = "HERO") -> int:
        return int(await link.evaluate(["call", "NPC_HASITEMS", ["inst", npc], ["sym", GOLD]]) or 0)

    async def near(npc: str, *, hearer: str | None = None) -> bool:
        await release()
        for other in participants - {npc, hearer}:
            await stand(other, parked)
        await stand(npc, field)
        if hearer is not None:
            await stand(hearer, field)
        await link.debug("teleport", near=npc, distance=250)

        def ready() -> bool:
            return all(
                (entry := talk.people.get(person)) is not None
                and entry.distance <= 600 and entry.earshot <= 600
                for person in (npc, hearer) if person is not None
            )

        return await _until(ready, 20)

    async def buy(npc: str, request: str, args: dict[str, Any], label: str) -> Deal | None:
        name = talk.people[npc].name
        dialogue.llm = ClaimScript(
            [(request.replace("ё", "е"), FakeStep(
                "Пять золотых сейчас — и договорились.", [ToolCall("propose_deal", args)]
            ))],
            [],
            "Договорились.",
            only=name,
        )
        await link.debug("type", text=f"{name}, {request}.")
        shown = await _until(lambda: npc in deals.open, 45)
        deal = deals.open.get(npc)
        window = await link.deal_window() or {}
        later = args.get("npc_later")
        if not check(
            f"{label}_window",
            shown and deal is not None and (
                not later or any(
                    "отдаст потом" in str(line).lower() and f"с дня {later['day']}" in str(line)
                    for line in window.get("npc_gives", [])
                )
            ),
            window,
        ):
            return None
        assert deal is not None
        before = await gold()
        await asyncio.sleep(1.2)
        pressed = await link.press_deal()
        accepted = await _until(lambda: bool(records("deal", deal.id)), 30)
        await settled()
        if not check(
            f"{label}_accepted",
            pressed == "sent" and accepted and await gold() == before - 5
            and await link.deal_window() is None,
            {"key": pressed, "deal": deal.id, "gold": [before, await gold()]},
        ):
            return None
        return deal

    class Reply:
        """Observe the ordinary refusal request; every other turn stays scripted, even live."""

        name = "cheat-observed-reply"

        def __init__(self, backend: LLMBackend) -> None:
            self.backend = backend
            self.prompts: list[str] = []
            self.words: list[str] = []

        async def stream(self, **kw: Any) -> AsyncIterator[LLMEvent]:
            last = kw["messages"][-1]
            observed = (
                not self.prompts and kw.get("cache_key") == LOBART
                and isinstance(last, Message)
                and ASK_PAY.replace("ё", "е") in last.content.replace("ё", "е")
            )
            if observed:
                self.prompts.append("\n".join(
                    m.content for m in kw["messages"] if isinstance(m, Message) and m.role == "developer"
                ))
            backend = self.backend if observed else FakeBackend("Ну, как знаешь.", delay=0)
            async for event in backend.stream(**kw):
                if observed and isinstance(event, Done):
                    self.words.append(event.text)
                yield event

    try:
        await link.wait_world()
        if link.session is None:
            await wait_message(link, Session, timeout=10)
        if not check("enabled", deals.cheat, "npc cheat is enabled"):
            return _logged(results)
        cards(1)
        dialogue.llm = FakeBackend("Договорились.", delay=0)
        talk.reflexes = Reflexes(FixedDecider({"posture": "listen"}))
        rumors.gate, rumors.stage = lambda *_: False, None
        await link.debug("player_hp", hp=5000)
        await link.debug("time", hour=8, minute=0)
        humans = {p["npc"] for p in await link.story_people()}
        farm = list(dict.fromkeys([
            *participants,
            *[p["npc"] for p in await link.story_nearby(LOBART, 7000) if p["npc"] in humans],
        ]))
        await link.story_effect(LOBART, [{"do": "end"}])
        points = {p["name"]: p for p in await link.waypoints() if not p.get("fp")}
        house = points[HOUSE]
        center = (house["x"], house["y"], house["z"])
        outside = [p for p in points.values() if p["name"].startswith("NW_FARM1_") and not p.get("room")]
        far = [p for p in outside if math.dist((p["x"], p["y"], p["z"]), center) > 8000]
        fields = [p for p in outside if "FIELD" in p["name"]]
        if not check("positions", bool(far) and bool(fields), f"{len(far)} parking, {len(fields)} fields"):
            return _logged(results)
        parked = min(far, key=lambda p: math.dist((p["x"], p["y"], p["z"]), center))["name"]
        field = min(fields, key=lambda p: math.dist((p["x"], p["y"], p["z"]), center))["name"]
        for npc in farm:
            await stand(npc, parked)
        if await gold() < 100:
            await link.story_effect(LOBART, [{"do": "give", "item": GOLD, "count": 100 - await gold()}])
        # The control has real money, independently of whether the first promise is kept.
        if await gold(LOBART) < 30:
            await link.story_effect(LOBART, [{"do": "give", "item": GOLD, "count": 30, "to": "self"}])

        if fake:
            await stand(LOBART, HOUSE)

            async def drop() -> bool:
                try:
                    await link.debug("npc_drop", npc=LOBART, item="ITMI_SILVERCUP")
                except GameError as err:
                    if "could not drop it now" not in str(err):
                        raise
                    return False
                return True

            if not check("owned_cup", await _until_async(drop, bool, 10), "native Lobart silver cup"):
                return _logged(results)
            await stand(LOBART, parked)
            prepared, detail = await prepare_theft_witness(link, HILDA)
            if not check("native_witness_ready", prepared, detail):
                return _logged(results)
            before = {f["id"] for f in facts("crime")}
            with WitnessReaction(link, chronicle, HILDA) as reaction:
                await link.debug("player_take", item="ITMI_SILVERCUP", distance=1200)
                await _until(lambda: any(f["id"] not in before for f in facts("crime")), 10)
                crime = next((f for f in facts("crime") if f["id"] not in before), None)
                if not check(
                    "native_reaction", crime is not None and await reaction.wait(str(crime["id"])),
                    reaction.detail,
                ):
                    return _logged(results)
            if not check(
                "native_theft", crime is not None and crime.get("kind") == "theft"
                and crime.get("owner") == LOBART and set(crime.get("witnesses", [])) == {HILDA},
                crime,
            ):
                return _logged(results)
            assert crime is not None
            cid = str(crime["id"])
            if not check("silence_near", await near(HILDA), "Hilda hears the offer"):
                return _logged(results)
            silence = await buy(
                HILDA, "возьми пять золотых и никому не говори о кубке",
                {"hero_gives": [{"item": GOLD, "count": 5}], "silence": [f"crime:{cid}"]}, "silence",
            )
            if silence is None:
                return _logged(results)
            if not check(
                "silence_intent", len(records("npc_intent", silence.id)) == 1
                and records("npc_intent", silence.id)[0].get("keeps") is False
                and not records("silenced", silence.id), records("npc_intent", silence.id),
            ):
                return _logged(results)
            await release()
            await rumors.tick()
            rumor = next((r for r in rumors.rumors.values() if r.crime == cid and HILDA in r.known), None)
            if not check("silence_source", rumor is not None, cid):
                return _logged(results)
            assert rumor is not None
            rumors.gate = lambda r, a, b: r.key == rumor.key and (a, b) == (HILDA, LOBART)
            passed = rumors.offer_status(rumor, HILDA, LOBART)
            if not check(
                "silence_betrayed", passed == "told" and LOBART in rumor.known
                and any(f.get("crime") == cid and f.get("from") == HILDA and f.get("to") == LOBART
                        for f in facts("rumor"))
                and len(records("betrayed", silence.id)) == 1
                and records("betrayed", silence.id)[0].get("how") == "told",
                {"pass": passed, "betrayed": records("betrayed", silence.id)},
            ):
                return _logged(results)
            check(
                "betrayed_once", rumors.offer_status(rumor, HILDA, LOBART) == "no"
                and len(records("betrayed", silence.id)) == 1, records("betrayed", silence.id),
            )
            rumors.gate = lambda *_: False

        if not check("later_near", await near(LOBART, hearer=VINO), "Vino can hear the refusal"):
            return _logged(results)
        state = await link.story_state()
        due = int(state.get("day", 0)) + 2  # tomorrow, as displayed in the situation and deal window
        args = {
            "hero_gives": [{"item": GOLD, "count": 5}],
            "npc_later": {"items": [{"item": GOLD, "count": 7}], "day": due},
        }
        later = await buy(LOBART, "возьми пять сейчас, а завтра отдашь мне семь", args, "later")
        if later is None:
            return _logged(results)
        if not check(
            "later_intent", len(records("npc_intent", later.id)) == 1
            and records("npc_intent", later.id)[0].get("keeps") is False,
            records("npc_intent", later.id),
        ):
            return _logged(results)
        held_gold = await gold()
        await deals.prepare(LOBART)
        if not check(
            "not_due", await gold() == held_gold and not records("npc_broken", later.id)
            and not records("npc_later_paid", later.id), f"day {int(state.get('day', 0)) + 1}, due {due}",
        ):
            return _logged(results)
        await release()
        await link.debug("time", hour=23, minute=59)
        await link.debug("time", hour=8, minute=0)
        if not check("due_near", await near(LOBART, hearer=VINO), "same NPC, next day, Vino hears"):
            return _logged(results)
        refused = Reply(FakeBackend("Про какие семь золотых ты толкуешь? Ничего я тебе не должен.", delay=0)
                        if fake else original_llm)
        dialogue.llm = refused  # type: ignore[assignment]
        previous = {r["id"] for r in turns()}
        await link.debug("type", text=ASK_PAY)
        answered = await _until(
            lambda: bool(refused.words) and any(
                r["id"] not in previous
                and ASK_PAY.replace("ё", "е") in r.get("trigger", {}).get("text", "").replace("ё", "е")
                and r.get("status") == "completed" for r in turns()
            ),
            180 if not fake else 45,
        )
        await settled()
        quote = " ".join(refused.words)
        log.info("cheat %s response: %s", "scripted" if fake else "live", quote)
        prompt = "\n".join(refused.prompts)
        if not check(
            "refusal_answer", answered and bool(quote) and len(refused.prompts) == 1
            and "ты решил не отдавать" in prompt,
            {"quote": quote, "requests": len(refused.prompts), "reminder": "ты решил не отдавать" in prompt},
        ):
            return _logged(results)
        if not check(
            "unpaid", await gold() == held_gold and len(records("npc_broken", later.id)) == 1
            and records("npc_broken", later.id)[0].get("how") == "unpaid"
            and not records("npc_later_paid", later.id), records("npc_broken", later.id),
        ):
            return _logged(results)
        knowledge = people.context(VINO) or ""
        npc_rumors = [
            r for r in rumors.rumors.values() if r.kind == "npc_broken" and r.subject == LOBART
        ]
        if not check(
            "hearer_knowledge", any(e.subject == LOBART and e.direct for e in people.of(VINO))
            and "Лобарт" in knowledge and "не отдал" in knowledge
            and len(npc_rumors) == 1 and npc_rumors[0].known.get(VINO) == 0,
            {"knowledge": knowledge, "rumors": [(r.subject, r.known) for r in npc_rumors]},
        ):
            return _logged(results)
        await deals.prepare(LOBART)
        check("unpaid_once", len(records("npc_broken", later.id)) == 1 and await gold() == held_gold,
              records("npc_broken", later.id))
        if not fake:
            return _logged(results)

        await release()
        cards(4)
        if not check("honest_near", await near(LOBART, hearer=VINO), "same NPC, scenario honesty 4"):
            return _logged(results)
        state = await link.story_state()
        due = int(state.get("day", 0)) + 2
        honest = await buy(
            LOBART, "ещё пять сейчас, а завтра отдашь мне семь",
            {**args, "npc_later": {"items": [{"item": GOLD, "count": 7}], "day": due}}, "honest",
        )
        if honest is None:
            return _logged(results)
        before_paid = await gold()
        await release()
        await link.debug("time", hour=23, minute=59)
        await link.debug("time", hour=8, minute=0)
        if not check("honest_due_near", await near(LOBART, hearer=VINO), "control payment is due"):
            return _logged(results)
        dialogue.llm = FakeBackend("Вот твои семь золотых, как и обещал.", delay=0)
        talk.hear("Лобарт, пора отдать обещанные семь золотых.", LOBART, {"via": "scenario"})
        paid = await _until(lambda: bool(records("npc_later_paid", honest.id)), 30)
        await settled()
        if not check(
            "honest_paid", paid and await gold() == before_paid + 7
            and len(records("npc_later_paid", honest.id)) == 1
            and not records("npc_broken", honest.id)
            and len(records("npc_intent", honest.id)) == 1
            and records("npc_intent", honest.id)[0].get("keeps") is True,
            {"gold": [before_paid, await gold()], "paid": records("npc_later_paid", honest.id)},
        ):
            return _logged(results)

        await release()
        kinds = {"deal", "npc_intent", "npc_broken", "npc_later_paid", "betrayed", "rumor"}
        snapshot = [f for f in facts() if f.get("fact") in kinds]
        saved_gold, saved_people = await gold(), people.of(VINO)
        saved = await _save(link, slot)
        if saved is None:
            check("branch", False, "save was refused")
            return _logged(results)
        loading = asyncio.create_task(wait_message(link, Session, lambda m: m.reason == "load", 60))
        await asyncio.sleep(0)
        await link.debug("load", slot)
        session = await loading
        await link.wait_world()
        # The original dishonest intent must survive the now-honest card and the loaded branch.
        await deals.prepare(LOBART)
        await deals.prepare(LOBART)
        check(
            "branch", session.parent == saved.branch and session.parent_seq == saved.seq
            and [f for f in facts() if f.get("fact") in kinds] == snapshot
            and people.of(VINO) == saved_people and await gold() == saved_gold,
            {"facts": len(snapshot), "gold": await gold(), "knowledge": people.context(VINO)},
        )
        check(
            "no_duplicates", len(records("npc_intent", later.id)) == 1
            and records("npc_intent", later.id)[0].get("keeps") is False
            and len(records("npc_broken", later.id)) == 1
            and len(records("npc_later_paid", honest.id)) == 1
            and not records("npc_later_paid", later.id),
            "loaded intent unchanged; neither payment nor violation repeated",
        )
        return _logged(results)
    finally:
        deals.cards, dialogue.personas.cards = original_cards, original_personas
        dialogue.personas._get.cache_clear()
        dialogue.llm, talk.reflexes = original_llm, original_reflexes
        rumors.gate, rumors.stage = original_gate, original_stage
