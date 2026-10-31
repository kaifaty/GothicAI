from __future__ import annotations

import asyncio
import json
import logging
import math
import time
from collections import Counter
from collections.abc import AsyncIterator, Callable
from dataclasses import replace
from pathlib import Path
from typing import Any, Literal, cast

from ..bodies import Bodies
from ..brain import Decider, NpcPlanner, Outcome
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
from ..gossip import WORDS_CM, GossipDesk
from ..guide import Guide
from ..initiative import ARRIVE_CM, CHECKINFO, InitiativeDesk
from ..llm import LLMEvent, Message, ToolCall
from ..llm.fake import FakeBackend, FakeStep
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
from .common import (
    ANDRE,
    HILDA,
    LOBART,
    RUEBEN,
    VINO,
    ClaimScript,
    FixedDecider,
    GossipScript,
    _ByNpc,
    _caught_by,
    _logged,
    _OnWords,
    _passes_text,
    _Recorded,
    _save,
    _scene_lines,
    _until,
    _until_async,
    attach_when_free,
    wait_message,
)

log = logging.getLogger("gothic_ai.scenarios")


async def lead_scenario(
    link: GameLink,
    dialogue: DialogueManager,
    talk: TalkDesk,
    deals: Deals,
    guide: Guide,
    npc: str = "BAU_954_MALETH",
    to: str = "FARM1_MILL",
) -> dict[str, str]:
    """The npc leads the hero (docs/npc-tools.md, wave 2 step 4: `lead`): «Малет, отведи меня к мельнице».
    Checks:
    - offered: a deal with the way there in its window («Проводит: мельница фермы Лобарта»);
    - sets_off: the hero says yes (Y), Malet sets off for the mill;
    - waits: the hero is put back where Malet started: it stops and waits for him; he comes up, it goes on;
    - there: the hero after it, it gets to the mill and says so; a fact `led` (arrived).
    How fast it walks is logged (`pace`). The model is scripted with LLM=fake; LLM=chatgpt answers itself."""
    await link.wait_world()
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    entries: list[dict[str, Any]] = []
    sink = dialogue.journal.sink

    def keep(entry: dict[str, Any]) -> None:
        entries.append(entry)
        if sink is not None:
            sink(entry)

    dialogue.journal.sink = keep
    if dialogue.llm.name == "fake":
        offer = {"npc_gives": [], "hero_gives": [], "lead": to}
        dialogue.llm = _OnWords(
            [
                ("отведи меня", FakeStep("Пойдём, покажу.", [ToolCall("propose_deal", offer, "lead1")])),
                ("ты ведёшь его", FakeStep("Иди за мной.")),
                ("Герой отстал", FakeStep("Эй, не отставай!")),
                ("Вы пришли", FakeStep("Вот и мельница.")),
            ],
            FakeStep("Да."),
        )
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

    talk.hear("Малет, отведи меня к мельнице.", npc, {"via": "scenario"})
    shown = await _until(lambda: npc in deals.open, 60)
    deal = deals.open.get(npc)
    check("offered", shown and deal is not None and deal.lead is not None and deal.lead.place.id == to,
          deal.terms() if deal is not None else "no deal")  # fmt: skip
    if deal is None:
        return _logged(results)
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
    called = any(e.get("kind") == "npc" and e.get("npc") == npc for e in entries[-20:])
    await link.debug("teleport", near=npc, distance=300)
    on = await _until(lambda: npc in guide.leads and guide.leads[npc].state == "going", 20)
    check("waits", waited and on, f"waited {waited}, called {called}, went on {on}")

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
    said = [e.get("text", "") for e in entries if e.get("kind") == "npc" and e.get("npc") == npc]
    check(
        "there",
        bool(led) and led[-1].get("how") == "arrived" and math.dist(snap.npc.pos, mill.center) < 2000,
        f"{led[-1].get('how') if led else 'no fact'}, at {snap.npc.nearest_waypoint} "
        f"({there.id if there else '?'}), {math.dist(snap.npc.pos, mill.center) / 100:.0f} m from {to}",
    )
    log.info("lead: %s said %s", npc, said)
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
) -> dict[str, str]:
    """A meeting with the hero (docs/npc-tools.md, wave 2 step 4: `meet`): at 19:00 the hero agrees with
    Malet to meet at dawn by the field in front of the city gates. Checks:
    - offered: a deal with the meeting in its window;
    - agreed: Y; the meeting is the desk's;
    - there: the clock leaps past when Malet sets off, as when the hero sleeps: the engine puts everyone at
      their routine's spot, and Malet is put at the meeting at once (`start_activity.place`);
    - kept: the hero comes, a fact `meet_kept`, and Malet speaks first (`first`).
    With `missed` (SCENARIO=meet_missed) the clock goes to a little before he sets off: he walks there himself
    and stands there by the hour. Then the hero does not come: the clock past its end, his word is broken
    (`broken`, promise meet), Malet goes back to his routine (`routine`), and when the hero comes up to him
    the missed meeting is in front of him (`reproach`; live: in his words).
    The model is scripted with LLM=fake; LLM=chatgpt answers itself."""
    await link.wait_world()
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    entries: list[dict[str, Any]] = []
    sink = dialogue.journal.sink

    def keep(entry: dict[str, Any]) -> None:
        entries.append(entry)
        if sink is not None:
            sink(entry)

    def said(since: int) -> list[str]:
        return [e.get("text", "") for e in entries[since:] if e.get("kind") == "npc" and e.get("npc") == npc]

    dialogue.journal.sink = keep
    live = dialogue.llm.name != "fake"
    await link.debug("time", hour=19, minute=0)
    await link.debug("skip_important", near=npc)  # Malet's own dialog would stop the hero at once
    await link.debug("teleport", near=npc, distance=300)
    if not await _until(lambda: npc in talk.people and talk.people[npc].distance <= 600, 20):
        return _logged({"roster": f"failed ({npc} is not around the hero)"})
    snap = await link.snapshot(npc, fresh=True)
    tomorrow = snap.world.day + 2  # as the npc sees days: from 1
    if not live:
        dawn = {"where": to, "day": tomorrow, "hour": 5, "minute": 0, "hours": 2}
        offer = ToolCall("propose_deal", {"meet": dawn}, "meet1")
        dialogue.llm = _OnWords(
            [
                ("встретимся", FakeStep("Ладно, на рассвете у поля перед воротами.", [offer])),
                ("сделка состоялась", FakeStep("Договорились. Не опаздывай.")),
                ("пришёл на встречу", FakeStep("Пришёл всё-таки. Хорошо.")),
                ("привет", FakeStep("Ты где был на рассвете? Я тебя ждал.")),
            ],
            FakeStep("Да."),
        )
    model = _Recorded(dialogue.llm)
    dialogue.llm = model

    talk.hear(
        "Малет, давай встретимся завтра на рассвете у поля перед городскими воротами. Мне нужно с тобой "
        "поговорить без лишних ушей.",
        npc,
        {"via": "scenario"},
    )
    shown = await _until(lambda: npc in deals.open and deals.open[npc].meet is not None, 60)
    if not shown:  # a live Malet asks what it is about first
        talk.hear(
            "Про бандитов на дороге к городу: я знаю, где они прячутся. Здесь говорить не буду — на рассвете "
            "у поля перед воротами.",
            npc,
            {"via": "scenario"},
        )
        shown = await _until(lambda: npc in deals.open and deals.open[npc].meet is not None, 90)
    if not shown and live:  # it agrees in words and asks the hour first («В шесть часов подойдёт?»)
        talk.hear("Да, в шесть подойдёт. Договорились.", npc, {"via": "scenario"})
        shown = await _until(lambda: npc in deals.open and deals.open[npc].meet is not None, 90)
    deal = deals.open.get(npc)
    check("offered", shown, deal.terms() if deal is not None else "no deal")
    if not shown or deal is None or deal.meet is None:
        return _logged(results)
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
        t0 = len(entries)
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
        spoke = await _until(lambda: bool(said(t0)), 60)
        check("first", spoke and talk.partner is not None and talk.partner.npc == npc, said(t0) or "silent")
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
    t0, asked = len(entries), len(model.asked)
    talk.hear("Малет, привет.", npc, {"via": "scenario"})
    await _until(lambda: bool(said(t0)), 60)
    words = " ".join(said(t0))
    shown_to_it = any("не пришёл на встречу" in a for a in model.asked[asked:])
    low = words.lower()
    spoke_of = any(w in low for w in ("встреч", "ждал", "прожд", "рассвет", "договар", "не пришёл"))
    check(
        "reproach",
        shown_to_it and (not live or spoke_of),
        f"in its situation: {shown_to_it}; {words or 'silent'}",
    )
    return _logged(results)


