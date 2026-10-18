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
from .common import _logged, _save, _until, _until_async, wait_message
from .crime_law import COUNTERS, PAYNOW, PETZMASTER, PMSCHULDEN
from .crimes import ANDRE, CITY_HOUSE, CITY_OWNER

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

    Only the scene is staged, as in crime_law: other people wait away from Matteo's house and his
    fight is stopped after ASSESSTHEFT. Ownership, guilds and native theft perceptions stay intact.
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

    async def stand(npc: str, point: str, *, face: str | None = None) -> None:
        await link.debug("skip_important", near=npc)
        await link.npc_act(
            npc, "start_activity", state="ZS_STAND_WP", waypoint=point, minutes=120, place=True
        )
        if face is not None:
            await link.npc_act(npc, "turn_to", npc=face)

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
        await link.start_talk(ANDRE)
        payment = law.graph.node(PAYNOW).description
        ready = await _until_async(link.dialog_menu, lambda menu: bool(payment) and payment in menu, 90)
        return {
            "ready": ready,
            "native": any(m.node == node and not m.made for m in chosen[start:]),
            "debt": (await values("ANDRE_SCHULDEN")).get("ANDRE_SCHULDEN"),
            "menu": await link.dialog_menu(),
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
            and CITY_HOUSE in points
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
        origin = points[CITY_HOUSE]
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
        await stand(GIVER, home)
        q = questbook.Quest(
            id=book.next_id(),
            giver=GIVER,
            title="Серебряный кубок",
            give=talk_line("Тебе что-нибудь нужно?", "Принеси серебряный кубок. Заплачу десять золотых."),
            stages=[
                questbook.Stage(
                    "Надо раздобыть серебряный кубок для Абуина.", [questbook.Goal("have", item=ITEM)]
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
        await link.debug("player_goto", waypoint=CITY_HOUSE)
        for person in await link.story_nearby("PC_HERO", 7000):
            if person["npc"] in people and person["npc"] != CITY_OWNER:
                await stand(person["npc"], parked)
        await stand(CITY_OWNER, CITY_HOUSE, face="PLAYER")
        await asyncio.sleep(0.5)

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
        await asyncio.sleep(1)
        await link.npc_act(CITY_OWNER, "turn_to", npc="PLAYER")
        before_crimes = {f["id"] for f in facts("crime")}
        await link.debug("player_take", item=ITEM, distance=800)
        await _until(lambda: any(f["id"] not in before_crimes for f in facts("crime")), 10)
        crime = next((f for f in facts("crime") if f["id"] not in before_crimes), {})
        await stand(CITY_OWNER, CITY_HOUSE, face="PLAYER")
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
            crime,
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
        await stand(GIVER, home)
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
