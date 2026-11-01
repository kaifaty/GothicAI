from __future__ import annotations

import asyncio
import json
import logging
import math
import re
import time
from collections import Counter
from collections.abc import Callable
from dataclasses import replace
from pathlib import Path
from typing import Any, Literal

from ..bodies import Bodies
from ..brain import NpcPlanner, Outcome
from ..budget import Budget
from ..chests import BACK, CHEST, OWNED, ChestDesk
from ..chronicle import Fact
from ..concerns import Concerns
from ..contracts import gold_text
from ..daylife import DayDesk
from ..deals import GOLD, Deals
from ..dialogue import DialogueManager
from ..errands import Errands
from ..game import GameError, GameLink
from ..gossip import CACHE_KEY as GOSSIP_KEY
from ..gossip import WORDS_CM, GossipDesk, labels_of
from ..guide import Guide
from ..initiative import ARRIVE_CM, CHECKINFO, InitiativeDesk
from ..llm import ToolCall
from ..meetings import DAY as GAME_DAY
from ..meetings import Meetings, clock
from ..perception import PerceptionDesk
from ..persona import habits_of
from ..protocol import (
    GameMessage,
    LineStarted,
    Loading,
    NpcActCmd,
    NpcActResult,
    NpcEvent,
    NpcIdle,
    NpcRoster,
    PlayerText,
    RoutineCmd,
    SayCmd,
    Session,
    Snapshot,
)
from ..quests import RUMOR_CM
from ..sightings import Sightings
from ..social import DAY, Rumor, Rumors
from ..storyguard import StoryGuard
from ..talk import TalkDesk
from ..trace import TraceWriter
from ..trace import read as read_trace
from .common import (
    ANDRE,
    HILDA,
    LOBART,
    RUEBEN,
    VINO,
    FixedDecider,
    _caught_by,
    _logged,
    _passes_text,
    _save,
    _scene_lines,
    _until,
    _until_async,
    attach_when_free,
    model_off,
    wait_message,
)
from .live import LiveCase, LiveSuite

SCENARIOS = {
    "chests": ("GIVEN", {"save": False, "timeout": 240}),
    "chests_lobart": ("LIVE", None),
    "day": ("GIVEN", {"save": True, "timeout": 480}),
    "errand": ("GIVEN", {"save": True, "timeout": 1500}),
    "errand_fork": ("GIVEN", {"save": True, "timeout": 1500}),
    "errand_far": ("GIVEN", {"save": True, "timeout": 1500}),
    "errand_give": ("GIVEN", {"save": True, "timeout": 240}),
    "initiative": ("GIVEN", {"save": True, "timeout": 240}),
    "lead": ("GIVEN", {"save": True, "timeout": 540}),
    "meet": ("GIVEN", {"save": True, "timeout": 600}),
    "meet_missed": ("GIVEN", {"save": True, "timeout": 700}),
    "noise": ("GIVEN", {"save": True, "timeout": 240}),
}

log = logging.getLogger("gothic_ai.scenarios")


