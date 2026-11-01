"""Real thefts become director occasions, a witness's concern and a victim's amends."""

from __future__ import annotations

import asyncio
import logging
import math
from contextlib import suppress
from typing import TYPE_CHECKING, Any

from .. import questbook
from ..amends import CACHE_KEY as AMENDS
from ..brain import GameApplier, NpcPlanner
from ..brain.reflex import Reflexes
from ..budget import Budget
from ..chains import CACHE_KEY as CHAINS
from ..game import GameError, GameLink
from ..initiative import InitiativeDesk
from ..llm import ToolCall
from ..protocol import Session
from ..showrunner import CACHE_KEY as SHOWRUNNER
from ..showrunner import occasion_members
from .common import (
    HILDA,
    LOBART,
    FixedDecider,
    WitnessReaction,
    _logged,
    _save,
    _until,
    _until_async,
    model_off,
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
    from .live import LiveCase

log = logging.getLogger(__name__)
GOLD = "ITMI_GOLD"
PREMISE = "Лобарт заметил пропажу из сундука и просит найти и принести вещь; кто вор, он не знает."
# AI_GotoNpc walks straight to the hero. Use one connected road segment; the old shortcut
# from PATH_01 to ENTRANCE_03 skips OUT_01 and can leave the native walk stuck 7 m away.
WITNESS_SPOT = "NW_FARM1_PATH_01"
WITNESS_HERO = "NW_FARM1_PATH_02"


def _missing_chain(item: str, count: int) -> ToolCall:
    def talk(description: str, answer: str) -> dict[str, Any]:
        return {
            "description": description,
            "lines": [{"speaker": "hero", "text": description}, {"speaker": "npc", "text": answer}],
            "remember": answer,
        }

    return ToolCall(
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
                                    "talk": talk("Я нашёл твою вещь. Держи.", "Моя вещь! Оставь её у меня."),
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
    live: LiveCase | None = None,
) -> dict[str, str]:
    """Three branches of save 1 with declared story, deal and compensation decisions.

    Theft, ownership and witnesses come from native actions. Reloading restores independent scenes.
    The rumor gate selects a real social tie, as crime_talk does; it bypasses no knowledge checks.
    The given initiative uses the normal planner, body, dialogue tools and priced deal window.
    """
    results: dict[str, str] = {}
    facts = service.chronicle.facts
    off = model_off(dialogue) if live is None else None
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
        off.given(
            "дано: Хильда продаёт молчание за 25 золотых",
            who=HILDA,
            when="Ты сам подошёл к герою",
            calls=[
                ToolCall(
                    "propose_deal",
                    {
                        "npc_gives": [],
                        "hero_gives": [{"item": GOLD, "count": price}],
                        "silence": [f"crime:{cid}"],
                    },
                )
            ],
        )
        off.silent(
            "дано: Хильда молчит (предложение молчания сделано)",
            who=HILDA,
            when="Ты сам подошёл к герою",
        )
        off.silent(
            "дано: Хильда молчит (сделка молчания состоялась)",
            who=HILDA,
            when="сделка состоялась",
        )
        log.info("given: дано: Хильда подходит к герою (FixedDecider react=approach_talk)")
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
            await _until(
                lambda: any(
                    f.get("npc") == HILDA and f.get("occasion") == f"crime_witness:{cid}"
                    for f in facts("initiative")
                ),
                60,
            )
            offered = await _until(lambda: HILDA in deals.open, 30)
            offer = deals.open.get(HILDA)
            if not check(
                "priced_silence",
                offered and offer is not None,
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
            # Finish the queued deal confirmation before the next scene loads its save branch.
            await _until(lambda: not dialogue.speaking(HILDA), 45)
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
    log.info("given: дано: собеседники слушают героя (FixedDecider posture=listen)")
    talk.reflexes = Reflexes(FixedDecider({"posture": "listen"}))
    rumors.gate = lambda _r, a, b: meeting and a == HILDA and b == LOBART
    rumors.stage = None
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
        if live is not None:
            from .story import play_book_chain

            outcome = await director.decide(occasion)
            quests = [director.book.all()[qid] for qid in (outcome or {}).get("quests", [])
                      if qid in director.book.all()]
            before_hero, before_owner = await has(item.item), await has(item.item, LOBART)
            passage = await play_book_chain(link, director.book, quests) if quests else {}
            await director.book.settle(LOBART)
            returned = bool(quests) and all(director.book.progress(q.id).state == "done" for q in quests)
            returned = (returned and await has(item.item) < before_hero
                        and await has(item.item, LOBART) > before_owner)
            live.finish(
                "success" if (outcome or {}).get("fact") == "line" and returned
                and all(value.startswith("done") for value in passage.values()) else "failure",
                evidence={"crime": crime, "decision": outcome, "passage": passage,
                          "quests": [q.id for q in quests], "returned": returned,
                          "rejected": facts("line_rejected") + facts("chain_rejected")},
            )
            return _logged(results)

        assert off is not None
        off.given(
            "дано: шоураннер предлагает Лобарту линию возврата пропавшей вещи",
            who=SHOWRUNNER,
            when=cid,
            calls=[
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
        )
        off.given(
            "дано: автор цепочек строит поиск и возврат вещи Лобарта",
            who=CHAINS,
            when=PREMISE,
            calls=[_missing_chain(item.item, 1)],
        )
        outcome = await director.decide(occasion)
        assert outcome is not None
        q = director.book.all()[outcome["quests"][0]]
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
        off.given(
            "дано: Amends требует для Лобарта штраф один золотой",
            who=AMENDS,
            when=f"Человек: Лобарт ({LOBART})",
            calls=[
                ToolCall(
                    "propose_amends",
                    {
                        "kind": "amends",
                        "close": [],
                        "lines": [
                            {"speaker": "npc", "text": "Хильда видела, как ты брал мой кубок."},
                            {"speaker": "npc", "text": "Заплати золотой за обиду, тогда сочтёмся."},
                        ],
                        "to": "self",
                        "gold": 1,
                        "description": "Вот золотой за кражу.",
                        "amends_lines": [{"speaker": "npc", "text": "Ладно, за это дело я тебя простил."}],
                        "title": "Долг перед Лобартом",
                        "journal_now": "Лобарт узнал от Хильды о моей краже. Надо заплатить золотой.",
                        "journal_done": "Я заплатил Лобарту за кражу.",
                        "reason": "пострадавший узнал от свидетельницы",
                    },
                )
            ],
        )
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
        rumors.gate, rumors.stage = original_gate, original_stage
        talk.reflexes = original_reflexes
