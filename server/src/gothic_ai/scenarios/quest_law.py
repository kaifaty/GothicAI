"""A book quest accepts Matteo's stolen cup; the city's native fine survives the turn-in."""

from __future__ import annotations

import asyncio
import logging
import math
from typing import TYPE_CHECKING, Any

from .. import questbook
from ..attitudes import non_hostile
from ..crime_law import REPORT_DELAYS, game_minutes
from ..crime_law_graph import law_var
from ..game import GameError, GameLink
from ..llm.fake import FakeBackend
from ..protocol import DialogChosen, Session
from ..validator import LOG_RUNNING, LOG_SUCCESS
from .common import (
    _logged,
    _save,
    _until,
    _until_async,
    prepare_npc_parking,
    prepare_npc_pose,
    prepare_street_theft_witness,
    wait_message,
    wait_npc_visibility,
)
from .crime_law import COUNTERS, HERO_POINT, PAYNOW, PETZMASTER, PMSCHULDEN, THEFT_POINT
from .crimes import ANDRE, CITY_OWNER

if TYPE_CHECKING:
    from ..crime_law_runtime import CrimeLawRuntime
    from ..crimes import Crimes
    from ..dialogue import DialogueManager
    from ..social import Rumors

log = logging.getLogger(__name__)
GIVER = "VLK_456_ABUYIN"
ITEM = "ITMI_SILVERCUP"
PAYLATER = f"{PETZMASTER}_PAYLATER"
CHECKS = ("quest_taken", "theft", "goal_by_loot", "branch", "law_knows", "turn_in", "not_forgiven", "paid")