def _game_minute(snap: Snapshot) -> int:
    return snap.world.day * GAME_DAY + snap.world.hour * 60 + snap.world.minute


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
    unseen, takes the gold out of the chest there; then asks Brago for a bow. Brago's model is scripted
    (LLM=fake); with a real one (`chests_lobart`: Lobart, his farmer's clothes in his chest) whether it goes
    to the chest is the model's own call. Checks:
    - chest: the bridge counts the cave's chest as Brago's (nobody owns it, it stands by his routine), with
      a bow in it;
    - knows: Brago's situation tells what lies in it, as he knows: the bow and the gold too;
    - fetched: told «продай лук», his model calls `chest`: he walks to the chest, opens it and takes the bow
      out; the bow is in his hands and no longer in the chest;
    - missing: having looked, he knows the gold is gone;
    - back: back from the chest, he goes on talking to the hero."""
    await link.wait_world()
    results: dict[str, str] = {}
    if chests is None:
        return _logged({"chests": "failed (run with --chests)"})

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    bow, gold = item, "ITMI_GOLD"
    asked: list[str] = []  # the situation of each of Brago's requests
    cues: list[str] = []  # ...and what each was about
    fetch = ToolCall(CHEST, {"take": [{"item": bow, "count": 1}], "put": [], "chest": None})
    # scripted (LLM=fake), or the real model (LLM=chatgpt): then what it does is its own call, to be read
    inner: Any = dialogue.llm
    if inner.name == "fake":
        say = FakeStep("В сундуке лежит. Сейчас принесу.", [fetch])
        inner = _ByNpc({name: _OnWords([("продай", say)], FakeStep("Ну."))}, FakeBackend("Хм.", delay=0))

    class Recorded:
        name = inner.name
        model = getattr(inner, "model", None)

        def __getattr__(self, attr: str) -> Any:
            return getattr(inner, attr)

        def stream(self, **kw: Any) -> AsyncIterator[LLMEvent]:
            if f"Ты — {name}" in kw["instructions"]:
                msgs = kw["messages"]
                last = msgs[-1] if msgs else None
                cues.append(last.content if isinstance(last, Message) else "")
                asked.append(
                    "\n".join(m.content for m in msgs if isinstance(m, Message) and m.role == "developer")
                )
            return cast(AsyncIterator[LLMEvent], inner.stream(**kw))

    dialogue.llm = Recorded()  # type: ignore[assignment]

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

    talk.hear(ask, npc, {"via": "scenario"})
    await _until(lambda: bool(asked), 30)
    situation = asked[0] if asked else ""
    line = next((s for s in situation.split("\n") if s.startswith(tuple(OWNED.values()))), "")
    check("knows", bow in line and gold in line, line or situation[-300:])

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
    if not note() and inner.name != "fake" and not chests._going:  # a real one names its price first
        talk.hear("Да, беру, по рукам.", npc, {"via": "scenario"})
        await _until(lambda: bool(chests._going), 30)
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
    await _until(lambda: BACK in cues, 20)
    await _until(lambda: not dialogue.speaking(npc), 30)  # what it says back from the chest, for the log
    watch.cancel()
    check("back", BACK in cues and 0 <= dist() <= 5, f"{dist():.1f} m from the hero; {cues}")
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
) -> dict[str, str]:
    """An npc comes up to the hero and speaks first (docs/npc-tools.md, wave 2 step 1), on the game. Hilda has
    her greeting waiting for the hero: the game's own Npc_CheckInfo says so and the initiative leaves her to
    the scripts. The hero owes Lobart 25 gold since yesterday (a debt written into the chronicle): a broken
    word, on Lobart's mind. The hero stands 12 m in front of him; Lobart comes up by himself, once, and speaks
    first about the debt, the hero's next words without a name go to him. The hero says he will pay: Lobart
    offers a deal that settles the debt, the hero accepts it, and nothing is on Lobart's mind any more. With
    the server's planner and its desk (DECIDER_URL) the behavioral model decides whether he comes up; without
    it a fixed stand-in does, for Lobart only. The words are scripted unless LLM=chatgpt."""
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

    # without the server's planner: a fixed stand-in, Lobart comes up and the others keep to their routines
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
    # Lobart's lines as the server sent them, with the trigger of the turn they came from
    said: list[tuple[float, str, str]] = []
    walked: list[float] = []  # when Lobart was sent to the hero (by the desk: no one else sends go_to_npc)
    send = link.send

    async def sending(cmd: Any) -> Any:
        if isinstance(cmd, NpcActCmd) and cmd.npc.upper() == lender and cmd.action == "go_to_npc":
            walked.append(time.monotonic())
        if isinstance(cmd, SayCmd) and cmd.npc.upper() == lender:
            conv = dialogue.conversations.get(lender)
            trigger = conv.trigger.content if conv is not None and conv.trigger is not None else ""
            said.append((time.monotonic(), cmd.text, trigger))
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
        if isinstance(dialogue.llm, FakeBackend):
            repay = {
                "npc_gives": [],
                "hero_gives": [],
                "forgive": [],
                "silence": [],
                "hero_later": [],
                "repay": [INITIATIVE_DEBT],
            }
            first = FakeStep("[angry] Эй, ты! Двадцать пять золотых за тобой, и срок вчера вышел.")
            dialogue.llm = ClaimScript(
                said=[
                    ("подошёл к герою", first),
                    ("Отдам", FakeStep("Давно бы так.", [ToolCall("propose_deal", repay, "repay")])),
                ],
                told=[],
                default="Вот и ладно.",
                only=name,
            )

        # 1. Hilda has her greeting for the hero (DIA_Hilda_Hallo): the game's own check says so, and the
        # initiative leaves her to the scripts; once it counts as told, nothing waits
        before, waiting = await checkinfo(HILDA), await desk.important(HILDA)
        await link.debug("skip_important", near=HILDA)
        after, left = await checkinfo(HILDA), await desk.important(HILDA)
        check(
            "important_guard",
            before is not None and bool(before) and waiting and after is not None and not after and not left,
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
            45 if own else 120,  # the live model decides when
        )
        entry, rec = talk.people.get(lender), planner.npcs.get(lender)
        where = f"{entry.distance / 100:.1f} m" if entry else "not around"
        body = guard.body(lender, entry.state if entry else "") or "may walk"
        check(
            "walked_up",
            came and len(walked) - walks == 1,
            f"{where} after {time.monotonic() - t0:.0f} s, {len(walked) - walks} walk(s), "
            f"vetoed {planner.stats.vetoed}, agenda {rec.agenda if rec else '-'!r}, body: {body}"
            f"{f', others close: {crowd}' if crowd else ''}",
        )
        if not came:
            return _logged(results)

        # 4. he speaks first, about the debt; the hero has said nothing
        def first_words() -> tuple[float, str, str] | None:
            return next((w for w in said if w[2].startswith("[Ты сам подошёл к герою")), None)

        await _until(lambda: first_words() is not None, 15)
        words = first_words()
        early = [t for t in typed if words is not None and t < words[0]]
        # the hero said nothing yet: only the desk can have made Lobart his partner, listening
        partner = talk.partner
        opened = partner is not None and partner.npc == lender and partner.posture == "listen"
        check(
            "spoke_first",
            words is not None and "25" in words[2] and not early and opened,
            f"«{words[1]}» on {words[2]!r}{f', the hero spoke {len(early)} time(s) before' if early else ''}"
            if words
            else f"silent; lines: {[w[1] for w in said][-3:]}",
        )

        # 5. the fact: who came up, about what, who saw it
        fact = next((f for f in reversed(chronicle.facts("initiative")) if f.get("npc") == lender), None)
        check(
            "fact",
            fact is not None and fact.get("occasion") == key and "PLAYER" in (fact.get("seen_by") or []),
            fact or "none",
        )

        # 6. the hero answers him without a name: it goes to Lobart, his partner now
        await _until(lambda: not dialogue.speaking(lender), 15)
        text = "Отдам, держи свои двадцать пять."
        await link.debug("type", text=text)
        await _until(lambda: talk.last is not None and talk.last[1] == text, 10)
        to = talk.last[0] if talk.last is not None and talk.last[1] == text else None
        check(
            "reply_goes_to_him",
            to is not None and to.id == lender,
            f"to {to.id} by {to.why}" if to else "not heard or to nobody",
        )

        # 7. he offers a deal that settles the debt: the window shows it
        def offered() -> bool:
            deal = deals.open.get(lender)
            return deal is not None and [d.id for d in deal.repay] == [INITIATIVE_DEBT]

        ok = await _until(offered, 30)
        owed = f"Долг: {gold_text(25)}"
        shown = await _until_async(
            link.deal_window, lambda w: w is not None and owed in (w.get("hero_gives") or []), 10
        )
        window = await link.deal_window() or {}
        check("offered_repay", ok and shown, window.get("hero_gives"))
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
    """The behavioral model of the day e2e: every request's questions are kept; the pinned npc's pinned
    questions get the pinned choice (if offered), every other question the stand-in's default (the first
    option, level 0), so no other body acts: Lobart, with the lie on his mind, would come up to the hero and
    hold Vino's scene. With the live model (DECIDER_URL) it is asked first and its own answers are kept."""

    def __init__(self, live: Decider | None, pin: dict[str, dict[str, str]]) -> None:
        self.live = live
        self.pin = {npc.upper(): questions for npc, questions in pin.items()}
        self.requests: list[tuple[str, dict[str, dict]]] = []  # (npc, its questions), in order
        self.own: list[tuple[str, dict[str, dict]]] = []  # (npc, the live model's own answers)
        self._rest = FixedDecider({}, npcs=())

    @property
    def model(self) -> str | None:
        return getattr(self.live, "model", None)

    async def decide(self, rid, lane, priority, deadline_ms, state, questions) -> Outcome:  # noqa: ANN001
        npc = rid.split("#", 1)[0].upper()
        self.requests.append((npc, questions))
        own = None
        if self.live is not None:
            own = await self.live.decide(rid, lane, priority, deadline_ms, state, questions)
            self.own.append((npc, own.answers or {}))
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
        if own is None:
            return Outcome("answers", answers, out.queue_ms, out.infer_ms)
        return Outcome("answers", answers, own.queue_ms, own.infer_ms)

    async def cancel(self, rid: str) -> None:
        if self.live is not None:
            await self.live.cancel(rid)


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

    No event or action is injected. Without DECIDER_URL a fixed decider investigates only for Maleth;
    with it the server's planner asks its live decider, records that answer and pins investigate.
    """
    from ..brain import GameApplier
    from ..brain.day import current_entry

    npc = "BAU_954_MALETH"
    results = {
        k: "failed (0 observations)"
        for k in ("heard", "decided", "sent", "went", "idle", "routine", "memory")
    }

    def check(name: str, ok: bool, detail: str) -> None:
        results[name] = f"{'done' if ok else 'failed'} ({detail})"

    await link.wait_world()
    own = planner is None
    if planner is None:
        planner = NpcPlanner(FixedDecider({"react": "investigate"}, npcs={npc}), GameApplier(link))
        link.listeners.append(planner.on_message)
    brain = planner
    decider, trace = brain.decider, brain.trace
    recorder = DayRecorder(decider, {npc: {"react": "investigate"}}) if not own else None
    if recorder is not None:
        brain.decider = recorder
    tap = _TraceTap(trace)
    brain.trace = tap  # type: ignore[assignment]
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
        rows_start = len(tap.rows)
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

        def decision() -> dict[str, Any] | None:
            return next(
                (
                    r
                    for r in tap.rows[rows_start:]
                    if r.get("npc") == npc
                    and r.get("question") == "react"
                    and "noise" in r.get("stimuli", [])
                    and r.get("choice") == "investigate"
                    and r.get("applied")
                ),
                None,
            )

        await _until(lambda: decision() is not None, 15)
        row = decision()
        noise_rows = [
            r for r in tap.rows[rows_start:] if r.get("npc") == npc and "noise" in r.get("stimuli", [])
        ]
        check(
            "decided",
            row is not None,
            f"applied {int(row is not None)}, noise decisions {len(noise_rows)}, "
            f"latency_ms {row.get('latency_ms', 0) if row else 0}",
        )
        if recorder is not None:
            questions = [q for n, q in recorder.requests if n == npc]
            answers = [a for n, a in recorder.own if n == npc]
            answer = next(
                (
                    a.get("react", {})
                    for q, a in zip(questions, answers, strict=False)
                    if "investigate" in q.get("react", {}).get("criteria", {})
                ),
                {},
            )
            # A live model may choose any option (or return no answer); its choice is informational.
            check(
                "model_answer",
                True,
                f"answers {len(recorder.own)}, choice {answer.get('choice')}, "
                f"P(investigate) {answer.get('probabilities', {}).get('investigate', 0):.3f}",
            )
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
        brain.decider, brain.trace = decider, trace
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
    his next hour has a walk to Malet to tell the news (about an hour); it is chosen (pinned: the live model's
    own answer is kept with DECIDER_URL) and Vino walks there: near the hero the two say it in a scene of
    step 2 (the fact how: talked) and the walk leaves a `day` fact the hero saw. From a save before the walk,
    with the hero by Vino's field, the same walk ends 72 m from the hero: Malet learns it as a fact (how:
    visited), without the model or words. Across the run: no sleep or dead option in any menu, and each
    routine asked a few times only. Without the server's planner (DECIDER_URL) the scenario runs its own.
    The lines are scripted unless LLM=chatgpt; voices with a TTS service (scripts/e2e.sh starts one)."""
    from ..brain import GameApplier
    from ..brain.planner import PlannerConfig

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
    recorder = DayRecorder(None, pin={VINO: {"do": "chat"}})

    # the server's planner with DECIDER_URL, its day settings for the run; else one of the scenario's own
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
        recorder.live = decider
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
        since, own_since, rows_since = len(recorder.requests), len(recorder.own), len(tap.rows)

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
        if not own:  # what the live model itself chose, informational: the walk is pinned either way
            mine = next((a["do"] for n, a in recorder.own[own_since:] if n == VINO and "do" in a), None) or {}
            p = float((mine.get("probabilities") or {}).get("chat", 0.0))
            check(
                f"model_answer{tag}", True, f"{mine.get('choice')}, P(chat) {p:.2f}" if mine else "no answer"
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

        ok = await _until(lambda: chosen() is not None, 30)
        chose_at = time.monotonic()
        row = chosen()
        theirs = [
            (r.get("question"), r.get("choice"), r.get("why"))
            for r in tap.rows[rows_since:]
            if r.get("npc") == VINO
        ]
        check(f"chosen{tag}", ok, f"applied {row.get('applied')}" if row else f"his rows: {theirs[-3:]}")
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
        live = None if isinstance(dialogue.llm, FakeBackend) else dialogue.llm
        ok, claim = await _caught_by(link, dialogue, talk, LOBART, facts)
        check("caught", ok, facts("caught")[-1:])
        if not ok or claim is None:
            return _logged(results)
        around = [str(p.get("npc", "")).upper() for p in await link.story_nearby(LOBART, RUMOR_CM)]
        r = rumor()
        check(
            "vino_away",
            VINO not in around and r is not None and VINO not in r.known,
            f"within {RUMOR_CM // 100} m of Lobart: {VINO in around}; knows: {sorted(r.known) if r else '-'}",
        )
        # the scene's lines, else Lobart's script (it answers every call with LLM=fake)
        script = GossipScript(
            claim, claim, live, lines=DAY_LINES, quiet=False, reason="Вино пришёл с новостью"
        )
        dialogue.llm = script

        # 2. Vino hears it from Lobart far from the hero: only the fact
        talk.end()
        await link.debug("player_goto", to=VINO, distance=8000)
        far = await _until(lambda: VINO not in talk.people, 10)
        check("hero_far", far, f"Vino {metres(VINO):.0f} m")
        lines_calls, scenes = script.lines_calls, gossip.scenes
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
            and script.lines_calls == lines_calls,
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
        want = len(lines) if live is not None else len(DAY_LINES)
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
            and len(said) == len(lines) == want
            and 2 <= want <= 4
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
        lines_calls, scenes = script.lines_calls, gossip.scenes
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
            and script.lines_calls == lines_calls
            and not says,
            f"{_passes_text(got)}; {gossip.scenes - scenes} scene(s), "
            f"{script.lines_calls - lines_calls} model call(s), lines {says}",
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
) -> dict[str, str]:
    """An errand (docs/npc-tools.md, wave 2 step 5: `plan`): Lobart catches the hero lying about the turnips,
    and to «ты кому-нибудь расскажешь?» sets out to tell Andre tomorrow at 7:00 (model scripted with
    LLM=fake; every pair's rumor held closed meanwhile, so that only the errand passes it). Checks:
    - planned: the goal, a fact;
    - near (SCENARIO=errand): the hero waits where Andre is by then; Lobart sets off at 7:00 from the farm,
      walks there for real (`there`), finds Andre and tells him in front of the hero: a rumor fact with
      `how: told` and `goal_done` (`told`), a scene of two voices (`scene`);
    - fork (SCENARIO=errand_fork): Andre is taken away before Lobart comes, another of the militia stands
      there: he hears it instead (`goal_done.instead_of`);
    - far (SCENARIO=errand_far): the hero is at the tower; at 7:00 nothing walks, and when Lobart would be
      there the fact comes (`far`), his body still at his routine, no model called.
    With LLM=chatgpt Lobart answers the question himself: whoever he sets out to tell and whenever, the rest
    follows his goal."""
    await link.wait_world()
    if link.session is None:
        await wait_message(link, Session, timeout=10)
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    facts = errands.chronicle.facts
    live = None if isinstance(dialogue.llm, FakeBackend) else dialogue.llm
    if live is not None and variant != "near":
        return _logged({"live": f"failed (SCENARIO=errand_{variant} is scripted only)"})
    if not await _until(lambda: rumors.social is not None, 30):
        return _logged({"social": "failed (no people)"})
    rumors.gate = lambda r, a, b: False  # the schedule passes nothing meanwhile: only the errand does
    try:
        await link.debug("time", hour=9, minute=0)
        ok, claim = await _caught_by(link, dialogue, talk, npc, facts)
        check("caught", ok, facts("caught")[-1:])
        if not ok or claim is None:
            return _logged(results)
        snap = await link.snapshot(npc, fresh=True)
        tomorrow = snap.world.day + 2  # as the npc sees days: from 1
        plan = {"do": "tell", "who": "Андре", "item": "", "count": 0, "day": tomorrow, "hour": 7, "minute": 0}
        claim.said.append(
            ("расскажешь", FakeStep("Ещё как. Завтра с утра схожу к Андре, пусть ополчение знает.",
                                    [ToolCall("plan", plan)]))
        )  # fmt: skip
        # live, Lobart's answer is his own model's
        model = _Recorded(GossipScript(live or claim, claim, live, lines=ERRAND_LINES, quiet=False))
        dialogue.llm = model
        name = talk.people[npc].name if npc in talk.people else "Лобарт"
        await link.debug("type", text=f"{name}, ты кому-нибудь расскажешь?")

        def goal() -> Any:
            return next((g for g in errands.goals.values() if g.npc == npc and g.do == "tell"), None)

        planned = await _until(lambda: goal() is not None, 60)
        g = goal()
        meant = g is not None and (live is not None or g.to == to)
        told = f"{g.when(errands.now)}: {g.what()}" if g is not None else "no goal"
        check("planned", planned and meant, told)
        if g is None:
            return _logged(results)
        to = g.to
        talk.end()

        # the night goes by (a leap, as a sleep: everyone at their routine's spots); the hero goes where
        # Andre will be, or far off
        before = g.start - 2
        if errands.now is None or before > errands.now:  # live, it may set off at once
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
        asked = len(model.asked)
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
            check("no_model", len(model.asked) == asked, f"{len(model.asked) - asked} request(s)")
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
        played = await _until(lambda: gossip is None or gossip.scenes > scenes, 60)
        check("scene", played, f"{gossip.scenes if gossip else 0} scene(s)")
        if variant == "fork":
            await link.detach(to)
        return _logged(results)
    finally:
        rumors.gate = None