async def lead_scenario(
    link: GameLink,
    dialogue: DialogueManager,
    talk: TalkDesk,
    deals: Deals,
    guide: Guide,
    npc: str = "BAU_954_MALETH",
    to: str = "FARM1_MILL",
    *, live: LiveCase | None = None,
) -> dict[str, str]:
    """The npc leads the hero (docs/npc-tools.md, wave 2 step 4: `lead`): «Малет, отведи меня к мельнице».
    Checks:
    - sets_off: the hero says yes (Y), Malet sets off for the mill;
    - waits: the hero is put back where Malet started: it stops and waits for him; he comes up, it goes on;
    - there: the hero follows it to the mill; a fact `led` (arrived).
    How fast it walks is logged (`pace`). Given: Malet offers to lead the hero to the mill."""
    await link.wait_world()
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    if live is None:
        off = model_off(dialogue)
        off.given(
            "дано: Малет предлагает проводить к мельнице", who=npc, when="отведи меня",
            calls=[ToolCall("propose_deal", {"npc_gives": [], "hero_gives": [], "lead": to})],
        )
        off.silent("дано: Малет молчит (предложение проводить показано)", who=npc, when="отведи меня")
        off.silent("дано: Малет молчит (старт провожатого)", who=npc, when="ты ведёшь его")
        off.silent("дано: Малет молчит (герой отстал)", who=npc, when="Герой отстал", times=2)
        off.silent("дано: Малет молчит (пришли к мельнице)", who=npc, when="Вы пришли")
    book = guide.book
    mill = book.get(to) if book is not None else None
    if mill is None or mill.center is None:
        return _logged({"book": f"failed (no {to} in the book of places)"})
    await link.debug("time", hour=10, minute=0)
    await link.debug("skip_important", near=npc)  # Malet's own dialog would stop the hero at once
    await link.debug("teleport", near=npc, distance=300)
    if not await _until(lambda: npc in talk.people and talk.people[npc].distance <= 600, 20):
        him = await link.snapshot(npc, fresh=True)
        around = sorted((round(e.distance / 100), e.id) for e in talk.people.values())[:5]
        return _logged({"roster": f"failed ({npc} at {him.npc.pos}, the hero at "
                        f"{him.player.pos if him.player else '?'}; around him {around})"})  # fmt: skip

    request = "Малет, отведи меня к мельнице."
    talk.hear(request, npc, {"via": "scenario"})
    if live is not None:
        await _until(
            lambda: bool(live.turns(npc, request)) and (
                not any(call.get("name") == "propose_deal" and call.get("outcome") == "accepted"
                        for call in live.calls(npc)) or npc in deals.open
            ),
            90,
        )
        rows = live.turns(npc, request)
        calls = [call for row in rows for step in row.get("steps", [])
                 for call in step.get("calls", [])]
        deal = deals.open.get(npc)
        offered = bool(
            deal is not None and deal.lead is not None
            and any(call.get("name") == "propose_deal" and call.get("outcome") == "accepted"
                    for call in calls)
        )
        live.finish("success" if offered else "failure" if rows else "no_decision", evidence={
            "decision": "lead_offered" if offered else "no_lead_offer",
            "calls": calls, "terms": deal.terms() if deal is not None else None,
            "words": [step.get("text", "") for row in rows
                      for step in row.get("steps", []) if step.get("text")],
        })
        return _logged(results)
    await _until(lambda: npc in deals.open, 60)
    deal = deals.open.get(npc)
    if deal is None:
        return _logged({**results, "setup": "failed (no guide deal to accept)"})
    await asyncio.sleep(1.2)  # Y waits until the terms have stood a moment
    pressed = await link.press_deal()
    started = await _until(lambda: npc in guide.leads and guide.leads[npc].state == "going", 30)
    start = await link.snapshot(npc, fresh=True)
    away = await _until_async(
        lambda: link.snapshot(npc, fresh=True), lambda snap: math.dist(snap.npc.pos, start.npc.pos) > 800, 30
    )
    check("sets_off", pressed == "sent" and started and away, f"Y: {pressed}")

    # where it is on the way, for the log, and how fast it goes while it goes (not while it waits)
    speeds: list[float] = []

    async def trace() -> None:
        last: tuple[float, tuple[float, float, float]] | None = None
        while True:
            snap = link.snapshots.get(npc)
            lead = guide.leads.get(npc)
            now = time.monotonic()
            if snap is not None and lead is not None and lead.state == "going":
                if last is not None and (v := math.dist(snap.npc.pos, last[1]) / 100 / (now - last[0])) > 0.3:
                    speeds.append(v)
                last = (now, snap.npc.pos)
            else:
                last = None
            if snap is not None and snap.player is not None:
                log.info("lead: %s at %s (%s), %.0f m from %s, the hero %.0f m off, %s", npc,
                         [round(x) for x in snap.npc.pos], snap.npc.nearest_waypoint,
                         math.dist(snap.npc.pos, mill.center) / 100, to, snap.player.distance / 100,
                         lead.state if lead else "over")  # fmt: skip
            await asyncio.sleep(2)

    tracing = asyncio.create_task(trace())
    # the hero is back where it set off: it stops and waits for him; he comes up, it goes on
    await _until_async(
        lambda: link.snapshot(npc, fresh=True), lambda snap: math.dist(snap.npc.pos, start.npc.pos) > 1500, 60
    )
    await link.debug("player_goto", waypoint=start.npc.nearest_waypoint or start.npc.waypoint or "")
    waited = await _until(lambda: npc in guide.leads and guide.leads[npc].state == "waiting", 20)
    await link.debug("teleport", near=npc, distance=300)
    on = await _until(lambda: npc in guide.leads and guide.leads[npc].state == "going", 20)
    check("waits", waited and on, f"waited {waited}, went on {on}")

    # he goes after it: put behind it now and then, as a player who follows
    async def after() -> None:
        while npc in guide.leads:
            if guide.leads[npc].state == "waiting":
                await link.debug("teleport", near=npc, distance=300)
            await asyncio.sleep(1)

    follow = asyncio.create_task(after())
    try:
        await _until(lambda: npc not in guide.leads, 180)
    finally:
        follow.cancel()
        tracing.cancel()
    led = [f for f in guide.chronicle.facts("led") if f.get("npc") == npc]
    snap = await link.snapshot(npc, fresh=True)
    there = book.of(snap.npc.nearest_waypoint) if book is not None else None
    pace = sorted(speeds)[len(speeds) // 2] if speeds else 0.0
    results["pace"] = f"done ({pace:.1f} m/s on the move, the hero runs)"
    await asyncio.sleep(4)
    check(
        "there",
        bool(led) and led[-1].get("how") == "arrived" and math.dist(snap.npc.pos, mill.center) < 2000,
        f"{led[-1].get('how') if led else 'no fact'}, at {snap.npc.nearest_waypoint} "
        f"({there.id if there else '?'}), {math.dist(snap.npc.pos, mill.center) / 100:.0f} m from {to}",
    )
    return _logged(results)


async def meet_scenario(
    link: GameLink,
    dialogue: DialogueManager,
    talk: TalkDesk,
    deals: Deals,
    meetings: Meetings,
    *,
    missed: bool = False,
    npc: str = "BAU_954_MALETH",
    to: str = "FARM1_WOLF",
    live: LiveCase | None = None,
) -> dict[str, str]:
    """A meeting with the hero (docs/npc-tools.md, wave 2 step 4: `meet`): at 19:00 the hero agrees with
    Malet to meet at dawn by the field in front of the city gates. Checks:
    - agreed: Y; the meeting is the desk's;
    - there: the clock leaps past when Malet sets off, as when the hero sleeps: the engine puts everyone at
      their routine's spot, and Malet is put at the meeting at once (`start_activity.place`);
    - kept: the hero comes, a fact `meet_kept`, and Malet speaks first (`first`).
    With `missed` (SCENARIO=meet_missed) the clock goes to a little before he sets off: he walks there himself
    and stands there by the hour. Then the hero does not come: the clock past its end, his word is broken
    (`broken`, promise meet), Malet goes back to his routine (`routine`), and when the hero comes up to him
    the missed meeting is in his situation (`reproach`), read without a model turn.
    Given: Malet agrees to meet the hero at dawn."""
    await link.wait_world()
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    off = model_off(dialogue) if live is None else None
    await link.debug("time", hour=19, minute=0)
    await link.debug("skip_important", near=npc)  # Malet's own dialog would stop the hero at once
    await link.debug("teleport", near=npc, distance=300)
    if not await _until(lambda: npc in talk.people and talk.people[npc].distance <= 600, 20):
        return _logged({"roster": f"failed ({npc} is not around the hero)"})
    snap = await link.snapshot(npc, fresh=True)
    tomorrow = snap.world.day + 2  # as the npc sees days: from 1
    dawn = {"where": to, "day": tomorrow, "hour": 5, "minute": 0, "hours": 2}
    if off is not None:
        off.given(
            "дано: Малет соглашается встретиться на рассвете", who=npc, when="встретимся",
            calls=[ToolCall("propose_deal", {"meet": dawn})],
        )
        off.silent("дано: Малет молчит (встреча предложена)", who=npc, when="встретимся")
        off.silent("дано: Малет молчит (сделка о встрече состоялась)", who=npc, when="сделка состоялась")
        if not missed:
            off.silent("дано: Малет молчит (герой пришёл на встречу)", who=npc, when="пришёл на встречу")

    request = (
        "Малет, давай встретимся завтра на рассвете у поля перед городскими воротами. Мне нужно с тобой "
        "поговорить без лишних ушей."
    )
    talk.hear(request, npc, {"via": "scenario"})
    if live is not None:
        effects_settled = await _until(
            lambda: bool(live.turns(npc, request)) and (
                not any(call.get("name") == "propose_deal" and call.get("outcome") == "accepted"
                        for call in live.calls(npc)) or npc in deals.open
            ),
            90,
        )
        rows = live.turns(npc, request)
        calls = [call for row in rows for step in row.get("steps", [])
                 for call in step.get("calls", [])]
        deal = deals.open.get(npc)
        term = deal.meet if deal is not None else None
        offered = bool(term is not None and any(
            call.get("name") == "propose_deal" and call.get("outcome") == "accepted" for call in calls
        ))
        requested = (tomorrow - 1) * GAME_DAY + 5 * 60
        live.finish("observed" if effects_settled else "failure" if rows else "no_decision", evidence={
            "decision": "pending_effect" if rows and not effects_settled else (
                ("agreed" if term.start == requested else "own_time") if offered else "no_meeting_offer"
            ),
            "effects_settled": effects_settled,
            "calls": calls, "terms": deal.terms() if deal is not None else None,
            "requested": dawn, "meeting": term.text() if term is not None else None,
            "words": [step.get("text", "") for row in rows
                      for step in row.get("steps", []) if step.get("text")],
        })
        return _logged(results)
    shown = await _until(lambda: npc in deals.open and deals.open[npc].meet is not None, 60)
    deal = deals.open.get(npc)
    if not shown or deal is None or deal.meet is None:
        return _logged({**results, "setup": "failed (no meeting deal to accept)"})
    await asyncio.sleep(1.2)  # Y waits until the terms have stood a moment
    pressed = await link.press_deal()

    def meeting() -> Any:
        return next((m for m in meetings.meetings.values() if m.npc == npc and m.deal == deal.id), None)

    agreed = await _until(lambda: meeting() is not None, 20)
    m = meeting()
    check("agreed", pressed == "sent" and agreed, f"Y: {pressed}, {m.term.text() if m else 'no meeting'}")
    if m is None:
        return _logged(results)
    await _until(lambda: not dialogue.speaking(npc), 20)
    talk.end("gone")
    released = await _until(lambda: npc not in talk.attached, 20)
    check("released", released, "the agreement conversation ended before the clock leap")
    if not released:
        return _logged(results)

    # the clock past when it sets off, near enough the hour for him to come (it is put there), or a little
    # before it sets off (it walks there by the hour)
    before = m.term.start - m.lead_min - 3 if missed else max(m.term.start - m.lead_min, m.term.start - 25)
    await link.debug("time", hour=before % GAME_DAY // 60, minute=before % 60)
    there = await _until(lambda: m.state in ("there", "kept"), 300)
    snap = await link.snapshot(npc, fresh=True)
    now = _game_minute(snap)
    far = math.dist(snap.npc.pos, m.term.way.pos) / 100
    detail = f"{m.state} at {clock(now)}, {far:.0f} m from it"
    check("there", there and far < 10 and now <= m.term.start, detail)
    if not there:
        return _logged(results)

    if not missed:
        await link.debug("skip_important", near=npc)
        await link.debug("teleport", near=npc, distance=300)
        ready = await _until(
            lambda: (entry := talk.people.get(npc)) is not None
            and entry.distance <= 700 and entry.earshot <= 700 and entry.activity != "down",
            20,
        )
        entry = talk.people.get(npc)
        check(
            "arrival_ready", ready,
            f"distance/hearing: {entry.distance}/{entry.earshot}" if entry else "no roster",
        )
        if not ready:
            return _logged(results)
        kept = await _until(lambda: m.state == "kept", 30)
        facts = [f for f in meetings.chronicle.facts("meet_kept") if f.get("meet") == m.id]
        check("kept", kept and bool(facts), m.state)
        def greeting_sent() -> bool:
            conv = dialogue.conversations.get(npc)
            return conv is not None and any("Герой пришёл на встречу" in m.content for m in conv.history)

        sent = await _until(greeting_sent, 60)
        check("first", sent and talk.partner is not None and talk.partner.npc == npc,
              f"meeting cue sent: {sent}, partner: {talk.partner.npc if talk.partner else 'none'}")
        return _logged(results)

    # he does not come: the clock past its end
    after = m.term.end + 10
    await link.debug("time", hour=after % GAME_DAY // 60, minute=after % 60)
    broken = await _until(lambda: m.state == "missed", 30)
    facts = [f for f in meetings.chronicle.facts("broken") if f.get("meet") == m.id]
    check("broken", broken and bool(facts) and facts[0].get("promise") == "meet", m.state)
    left = await _until_async(
        lambda: link.snapshot(npc, fresh=True), lambda s: math.dist(s.npc.pos, m.term.way.pos) > 800, 180
    )
    snap = await link.snapshot(npc, fresh=True)
    check("routine", left, f"{math.dist(snap.npc.pos, m.term.way.pos) / 100:.0f} m from the meeting")
    await link.debug("skip_important", near=npc)
    await link.debug("teleport", near=npc, distance=300)
    await _until(lambda: npc in talk.people and talk.people[npc].distance <= 600, 20)
    situation = await dialogue.situation(npc, "Малет")
    shown_to_it = "не пришёл на встречу" in situation
    check("reproach", shown_to_it, f"missed meeting in situation: {shown_to_it}")
    return _logged(results)


def _game_minute(snap: Snapshot) -> int:
    return snap.world.day * GAME_DAY + snap.world.hour * 60 + snap.world.minute


async def _chests_lobart(
    link: GameLink,
    dialogue: DialogueManager,
    talk: TalkDesk,
    chests: ChestDesk | None,
    npc: str,
    item: str,
    ask: str,
) -> dict[str, str]:
    """Observe a real chest retrieval and delivery, rather than the promise to fetch it."""
    suite = LiveSuite("chests_lobart", dialogue.trace)
    for case_name in suite.selected:
        with suite.case(case_name) as case:
            if chests is None:
                raise GameError("chests_lobart requires the chest subsystem")
            await link.wait_world()
            await link.debug("skip_important", near=npc)
            await link.debug("player_goto", to=npc, distance=300)
            if not await _until(lambda: npc in talk.people and talk.people[npc].distance <= 800, 20):
                raise GameError("Lobart is not near the hero")
            chest = next(
                (c for c in await link.npc_chests(npc) if any(i.item == item and i.count for i in c.items)),
                None,
            )
            if chest is None:
                raise GameError("Lobart's chest does not contain the requested clothes")

            async def hero_has() -> int:
                expr = ["call", "NPC_HASITEMS", ["inst", "HERO"], ["sym", item]]
                return int(await link.evaluate(expr) or 0)

            before = await hero_has()
            # The hero can pay an ordinary clothing price when an actual offer is made.
            await link.story_effect(npc, [{"do": "give", "item": GOLD, "count": 1000}])
            start = await link.snapshot(npc, fresh=True)
            positions = [start.npc.pos]
            entries: list[dict[str, Any]] = []

            def refusal_words(saved: list[dict[str, Any]] = entries) -> list[str]:
                replies = [e["text"] for e in saved if e.get("kind") == "npc" and e.get("npc") == npc]
                # Only an explicit refusal to sell is a failure; a promise without delivery is undecided.
                refusals = (
                    "не продам", "не продаю", "не продается", "не продаётся", "не продаются",
                    "не буду продавать", "не собираюсь продавать", "не могу продать", "не для продажи",
                )
                answer = replies[-1].lower().strip(" —–-") if replies else ""
                refused = re.match(r"нет\b(?!\s+проблем\b)", answer) or any(p in answer for p in refusals)
                return replies[-1:] if refused else []

            def position(msg: GameMessage, samples: list[tuple[float, float, float]] = positions) -> None:
                if isinstance(msg, Snapshot) and msg.npc.id.upper() == npc:
                    samples.append(msg.npc.pos)

            link.listeners.append(position)
            journals = [dialogue.journal]
            if chests.journal is not dialogue.journal:
                journals.append(chests.journal)
            sinks = [journal.sink for journal in journals]
            for journal, sink in zip(journals, sinks, strict=True):

                def keep(
                    entry: dict[str, Any], previous: Any = sink, saved: list[dict[str, Any]] = entries,
                ) -> None:
                    saved.append(entry)
                    if previous is not None:
                        previous(entry)

                journal.sink = keep
            try:
                talk.hear(ask, npc, {"via": "scenario"})
                deadline = time.monotonic() + 240
                confirmed = False
                after = before
                accepted: list[str] = []
                while time.monotonic() < deadline:
                    after = await hero_has()
                    fetched = any(
                        e.get("kind") == "chest" and e.get("npc") == npc
                        and e.get("chest") == chest.id and e.get("took", {}).get(item, 0) > 0
                        for e in entries
                    )
                    if fetched and after > before:
                        break
                    window = await link.deal_window()
                    if window is not None and window.get("npc") == npc:
                        accepted.append(await link.press_deal())
                    elif not dialogue.speaking(npc) and npc not in chests._going:
                        replies = [e for e in entries if e.get("kind") == "npc" and e.get("npc") == npc]
                        if refusal_words():
                            break
                        if replies and not confirmed:
                            # A real model may name its price before it starts walking.
                            talk.hear("Да, беру, по рукам.", npc, {"via": "scenario"})
                            confirmed = True
                    await asyncio.sleep(1)

                await _until(lambda: not dialogue.speaking(npc), 30)
                end = await link.snapshot(npc, fresh=True)
                after = await hero_has()
                positions.append(end.npc.pos)
                moved_cm = max(math.dist(start.npc.pos, pos) for pos in positions)
                chest_facts = [e for e in entries if e.get("kind") == "chest" and e.get("npc") == npc]
                fetched = any(
                    e.get("chest") == chest.id and e.get("took", {}).get(item, 0) > 0 for e in chest_facts
                )
                words = [e["text"] for e in entries if e.get("kind") == "npc" and e.get("npc") == npc]
                calls = [
                    call for row in case.records
                    for step in row.get("steps", []) for call in step.get("calls", [])
                ]
                attempted = any(call.get("name") == CHEST for call in calls)
                refusal = refusal_words()
                outcome = "success" if moved_cm >= 100 and fetched and after > before else (
                    "failure" if refusal else "no_decision"
                )
                decision = "delivered" if outcome == "success" else (
                    "refused" if refusal else "incomplete" if words or attempted else "none"
                )
                case.finish(outcome, evidence={
                    "decision": decision,
                    "chest": chest.id, "chest_facts": chest_facts, "tool_outcomes": calls,
                    "npc_words": words, "refusal_words": refusal, "moved_cm": round(moved_cm, 1),
                    "hero_items_before": before, "hero_items_after": after,
                    "deal_acceptance": accepted, "timeout_s": 240,
                })
            finally:
                link.listeners.remove(position)
                for journal, sink in zip(journals, sinks, strict=True):
                    journal.sink = sink
    return suite.results()


async def chests_scenario(
    link: GameLink,
    dialogue: DialogueManager,
    talk: TalkDesk,
    chests: ChestDesk | None,
    npc: str = "BDT_1014_BANDIT_L",
    name: str = "Браго",
    item: str = "ITRW_BOW_L_01",
    ask: str = "Браго, продай мне лук.",
) -> dict[str, str]:
    """Npc's chests (docs/npc-tools.md «Сундуки»; the user, 2026-10-14: Brago should remember what lies in his
    chest, and go to take it out). The hero goes up to Brago in the bandits' cave below Xardas's tower and,
    unseen, takes the gold out of the chest there; then asks Brago for a bow. Given: Brago takes the bow
    from his chest. The separate live `chests_lobart` scenario keeps the model's own decision. Checks:
    - chest: the bridge counts the cave's chest as Brago's (nobody owns it, it stands by his routine), with
      a bow in it;
    - knows: Brago's situation tells what lies in it, as he knows: the bow and the gold too;
    - fetched: the given `chest` call makes him walk to the chest, open it and take the bow
      out; the bow is in his hands and no longer in the chest;
    - missing: having looked, he knows the gold is gone;
    - back: back from the chest, he receives the cue to continue the conversation with the hero."""
    if npc == LOBART:
        return await _chests_lobart(link, dialogue, talk, chests, npc, item, ask)
    await link.wait_world()
    results: dict[str, str] = {}
    if chests is None:
        return _logged({"chests": "failed (run with --chests)"})

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    bow, gold = item, "ITMI_GOLD"
    fetch = ToolCall(CHEST, {"take": [{"item": bow, "count": 1}], "put": [], "chest": None})
    off = model_off(dialogue)
    off.given("дано: Браго берёт лук из сундука", who=npc, when="продай", calls=[fetch])
    off.silent("дано: Браго молчит (решение идти к сундуку принято)", who=npc, when="продай")
    off.silent("дано: Браго молчит (вернулся от сундука)", who=npc, when=BACK)

    def dist() -> float:
        return talk.people[npc].distance / 100 if npc in talk.people else -1.0

    try:
        await link.debug("player_equip", item="ITAR_PAL_H")
    except GameError as err:
        log.info("chests: no armor for the hero: %s", err)
    await link.debug("skip_important", near=npc)
    await link.debug("player_goto", to=npc, distance=300)
    if not await _until(lambda: 0 <= dist() <= 8, 20):
        return _logged({"roster": f"failed ({npc} {dist():.1f} m from the hero)"})

    found = await link.npc_chests(npc)
    log.info(
        "chests: %s's: %s",
        npc,
        [(c.id, c.why, c.waypoint, [(i.item, i.count) for i in c.items]) for c in found],
    )
    cave = next((c for c in found if any(i.item == bow for i in c.items)), None)
    check("chest", cave is not None, [(c.id, c.why, c.waypoint) for c in found] or "none")
    if cave is None:
        return _logged(results)
    await link.debug("chest_take", chest=cave.id, item=gold, count=1000)
    log.info("chests: the hero took the gold out of %s unseen", cave.id)

    situation = await dialogue.situation(npc, name)
    line = next((s for s in situation.split("\n") if s.startswith(tuple(OWNED.values()))), "")
    check("knows", bow in line and gold in line, line or situation[-300:])

    talk.hear(ask, npc, {"via": "scenario"})
    await _until(lambda: npc in dialogue.conversations, 30)
    conv = dialogue.conversations.get(npc)

    def note() -> str:
        # the body's note of the walk (not its own words «схожу к сундуку»)
        done = ("[Ты сходил к своему сундуку", "[Не вышло", "[Тело не смогло")
        return next((m.content for m in conv.history if m.content.startswith(done)), "") if conv else ""

    async def where() -> None:  # how far it is from the hero while it goes, for the log
        while True:
            await asyncio.sleep(3)
            snap = link.snapshots.get(npc)
            log.info(
                "chests: %s %.1f m from the hero, at %s",
                npc,
                dist(),
                snap.npc.nearest_waypoint if snap else "?",
            )

    watch = asyncio.create_task(where())
    # there and back: a chest across a house takes a minute and a half (Lobart's)
    await _until(lambda: bool(note()) or chests._going, 5)
    await _until(lambda: bool(note()) or chests._going or not dialogue.speaking(npc), 40)
    await _until(lambda: bool(note()), 180)
    snap = await link.snapshot(npc, fresh=True)
    after = next((c for c in await link.npc_chests(npc) if c.id == cave.id), None)
    in_hands = any(i.item == bow for i in snap.npc.inventory)
    still = after is not None and any(i.item == bow for i in after.items)
    check(
        "fetched",
        in_hands and not still and "достал" in note(),
        f"in hands {in_hands}, in the chest {still}: {note()}",
    )
    check("missing", "пропало" in note() and "Золото" in note(), note())
    def returned() -> bool:
        return conv is not None and any(m.content == BACK for m in conv.history)

    await _until(returned, 20)
    await _until(lambda: not dialogue.speaking(npc), 30)  # what it says back from the chest, for the log
    watch.cancel()
    check("back", returned() and 0 <= dist() <= 5, f"{dist():.1f} m from the hero; return cue: {returned()}")
    return _logged(results)


INITIATIVE_DEBT = "e2einit"
# Lobart's farmhands: their important dialogs would stop the hero (Hilda's is the initiative's first check)
FARMHANDS = ("BAU_954_MALETH", "BAU_952_VINO")
CROWD_CM = 600.0  # nobody else this close to the hero: his line without a name could go to them


async def initiative_scenario(
    link: GameLink,
    dialogue: DialogueManager,
    talk: TalkDesk,
    deals: Deals,
    concerns: Concerns,
    guard: StoryGuard,
    bodies: Bodies,
    perception: PerceptionDesk,
    desk: InitiativeDesk | None = None,
    lender: str = LOBART,
    *,
    live: LiveCase | None = None,
) -> dict[str, str]:
    """An npc comes up to the hero and speaks first (docs/npc-tools.md, wave 2 step 1), on the game. Hilda has
    her greeting waiting for the hero: the game's own Npc_CheckInfo says so and the initiative leaves her to
    the scripts. The hero owes Lobart 25 gold since yesterday (a debt written into the chronicle): a broken
    word, on Lobart's mind. The hero stands 12 m in front of him; Lobart comes up by himself, once, and speaks
    first about the debt, the hero's next words without a name go to him. The hero says he will pay: Lobart
    offers a deal that settles the debt, the hero accepts it, and nothing is on Lobart's mind any more.
    A fixed decision makes Lobart come up; his opening and the completed deal are silent givens."""

    if live is None:
        off = model_off(dialogue)
        off.silent("дано: Лобарт молчит (подошёл потребовать долг)", who=lender, when="подошёл к герою")
        off.given(
            "дано: Лобарт предлагает погасить долг",
            who=lender,
            when="Отдам",
            calls=[ToolCall("propose_deal", {"npc_gives": [], "hero_gives": [], "repay": [INITIATIVE_DEBT]})],
        )
        off.silent("дано: Лобарт молчит (предложил погасить долг)", who=lender, when="Отдам")
        off.silent("дано: Лобарт молчит (сделка состоялась)", who=lender, when="сделка состоялась")
        log.info("given: дано: Лобарт подходит к герою (react=approach_talk)")
    elif desk is None:
        raise GameError("live initiative requires the behavioral model's initiative desk")
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    await link.wait_world()
    if link.session is None:
        await wait_message(link, Session, timeout=10)
    await asyncio.sleep(1)
    chronicle = deals.chronicle
    hero = ["inst", "HERO"]
    name = "Лобарт"

    async def has(item: str) -> int:
        return int(await link.evaluate(["call", "NPC_HASITEMS", hero, ["sym", item]]) or 0)

    async def checkinfo(npc: str) -> Any:
        """The game's own Npc_CheckInfo(npc, 1); None if it cannot tell."""
        try:
            return (await link.story_eval(npc, [CHECKINFO]))[0]
        except (GameError, TimeoutError, IndexError) as err:
            log.info("Npc_CheckInfo of %s: %s", npc, err)
            return None

    # Reuse the server's desk if present; both paths supply the same declared decision.
    own = desk is None
    runner: asyncio.Task[None] | None = None
    on_change: Callable[[str], None] | None = None
    talk_planner = talk.planner
    if desk is None:
        from ..brain import GameApplier

        planner = NpcPlanner(
            FixedDecider({"react": "approach_talk"}, npcs={lender}),
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
            chronicle,
            bodies=bodies,
            perception=perception,
            journal=dialogue.journal,
        )
        talk.planner = planner  # its conversation with the hero leaves the npc to it

        def agenda(npc: str) -> None:
            """What it wants of the hero follows its concerns (cli.py does so for the server's planner)."""
            planner.set_agenda(npc, concerns.agenda(npc))

        on_change = agenda
        concerns.on_change.append(on_change)
    planner = desk.planner
    decider = planner.decider
    if live is None:
        planner.decider = FixedDecider({"react": "approach_talk"}, npcs={lender})
    walked: list[float] = []  # when Lobart was sent to the hero (by the desk: no one else sends go_to_npc)
    send = link.send

    async def sending(cmd: Any) -> Any:
        if isinstance(cmd, NpcActCmd) and cmd.npc.upper() == lender and cmd.action == "go_to_npc":
            walked.append(time.monotonic())
        return await send(cmd)

    link.send = sending  # type: ignore[method-assign]
    typed: list[float] = []  # when the hero said something

    def heard(m: GameMessage) -> None:
        if isinstance(m, PlayerText):
            typed.append(time.monotonic())

    link.listeners.append(heard)
    try:
        # their important dialogs would stop the hero as he comes; not Hilda's yet (the first check)
        for npc in (lender, *FARMHANDS):
            try:
                await link.debug("skip_important", near=npc)
            except GameError as err:
                log.info("skip_important %s: %s", npc, err)
        # gold: Lobart has some to give, the hero 30 of it to pay back with
        await attach_when_free(link, lender)
        await link.act(lender, "debug_add_item", item=GOLD, count=100)
        await link.detach(lender)
        await link.story_effect(lender, [{"do": "give", "item": GOLD, "count": 30}])
        # the hero owes him 25 gold by yesterday
        day = int((await link.story_state()).get("day", 0))
        chronicle.record(
            {
                "fact": "debt",
                "id": INITIATIVE_DEBT,
                "npc": lender,
                "npc_name": name,
                "item": GOLD,
                "count": 25,
                "name": "золото",
                "due": day - 1,
                "day": day - 1,
            }
        )
        deals.reload()
        await link.debug("time", hour=10, minute=0)
        # 1. Hilda has her greeting for the hero (DIA_Hilda_Hallo): the game's own check says so, and the
        # initiative leaves her to the scripts; once it counts as told, nothing waits
        before, waiting = await checkinfo(HILDA), await desk.important(HILDA)
        await link.debug("skip_important", near=HILDA)
        after, left = await checkinfo(HILDA), await desk.important(HILDA)
        if live is None:
            check(
                "important_guard",
                before is not None and bool(before) and waiting
                and after is not None and not after and not left,
                f"Npc_CheckInfo {before} -> {after}, the desk {waiting} -> {left}",
            )

        # 2. the debt is past its day (the deals look every 10 s): a broken word, on Lobart's mind
        ok = await _until(
            lambda: any(f.get("debt") == INITIATIVE_DEBT for f in chronicle.facts("broken")), 20
        )
        top = concerns.top(lender)
        key = f"debt_broken:{INITIATIVE_DEBT}"
        check("concern", ok and top is not None and top.key == key, f"{top.key}: {top.en}" if top else "none")

        # 3. the hero 12 m in front of him, facing him, nobody else close: Lobart comes up by himself
        walks = len(walked)
        if own:
            runner = asyncio.create_task(planner.run())
        t0 = time.monotonic()
        crowd: list[str] = []
        for distance in (1200, 1500, 900):
            await link.debug("teleport", near=lender, distance=distance)
            await asyncio.sleep(1.5)  # the roster comes twice a second
            crowd = [
                f"{e.name} {e.distance / 100:.0f} m"
                for k, e in talk.people.items()
                if k != lender and e.activity != "down" and e.distance <= CROWD_CM
            ]
            if not crowd:
                break
        came = await _until(
            lambda: (
                len(walked) > walks and lender in talk.people and talk.people[lender].distance <= ARRIVE_CM
            ),
            45,
        )
        entry, rec = talk.people.get(lender), planner.npcs.get(lender)
        where = f"{entry.distance / 100:.1f} m" if entry else "not around"
        body = guard.body(lender, entry.state if entry else "") or "may walk"
        if live is not None:
            live.evidence.update(
                approached=came, walks=len(walked) - walks, distance=where,
                elapsed_s=round(time.monotonic() - t0, 2), body=body, crowd=crowd,
            )
            if not came:
                live.finish("failure")
                return _logged(results)
        else:
            check(
                "walked_up",
                came and len(walked) - walks == 1,
                f"{where} after {time.monotonic() - t0:.0f} s, {len(walked) - walks} walk(s), "
                f"vetoed {planner.stats.vetoed}, agenda {rec.agenda if rec else '-'!r}, body: {body}"
                f"{f', others close: {crowd}' if crowd else ''}",
            )
        if not came:
            return _logged(results)

        # 4. the initiative cue is delivered before any hero input, with Lobart listening
        def first_trigger() -> str:
            conv = dialogue.conversations.get(lender)
            if conv is None:
                return ""
            return next(
                (e.content for e in conv.memory.entries if e.content.startswith("[Ты сам подошёл к герою")),
                "",
            )

        await _until(lambda: bool(first_trigger()), 15)
        trigger = first_trigger()
        partner = talk.partner
        opened = partner is not None and partner.npc == lender and partner.posture == "listen"
        if live is None:
            check(
                "spoke_first",
                "25" in trigger and not typed and opened,
                f"cue {trigger!r}, hero inputs {len(typed)}, listening {opened}",
            )

        # 5. the fact: who came up, about what, who saw it
        fact = next((f for f in reversed(chronicle.facts("initiative")) if f.get("npc") == lender), None)
        if live is None:
            check(
                "fact",
                fact is not None and fact.get("occasion") == key and "PLAYER" in (fact.get("seen_by") or []),
                fact or "none",
            )
        else:
            live.evidence.update(initiative=fact, opening_cue=trigger, listening=opened)

        # 6. the hero answers him without a name: it goes to Lobart, his partner now
        await _until(lambda: not dialogue.speaking(lender), 90 if live is not None else 15)
        text = "Отдам, держи свои двадцать пять."
        deadline = time.monotonic() + 90
        await link.debug("type", text=text)
        await _until(lambda: talk.last is not None and talk.last[1] == text, 10)
        to = talk.last[0] if talk.last is not None and talk.last[1] == text else None
        if live is None:
            check(
                "reply_goes_to_him",
                to is not None and to.id == lender,
                f"to {to.id} by {to.why}" if to else "not heard or to nobody",
            )

        # 7. he offers a deal that settles the debt: the window shows it
        def offered() -> bool:
            deal = deals.open.get(lender)
            return deal is not None and [d.id for d in deal.repay] == [INITIATIVE_DEBT]

        if live is not None:
            answered = await live.wait_turn(
                dialogue, lender, text, timeout=max(0, deadline - time.monotonic()),
            )
            calls = [
                call for turn in live.turns(lender, text) for step in turn.get("steps", [])
                for call in step.get("calls", []) if call.get("name") == "propose_deal"
            ]
            # Deals._repay strips ids; their spelling and case remain significant.
            accepted = any(
                call.get("outcome") == "accepted"
                and INITIATIVE_DEBT in [str(value).strip() for value in
                                       (call.get("arguments", {}).get("repay") or [])]
                for call in calls
            )
            proposed = await _until(offered, max(0, deadline - time.monotonic())) if accepted else False
            live.finish("success" if came and answered and accepted and proposed else "failure", evidence={
                "answered": answered, "reply_to": to.id if to else None,
                "propose_deal": calls, "repayment_proposed": proposed,
                "window": await link.deal_window(),
            })
            return _logged(results)

        ok = await _until(offered, 30)
        owed = f"Долг: {gold_text(25)}"
        shown = await _until_async(
            link.deal_window, lambda w: w is not None and owed in (w.get("hero_gives") or []), 10
        )
        window = await link.deal_window() or {}
        check("debt_window", shown, window.get("hero_gives"))
        if not ok:
            return _logged(results)

        # 8. the hero accepts: the debt is paid, nothing is on Lobart's mind, he does not come up again
        gold = await has(GOLD)
        await asyncio.sleep(1.2)  # Y waits until the terms have stood a moment
        pressed = await link.press_deal()
        debt = deals.debts.get(INITIATIVE_DEBT)
        paid = await _until(lambda: debt is not None and debt.paid, 20)
        left_gold = await has(GOLD)
        walks = len(walked)
        await asyncio.sleep(20)
        top, rec = concerns.top(lender), planner.npcs.get(lender)
        check(
            "settled",
            pressed == "sent"
            and paid
            and top is None
            and rec is not None
            and rec.agenda is None
            and len(walked) == walks,
            f"{pressed}, gold {gold} -> {left_gold}, concern {top.key if top else None}, "
            f"agenda {rec.agenda if rec else '-'!r}, {len(walked) - walks} walk(s) after",
        )
    finally:
        link.send = send  # type: ignore[method-assign]
        link.listeners.remove(heard)
        planner.decider = decider
        if own:
            if runner is not None:
                runner.cancel()
            link.listeners.remove(planner.on_message)
            if on_change is not None:
                concerns.on_change.remove(on_change)
            talk.planner = talk_planner
    return _logged(results)


MALETH = "BAU_954_MALETH"
# Malet guards the sheep here 08:00-22:00, 56.4 m from Vino's field: they meet only in the stable's beds
MALETH_SPOT = "NW_FARM1_PATH_CITY_SHEEP_09"
# the hero by Malet: 12.2 m from his spot, 45.5 m from Vino's FIELD_04, 24.5 m from Lobart (routines' dump)
DAY_SCENE_HERO = "NW_FARM1_PATH_CITY_SHEEP_13"
DAY_FIELD_HERO = "NW_FARM1_FIELD_01_03"  # 18.5 m from FIELD_04, 72.4 m from Malet's spot
DAY_SLOT = 9  # the save after Vino heard the news: his walk near the hero, then again from there far from him
# the scene at Malet's spot as the scripted model writes it: (0 Vino, the teller, 1 Malet), text, mood
DAY_LINES = (
    (0, "Слыхал? Чужак Лобарту про репу наврал, а тот его и поймал.", "neutral"),
    (1, "Вот как? Пригляжу за ним у стада.", "neutral"),
    (0, "Пригляди. Мне-то что, лишь бы выпивку ставил.", "neutral"),
)
DEAD_OPTIONS = {"pick_up", "give_back", "rest", "socialize"}  # removed in wave 2 step 6


class DayRecorder:
    """Record the day menus and supply the declared choice, leaving other NPCs in their routines."""

    def __init__(self, pin: dict[str, dict[str, str]]) -> None:
        self.pin = {npc.upper(): questions for npc, questions in pin.items()}
        self.requests: list[tuple[str, dict[str, dict]]] = []
        self._rest = FixedDecider({}, npcs=())

    async def decide(self, rid, lane, priority, deadline_ms, state, questions) -> Outcome:  # noqa: ANN001
        npc = rid.split("#", 1)[0].upper()
        self.requests.append((npc, questions))
        out = await self._rest.decide(rid, lane, priority, deadline_ms, state, questions)
        answers = dict(out.answers or {})
        for name, choice in self.pin.get(npc, {}).items():
            if choice in questions.get(name, {}).get("criteria", {}):
                answers[name] = {
                    "type": "choice",
                    "choice": choice,
                    "probabilities": {choice: 0.9},
                    "confidence": 0.9,
                }
        return Outcome("answers", answers, out.queue_ms, out.infer_ms)

    async def cancel(self, rid: str) -> None:
        pass


class _TraceTap:
    """The planner's trace rows as they are written, kept for the checks (and still written)."""

    def __init__(self, inner: TraceWriter | None) -> None:
        self.inner = inner
        self.rows: list[dict[str, Any]] = []

    def write(self, row: dict[str, Any]) -> None:
        self.rows.append(row)
        if self.inner is not None:
            self.inner.write(row)


async def noise_scenario(
    link: GameLink, sightings: Sightings, planner: NpcPlanner | None = None
) -> dict[str, str]:
    """A native item impact behind Maleth: hear, decide, investigate, then resume his guard routine.

    No event or action is injected. A declared fixed decision makes Maleth investigate.
    """
    from ..brain import GameApplier
    from ..brain.day import current_entry

    npc = "BAU_954_MALETH"
    results = {
        k: "failed (0 observations)"
        for k in ("heard", "sent", "went", "idle", "routine", "memory")
    }

    def check(name: str, ok: bool, detail: str) -> None:
        results[name] = f"{'done' if ok else 'failed'} ({detail})"

    await link.wait_world()
    own = planner is None
    if planner is None:
        planner = NpcPlanner(FixedDecider({"react": "investigate"}, npcs={npc}), GameApplier(link))
        link.listeners.append(planner.on_message)
    brain = planner
    decider = brain.decider
    brain.decider = FixedDecider({"react": "investigate"}, npcs={npc})
    log.info("given: дано: Малет идёт проверить шум (react=investigate)")
    runner = asyncio.create_task(brain.run()) if own else None
    events: list[NpcEvent] = []
    receipts: list[tuple[float, NpcActResult]] = []
    idles: list[tuple[float, NpcIdle]] = []
    sent: list[tuple[float, NpcActCmd]] = []
    samples: list[tuple[float, Snapshot]] = []
    roster: dict[str, Any] = {}
    roster_at = 0.0
    started: float | None = None

    def heard(msg: GameMessage) -> None:
        nonlocal roster_at
        if isinstance(msg, NpcRoster):
            roster.update({e.id.upper(): e for e in msg.npcs})
            roster_at = time.monotonic()
        if started is None or not isinstance(msg, (NpcEvent, NpcActResult, NpcIdle)):
            return
        if msg.npc.upper() != npc:
            return
        now = time.monotonic()
        if isinstance(msg, NpcEvent) and msg.kind == "noise":
            events.append(msg)
        elif isinstance(msg, NpcActResult):
            receipts.append((now, msg))
        elif isinstance(msg, NpcIdle):
            idles.append((now, msg))

    send = link.send

    async def sending(cmd: Any) -> Any:
        if started is not None and isinstance(cmd, NpcActCmd) and cmd.npc.upper() == npc:
            sent.append((time.monotonic(), cmd))
        return await send(cmd)

    async def sample() -> None:
        while True:
            samples.append((time.monotonic(), await link.snapshot(npc, fresh=True)))
            await asyncio.sleep(0.2)

    link.listeners.append(heard)
    link.send = sending  # type: ignore[method-assign]
    sampling: asyncio.Task[None] | None = None
    try:
        await link.debug("time", hour=12, minute=0)
        await link.debug("skip_important", near=npc)
        # Signed distance puts the hero behind the guard, facing in the same direction. Native drop
        # starts at the hand, several metres away; no fixture freezes or clears the NPC's AI queue.
        await link.debug("teleport", near=npc, distance=-400)
        await link.debug("player_weapon", state="none")
        info = await link.routine(npc)
        before = await link.snapshot(npc, fresh=True)
        entry = current_entry(info.entries, before.world.hour, before.world.minute)
        points = await link.waypoints()
        post = next(
            (
                tuple(float(point[axis]) for axis in ("x", "y", "z"))
                for point in points
                if entry is not None and point["name"].upper() == entry.waypoint.upper()
            ),
            None,
        )
        sampling = asyncio.create_task(sample())

        def at_post(after: float = 0.0) -> bool:
            return bool(
                post is not None
                and samples
                and samples[-1][0] > after
                and math.dist(samples[-1][1].npc.pos, post) < 150
            )

        ready = await _until(
            lambda: (
                npc in roster
                and roster[npc].activity == "routine"
                and entry is not None
                and roster[npc].state.upper() == entry.state.upper()
                and npc in brain.npcs
                and not brain.npcs[npc].talking
                and not link.body_owned(npc)
                and at_post()
            ),
            20,
        )
        check(
            "setup",
            ready,
            f"ready {int(ready)}, state {roster[npc].state if npc in roster else '?'}; "
            f"routine entries {len(info.entries)}, post {entry.waypoint if entry else None}, at {post}",
        )
        if not ready:
            return _logged(results)
        started = time.monotonic()
        dropped = False
        attempts = 0
        for _ in range(20):
            attempts += 1
            try:
                await link.debug("player_drop", item="ITAR_PAL_H")
                dropped = True
                break
            except GameError as err:
                if str(err) != "the hero could not drop it now":
                    raise
                await asyncio.sleep(0.2)  # the saved or newly closed weapon may still be animating
        check("setup_drop", dropped, f"attempts {attempts}, dropped {int(dropped)}")
        if not dropped:
            return _logged(results)
        await _until(lambda: bool(events), 15)
        event = events[0] if events else None
        check(
            "heard",
            event is not None
            and event.at is not None
            and bool(event.place)
            and (event.loudness or 0) >= 0.35
            and bool(event.source)
            and event.distance is not None
            and event.distance > 200
            and not event.other
            and not event.other_name,
            f"events {len(events)}, at {event.at if event else None}, "
            f"place {event.place if event else None}, loudness {event.loudness if event else 0}, "
            f"source {event.source if event else None}, distance_cm {event.distance if event else 0}, "
            f"other {event.other if event else None!r}",
        )
        if event is None or event.at is None:
            return _logged(results)

        await _until(lambda: any(c.action == "investigate" for _, c in sent), 5)
        when_sent, act = next(((t, c) for t, c in sent if c.action == "investigate"), (0.0, None))
        await _until(lambda: act is not None and any(m.id == act.id for _, m in receipts), 5)
        receipt = next((m for _, m in receipts if act is not None and m.id == act.id), None)
        check(
            "sent",
            act is not None
            and act.args.get("at") == list(event.at)
            and 2000 <= act.args.get("ms", 0) <= 8000
            and receipt is not None
            and receipt.status == "queued",
            f"acts {len(sent)}, id {act.id if act else None}, at {act.args.get('at') if act else None}, "
            f"ms {act.args.get('ms', 0) if act else 0}, receipt {receipt.status if receipt else None}",
        )
        if act is None:
            return _logged(results)
        # B_ASSESSQUIETSOUND can keep the native AI queue busy for ~2 s: allow the real investigation
        # to start and finish itself, without another action, teleport or clearing its queue.
        await _until(
            lambda: any(m.act == act.id for _, m in idles), max(0, when_sent + 45 - time.monotonic())
        )
        idle_at, idle = next(((t, m) for t, m in idles if m.act == act.id), (0.0, None))
        distances = [
            math.dist(s.npc.pos, event.at)
            for t, s in samples
            if when_sent <= t <= (idle_at or time.monotonic())
        ]
        closest = min(distances, default=float("inf"))
        check(
            "went",
            closest < 200,
            f"snapshots {len(distances)}, closest_cm {closest:.1f}, start_cm {event.distance}",
        )
        elapsed = idle_at - when_sent if idle else time.monotonic() - when_sent
        check(
            "idle",
            idle is not None and idle.reason in (None, "done") and 0 <= elapsed <= 45,
            f"matching {int(idle is not None)}, id {idle.act if idle else None}, "
            f"reason {idle.reason if idle else None}, elapsed_s {elapsed:.2f}",
        )
        resumed = await _until(
            lambda: (
                idle is not None
                and roster_at > idle_at
                and npc in roster
                and roster[npc].activity == "routine"
                and roster[npc].state.upper() == entry.state.upper()
                and not link.body_owned(npc)
                # Investigate preserves this script state. Only a fresh position back at the actual
                # routine waypoint proves that its native routine resumed after the matching idle.
                and at_post(idle_at)
            ),
            15,
        )
        after = await link.routine(npc)
        post_distance = (
            math.dist(samples[-1][1].npc.pos, post) if post is not None and samples else float("inf")
        )
        check(
            "routine",
            resumed and after.current.upper() == entry.state.upper(),
            f"resumed {int(resumed)}, current {after.current}, expected {entry.state}, "
            f"post {entry.waypoint}, distance_cm {post_distance:.1f}, "
            f"snapshots_after_idle {sum(t > idle_at for t, _ in samples)}, "
            f"elapsed_s {time.monotonic() - when_sent:.2f}",
        )
        records = (
            [json.loads(raw) for raw in sightings.memory.seen(sightings.branch, npc)]
            if sightings.branch
            else []
        )
        book = sightings.desk.places
        place = book.of(event.place) if book is not None and event.place else None
        sounds = [
            s
            for s in sightings.mind(npc).heard
            if s.time == event.time and (s.place_id == place.id if place else event.place in s.place)
        ]
        saved = [
            r
            for r in records
            if r.get("observation") == "heard"
            and r.get("time") == event.time
            and any(r == s.record() for s in sounds)
        ]
        words = sightings.situation(npc) or ""
        clock = f"{(event.time or 0) % 1440 // 60:02d}:{(event.time or 0) % 60:02d}"
        check(
            "memory",
            bool(sounds)
            and bool(saved)
            and clock in words
            and any(f"слышал грохот {s.place}" in words for s in sounds),
            f"heard {len(sounds)}, saved {len(saved)}, game_minute {event.time}, clock {clock}, "
            f"places {[s.place for s in sounds]}",
        )
    finally:
        if sampling is not None:
            sampling.cancel()
            await asyncio.gather(sampling, return_exceptions=True)
        link.send = send  # type: ignore[method-assign]
        link.listeners.remove(heard)
        brain.decider = decider
        if runner is not None:
            runner.cancel()
            await asyncio.gather(runner, return_exceptions=True)
            link.listeners.remove(brain.on_message)
    return _logged(results)


async def day_scenario(
    link: GameLink,
    dialogue: DialogueManager,
    talk: TalkDesk,
    rumors: Rumors,
    gossip: GossipDesk,
    bodies: Bodies,
    perception: PerceptionDesk,
    day: DayDesk,
    planner: NpcPlanner | None = None,
    log_dir: str | None = None,
) -> dict[str, str]:
    """Everyday life from concerns (docs/npc-tools.md, wave 2 step 6), on the game. Lobart catches the hero
    lying; Vino, on his field, hears it from him far from the hero. With the hero near Malet, Vino's menu of
    his next hour has a walk to Malet to tell the news (about an hour); a declared decision selects it
    and Vino walks there: near the hero the two say it in a scene of
    step 2 (the fact how: talked) and the walk leaves a `day` fact the hero saw. From a save before the walk,
    with the hero by Vino's field, the same walk ends 72 m from the hero: Malet learns it as a fact (how:
    visited), without the model or words. Across the run: no sleep or dead option in any menu, and each
    routine asked a few times only. Without the server's planner (DECIDER_URL) the scenario runs its own.
    The scene's lines are given for the voice and queue checks; scripts/e2e.sh starts a TTS service."""
    from ..brain import GameApplier
    from ..brain.planner import PlannerConfig

    off = model_off(dialogue)
    log.info("given: дано: Вино выбирает пойти рассказать Малету (do=chat)")
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    await link.wait_world()
    if link.session is None:
        await wait_message(link, Session, timeout=10)
    await asyncio.sleep(1)
    facts = rumors.chronicle.facts
    open_pairs: set[tuple[str, str]] = set()
    starts: dict[str, tuple[float, bool]] = {}  # line -> when it started, voiced
    sent: list[tuple[float, Any]] = []  # the says and npc_act's as the server sent them
    asks: Counter[str] = Counter()  # the routines asked of the game, per npc
    idles: list[NpcIdle] = []
    loaded_with: list[int] = []  # how many routines the planner knew at each Loading
    recorder = DayRecorder(pin={VINO: {"do": "chat"}})

    # Reuse the server's planner if present, with the declared choices and these day settings.
    own = planner is None
    if planner is None:
        planner = NpcPlanner(
            recorder,
            GameApplier(link),
            config=PlannerConfig(
                apply_day=True, plan_period_s=5.0, far_period_s=(5.0, 8.0), ask_attitude=False
            ),
            routine_of=link.routine,
            habits_of=habits_of,
            view_of=perception.english,
            trace=TraceWriter(Path(log_dir, "traces")) if log_dir else None,
        )
        day.follow(planner, gossip)  # the server's planner follows it already
        link.listeners.append(planner.on_message)
    brain = planner
    decider, cfg, trace = brain.decider, brain.cfg, brain.trace
    if not own:
        brain.decider = recorder
        brain.cfg = replace(
            cfg, apply_day=True, plan_period_s=5.0, far_period_s=(5.0, 8.0), ask_attitude=False
        )
    tap = _TraceTap(trace)
    brain.trace = tap  # type: ignore[assignment]
    runner = asyncio.create_task(brain.run()) if own else None

    def rumor() -> Rumor | None:
        return next((r for r in rumors.rumors.values() if r.quest == RUEBEN), None)

    def told(to: str) -> list[Fact]:
        return [f for f in facts("rumor") if f.get("quest") == RUEBEN and f.get("to") == to]

    def walked() -> list[Fact]:
        return [f for f in facts("day") if f.get("npc") == VINO and f.get("to") == MALETH]

    def metres(npc: str) -> float:
        return talk.people[npc].distance / 100 if npc in talk.people else float("inf")

    def heard(m: GameMessage) -> None:
        match m:
            case LineStarted():
                starts[m.line] = (time.monotonic(), m.voiced)
            case NpcIdle():
                idles.append(m)
            case Loading():
                loaded_with.append(len(brain.routines))  # the planner's listener came first

    send = link.send

    async def sending(cmd: Any) -> Any:
        if isinstance(cmd, RoutineCmd):
            asks[cmd.npc.upper()] += 1
        elif isinstance(cmd, (SayCmd, NpcActCmd)):
            sent.append((time.monotonic(), cmd))
        return await send(cmd)

    async def walk(tag: str = "") -> tuple[float, NpcActCmd | None]:
        """Vino is asked of his next hour: the menu has the walk to Malet, it is chosen and he walks there.
        When it was chosen and his act."""
        # from here on the lot lets Vino tell Malet: opened only now, so no menu before the counts below has
        # the walk (a check of his as the hero's goto brings him back would send him off uncounted)
        open_pairs.add((VINO, MALETH))
        rec = brain.npcs.get(VINO)
        if rec is not None:
            rec.next_check = brain.clock()  # looked at now, not at his next background turn
        since, rows_since = len(recorder.requests), len(tap.rows)

        def asked(chat: bool = True) -> dict[str, Any] | None:
            """His do question since then: the first with the walk, or (not `chat`) the latest."""
            mine = [q["do"] for n, q in recorder.requests[since:] if n == VINO and "do" in q]
            return next((q for q in mine if "chat" in q["criteria"]), None) if chat else (mine or [None])[-1]

        await _until(lambda: asked() is not None, 60)
        question = asked() or asked(chat=False)
        options = dict(question["criteria"]) if question is not None else {}
        place = day.book.of(MALETH_SPOT) if day.book is not None else None
        where = place.where_en if place is not None else "?"
        text = options.get("chat", "")
        menu = day.menus.get(VINO)
        opt = menu.do.get("chat") if menu is not None else None
        args = {"state": "ZS_STAND_WP", "waypoint": MALETH_SPOT, "minutes": 61}
        got = {**opt.args, "waypoint": str(opt.args.get("waypoint")).upper()} if opt and opt.args else {}
        check(
            f"menu{tag}",
            set(options) == {"keep_routine", "chat"}
            and "Малет" in text
            and where in text
            and "(about an hour)" in text
            and opt is not None
            and opt.trip
            and opt.partner == MALETH
            and got == args,
            f"{list(options)}; chat: {text!r}; {got or 'no option'}",
        )
        def chosen() -> dict[str, Any] | None:
            return next(
                (
                    r
                    for r in tap.rows[rows_since:]
                    if r.get("npc") == VINO
                    and r.get("question") == "do"
                    and r.get("choice") == "chat"
                    and r.get("why") == "day desk"
                ),
                None,
            )

        await _until(lambda: chosen() is not None, 30)
        chose_at = time.monotonic()
        await _until(lambda: any(t >= chose_at - 1 and _is_walk(c) for t, c in sent), 10)
        act = next((c for t, c in sent if t >= chose_at - 1 and _is_walk(c)), None)
        return chose_at, act

    def arrived(act: NpcActCmd | None) -> NpcIdle | None:
        return next((m for m in idles if act is not None and m.act == act.id), None)

    link.listeners.append(heard)
    link.send = sending  # type: ignore[method-assign]
    rumors.gate = lambda r, a, b: (a, b) in open_pairs  # every pair not opened here is held closed
    try:
        # 0. Vino and Malet know each other only from the stable's beds: their routines never bring them
        # together by day
        if not await _until(lambda: rumors.social is not None, 30):
            return _logged({"social": "failed (no people)"})
        social = rumors.social
        assert social is not None
        tie = social.ties.get(VINO, {}).get(MALETH)
        about = next((d for d in social.describe(VINO) if f"({MALETH})" in d), "no tie")
        night = tie is not None and bool(tie.meets) and all(e <= 8 * 60 or s >= 22 * 60 for s, e in tie.meets)
        check("ties", night, about)
        await link.debug("time", hour=9, minute=0)
        for npc in (MALETH, VINO, HILDA):  # their important dialogs would stop the hero (Malet's at once)
            try:
                await link.debug("skip_important", near=npc)
            except GameError as err:
                log.info("skip_important %s: %s", npc, err)
        await _until(lambda: rumors.clock is not None and rumors.clock % DAY >= 9 * 60, 15)
        t = (rumors.clock or 0) // DAY * DAY + 9 * 60 + 30
        soon = list(tie.occasions(VINO, MALETH, t, t + 60)) if tie is not None else []
        check("no_meeting_today", tie is not None and not soon, f"at {soon}" if soon else "")
        await link.debug("player_goto", waypoint=DAY_SCENE_HERO)

        def doing(npc: str) -> str:
            return talk.people[npc].state if npc in talk.people else "not around"

        ok = await _until(lambda: (doing(VINO), doing(MALETH)) == ("ZS_RAKE_FP", "ZS_STAND_GUARDING"), 40)
        check("routines", ok, f"Vino {doing(VINO)}, Malet {doing(MALETH)}")

        # 1. Lobart catches the hero lying; Vino, on his field 44 m off, does not hear it
        ok, _ = await _caught_by(link, dialogue, talk, LOBART, facts)
        if not ok:
            return _logged({**results, "setup": "failed (turnip lie did not produce its consequences)"})
        around = [str(p.get("npc", "")).upper() for p in await link.story_nearby(LOBART, RUMOR_CM)]
        r = rumor()
        check(
            "vino_away",
            VINO not in around and r is not None and VINO not in r.known,
            f"within {RUMOR_CM // 100} m of Lobart: {VINO in around}; knows: {sorted(r.known) if r else '-'}",
        )
        scene_given = "дано: реплики сцены Вино рассказывает Малету"
        labels = labels_of(social.names[VINO], social.names[MALETH])
        off.given(
            scene_given,
            who=GOSSIP_KEY,
            calls=[
                ToolCall(
                    "lines",
                    {
                        "lines": [
                            {"speaker": labels[who], "text": text, "mood": mood}
                            for who, text, mood in DAY_LINES
                        ],
                        "quiet": False,
                        "reason": "Вино пришёл с новостью",
                    },
                )
            ],
        )

        # 2. Vino hears it from Lobart far from the hero: only the fact
        talk.end()
        await link.debug("player_goto", to=VINO, distance=8000)
        far = await _until(lambda: VINO not in talk.people, 10)
        check("hero_far", far, f"Vino {metres(VINO):.0f} m")
        lines_calls, scenes = off.used.count(scene_given), gossip.scenes
        passed = False
        for _ in range(3):
            # opened only within this synchronous call: no tick sees the pair open
            open_pairs.add((LOBART, VINO))
            passed = r is not None and rumors.offer(r, LOBART, VINO)
            open_pairs.discard((LOBART, VINO))
            if passed:
                break
            await asyncio.sleep(1.5)  # it waits (a gap not known yet): asked again
        got = told(VINO)
        check(
            "vino_heard",
            passed
            and len(got) == 1
            and got[0].get("from") == LOBART
            and int(got[0].get("hops", 0)) == 1
            and got[0].get("how") == "met"
            and gossip.scenes == scenes
            and off.used.count(scene_given) == lines_calls,
            _passes_text(got),
        )
        if not passed:
            return _logged(results)

        # 3. a save to walk it twice; the lot lets Vino tell Malet once he is asked (walk)
        saved = await _save(link, DAY_SLOT)
        check("saved", saved is not None, f"{saved.branch}@{saved.seq}" if saved is not None else "refused")
        if saved is None:
            return _logged(results)

        # 4. the hero by Malet: Vino is asked of his next hour and walks over; near the hero they say it
        await link.debug("player_goto", waypoint=DAY_SCENE_HERO)
        await asyncio.sleep(1.5)  # the roster comes twice a second: not the one from before he moved
        ok = await _until(lambda: metres(MALETH) <= WORDS_CM / 100 and 30 <= metres(VINO) <= 60, 10)
        check("hero_at_malet", ok, f"Malet {metres(MALETH):.1f} m, Vino {metres(VINO):.1f} m")
        first = gossip.scenes
        chose_at, act = await walk()
        ok = act is not None and await _until(lambda: arrived(act) is not None, 150)
        idle = arrived(act)
        near = [str(p.get("npc", "")).upper() for p in await link.story_nearby(MALETH, 600)]
        check(
            "walks",
            act is not None
            and act.args.get("arrive") is True
            and ok
            and idle is not None
            and idle.reason == "arrived"
            and VINO in near,
            f"{act.args if act else 'no act'}; {idle.reason if idle else 'no end'} after "
            f"{time.monotonic() - chose_at:.0f} s; within 6 m of Malet: {VINO in near}",
        )
        partner = talk.partner
        check("no_partner", partner is None, partner.npc if partner is not None else "")
        done = await _until(
            lambda: gossip.scenes > first and gossip.scene is None and gossip.last is not None, 60
        )
        scene = gossip.last if gossip.scenes > first else None
        lines = _scene_lines(scene)
        said = sorted((x for x in lines if x.line in starts), key=lambda x: starts[x.line][0])
        speakers = [x.npc for x in said]
        says = [
            c for t, c in sent if t >= chose_at and isinstance(c, SayCmd) and c.npc.upper() in (VINO, MALETH)
        ]
        to_other = bool(says) and all(
            (c.to or "").upper() == (MALETH if c.npc.upper() == VINO else VINO) for c in says
        )
        voiced = [x for x in said if starts[x.line][1]]
        check(
            "scene",
            done
            and scene is not None
            and scene.state == "done"
            and bool(lines)
            and len(said) == len(lines)
            and all(n == (VINO, MALETH)[i % 2] for i, n in enumerate(speakers))
            and to_other
            and len(voiced) == len(said),
            f"{scene.state + ' ' + scene.why if scene else 'no scene'}; {len(said)} of {len(lines)} started: "
            f"{' -> '.join(social.names.get(n, n) for n in speakers)}; each to the other: {to_other}; "
            f"voiced {len(voiced)}",
        )
        got = told(MALETH)
        check(
            "one_fact_talked",
            len(got) == 1
            and got[0].get("from") == VINO
            and int(got[0].get("hops", 0)) == 2
            and got[0].get("how") == "talked",
            _passes_text(got),
        )
        await _until(lambda: bool(walked()), 15)
        mine = walked()
        fact = mine[-1] if mine else {}
        check(
            "day_fact",
            len(mine) == 1
            and fact.get("what") == "chat"
            and fact.get("outcome") == "told"
            and fact.get("where") == "FARM1_PATH_CITY"
            and "PLAYER" in (fact.get("seen_by") or []),
            fact or "none",
        )

        # 5. from the save, the hero by Vino's field: the same walk ends far from him, a fact only
        asked_before = asks[VINO]
        loading = asyncio.ensure_future(wait_message(link, Session, lambda m: m.reason == "load", timeout=60))
        await link.debug("load", DAY_SLOT)
        await loading
        await link.wait_world()
        if not await _until(lambda: rumors.social is not None and rumors.clock is not None, 30):
            return _logged({**results, "reloaded": "failed (no rumors after the load)"})
        await link.debug("player_goto", waypoint=DAY_FIELD_HERO)
        await asyncio.sleep(1.5)
        ok = await _until(lambda: metres(VINO) <= 20 and MALETH not in talk.people, 10)
        check("hero_at_field", ok, f"Vino {metres(VINO):.1f} m, Malet {metres(MALETH):.0f} m")
        again = await _until(lambda: asks[VINO] > asked_before, 30)
        check(
            "reloaded",
            bool(loaded_with) and not any(loaded_with) and again,
            f"routines known at the load: {loaded_with}; Vino's asked again: {again}",
        )
        lines_calls, scenes = off.used.count(scene_given), gossip.scenes
        chose_at, act = await walk("_again")
        ok = act is not None and await _until(lambda: arrived(act) is not None, 150)
        idle = arrived(act)
        check(
            "walks_again",
            ok and idle is not None and idle.reason == "arrived",
            f"{idle.reason if idle else 'no end'} after {time.monotonic() - chose_at:.0f} s",
        )
        await _until(lambda: bool(told(MALETH)) and bool(walked()), 30)
        got = told(MALETH)
        await asyncio.sleep(3)  # nothing is said after it
        says = [
            c.text
            for t, c in sent
            if t >= chose_at and isinstance(c, SayCmd) and c.npc.upper() in (VINO, MALETH)
        ]
        check(
            "far_fact",
            len(got) == 1
            and got[0].get("from") == VINO
            and int(got[0].get("hops", 0)) == 2
            and got[0].get("how") == "visited"
            and gossip.scenes == scenes
            and off.used.count(scene_given) == lines_calls
            and not says,
            f"{_passes_text(got)}; {gossip.scenes - scenes} scene(s), "
            f"{off.used.count(scene_given) - lines_calls} model call(s), lines {says}",
        )
        mine = walked()
        fact = mine[-1] if mine else {}
        check(
            "day_fact_far",
            len(mine) == 1 and fact.get("outcome") == "told" and "PLAYER" not in (fact.get("seen_by") or []),
            fact or "none",
        )

        # 6. no sleep and no dead option in any menu of the two, all run long (both have a bed entry)
        bad: list[str] = []
        for npc, questions in recorder.requests:
            if npc not in (VINO, MALETH):
                continue
            for name, q in questions.items():
                criteria = q.get("criteria", {})
                keys = criteria if isinstance(criteria, dict) else {}
                bad += [f"{npc} {name}: {k}" for k in keys if k in DEAD_OPTIONS]
                if name in ("plan", "do"):
                    bad += [f"{npc} {name} {k}: {v!r}" for k, v in keys.items() if "sleep" in str(v).lower()]
        for row in tap.rows:
            if row.get("npc") not in (VINO, MALETH) or row.get("question") not in ("plan", "do"):
                continue
            for key, what in (row.get("args") or {}).items():
                args = (what or {}).get("args") or {}
                state, wp = str(args.get("state", "")).upper(), str(args.get("waypoint", "")).upper()
                if state.startswith(("ZS_GOTOBED", "ZS_SLEEP")) or wp == "NW_FARM1_INSTABLE_BED":
                    bad.append(f"{row['npc']} {row['question']} {key}: {state}@{wp}")
        asked = sum(1 for npc, _ in recorder.requests if npc in (VINO, MALETH))
        check("no_sleep_or_dead", not bad, "; ".join(bad[:5]) if bad else f"{asked} request(s) of the two")

        # 7. each routine asked only a few times: the first time, after its TTL, after the load
        many = {npc: n for npc, n in asks.items() if n > 3}
        check(
            "routine_asks",
            not many,
            f"too often: {many}" if many else f"Vino {asks[VINO]}, Malet {asks[MALETH]}, {len(asks)} npc's",
        )
    finally:
        rumors.gate = None
        link.send = send  # type: ignore[method-assign]
        link.listeners.remove(heard)
        brain.decider, brain.cfg, brain.trace = decider, cfg, trace
        if runner is not None:
            runner.cancel()
            link.listeners.remove(brain.on_message)
    return _logged(results)


def _is_walk(cmd: Any) -> bool:
    """Vino's start_activity (the day desk's walk)."""
    return isinstance(cmd, NpcActCmd) and cmd.npc.upper() == VINO and cmd.action == "start_activity"


ERRAND_LINES = (
    (0, "Андре, слушай. Этот бродяга мне про репу наврал — сказал, что в амбаре, а там пусто.", "angry"),
    (1, "Вот как? Запомню. В ополчении такие не нужны.", "neutral"),
)
ERRAND_FAR_WP = "NW_XARDAS_START"  # the tower: far from the farm and from the city


async def errand_scenario(
    link: GameLink,
    dialogue: DialogueManager,
    talk: TalkDesk,
    errands: Errands,
    rumors: Rumors,
    gossip: GossipDesk | None,
    *,
    variant: Literal["near", "fork", "far"] = "near",
    npc: str = LOBART,
    to: str = ANDRE,
    live: LiveCase | None = None,
) -> dict[str, str]:
    """Given Lobart exposes the turnip lie and decides to tell Andre tomorrow at 7:00.
    Every pair's rumor stays closed meanwhile, so only the errand passes it. Checks:
    - near (SCENARIO=errand): the hero waits where Andre is by then; Lobart sets off at 7:00 from the farm,
      walks there for real (`there`), finds Andre and tells him in front of the hero: a rumor fact with
      `how: told` and `goal_done` (`told`), a scene of two voices (`scene`);
    - fork (SCENARIO=errand_fork): Andre is taken away before Lobart comes, another of the militia stands
      there: he hears it instead (`goal_done.instead_of`);
    - far (SCENARIO=errand_far): the hero is at the tower; at 7:00 nothing walks, and when Lobart would be
      there the fact comes (`far`), his body still at his routine, no model called.
    The near and fork scenes use given lines to exercise the scene queue and voices."""

    off = model_off(dialogue) if live is None else None
    await link.wait_world()
    if link.session is None:
        await wait_message(link, Session, timeout=10)
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    facts = errands.chronicle.facts
    if not await _until(lambda: rumors.social is not None, 30):
        return _logged({"social": "failed (no people)"})
    rumors.gate = lambda r, a, b: False  # the schedule passes nothing meanwhile: only the errand does
    starts: dict[str, tuple[float, bool]] = {}

    def heard(message: GameMessage) -> None:
        if isinstance(message, LineStarted):
            starts[message.line] = (time.monotonic(), message.voiced)

    link.listeners.append(heard)
    try:
        await link.debug("time", hour=9, minute=0)
        if live is not None:
            # The discovery is the initial world state; only the subsequent decision is live.
            await link.debug("skip_important", near=npc)
            await link.story_effect(npc, [{"do": "set", "var": RUEBEN, "value": 1}])
            await link.debug("player_goto", to=npc, distance=250)
            if not await _until(
                lambda: npc in talk.people and talk.people[npc].distance <= 600, 20,
            ):
                return _logged({"roster": f"failed ({npc} is not around the hero)"})
            name = talk.people[npc].name
            snap = await link.snapshot(npc, fresh=True)
            caught: Fact = {
                "fact": "caught", "npc": npc, "name": name, "quest": RUEBEN,
                "title": "Сбор репы", "how": "Герой сказал, что репа в амбаре, но там её нет.",
                "day": snap.world.day,
            }
            errands.chronicle.record(caught)
            prior_goals = {f.get("id") for f in facts("goal")}
            text = f"{name}, ты кому-нибудь расскажешь?"
            deadline = time.monotonic() + 90
            await link.debug("type", text=text)
            answered = await live.wait_turn(
                dialogue, npc, text, timeout=max(0, deadline - time.monotonic()),
            )
            calls = live.calls(npc, "plan")
            accepted = any(call.get("outcome") == "accepted" for call in calls)

            def planned() -> list[Fact]:
                return [f for f in facts("goal") if f.get("npc") == npc
                        and f.get("id") not in prior_goals]

            # The handler persists normalized destination ids and the resolved game time.
            if accepted:
                await _until(lambda: bool(planned()), max(0, deadline - time.monotonic()))
            live.finish("observed", evidence={
                "caught": caught, "answered": answered, "plan": calls,
                "goals": planned(),
                "decision": "plan" if planned() else "no_plan",
            })
            return _logged(results)
        ok, _ = await _caught_by(link, dialogue, talk, npc, facts)
        if not ok:
            return _logged({"setup": "failed (turnip lie did not produce its consequences)"})
        snap = await link.snapshot(npc, fresh=True)
        tomorrow = snap.world.day + 2  # as the npc sees days: from 1
        plan = {"do": "tell", "who": "Андре", "item": "", "count": 0, "day": tomorrow, "hour": 7, "minute": 0}
        off.given(
            "дано: Лобарт решил рассказать Андре завтра в 7:00",
            who=npc, when="расскажешь", calls=[ToolCall("plan", plan)],
        )
        off.silent(
            "дано: Лобарт молчит (поручение принято)", who=npc, when="расскажешь",
        )
        name = talk.people[npc].name if npc in talk.people else "Лобарт"
        await link.debug("type", text=f"{name}, ты кому-нибудь расскажешь?")

        def goal() -> Any:
            return next((g for g in errands.goals.values() if g.npc == npc and g.do == "tell"), None)

        await _until(lambda: goal() is not None, 60)
        g = goal()
        if g is None:
            return _logged({"setup": "failed (no errand to follow)"})
        await _until(lambda: not dialogue.speaking(npc), 10)
        to = g.to
        talk.end()

        # the night goes by (a leap, as a sleep: everyone at their routine's spots); the hero goes where
        # Andre will be, or far off
        before = g.start - 2
        if errands.now is None or before > errands.now:
            await link.debug("time", hour=before % GAME_DAY // 60, minute=before % 60)
        await asyncio.sleep(2)
        here = await link.snapshot(npc, fresh=True)
        walk = math.ceil(math.dist(here.npc.pos, errands._spot(to, g.start).pos) / 100 * 1.5 / 3.0)  # type: ignore[union-attr]
        there = errands._spot(to, g.start + walk)
        if there is None:
            return _logged({**results, "spot": f"failed (no spot of {to})"})
        if variant == "far":
            await link.debug("player_goto", waypoint=ERRAND_FAR_WP)
        else:
            await link.debug("player_goto", waypoint=there.waypoint)
            await link.debug("skip_important", near=to)  # Andre's greeting would stop the hero
        asked = len(off.used)
        if variant == "near":
            labels = labels_of(name, rumors.social.names[to])
            off.given(
                "дано: реплики сцены рассказа Лобарта Андре",
                who=GOSSIP_KEY,
                calls=[ToolCall("lines", {
                    "lines": [
                        {"speaker": labels[who], "text": text, "mood": mood}
                        for who, text, mood in ERRAND_LINES
                    ],
                    "quiet": False,
                    "reason": "Лобарт передаёт новость по поручению",
                })],
            )
        scenes = gossip.scenes if gossip is not None else 0  # before it sets off: the scene may come fast
        set_off = await _until(lambda: g.state not in ("agreed", "leaving"), 60)
        if variant == "far":
            check("away", set_off and g.state == "away", f"{g.state}, there at {clock(g.arrive)}")
            if g.state != "away":
                return _logged(results)
            then = g.arrive + 2
            await link.debug("time", hour=then % GAME_DAY // 60, minute=then % 60)
            done = await _until(lambda: g.state in ("done", "failed"), 30)
            fact = next((f for f in facts("rumor") if f.get("from") == npc and f.get("to") == to), None)
            snap = await link.snapshot(npc, fresh=True)
            home = rumors.social.spot(npc, then) if rumors.social is not None else None
            far = math.dist(snap.npc.pos, home.pos) / 100 if home is not None and home.pos else -1
            check("far", done and fact is not None and fact.get("how") == "told", f"{g.state}, {fact}")
            check("body", 0 <= far < 20, f"{far:.0f} m from its routine's spot")
            check("no_model", len(off.used) == asked, f"{len(off.used) - asked} request(s)")
            return _logged(results)
        check("set_off", set_off and g.state in ("going", "there"), f"{g.state}, to {there.waypoint}")
        if g.state not in ("going", "there"):
            return _logged(results)
        if variant == "fork":
            # Andre is taken away before Lobart comes (held there), and one of the militia stands there
            social = rumors.social
            guild = social.guilds.get(to) if social is not None else None
            others = sorted(
                (n for n, spot in (social.spots() if social is not None else ())
                 if n not in (npc, to) and social is not None and social.guilds.get(n) == guild
                 and spot.pos is not None and social.spot(n, g.start + walk) is spot),
                key=lambda n: math.dist(social.spot(n, g.start + walk).pos, there.pos),  # type: ignore[union-attr, arg-type]
            )  # fmt: skip
            if not others:
                return _logged({**results, "fork": "failed (no other of the guild)"})
            stand_in = others[0]
            labels = labels_of(name, social.names[stand_in])
            off.given(
                "дано: реплики сцены рассказа Лобарта другому ополченцу",
                who=GOSSIP_KEY,
                calls=[ToolCall("lines", {
                    "lines": [
                        {"speaker": labels[who], "text": text, "mood": mood}
                        for who, text, mood in ERRAND_LINES
                    ],
                    "quiet": False,
                    "reason": "Лобарт передаёт новость по поручению",
                })],
            )
            await link.debug("npc_goto", npc=to, waypoint=ERRAND_FAR_WP)
            await link.attach(to)
            await link.debug("npc_goto", npc=stand_in, waypoint=there.waypoint)
        arrived = await _until(lambda: g.state in ("there", "done", "failed"), 1200)
        snap = await link.snapshot(npc, fresh=True)
        far = math.dist(snap.npc.pos, there.pos) / 100
        check("there", arrived and far < 15, f"{g.state}, {far:.0f} m from {there.waypoint}")
        done = await _until(lambda: g.state in ("done", "failed"), 120)
        got = next((f for f in facts("goal_done") if f.get("goal") == g.id), None)
        hearer = got.get("to") if got else None
        fact = next((f for f in facts("rumor") if f.get("from") == npc and f.get("to") == hearer), None)
        if variant == "fork":
            ok = done and got is not None and got.get("instead_of") == to and hearer != to
        else:
            ok = done and got is not None and hearer == to
        check("told", ok and fact is not None and fact.get("how") == "told", f"{g.state}, {got}, {fact}")
        played = await _until(
            lambda: gossip is not None and gossip.scenes > scenes
            and gossip.scene is None and gossip.last is not None,
            60,
        )
        scene = gossip.last if gossip is not None and gossip.scenes > scenes else None
        lines = _scene_lines(scene)
        said = sorted((line for line in lines if line.line in starts), key=lambda line: starts[line.line][0])
        check(
            "scene",
            played and scene is not None and scene.state == "done" and not scene.why
            and bool(lines) and len(said) == len(lines)
            and all(starts[line.line][1] for line in said)
            and all(line.npc == (npc, hearer)[i % 2] for i, line in enumerate(said))
            and all(line.to == (hearer if line.npc == npc else npc) for line in said),
            f"{scene.state + ' ' + scene.why if scene else 'no scene'}, "
            f"{len(said)} of {len(lines)} lines started, voiced {sum(starts[line.line][1] for line in said)}",
        )
        if variant == "fork":
            await link.detach(to)
        return _logged(results)
    finally:
        link.listeners.remove(heard)
        rumors.gate = None


async def errand_give_scenario(
    link: GameLink, dialogue: DialogueManager, talk: TalkDesk, errands: Errands, *, npc: str = HILDA
) -> dict[str, str]:
    """A thing taken to someone (docs/npc-tools.md, wave 2 step 5: `plan` with give): the hero asks Hilda to
    take Lobart 25 gold; she walks out to him and hands it over: her gold −25, his +25, a fact `gave`
    (`gave`). Then Canthar's letter (a mission item, v39): the errand is turned down with the reason
    (`letter`). Hilda's two plan calls are givens, handled by the real errand checks."""

    off = model_off(dialogue)
    if dialogue.trace is None:
        raise GameError("scenario errand_give requires a trace directory")
    trace = dialogue.trace
    await link.wait_world()
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    gold, letter = "ITMI_GOLD", "ITWR_CANTHARS_KOMPROBRIEF_MIS"
    await link.debug("time", hour=9, minute=0)
    await link.debug("skip_important", near=npc)
    await link.debug("skip_important", near=LOBART)
    await link.story_effect(
        npc, [{"do": "create", "item": gold, "count": 25}, {"do": "create", "item": letter}]
    )
    await link.debug("player_goto", to=npc, distance=250)
    if not await _until(lambda: npc in talk.people and talk.people[npc].distance <= 600, 20):
        return _logged({"roster": f"failed ({npc} is not around the hero)"})

    def count(snap: Snapshot, item: str) -> int:
        return next((i.count for i in snap.npc.inventory if i.item.upper() == item), 0)

    hers, his = await link.snapshot(npc, fresh=True), await link.snapshot(LOBART, fresh=True)
    marked = next((i.mission for i in hers.npc.inventory if i.item.upper() == letter), None)
    check("marked", marked is True, f"mission: {marked}")
    name = talk.people[npc].name
    give = {"do": "give", "who": "Лобарт", "item": gold, "count": 25, "day": 0, "hour": 0, "minute": 0}
    off.given(
        "дано: Хильда несёт Лобарту 25 золотых",
        who=npc, when="золот", calls=[ToolCall("plan", give)],
    )
    off.silent("дано: Хильда молчит (поручение принято)", who=npc, when="золот")
    off.given(
        "дано: Хильда пробует отнести письмо Кантара",
        who=npc, when="письмо", calls=[ToolCall("plan", {**give, "item": letter, "count": 1})],
    )
    off.silent("дано: Хильда молчит (письмо — вещь задания)", who=npc, when="письмо")
    await link.debug("type", text=f"{name}, отнеси Лобарту 25 золотых.")

    def goal() -> Any:
        return next((g for g in errands.goals.values() if g.npc == npc and g.item == gold), None)

    await _until(lambda: goal() is not None, 60)
    g = goal()
    if g is None:
        return _logged({**results, "setup": "failed (no delivery to follow)"})
    await _until(lambda: not dialogue.speaking(npc), 10)
    talk.end()
    done = await _until(lambda: g.state in ("done", "failed"), 180)
    gave = next((f for f in errands.chronicle.facts("gave") if f.get("goal") == g.id), None)
    hers2, his2 = await link.snapshot(npc, fresh=True), await link.snapshot(LOBART, fresh=True)
    moved = (count(hers, gold) - count(hers2, gold), count(his2, gold) - count(his, gold))
    check("gave", done and gave is not None and moved == (25, 25), f"{g.state}, {moved}, {gave}")
    await asyncio.sleep(3)
    await link.debug("player_goto", to=npc, distance=250)
    await _until(lambda: npc in talk.people and talk.people[npc].distance <= 600, 20)
    text = f"{name}, а еще отнеси Лобарту письмо."
    await link.debug("type", text=text)

    def rejected() -> list[dict[str, Any]]:
        if not trace.path().exists():
            return []
        return [
            call
            for row in read_trace(trace.path())
            if row.get("type") == "turn" and row.get("npc") == npc
            and row.get("trigger", {}).get("text") == text
            for step in row.get("steps", []) for call in step.get("calls", [])
            if call.get("name") == "plan" and call.get("outcome") == "rejected"
            and "вещь для задания" in call.get("result", "")
        ]

    refused = await _until(lambda: bool(rejected()), 60)
    check("letter", refused and not any(x.item == letter for x in errands.goals.values()), rejected())
    return _logged(results)