async def quest_law(
    link: GameLink,
    service: Crimes,
    law: CrimeLawRuntime,
    book: questbook.QuestBook,
    dialogue: DialogueManager,
    rumors: Rumors,
    slot: int = 7,
) -> dict[str, str]:
    """Save 1, fake model: have -> bring, native theft, reload, fine, turn-in, fine payment.

    Only the scene is staged, as in crime_law: other people wait away from Matteo's street and his
    rule-based reaction finishes before he is moved. Ownership, guilds and native perceptions stay intact.
    Random rumor meetings are paused so Andre learns on the law's deadline, and the peaceful giver
    is neither a witness nor a rumor recipient. No model writes the quest or decides its outcome.
    """
    results = dict.fromkeys(CHECKS, "failed (not reached)")
    current = "quest_taken"
    facts = service.chronicle.facts
    chosen: list[DialogChosen] = []
    original_gate, original_stage = rumors.gate, rumors.stage

    def check(name: str, ok: bool, detail: object) -> bool:
        results[name] = f"{'done' if ok else 'failed'} ({detail})"
        log.info("quest_law %-16s %s", name, results[name])
        return ok

    def record(message: object) -> None:
        if isinstance(message, DialogChosen):
            chosen.append(message)

    async def values(*names: str) -> dict[str, int]:
        return dict((await link.story_state(vars=list(names))).get("vars", {}))

    async def has(item: str, npc: str = "HERO") -> int:
        return int(await link.evaluate(["call", "NPC_HASITEMS", ["inst", npc], ["sym", item]]) or 0)

    async def pose(npc: str, point: str, *, face: str | None = None) -> bool:
        if point == parked:
            expected = points[point]
            ready, detail = await prepare_npc_parking(
                link, npc, point, (expected["x"], expected["y"], expected["z"]),
            )
        else:
            if npc == CITY_OWNER:
                await link.debug("skip_important", near=npc)
                await link.debug("npc_goto", npc=npc, waypoint=point)
                await link.debug("player_goto", waypoint=HERO_POINT)
            elif npc == GIVER:
                await link.debug("player_goto", waypoint=point)
            ready, detail = await prepare_npc_pose(link, npc, point, face=face)
        if not ready:
            check(current, False, {"preparation": "native pose", **detail})
        return ready

    async def end(npc: str) -> None:
        await link.story_effect(npc, [{"do": "end"}])
        await _until_async(link.story_state, lambda state: not state.get("dialog"), 15)

    async def giver_dialog(node: str) -> None:
        await link.debug("skip_important", near=GIVER)
        await link.debug("player_goto", to=GIVER, distance=300)
        await _until_async(link.story_state, lambda state: not state.get("dialog"), 30)
        await link.choose_dialog(GIVER, node)

    def selected(node: str, *, made: bool) -> bool:
        return any(m.node == node and m.made == made for m in chosen)

    def news(cid: str) -> list[dict[str, Any]]:
        return [f for f in facts("crime_known") if f.get("community") == "city" and f.get("crime") == cid]

    def absolved(cid: str) -> bool:
        return any(f.get("community") == "city" and cid in f.get("crimes", ()) for f in facts("absolved"))

    def remembers(cid: str) -> bool:
        return any(
            k.crime["id"] == cid and k.degree == "seen" and not k.forgiven for k in service.known(CITY_OWNER)
        )

    def deliveries(cid: str) -> list[tuple[str, int, str]]:
        return [(d.via, d.due, d.status) for d in law.law.deliveries() if d.crime["id"] == cid]

    async def fine(node: str) -> dict[str, Any]:
        start = len(chosen)
        await link.debug("skip_important", near=ANDRE)
        await link.debug("player_goto", to=ANDRE, distance=250)
        for person in await link.story_nearby(ANDRE, 1000):
            if person["npc"] in people and person["npc"] != ANDRE:
                if not await pose(person["npc"], parked):
                    return {"ready": False, "preparation": results[current]}
        keeper = await link.snapshot(ANDRE, fresh=True)
        point = keeper.npc.nearest_waypoint
        if point is None:
            return {"ready": False, "preparation": "Andre has no native waypoint"}
        posed, detail = await prepare_npc_pose(link, ANDRE, point, face="PC_HERO")
        if not posed:
            return {"ready": False, "preparation": detail}
        # Use the initialized waypoint and prove sight instead of placing through a wall.
        await link.debug("teleport", near=ANDRE, distance=250)
        await link.debug("npc_goto", npc=ANDRE, waypoint=point, face="PC_HERO")
        visible, detail = await wait_npc_visibility(link, ANDRE, "HERO", distance=700)
        if not visible:
            return {"ready": False, "preparation": detail}
        await link.start_talk(ANDRE)
        payment = law.graph.node(PAYNOW).description
        ready = await _until_async(link.dialog_menu, lambda menu: bool(payment) and payment in menu, 120)
        keeper = await link.snapshot(ANDRE, fresh=True)
        return {
            "ready": ready,
            "native": any(m.node == node and not m.made for m in chosen[start:]),
            "debt": (await values("ANDRE_SCHULDEN")).get("ANDRE_SCHULDEN"),
            "menu": await link.dialog_menu(),
            "dialog": (await link.story_state()).get("dialog"),
            "keeper": keeper.npc.model_dump(exclude={"inventory"}),
            "hero": keeper.player.model_dump() if keeper.player is not None else None,
        }

    def talk_line(said: str, answer: str, remember: str = "") -> questbook.Talk:
        return questbook.Talk(said, [{"speaker": "self", "text": answer}], remember)

    if not isinstance(dialogue.llm, FakeBackend):
        check(current, False, "quest_law requires LLM=fake")
        return _logged(results)
    await link.wait_world()
    if link.session is None:
        await wait_message(link, Session, timeout=10)
    rumors.gate, rumors.stage = lambda *_args: False, None
    link.listeners.append(record)
    try:
        await link.debug("player_hp", hp=5000)
        await link.debug("time", hour=8, minute=0)
        people = {p["npc"] for p in await link.story_people()}
        points = {p["name"]: p for p in await link.waypoints() if not p.get("fp")}
        eligible = await non_hostile(link, [GIVER], context=GIVER)
        identity = await link.story_eval(
            GIVER, [["field", ["inst", "SELF"], var] for var in ("GUILD", "NPCTYPE")]
        )
        routine = await link.routine(GIVER)
        home = next(
            (
                e.waypoint
                for e in routine.entries
                if e.waypoint in points and e.waypoint.startswith("NW_CITY_")
            ),
            "",
        )
        prepared = (
            {GIVER, CITY_OWNER, ANDRE} <= people
            and {THEFT_POINT, HERO_POINT} <= points.keys()
            and bool(home)
            and GIVER in eligible
            and not book.rules.plot_npc(GIVER)
            and service.community(*identity) == "city"
            and not facts("crime")
            and await has(ITEM) == 0
        )
        if not prepared:
            check(
                current,
                False,
                {
                    "giver": GIVER,
                    "non_hostile": sorted(eligible),
                    "guild/npctype": identity,
                    "home": home,
                    "initial_cups": await has(ITEM),
                    "initial_crimes": len(facts("crime")),
                },
            )
            return _logged(results)
        origin = points[THEFT_POINT]
        position = (origin["x"], origin["y"], origin["z"])
        far = [
            p
            for p in points.values()
            if p["name"].startswith("NW_FARM1_")
            and not p.get("room")
            and math.dist((p["x"], p["y"], p["z"]), position) > 10000
        ]
        if not far:
            check(current, False, "no parking point away from Matteo")
            return _logged(results)
        parked = min(far, key=lambda p: math.dist((p["x"], p["y"], p["z"]), position))["name"]
        await end(CITY_OWNER)
        reset = [*COUNTERS, "ABSOLUTIONLEVEL_CITY"]
        reset += [
            f"{npc}_{var}"
            for npc in ("ANDRE", "HAGEN")
            for var in ("SCHULDEN", "LASTPETZCOUNTER", "LASTPETZCRIME")
        ]
        await link.story_effect(ANDRE, [{"do": "set", "var": var, "value": 0} for var in reset])
        if await has("ITMI_GOLD") < 100:
            await link.story_effect(ANDRE, [{"do": "give", "item": "ITMI_GOLD", "count": 100}])
        await law.tick()
        if not await pose(GIVER, home):
            return _logged(results)
        q = questbook.Quest(
            id=book.next_id(),
            giver=GIVER,
            title="Серебряный кубок",
            give=talk_line("Тебе что-нибудь нужно?", "Принеси серебряный кубок. Заплачу десять золотых."),
            stages=[
                questbook.Stage(
                    "Надо раздобыть серебряный кубок для Абуина.",
                    [questbook.Goal("have", npc=CITY_OWNER, item=ITEM)],
                ),
                questbook.Stage(
                    "Кубок у меня. Надо отнести его Абуину.",
                    [
                        questbook.Goal(
                            "bring",
                            npc=GIVER,
                            item=ITEM,
                            talk=talk_line(
                                "Вот серебряный кубок.",
                                "Отлично. Вот обещанные десять золотых.",
                                "Герой принёс кубок, ты заплатил ему.",
                            ),
                        )
                    ],
                ),
            ],
            done_journal="Я отдал Абуину кубок и получил десять золотых.",
            asked="принести серебряный кубок",
            gold=10,
            xp=20,
            names={GIVER: "Абуин"},
            reason="e2e: краденая вещь в квесте не отменяет закон города",
        )
        errors = await book.make(q)
        if errors:
            check(current, False, errors)
            return _logged(results)
        status, stage = q.id, questbook.stage_var(q)
        give, done = questbook.give_node(q), questbook.done_node(q)
        await giver_dialog(give)
        running = {status: LOG_RUNNING, stage: 1}
        ready = await _until_async(lambda: values(status, stage), lambda v: v == running, 15)
        await book.settle()
        if not check(
            current,
            ready and book.progress(q.id).state == "given" and selected(give, made=True),
            {
                "quest": await values(status, stage),
                "book": book.progress(q.id),
                "giver": GIVER,
                "non_hostile": sorted(eligible),
                "guild/npctype": identity,
            },
        ):
            return _logged(results)
        await end(GIVER)

        current = "theft"
        await link.debug("player_goto", waypoint=HERO_POINT)
        for person in await link.story_nearby("PC_HERO", 7000):
            if person["npc"] in people and person["npc"] != CITY_OWNER:
                if not await pose(person["npc"], parked):
                    return _logged(results)
        if not await pose(CITY_OWNER, THEFT_POINT, face="PC_HERO"):
            return _logged(results)

        async def drop() -> bool:
            try:
                await link.debug("npc_drop", npc=CITY_OWNER, item=ITEM)
            except GameError as error:
                if "could not drop it now" not in str(error):
                    raise
                return False
            return True

        if not await _until_async(drop, bool, 10):
            check(current, False, "Matteo could not drop the native silver cup")
            return _logged(results)
        witness_ready, witness_view = await prepare_street_theft_witness(
            link, CITY_OWNER, THEFT_POINT, ITEM,
        )
        if not witness_ready:
            check(
                current, False, {"preparation": "Matteo must see the hero before the theft", **witness_view}
            )
            return _logged(results)
        before_crimes = {f["id"] for f in facts("crime")}
        await link.debug("player_take", item=ITEM, distance=800)
        await _until(lambda: any(f["id"] not in before_crimes for f in facts("crime")), 10)
        crime = next((f for f in facts("crime") if f["id"] not in before_crimes), {})
        # Finish the held perception before replacing the witness's native reaction.
        reacted = bool(crime) and await _until(
            lambda: any(
                fact.get("crime") == crime["id"] and fact.get("npc") == CITY_OWNER
                and fact.get("verdict") == "server" for fact in facts("crime_reaction")
            ),
            10,
        )
        if not reacted:
            check(current, False, {"crime": crime, "reactions": facts("crime_reaction")})
            return _logged(results)
        if not await pose(CITY_OWNER, THEFT_POINT, face="PC_HERO"):
            return _logged(results)
        await law.tick()
        cid = str(crime.get("id", ""))
        if not check(
            current,
            crime.get("kind") == "theft"
            and crime.get("item") == ITEM
            and crime.get("owner") == CITY_OWNER
            and crime.get("community") == "city"
            and set(crime.get("witnesses", ())) == {CITY_OWNER}
            and remembers(cid),
            {"crime": crime, "witness_before": witness_view},
        ):
            return _logged(results)

        current = "goal_by_loot"
        turn_in = {status: LOG_RUNNING, stage: 2}
        ready = await _until_async(lambda: values(status, stage), lambda v: v == turn_in, 15)
        await book.settle()
        offered = await link.offered_dialogs(GIVER)
        counts = {t["id"]: t["count"] for t in await link.triggers() if t["id"].startswith(f"{q.id}_")}
        if not check(
            current,
            ready
            and await has(ITEM) == 1
            and book.progress(q.id).stage == 2
            and counts == {questbook.goal_id(q, 1, 1): 1, questbook.stage_trigger(q, 1): 1}
            and done in offered
            and give not in offered,
            {
                "quest": await values(status, stage),
                "cups": await has(ITEM),
                "triggers": counts,
                "offered": offered,
            },
        ):
            return _logged(results)

        current = "branch"
        # Save the travelling report too: reload must retain its original deadline, not start it anew.
        before_law = await values(law_var("city"), *COUNTERS, "ANDRE_SCHULDEN")
        pending = deliveries(cid)
        branch = service.chronicle.branch
        saved = await _save(link, slot)
        if saved is None:
            check(current, False, "could not save after the theft")
            return _logged(results)
        loading = asyncio.create_task(wait_message(link, Session, lambda m: m.reason == "load", 60))
        await asyncio.sleep(0)
        await link.debug("load", slot)
        await loading
        await link.wait_world()
        await asyncio.sleep(1)
        await law.tick()
        await book.settle()
        after_law = await values(law_var("city"), *COUNTERS, "ANDRE_SCHULDEN")
        restored = book.all().get(q.id)
        if not check(
            current,
            await values(status, stage) == turn_in
            and book.progress(q.id).stage == 2
            and restored == q
            and before_law == after_law
            and before_law[law_var("city")] == 0
            and pending == [(CITY_OWNER, game_minutes(crime) + REPORT_DELAYS["theft"], "pending")]
            and deliveries(cid) == pending
            and not news(cid)
            and not absolved(cid)
            and remembers(cid)
            and crime in facts("crime")
            and await has(ITEM) == 1
            and done in await link.offered_dialogs(GIVER)
            and PETZMASTER not in await link.offered_dialogs(ANDRE),
            {
                "branches": [branch, service.chronicle.branch],
                "quest": await values(status, stage),
                "law_before": before_law,
                "law_after": after_law,
                "reports": deliveries(cid),
            },
        ):
            return _logged(results)

        current = "law_knows"
        # Same sleeping rule as crime_law: an earlier hour advances to tomorrow, then pass 24 hours.
        await link.debug("time", hour=7, minute=59)
        await link.debug("time", hour=9, minute=0)
        await law.tick()
        native_fine = await fine(PETZMASTER)
        if not check(
            current,
            bool(news(cid))
            and native_fine["ready"]
            and native_fine["native"]
            and native_fine["debt"] == 50
            and [f["id"] for f in law.law.unpaid("city")] == [cid]
            and news(cid)[0].get("at") == pending[0][1]
            and not absolved(cid),
            {"news": news(cid), **native_fine},
        ):
            return _logged(results)
        # Decline through Andre's own choice; the next visit must use native PMSCHULDEN.
        await link.pick_in_menu(law.graph.node(PAYLATER).description)
        await _until(lambda: selected(PAYLATER, made=False), 15)
        await _until_async(link.story_state, lambda state: not state.get("dialog"), 30)

        current = "turn_in"
        if not await pose(GIVER, home):
            return _logged(results)
        gold, xp, cups = (
            await has("ITMI_GOLD"),
            await link.evaluate(["field", ["inst", "HERO"], "EXP"]),
            await has(ITEM),
        )
        await giver_dialog(done)
        success = {status: LOG_SUCCESS, stage: 3}
        ready = await _until_async(lambda: values(status, stage), lambda v: v == success, 15)
        await book.settle()
        after_gold = await has("ITMI_GOLD")
        after_xp = await link.evaluate(["field", ["inst", "HERO"], "EXP"])
        if not check(
            current,
            ready
            and book.progress(q.id).state == "done"
            and selected(done, made=True)
            and after_gold == gold + q.gold
            and after_xp == xp + q.xp
            and await has(ITEM) == cups - 1
            and not absolved(cid)
            and [f["id"] for f in law.law.unpaid("city")] == [cid]
            and (await values("ANDRE_SCHULDEN")).get("ANDRE_SCHULDEN") == 50,
            {
                "quest": await values(status, stage),
                "gold": [gold, after_gold],
                "xp": [xp, after_xp],
                "cups": [cups, await has(ITEM)],
                "debt": await values("ANDRE_SCHULDEN"),
            },
        ):
            return _logged(results)
        await end(GIVER)

        current = "not_forgiven"
        await law.tick()
        native_fine = await fine(PMSCHULDEN)
        summary = service.situation(CITY_OWNER) or ""
        if not check(
            current,
            native_fine["ready"]
            and native_fine["native"]
            and native_fine["debt"] == 50
            and remembers(cid)
            and service.describe(crime) in summary
            and not absolved(cid)
            and [f["id"] for f in law.law.unpaid("city")] == [cid]
            and await values(status, stage) == success,
            {**native_fine, "matteo": summary, "quest": await values(status, stage)},
        ):
            return _logged(results)

        current = "paid"
        before_pay = await has("ITMI_GOLD")
        await link.pick_in_menu(law.graph.node(PAYNOW).description)
        paid = await _until(lambda: absolved(cid), 30)
        await end(ANDRE)
        await law.tick()
        await book.settle()
        # A completed quest must reject a repeated turn-in, including after the law's graph update.
        repeated = False
        try:
            await link.choose_dialog(GIVER, done)
            repeated = True
        except GameError:
            pass
        left = await link.offered_dialogs(GIVER)
        final = await values(law_var("city"), *COUNTERS, "ANDRE_SCHULDEN")
        check(
            current,
            paid
            and selected(PAYNOW, made=False)
            and not repeated
            and await has("ITMI_GOLD") == before_pay - 50 == after_gold - 50
            and await link.evaluate(["field", ["inst", "HERO"], "EXP"]) == after_xp
            and await values(status, stage) == success
            and book.progress(q.id).state == "done"
            and not any(n.startswith(f"{q.id}_") for n in left)
            and sum(m.node == done and m.made for m in chosen) == 1
            and not law.law.unpaid("city")
            and all(value == 0 for value in final.values())
            and not {PETZMASTER, PMSCHULDEN} & set(await link.offered_dialogs(ANDRE))
            and remembers(cid),
            {
                "quest": await values(status, stage),
                "gold": [before_pay, await has("ITMI_GOLD")],
                "law": final,
                "repeated_turn_in": repeated,
                "giver_offered": left,
                "absolved": facts("absolved"),
            },
        )
        return _logged(results)
    except (GameError, TimeoutError) as error:
        check(current, False, f"{type(error).__name__}: {error}")
        return _logged(results)
    finally:
        rumors.gate, rumors.stage = original_gate, original_stage
        link.listeners.remove(record)