async def errand_give_scenario(
    link: GameLink, dialogue: DialogueManager, talk: TalkDesk, errands: Errands, *, npc: str = HILDA
) -> dict[str, str]:
    """A thing taken to someone (docs/npc-tools.md, wave 2 step 5: `plan` with give): the hero asks Hilda to
    take Lobart 25 gold; she walks out to him and hands it over: her gold −25, his +25, a fact `gave`
    (`gave`). Then Canthar's letter (a mission item, v39): the errand is turned down with the reason
    (`letter`). The model scripted with LLM=fake."""
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
    script = ClaimScript(
        said=[
            ("письмо", FakeStep("Письмо? Ладно, отнесу.", [ToolCall("plan", {**give, "item": letter,
                                                                             "count": 1})])),
            ("золот", FakeStep("Ладно, отнесу ему.", [ToolCall("plan", give)])),
        ],
        told=[],
        default="Чего тебе?",
        only=name,
        others=None if isinstance(dialogue.llm, FakeBackend) else dialogue.llm,
    )  # fmt: skip
    dialogue.llm = script
    await link.debug("type", text=f"{name}, отнеси Лобарту 25 золотых.")

    def goal() -> Any:
        return next((g for g in errands.goals.values() if g.npc == npc and g.item == gold), None)

    planned = await _until(lambda: goal() is not None, 60)
    g = goal()
    check("planned", planned and g is not None, g.what() if g is not None else script.outputs[-1:])
    if g is None:
        return _logged(results)
    talk.end()
    done = await _until(lambda: g.state in ("done", "failed"), 180)
    gave = next((f for f in errands.chronicle.facts("gave") if f.get("goal") == g.id), None)
    hers2, his2 = await link.snapshot(npc, fresh=True), await link.snapshot(LOBART, fresh=True)
    moved = (count(hers, gold) - count(hers2, gold), count(his2, gold) - count(his, gold))
    check("gave", done and gave is not None and moved == (25, 25), f"{g.state}, {moved}, {gave}")
    await asyncio.sleep(3)
    await link.debug("player_goto", to=npc, distance=250)
    await _until(lambda: npc in talk.people and talk.people[npc].distance <= 600, 20)
    outputs = len(script.outputs)
    await link.debug("type", text=f"{name}, а ещё отнеси Лобарту письмо.")
    answered = await _until(lambda: len(script.outputs) > outputs, 60)
    said = script.outputs[outputs:]
    refused = any("вещь для задания" in o for o in said)
    check("letter", answered and refused and not any(x.item == letter for x in errands.goals.values()), said)
    return _logged(results)
