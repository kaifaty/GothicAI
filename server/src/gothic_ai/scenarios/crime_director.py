"""Real thefts become director occasions, a witness's concern and a victim's amends."""

from __future__ import annotations

import asyncio
import json
import logging
import math
import re
from collections.abc import AsyncIterator
from contextlib import suppress
from typing import TYPE_CHECKING, Any

from .. import questbook
from ..brain import GameApplier, NpcPlanner
from ..brain.reflex import Reflexes
from ..budget import Budget
from ..game import GameError, GameLink
from ..initiative import InitiativeDesk
from ..llm import LLMEvent, Message, ToolCall
from ..llm.fake import FakeBackend, FakeStep
from ..protocol import Session
from ..showrunner import occasion_members
from .common import (
    HILDA,
    LOBART,
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
from .crimes import HOUSE

if TYPE_CHECKING:
    from ..amends import Amends
    from ..bodies import Bodies
    from ..chests import ChestDesk
    from ..concerns import Concerns
    from ..crimes import Crimes
    from ..deals import Deals
    from ..dialogue import DialogueManager
    from ..perception import PerceptionDesk
    from ..showrunner import Showrunner
    from ..sightings import Sightings
    from ..social import Rumors
    from ..storyguard import StoryGuard
    from ..talk import TalkDesk

log = logging.getLogger(__name__)
GOLD = "ITMI_GOLD"
PREMISE = "Лобарт заметил пропажу из сундука и просит найти и принести вещь; кто вор, он не знает."
# AI_GotoNpc walks straight to the hero. Use one connected road segment; the old shortcut
# from PATH_01 to ENTRANCE_03 skips OUT_01 and can leave the native walk stuck 7 m away.
WITNESS_SPOT = "NW_FARM1_PATH_01"
WITNESS_HERO = "NW_FARM1_PATH_02"


class CrimeAmendsScript(FakeBackend):
    """The victim asks for one gold, within every positive crime compensation limit."""

    async def stream(self, **kw: Any) -> AsyncIterator[LLMEvent]:
        brief = next(m.content for m in kw["messages"] if isinstance(m, Message))
        stakes = re.findall(r"^- (\S+): «", brief, re.M)
        mine = f"({LOBART})" in brief.splitlines()[0]
        args = {
            "kind": "amends" if mine else "none",
            "close": stakes[:1] if mine else [],
            "lines": [
                {"speaker": "npc", "text": "Хильда видела, как ты брал мой кубок."},
                {"speaker": "npc", "text": "Заплати золотой за обиду, тогда сочтёмся."},
            ]
            if mine
            else [],
            "to": "self",
            "gold": 1 if mine else 0,
            "description": "Вот золотой за кражу." if mine else "",
            "amends_lines": [{"speaker": "npc", "text": "Ладно, за это дело я тебя простил."}]
            if mine
            else [],
            "title": "Долг перед Лобартом" if mine else "",
            "journal_now": "Лобарт узнал от Хильды о моей краже. Надо заплатить золотой." if mine else "",
            "journal_done": "Я заплатил Лобарту за кражу." if mine else "",
            "reason": "пострадавший узнал от свидетельницы" if mine else "не его дело",
        }
        backend = FakeBackend("", tool_calls=[ToolCall("propose_amends", args)], delay=0)
        async for event in backend.stream(**kw):
            yield event


def _missing_builder(item: str, count: int) -> FakeBackend:
    def talk(description: str, answer: str) -> dict[str, Any]:
        return {
            "description": description,
            "lines": [{"speaker": "hero", "text": description}, {"speaker": "npc", "text": answer}],
            "remember": answer,
        }

    return FakeBackend(
        "",
        tool_calls=[
            ToolCall(
                "propose_chain",
                {
                    "cast": [],
                    "quests": [
                        {
                            "title": "Пропажа из сундука",
                            "give": talk("У тебя что-то пропало?", "Да. Найди мою вещь и принеси мне."),
                            "stages": [
                                {
                                    "journal": "Надо найти вещь Лобарта.",
                                    "goals": [
                                        {"kind": "have", "npc": "PC_HERO", "item": item, "count": count},
                                    ],
                                },
                                {
                                    "journal": "Вещь у меня. Надо отнести её Лобарту.",
                                    "goals": [
                                        {
                                            "kind": "bring",
                                            "npc": "giver",
                                            "item": item,
                                            "count": count,
                                            "talk": talk(
                                                "Я нашёл твою вещь. Держи.", "Моя вещь! Оставь её у меня."
                                            ),
                                        }
                                    ],
                                },
                            ],
                            "done_journal": "Я принёс Лобарту вещь из его сундука.",
                            "knows": "Из моего сундука пропала вещь. Я не знаю, кто её взял.",
                            "asked": "Найти пропавшую вещь и принести мне.",
                            "gold": 0,
                            "xp": 0,
                        }
                    ],
                    "reason": "Герой может принести собственную добычу, не признаваясь в краже.",
                },
            )
        ],
        delay=0,
    )


async def crime_director(
    link: GameLink,
    service: Crimes,
    director: Showrunner,
    dialogue: DialogueManager,
    talk: TalkDesk,
    rumors: Rumors,
    sightings: Sightings,
    deals: Deals,
    chests: ChestDesk,
    concerns: Concerns,
    amends: Amends,
    *,
    guard: StoryGuard,
    bodies: Bodies,
    perception: PerceptionDesk,
    initiative: InitiativeDesk | None = None,
    slot: int = 7,
) -> dict[str, str]:
    """Three branches of save 1; LLM=chatgpt runs just a witnessed theft and the real showrunner.

    Theft, ownership and witnesses come from native actions. Reloading restores independent scenes.
    The rumor gate selects a real social tie, as crime_talk does; it bypasses no knowledge checks.
    The scripted initiative uses the normal planner, body, dialogue tools and priced deal window.
    """
    results: dict[str, str] = {}
    facts = service.chronicle.facts
    live = director.model.llm.name != "fake"
    original_llm = dialogue.llm
    original_model, original_builder = director.model.llm, director.chains.model.llm
    original_amends = amends.model.llm
    original_gate, original_stage = rumors.gate, rumors.stage
    original_reflexes = talk.reflexes
    meeting = False

    def check(name: str, ok: bool, detail: object = "") -> bool:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"
        log.info("crime_director %-24s %s", name, results[name])
        return ok

    def knows(npc: str, cid: str, degree: str | None = None) -> bool:
        return any(
            k.crime.get("id") == cid and (degree is None or k.degree == degree) for k in service.known(npc)
        )

    def witness_concerns(cid: str) -> list[Any]:
        return [c for c in concerns.of(HILDA) if c.key == f"crime_witness:{cid}"]

    async def has(item: str, npc: str = "HERO") -> int:
        return int(await link.evaluate(["call", "NPC_HASITEMS", ["inst", npc], ["sym", item]]) or 0)

    async def stand(npc: str, point: str, *, face: str | None = None) -> None:
        await link.debug("skip_important", near=npc)
        await link.npc_act(
            npc, "start_activity", state="ZS_STAND_WP", waypoint=point, minutes=120, place=True
        )
        if face is not None:
            await link.npc_act(npc, "turn_to", npc=face)

    async def clean(label: str) -> bool:
        nonlocal meeting
        meeting = False
        dialogue.llm = original_llm
        loading = asyncio.create_task(wait_message(link, Session, lambda m: m.reason == "load", 60))
        await asyncio.sleep(0)
        await link.debug("load", slot + 1)
        await loading
        await link.wait_world()
        await asyncio.sleep(0.5)
        return check(f"{label}_branch", not facts("crime"), f"{len(facts('crime'))} crimes")

    async def witnessed(label: str) -> dict[str, Any] | None:
        await link.debug("player_goto", waypoint="NW_FARM1_INHOUSE_01")
        for npc in farm:
            await stand(npc, parked)
        await stand(LOBART, HOUSE)

        async def drop() -> bool:
            try:
                await link.debug("npc_drop", npc=LOBART, item="ITMI_SILVERCUP")
                return True
            except GameError as err:
                if "could not drop it now" not in str(err):
                    raise
                return False

        if not check(f"{label}_drop", await _until_async(drop, bool, 10), "native silver cup"):
            return None
        await asyncio.sleep(1)
        await stand(LOBART, parked)
        prepared, preparation = await prepare_theft_witness(link, HILDA)
        if not check(f"{label}_witness_ready", prepared, preparation):
            return None
        before = {f["id"] for f in facts("crime")}
        with WitnessReaction(link, service.chronicle, HILDA) as reaction:
            await link.debug("player_take", item="ITMI_SILVERCUP", distance=1200)
            await _until(lambda: any(f["id"] not in before for f in facts("crime")), 10)
            crime = next((f for f in facts("crime") if f["id"] not in before), None)
            if crime is not None and not check(
                f"{label}_reaction", await reaction.wait(str(crime["id"])), reaction.detail
            ):
                return None
        # Preserve the held rule and native SVM before staging the director's later scene.
        await stand(HILDA, field)
        if not check(
            f"{label}_theft",
            crime is not None
            and crime.get("kind") == "theft"
            and crime.get("owner") == LOBART
            and set(crime.get("witnesses", ())) == {HILDA}
            and knows(HILDA, str(crime["id"]), "seen")
            and not knows(LOBART, str(crime["id"])),
            crime,
        ):
            return None
        return crime

    async def deed(cid: str) -> dict[str, Any] | None:
        def recorded() -> dict[str, Any] | None:
            return next(
                (f for f in facts("occasion") if f.get("kind") == "deed" and f.get("crime") == cid), None
            )

        if not await _until(recorded, 20):
            check("deed", False, cid)
            return None
        occasion = await asyncio.wait_for(director.occasions.next(), director.occasions.max_gather_s + 20)
        if not check("deed", any(e.get("crime") == cid for e in occasion_members(occasion)), occasion):
            return None
        return occasion

    async def silence_scene(cid: str) -> bool:
        if not check("witness_concern", bool(witness_concerns(cid)), witness_concerns(cid)):
            return False
        price = 25
        gold = await has(GOLD)
        if gold < price:
            await link.story_effect(LOBART, [{"do": "give", "item": GOLD, "count": price - gold}])
        dialogue.llm = ClaimScript(
            said=[
                (
                    "Ты сам подошёл к герою",
                    FakeStep(
                        "Я видела, как ты взял кубок. Двадцать пять золотых — и я молчу.",
                        [
                            ToolCall(
                                "propose_deal",
                                {
                                    "npc_gives": [],
                                    "hero_gives": [{"item": GOLD, "count": price}],
                                    "silence": [f"crime:{cid}"],
                                },
                            )
                        ],
                    ),
                )
            ],
            told=[],
            default="Договорились.",
            only="Хильда",
        )
        own = initiative is None
        desk = initiative
        old_planner = talk.planner
        runner: asyncio.Task[None] | None = None
        if desk is None:
            planner = NpcPlanner(
                FixedDecider({"react": "approach_talk"}, npcs={HILDA}),
                GameApplier(link),
                agenda_of=concerns.agenda,
                view_of=perception.english,
            )
            link.listeners.append(planner.on_message)
            desk = InitiativeDesk(
                link,
                dialogue,
                talk,
                planner,
                concerns,
                guard,
                Budget(),
                service.chronicle,
                bodies=bodies,
                perception=perception,
                journal=dialogue.journal,
            )
            talk.planner = planner
        planner = desk.planner
        old_decider = planner.decider
        planner.decider = FixedDecider({"react": "approach_talk"}, npcs={HILDA})

        def agenda(npc: str) -> None:
            planner.set_agenda(npc, concerns.agenda(npc))

        concerns.on_change.append(agenda)
        agenda(HILDA)
        try:
            await link.debug("player_goto", waypoint=WITNESS_HERO)
            await link.debug("skip_important", near=HILDA)
            settled = await bodies.act(
                HILDA,
                "start_activity",
                state="ZS_STAND_WP",
                waypoint=WITNESS_SPOT,
                minutes=120,
                place=True,
                arrive=True,
                radius=150,
                timeout=20,
            )
            turned = await bodies.act(HILDA, "turn_to", npc="PLAYER", timeout=20)
            snap = await link.snapshot(HILDA, fresh=True)
            views = await link.npc_view([HILDA])
            ready = bool(views and views[0].state == "ZS_STAND_WP" and views[0].body == "stand")
            connected = WITNESS_HERO in points[WITNESS_SPOT].get("conn", [])
            positioned = (
                settled.status == "arrived"
                and turned.status == "done"
                and ready
                and connected
                and snap.player is not None
                and 500 <= snap.player.distance <= 1500
                and abs(snap.player.pos[1] - snap.npc.pos[1]) <= 200
            )
            if not check(
                "witness_position",
                positioned,
                {
                    "settled": settled.status,
                    "turned": turned.status,
                    "ready": ready,
                    "connected": connected,
                    "npc": snap.npc.pos,
                    "hero": snap.player.pos if snap.player is not None else None,
                    "distance": snap.player.distance if snap.player is not None else None,
                },
            ):
                return False
            if own:
                runner = asyncio.create_task(planner.run())
            approached = await _until(
                lambda: any(
                    f.get("npc") == HILDA and f.get("occasion") == f"crime_witness:{cid}"
                    for f in facts("initiative")
                ),
                60,
            )
            if not check("witness_initiative", approached, facts("initiative")[-1:]):
                return False
            offered = await _until(lambda: HILDA in deals.open, 30)
            offer = deals.open.get(HILDA)
            if not check(
                "priced_silence",
                offered and offer is not None and offer.gold() == price,
                offer.terms() if offer else "no deal",
            ):
                return False
            before = await has(GOLD)
            await asyncio.sleep(1.2)
            pressed = await link.press_deal()
            paid = await _until(
                lambda: any(f.get("npc") == HILDA and f.get("crime") == cid for f in facts("silenced")), 20
            )
            cleared = await _until(lambda: not witness_concerns(cid), 10)
            return check(
                "silence_clears",
                pressed == "sent" and paid and cleared and await has(GOLD) == before - price,
                facts("silenced")[-1:],
            )
        finally:
            concerns.on_change.remove(agenda)
            planner.decider = old_decider
            if own:
                if runner is not None:
                    runner.cancel()
                    with suppress(asyncio.CancelledError):
                        await runner
                link.listeners.remove(planner.on_message)
                link.listeners.remove(desk._on_message)
                talk.planner = old_planner

    await link.wait_world()
    if link.session is None:
        await wait_message(link, Session, timeout=10)
    talk.reflexes = Reflexes(FixedDecider({"posture": "listen"}))
    rumors.gate = lambda _r, a, b: meeting and a == HILDA and b == LOBART
    rumors.stage = None
    # No incidental conversation or Amends call spends a live run's quota. The showrunner and its
    # builders retain the real model, its ordinary instructions, pacing and budget.
    dialogue.llm = FakeBackend("Хм.", delay=0)
    amends.model.llm = CrimeAmendsScript(delay=0)
    try:
        await link.debug("player_hp", hp=5000)
        await link.debug("time", hour=8, minute=0)
        humans = {p["npc"] for p in await link.story_people()}
        farm = list(
            dict.fromkeys(
                [
                    LOBART,
                    HILDA,
                    *[p["npc"] for p in await link.story_nearby(LOBART, 7000) if p["npc"] in humans],
                ]
            )
        )
        await link.story_effect(LOBART, [{"do": "end"}])
        points = {p["name"]: p for p in await link.waypoints() if not p.get("fp")}
        if not check("farm", HOUSE in points and {LOBART, HILDA} <= humans, HOUSE):
            return _logged(results)
        house = points[HOUSE]
        house_pos = (house["x"], house["y"], house["z"])
        outside = [p for p in points.values() if p["name"].startswith("NW_FARM1_") and not p.get("room")]
        far = [p for p in outside if math.dist((p["x"], p["y"], p["z"]), house_pos) > 8000]
        fields = [p for p in outside if "FIELD" in p["name"]]
        if not check("positions", bool(far) and bool(fields), f"{len(far)} parking, {len(fields)} field"):
            return _logged(results)
        parked = min(far, key=lambda p: math.dist((p["x"], p["y"], p["z"]), house_pos))["name"]
        field = min(fields, key=lambda p: math.dist((p["x"], p["y"], p["z"]), house_pos))["name"]
        if not await _until(lambda: rumors.social is not None, 30):
            return _logged({**results, "social": "failed (no social graph)"})
        assert rumors.social is not None
        if not check("tie", LOBART in rumors.social.ties.get(HILDA, {}), "Hilda knows Lobart"):
            return _logged(results)
        if not check("baseline", not facts("crime") and await _save(link, slot + 1) is not None):
            return _logged(results)

        if live:
            crime = await witnessed("live")
            if crime is None:
                return _logged(results)
            cid = str(crime["id"])
            occasion = await deed(cid)
            if occasion is None:
                return _logged(results)
            outcome = await director.decide(occasion)
            log.info("crime_director live outcome: %s", json.dumps(outcome, ensure_ascii=False))
            if not check(
                "live_outcome",
                outcome is not None
                and (
                    outcome.get("fact") == "line"
                    or (outcome.get("fact") == "line_none" and bool(outcome.get("reason")))
                ),
                outcome,
            ):
                return _logged(results)
            assert outcome is not None
            if outcome["fact"] == "line":
                giver = str(outcome.get("giver", ""))
                eligible = knows(giver, cid) or giver == crime.get("victim") or giver == crime.get("owner")
                check("live_canon", outcome.get("crime") == cid and eligible, giver)
                check("live_premise", bool(outcome.get("premise")), outcome.get("premise", ""))
            else:
                check("live_reason", bool(str(outcome.get("reason", "")).strip()), outcome["reason"])
            return _logged(results)

        # Scene 1: nobody sees the chest theft. Its owner discovers the loss by opening it.
        await link.debug("player_goto", waypoint=HOUSE)
        for npc in farm:
            await stand(npc, parked)
        await stand(LOBART, HOUSE)
        found = [c for c in await chests.chests(LOBART, fresh=True) if c.items and c.distance < 4000]
        found.sort(key=lambda c: (c.why != "owner", c.why != "key", c.distance))
        if not check("missing_chest", bool(found), [(c.id, c.why) for c in found]):
            return _logged(results)
        chest = found[0]
        item = next((i for i in chest.items if i.item != GOLD), chest.items[0])
        await stand(LOBART, parked)
        await link.debug("player_goto", pos=(chest.pos[0] + 100, chest.pos[1], chest.pos[2]))
        nearby = await link.story_nearby("PC_HERO", 3500)
        if not check("missing_alone", not nearby, [p["npc"] for p in nearby]):
            return _logged(results)
        await link.debug("chest_take", chest=chest.id, item=item.item, count=1)
        await _until(lambda: any(f.get("chest") == chest.id for f in facts("crime")), 10)
        crime = next((f for f in facts("crime") if f.get("chest") == chest.id), None)
        if not check(
            "missing_theft",
            crime is not None and not crime.get("witnesses") and not service.known(LOBART),
            crime,
        ):
            return _logged(results)
        assert crime is not None
        cid = str(crime["id"])
        await link.debug("player_goto", pos=(chest.pos[0], chest.pos[1], chest.pos[2] + 200))
        await link.debug("npc_goto", npc=LOBART, to="PC_HERO", distance=0)
        await link.debug("player_goto", waypoint="NW_FARM1_INHOUSE_02")
        conv = dialogue.open(LOBART, "Лобарт")
        held = await dialogue.borrow_body(conv, "scenario-crime-chest")
        try:
            opened = await link.act(LOBART, "use_chest", chest=chest.id, take=[], put=[], timeout=90)
            note = await chests.looked(LOBART, chest.id, opened.detail) if opened.status == "done" else ""
        finally:
            if held:
                dialogue.return_body(conv, "scenario-crime-chest")
        if not check("missing_noticed", opened.status == "done" and "пропало" in note, note):
            return _logged(results)
        occasion = await deed(cid)
        if occasion is None:
            return _logged(results)
        director.model.llm = FakeBackend(
            "",
            tool_calls=[
                ToolCall(
                    "propose_line",
                    {
                        "kind": "quest",
                        "giver": LOBART,
                        "place": "",
                        "new_people": False,
                        "premise": PREMISE,
                        "reason": "хозяин заметил пропажу",
                        "follows": "",
                        "crime": cid,
                    },
                )
            ],
            delay=0,
        )
        director.chains.model.llm = _missing_builder(item.item, 1)
        outcome = await director.decide(occasion)
        if not check(
            "missing_line",
            outcome is not None
            and outcome.get("fact") == "line"
            and outcome.get("crime") == cid
            and outcome.get("giver") == LOBART,
            outcome,
        ):
            return _logged(results)
        assert outcome is not None
        made = [director.book.all()[qid] for qid in outcome.get("quests", []) if qid in director.book.all()]
        if not check(
            "missing_goals",
            len(made) == 1 and [g.item for s in made[0].stages for g in s.goals] == [item.item, item.item],
            outcome.get("quests"),
        ):
            return _logged(results)
        q = made[0]
        await link.debug("player_goto", to=LOBART, distance=300)
        hero_before, owner_before = await has(item.item), await has(item.item, LOBART)
        await link.choose_dialog(LOBART, questbook.give_node(q))
        # The hero already holds his own loot. Let the real have trigger advance the stage; the
        # generic debug chain player would try to loot the stated source, which here is the hero.
        ready = await _until_async(
            lambda: link.offered_dialogs(LOBART), lambda dialogs: questbook.done_node(q) in dialogs, 30
        )
        if not check("missing_own_loot", ready and hero_before >= 1, f"{item.item} ×{hero_before}"):
            return _logged(results)
        await link.choose_dialog(LOBART, questbook.done_node(q))
        await _until_async(
            lambda: link.story_state(vars=[q.id]), lambda state: state.get("vars", {}).get(q.id) == 2, 20
        )
        await director.book.settle(LOBART)
        check(
            "missing_completed",
            director.book.progress(q.id).state == "done"
            and await has(item.item) == hero_before - 1
            and await has(item.item, LOBART) == owner_before + 1,
            q.id,
        )
        check("return_no_forgiveness", not any(f.get("crime") == cid for f in facts("forgiven")))

        # Scene 2: the witness approaches of her own concern; the deal has an actual price.
        if not await clean("silence"):
            return _logged(results)
        crime = await witnessed("silence")
        if crime is None or not await silence_scene(str(crime["id"])):
            return _logged(results)

        # Scene 3: the victim hears a real rumor, demands amends and receives payment.
        if not await clean("amends"):
            return _logged(results)
        crime = await witnessed("amends")
        if crime is None:
            return _logged(results)
        cid = str(crime["id"])
        # Place the victim before its director's important dialog exists: skip_important is setup,
        # and must never consume the storylet whose automatic opening this scene checks.
        await stand(LOBART, field)
        meeting = True
        await link.debug("time", hour=7, minute=0)
        await rumors.tick()
        if not check(
            "amends_heard", await _until(lambda: knows(LOBART, cid, "heard"), 20), service.situation(LOBART)
        ):
            return _logged(results)
        check("reported_clears_concern", not witness_concerns(cid), witness_concerns(cid))
        key = f"crime:{cid}"
        await _until(lambda: any(s.npc == LOBART and s.lie == key for s in amends.storylets.values()), 120)
        storylet = next((s for s in amends.storylets.values() if s.npc == LOBART and s.lie == key), None)
        if not check(
            "amends_storylet",
            storylet is not None,
            [f for f in facts() if f.get("fact") in {"amends_open", "amends_none", "amends_rejected"}],
        ):
            return _logged(results)
        assert storylet is not None
        await stand(HILDA, parked)
        await link.debug("player_goto", to=LOBART, distance=250)
        if not await _until(lambda: storylet.stage != "open", 20):
            await link.choose_dialog(LOBART, storylet.talk)
        if not check("amends_dialogue", await _until(lambda: storylet.stage != "open", 20), storylet.talk):
            return _logged(results)
        before = await has(GOLD)
        if before < storylet.gold:
            await link.story_effect(LOBART, [{"do": "give", "item": GOLD, "count": storylet.gold - before}])
            before = await has(GOLD)
        await link.choose_dialog(storylet.to, storylet.pay)
        paid = await _until(lambda: storylet.stage == "done", 20)
        check(
            "amends_paid",
            paid and storylet.gold > 0 and await has(GOLD) == before - storylet.gold,
            f"{before} -> {await has(GOLD)}, price={storylet.gold}",
        )
        check(
            "forgiven_paid",
            any(
                f.get("npc") == LOBART and f.get("crime") == cid and f.get("paid") for f in facts("forgiven")
            ),
            facts("forgiven")[-1:],
        )
        return _logged(results)
    finally:
        dialogue.llm = original_llm
        director.model.llm, director.chains.model.llm = original_model, original_builder
        amends.model.llm = original_amends
        rumors.gate, rumors.stage = original_gate, original_stage
        talk.reflexes = original_reflexes
