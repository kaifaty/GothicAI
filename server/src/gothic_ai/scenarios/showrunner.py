"""The director turns a real occasion into a plot line, on a save with scripted models."""

from __future__ import annotations

import asyncio
import copy
import json
import logging
import math
from collections.abc import AsyncIterator, Awaitable, Callable, Collection, Sequence
from contextlib import suppress
from typing import TYPE_CHECKING, Any
from unittest.mock import patch

from ..game import GameLink
from ..llm import Done, Item, LLMEvent, Message, Tool, ToolCall, ToolChoice
from ..llm.fake import FakeBackend
from ..people_knowledge import PeopleKnowledge
from ..protocol import Session
from ..showrunner import MIN_REAL_SECONDS, OUTCOMES, occasion_members
from .common import _logged, _save_or_fail, _until, wait_message
from .story import play_book_chain

if TYPE_CHECKING:
    from ..showrunner import Showrunner

log = logging.getLogger(__name__)

PREMISE = (
    "На дороге рядом с жильём бросили грязные остатки припасов. "
    "Хозяин просит расспросить соседа, осмотреть обочину и принести образец, пока его не растащили звери."
)


class ShowrunnerScript:
    """First a forbidden giver, then an eligible one; after the budget's pause, no new line."""

    name = "showrunner-script"

    def __init__(self, giver: str, plot: str) -> None:
        self.giver, self.plot = giver, plot
        self.calls: list[str] = []

    async def aclose(self) -> None:
        pass

    async def stream(
        self,
        *,
        instructions: str,
        messages: Sequence[Item],
        tools: list[Tool] | None = None,
        tool_choice: ToolChoice | None = None,
        allowed: Collection[str] | None = None,
        cache_key: str | None = None,
    ) -> AsyncIterator[LLMEvent]:
        self.calls.append("\n".join(m.content for m in messages if isinstance(m, Message)))
        n = len(self.calls)
        args = {
            "kind": "quest" if n <= 2 else "none",
            "giver": self.plot if n == 1 else self.giver if n == 2 else "",
            "place": "",
            "new_people": False,
            "premise": PREMISE if n <= 2 else "",
            "reason": "надо убрать дрянь с дороги" if n <= 2 else "герою пока хватает поручений",
            "challenge": {"kind": "no_blood"} if n <= 2 else None,
        }
        call = ToolCall("propose_line", args, f"showrunner{n}")
        yield call
        yield Done("", (call,))


class ErrandScript:
    """One book quest: ask a neighbor, inspect a place, collect a sample, then bring it back."""

    name = "errand-script"

    def __init__(
        self, *, place: str, where: str, item: str, name: str, gold: int, xp: int, neighbor: str,
    ) -> None:
        self.place, self.where, self.item, self.item_name = place, where, item, name
        self.gold, self.xp = gold, xp
        self.neighbor = neighbor
        self.calls: list[str] = []

    @classmethod
    async def for_giver(cls, director: Showrunner, giver: str) -> ErrandScript | None:
        brief = await director.chains.brief(giver)
        if brief is None or not brief.places or not brief.base.items:
            return None
        neighbor = next((
            npc for npc in sorted(brief.people)
            if npc not in {giver, "PLAYER", "PC_HERO"} and not director.rules.plot_npc(npc)
        ), None)
        if neighbor is None:
            return None
        place = next((p for p in brief.places.values() if p.waypoints), None)
        if place is None:
            return None
        samples = {"ITFO_SMELLYFISH": 0, "ITAT_MEATBUGFLESH": 1, "ITFO_ADDON_SHELLFLESH": 2}
        items = sorted(brief.base.items, key=lambda item: (samples.get(item, 3), item))
        item = items[0]
        for candidate in items:
            count = await director.link.evaluate(
                ["call", "NPC_HASITEMS", ["inst", "HERO"], ["sym", candidate]]
            )
            if not count:
                item = candidate
                break
        return cls(
            place=place.id,
            where=place.where_ru,
            item=item,
            name=brief.base.items[item],
            gold=brief.base.gold,
            xp=brief.base.xp,
            neighbor=neighbor,
        )

    async def aclose(self) -> None:
        pass

    async def stream(
        self,
        *,
        instructions: str,
        messages: Sequence[Item],
        tools: list[Tool] | None = None,
        tool_choice: ToolChoice | None = None,
        allowed: Collection[str] | None = None,
        cache_key: str | None = None,
    ) -> AsyncIterator[LLMEvent]:
        self.calls.append("\n".join(m.content for m in messages if isinstance(m, Message)))

        def talk(description: str, reply: str) -> dict[str, Any]:
            return {
                "description": description,
                "lines": [{"speaker": "hero", "text": description}, {"speaker": "npc", "text": reply}],
                "remember": reply,
            }

        call = ToolCall(
            "propose_chain",
            {
                "cast": [],
                "quests": [
                    {
                        "title": "Дрянь на дороге",
                        "give": talk("Что стряслось на дороге?", "Кто-то выбросил там припасы. Надо убрать."),
                        "stages": [
                            {
                                "journal": "Надо расспросить соседа, где осмотреть дорогу.",
                                "goals": [
                                    {
                                        "kind": "talk",
                                        "npc": self.neighbor,
                                        "talk": talk(
                                            "Где искать эту дрянь?",
                                            f"Посмотри {self.where}. Там валяется {self.item_name}. "
                                            "Отнеси образец хозяину, он сам с ним разберётся.",
                                        ),
                                    }
                                ],
                            },
                            {
                                "journal": f"Надо осмотреть дорогу: {self.where}.",
                                "goals": [{"kind": "reach", "place": self.place}],
                            },
                            {
                                "journal": f"Я осмотрел дорогу. Надо забрать образец: {self.item_name}.",
                                "goals": [{"kind": "have", "item": self.item, "place": self.place}],
                            },
                            {
                                "journal": "Образец у меня. Хозяин просил принести его.",
                                "goals": [
                                    {
                                        "kind": "bring",
                                        "npc": "giver",
                                        "item": self.item,
                                        "talk": talk(
                                            "Вот что я убрал с дороги.", "Давай сюда. Вот за труды."
                                        ),
                                    }
                                ],
                            },
                        ],
                        "done_journal": "Я осмотрел дорогу и принёс образец выброшенных припасов хозяину.",
                        "knows": "На дороге бросили грязные остатки припасов. Надо осмотреть и убрать их.",
                        "asked": "Расспросить соседа, осмотреть дорогу и принести образец.",
                        "gold": min(10, self.gold),
                        "xp": self.xp,
                    }
                ],
                "reason": "Осмотреть и очистить дорогу, а не искать чужую пропажу.",
            },
            f"errand{len(self.calls)}",
        )
        yield call
        yield Done("", (call,))


async def showrunner(link: GameLink, director: Showrunner, slot: int = 7) -> dict[str, str]:
    """Arrived → canon retry → quest, queued arrival during the pause, then save/load of the budget.

    The running occasion watcher samples the real game. Only the director's wall clock is advanced in
    the scenario, so checking its fifteen-minute pause never requires a real fifteen-minute wait.
    """
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    await link.wait_world()
    if link.session is None:
        await wait_message(link, Session, timeout=10)
    if not isinstance(director.model.llm, FakeBackend):
        return _logged({"scripted_model": "failed (showrunner requires LLM=fake)"})
    service, author = director.occasions, director.author
    people = await link.story_people()
    context = people[0]["npc"] if people else "PC_HERO"
    nearest = await link.story_eval(context, [["call", "NPC_GETNEARESTWP", ["inst", "HERO"]]])
    start = service.places.of(nearest[0]) if nearest and isinstance(nearest[0], str) else None
    if start is None:
        return _logged({"start": "failed (no place for the hero's nearest waypoint)"})
    plot = next((p["npc"] for p in people if author.rules.plot_npc(p["npc"])), None)
    if plot is None:
        return _logged({"plot_giver": "failed (no plot person for the canon check)"})

    # A safe resident's actual routine supplies the target, as in the occasions scenario. Exclude the
    # loaded hero's place: this must be an arrival somewhere new, rather than its initial sample.
    points = {p["name"].upper() for p in await link.waypoints() if not p.get("fp")}
    homes: dict[str, tuple[str, list[str]]] = {}
    for person in people:
        npc = person["npc"]
        if author.rules.plot_npc(npc) or not npc.startswith(("BAU_", "VLK_", "MIL_")):
            continue
        for entry in person.get("routine", []):
            waypoint = str(entry.get("waypoint", "")).upper()
            place = service.places.of(waypoint)
            if place is None or waypoint not in points:
                continue
            if place.id not in homes:
                homes[place.id] = (waypoint, [])
            homes[place.id][1].append(npc)
    targets = [
        p
        for p in service.places.places
        if p.id in homes and p.id != start.id and p.id not in service.visited and p.center is not None
    ]
    hint_targets = [p for p in targets if len(set(homes[p.id][1])) >= 2]
    if not hint_targets:
        return _logged({"target": f"failed (no unvisited place with two residents near {start.id})"})
    target = min(hint_targets, key=lambda p: math.dist(start.center or (0, 0, 0), p.center))
    waypoint, residents = homes[target.id]
    for npc in dict.fromkeys(residents):
        await link.debug("skip_important", near=npc)
    await link.debug("player_hp", hp=5000)
    await link.debug("player_goto", waypoint=waypoint)
    settled = await _until(lambda: service.current is not None and service.current.id == target.id, 25)
    if not settled:
        return _logged({"arrived": f"failed (hero did not settle at {target.id}, {waypoint})"})

    handoff_timeout = service.max_gather_s + service.poll_s * 3
    occasion = await asyncio.wait_for(service.next(), timeout=handoff_timeout)
    while not any(
        event.get("kind") == "arrived" and event.get("place") == target.id
        for event in occasion_members(occasion)
    ):
        occasion = await asyncio.wait_for(service.next(), timeout=handoff_timeout)
    check("arrived", target.id in service.visited, f"{target.id}, {waypoint}")
    brief = await director.brief(occasion)
    giver = None
    builder = None
    for npc in brief.candidates:
        if (candidate := await ErrandScript.for_giver(director, npc)) is not None:
            giver = npc
            builder = candidate
            break
    check("candidates", giver is not None and plot not in brief.candidates, list(brief.candidates))
    if giver is None or builder is None:
        return _logged(results)

    script = ShowrunnerScript(giver, plot)
    director.model.llm, director.chains.model.llm = script, builder
    real_clock = director.clock
    wall = [real_clock()]
    director.clock = lambda: wall[0]
    hint_chance = patch("gothic_ai.challenges._chance", return_value=0.0)
    hint_chance.start()
    try:
        before_rejected = len(director.chronicle.facts("line_rejected"))
        line = await director.decide(occasion)
        rejected = director.chronicle.facts("line_rejected")[before_rejected:]
        reasons = [str(why) for fact in rejected for why in fact.get("why", [])]
        check("rejected", len(script.calls) == 2 and bool(reasons), rejected)
        check("retry_reasons", bool(reasons) and all(r in script.calls[1] for r in reasons), reasons)
        check("line", line is not None and line.get("fact") == "line", line)
        if line is None or line.get("fact") != "line":
            return _logged(results)
        check(
            "giver",
            line.get("giver") == giver and giver in brief.candidates and not author.rules.plot_npc(giver),
            giver,
        )
        quests = line.get("quests", [])
        line_seq = next(seq for seq, fact in director.chronicle.entries("line") if fact == line)
        challenge = next(
            (
                (seq, fact)
                for seq, fact in director.chronicle.entries("challenge")
                if fact.get("line") == line_seq
            ),
            None,
        )
        check(
            "challenge",
            challenge is not None
            and challenge[1].get("kind") == "no_blood"
            and challenge[1].get("quests") == quests
            and giver in challenge[1].get("people", []),
            challenge,
        )

        def challenge_hint() -> dict[str, Any] | None:
            return next((
                fact for fact in director.chronicle.facts("challenge_hint")
                if challenge is not None and fact.get("challenge") == challenge[0]
            ), None)

        await _until(challenge_hint, 5)
        hint = challenge_hint()
        sources = hint.get("witnesses", []) if hint is not None else []
        check(
            "challenge_hint",
            hint is not None and hint.get("kind") == "no_blood" and hint.get("npc") == giver
            and sources == [builder.neighbor],
            hint,
        )
        knowledge = PeopleKnowledge(director.chronicle)
        check(
            "challenge_hint_known",
            hint is not None and len(sources) == 1 and any(
                entry.key == f"challenge_hint:{hint['id']}" and entry.subject == giver
                and entry.direct for entry in knowledge.of(sources[0])
            ),
            sources,
        )
        made = [director.book.all()[q] for q in quests if q in director.book.all()]
        check(
            "book",
            len(made) == len(quests) == 1
            and [g.kind for stage in made[0].stages for g in stage.goals]
            == ["talk", "reach", "have", "bring"],
            quests,
        )
        check("premise", bool(builder.calls) and f"Замысел режиссёра: {PREMISE}" in builder.calls[0])
        check("open", director.is_open(line))

        calls = len(script.calls)
        skipped = await director.pitch()
        check(
            "pitch_paused",
            skipped is not None
            and skipped.get("fact") == "line_skipped"
            and len(script.calls) == calls
            and "рано" in str(skipped),
            skipped,
        )

        # Two half-days pass a full game day even when the loaded save is near midnight.
        before = await link.story_state()
        hour, minute = int(before["hour"]), int(before["minute"])
        await link.debug("time", hour=(hour + 12) % 24, minute=minute)
        await link.debug("time", hour=hour, minute=minute)
        after = await link.story_state()
        elapsed = (
            (int(after["day"]) - int(before["day"])) * 24 * 60
            + int(after["hour"]) * 60
            + int(after["minute"])
            - hour * 60
            - minute
        )
        # Create the arrival after advancing the game day: an earlier arrival would correctly expire.
        # The real-clock pause still holds, so the production consumer must leave this one queued.
        waiting_targets = [p for p in targets if p.id not in service.visited]
        if not waiting_targets:
            return _logged({**results, "waiting_target": "failed (no second unvisited inhabited place)"})
        waiting_target = min(waiting_targets, key=lambda p: math.dist(target.center or (0, 0, 0), p.center))
        waiting_waypoint, waiting_residents = homes[waiting_target.id]
        for npc in dict.fromkeys(waiting_residents):
            await link.debug("skip_important", near=npc)
        await service.poll()
        async with service._lock:
            service.pending.clear()
            service._events.clear()
        before_occasions = len(director.chronicle.facts("occasion"))
        loop = asyncio.create_task(director.run())
        waiting: dict[str, Any] | None = None

        def uses_waiting(fact: dict[str, Any]) -> bool:
            return waiting is not None and waiting in occasion_members(fact.get("occasion", {}))

        def stop_on_outcome(fact: dict[str, Any]) -> None:
            if fact.get("fact") in OUTCOMES - {"line_rejected"} and uses_waiting(fact):
                loop.cancel()

        director.chronicle.on_fact.append(stop_on_outcome)
        try:
            await link.debug("player_goto", waypoint=waiting_waypoint)

            def arrival() -> dict[str, Any] | None:
                return next(
                    (
                        fact
                        for fact in director.chronicle.facts("occasion")[before_occasions:]
                        if fact.get("kind") == "arrived" and fact.get("place") == waiting_target.id
                    ),
                    None,
                )

            if not await _until(arrival, 30):
                check("queued_arrival", False, f"no arrival at {waiting_target.id}")
                return _logged(results)
            waiting = arrival()
            await asyncio.sleep(service.gather_s + service.poll_s * 2)
            used = [
                fact
                for fact in director.chronicle.facts()
                if fact.get("fact") in OUTCOMES and uses_waiting(fact)
            ]
            check(
                "queued_arrival",
                waiting in service.pending and not used and len(script.calls) == calls,
                f"{waiting_target.id}; queued={waiting in service.pending}; outcomes={len(used)}",
            )
            wall[0] += MIN_REAL_SECONDS + 1

            def outcome() -> dict[str, Any] | None:
                return next(
                    (
                        fact
                        for fact in reversed(director.chronicle.facts())
                        if fact.get("fact") in {"line", "line_none", "line_failed", "line_skipped"}
                        and uses_waiting(fact)
                    ),
                    None,
                )

            await _until(outcome, 60)
            none = outcome()
            check(
                "none",
                elapsed >= 24 * 60
                and len(script.calls) == calls + 1
                and none is not None
                and none.get("fact") == "line_none"
                and waiting not in service.pending,
                f"{elapsed} game minutes; {none}",
            )
        finally:
            director.chronicle.on_fact.remove(stop_on_outcome)
            loop.cancel()
            with suppress(asyncio.CancelledError):
                await loop

        at = await link.story_state(vars=["KAPITEL"])
        chapter = int(at.get("vars", {}).get("KAPITEL") or 1)
        lines = copy.deepcopy(director.lines)
        budget = director.budget(chapter, at).text()
        opened = [director.is_open(old) for old in lines]
        await asyncio.sleep(0.5)  # the game's save includes the latest chronicle memory head
        await _save_or_fail(link, slot)
        loaded = asyncio.create_task(wait_message(link, Session, lambda m: m.reason == "load"))
        await asyncio.sleep(0)
        await link.debug("load", slot)
        await loaded
        await link.wait_world()
        check(
            "after_load",
            director.lines == lines
            and [director.is_open(old) for old in director.lines] == opened
            and director.budget(chapter, at).text() == budget,
            f"{len(lines)} lines; {budget}",
        )
        check("book_after_load", all(q in director.book.all() for q in quests), quests)
        played = await play_book_chain(link, director.book, made)
        results.update({f"passage_{key}": value for key, value in played.items()})
        await director.book.settle(giver)
        check("completed", bool(made) and all(director.book.progress(q.id).state == "done" for q in made))

        def challenge_met() -> dict[str, Any] | None:
            return next(
                (
                    fact
                    for fact in director.chronicle.facts("challenge_met")
                    if challenge is not None and fact.get("challenge") == challenge[0]
                ),
                None,
            )

        await _until(challenge_met, 5)
        met = challenge_met()
        evidence = met.get("evidence", []) if met is not None else []
        facts = dict(director.chronicle.entries())
        check(
            "challenge_met",
            met is not None
            and met.get("kind") == "no_blood"
            and met.get("line") == line_seq
            and bool(evidence)
            and all(seq in facts for seq in evidence)
            and any(
                facts[seq].get("fact") == "quest_outcome" and facts[seq].get("quest") in quests
                for seq in evidence if seq in facts
            ),
            met,
        )
    finally:
        hint_chance.stop()
        director.clock = real_clock
    return _logged(results)


async def showrunner_live(link: GameLink, director: Showrunner) -> dict[str, str]:
    """Three real occasions, live decisions/builders and the common book player, in one loaded game.

    Each occasion needs a line or a reasoned refusal; at least one built line must finish.
    The production consumer runs during each occasion. Between them it is stopped while the debug
    player finishes the line and moves time: its incidental arrivals, completions and quiet days must
    not spend the live model's quota. The watcher itself keeps sampling the real game throughout.
    """
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    def valid_outcome(fact: dict[str, Any]) -> bool:
        reason = fact.get("reason")
        return fact["fact"] == "line" or (
            fact["fact"] == "line_none" and isinstance(reason, str) and bool(reason.strip())
        )

    await link.wait_world()
    if link.session is None:
        await wait_message(link, Session, timeout=10)
    if director.model.llm.name != "chatgpt":
        return _logged({"live_model": "failed (showrunner_live requires LLM=chatgpt)"})
    service = director.occasions
    people = await link.story_people()
    context = people[0]["npc"] if people else "PC_HERO"
    [nearest] = await link.story_eval(context, [["call", "NPC_GETNEARESTWP", ["inst", "HERO"]]])
    start = service.places.of(nearest)
    if start is None:
        return _logged({"start": f"failed (no place for {nearest})"})
    points = {p["name"].upper() for p in await link.waypoints() if not p.get("fp")}
    homes: dict[str, tuple[str, list[str]]] = {}
    for person in people:
        npc = person["npc"]
        if director.rules.plot_npc(npc) or not npc.startswith(("BAU_", "VLK_", "MIL_")):
            continue
        for entry in person.get("routine", []):
            waypoint = str(entry.get("waypoint", "")).upper()
            place = service.places.of(waypoint)
            if place is None or waypoint not in points:
                continue
            if place.id not in homes:
                homes[place.id] = (waypoint, [])
            homes[place.id][1].append(npc)
    targets = [
        p
        for p in service.places.places
        if p.id in homes and p.id != start.id and p.id not in service.visited and p.center is not None
    ]
    if not targets:
        return _logged({"target": "failed (no new inhabited place with non-plot residents)"})
    target = min(targets, key=lambda p: math.dist(start.center or (0, 0, 0), p.center))
    waypoint, residents = homes[target.id]
    for npc in dict.fromkeys(residents):
        await link.debug("skip_important", near=npc)
    await link.debug("player_hp", hp=1_000_000)

    async def resolve(kind: str, trigger: Callable[[], Awaitable[Any]]) -> dict[str, Any] | None:
        # Sample and discard only setup events, before the real loop and the requested stimulus.
        await service.poll()
        if service._raw_place is not None and not await _until(
            lambda: service.current is not None and service.current.id == service._raw_place, 25
        ):
            check(f"{kind}_setup", False, "hero's place did not settle after the debug player")
            return None
        await service.poll()
        async with service._lock:
            service.pending.clear()
            service._events.clear()
        before = len(director.chronicle.facts("occasion"))
        loop = asyncio.create_task(director.run())

        def stop_on_outcome(fact: dict[str, Any]) -> None:
            if fact.get("fact") in OUTCOMES - {"line_rejected"} and any(
                event.get("kind") == kind for event in occasion_members(fact.get("occasion", {}))
            ):
                # Stop at the receipt, before run() can take a second incidental occasion.
                loop.cancel()

        director.chronicle.on_fact.append(stop_on_outcome)
        try:
            await trigger()

            def occasion() -> dict[str, Any] | None:
                return next(
                    (f for f in director.chronicle.facts("occasion")[before:] if f.get("kind") == kind),
                    None,
                )

            if not await _until(occasion, 45):
                check(f"{kind}_occasion", False, "watcher did not record the occasion")
                return None
            event = occasion()
            check(f"{kind}_occasion", True, event)

            def outcome() -> dict[str, Any] | None:
                return next(
                    (
                        f
                        for f in reversed(director.chronicle.facts())
                        if f.get("fact") in OUTCOMES
                        and event in occasion_members(f.get("occasion", {}))
                        and (f["fact"] != "line_rejected" or director._work is None)
                    ),
                    None,
                )

            if not await _until(outcome, 300):
                check(f"{kind}_outcome", False, "no outcome in the chronicle within 300 s")
                return None
            fact = outcome()
            assert fact is not None
            log.info("showrunner live %s outcome: %s", kind, json.dumps(fact, ensure_ascii=False))
            check(
                f"{kind}_outcome",
                valid_outcome(fact),
                fact,
            )
            return fact
        finally:
            director.chronicle.on_fact.remove(stop_on_outcome)
            loop.cancel()
            with suppress(asyncio.CancelledError):
                await loop

    async def arrive() -> None:
        await link.debug("player_goto", waypoint=waypoint)

    async def kill() -> None:
        bandits = [p["npc"] for p in await link.story_people() if p["npc"].startswith("BDT_")]
        state = await link.story_state(npcs=bandits)
        living = {n for n, s in state.get("npcs", {}).items() if s.get("state") == "alive"}
        victim = next(
            (n for n in ("BDT_1015_BANDIT_L", "BDT_1014_BANDIT_L", "BDT_1013_BANDIT_L") if n in living),
            next((n for n in sorted(living) if n.startswith("BDT_")), None),
        )
        if victim is None:
            raise RuntimeError("no living human bandit for the deed")
        await link.debug("kill", instance=victim, near=victim, by="PLAYER")

    async def chapter() -> None:
        state = await link.story_state(vars=["KAPITEL"])
        value = int(state["vars"]["KAPITEL"]) + 1
        await link.story_effect(context, [{"do": "set", "var": "KAPITEL", "value": value}])

    real_clock = director.clock
    wall = [real_clock()]
    director.clock = lambda: wall[0]
    lines = 0
    completed_lines = 0
    outcomes = 0
    try:
        for index, (kind, trigger) in enumerate((("arrived", arrive), ("deed", kill), ("chapter", chapter))):
            if index:
                before = await link.story_state()
                hour, minute = int(before["hour"]), int(before["minute"])
                await link.debug("time", hour=(hour + 12) % 24, minute=minute)
                await link.debug("time", hour=hour, minute=minute)
                after = await link.story_state()
                elapsed = (
                    (int(after["day"]) - int(before["day"])) * 1440
                    + int(after["hour"]) * 60
                    + int(after["minute"])
                    - hour * 60
                    - minute
                )
                check(f"{kind}_day", elapsed >= 1440, elapsed)
                wall[0] += MIN_REAL_SECONDS + 1
            fact = await resolve(kind, trigger)
            if fact is None or not valid_outcome(fact):
                break
            outcomes += 1
            if fact["fact"] == "line_none":
                continue
            lines += 1
            quests = [director.book.all()[q] for q in fact["quests"]]
            for q in quests:
                log.info("showrunner live %s quest: %s", kind, json.dumps(q.to_dict(), ensure_ascii=False))
            characters = [
                s.model_dump() for s in director.casting.cast.characters() if s.npc in fact["characters"]
            ]
            log.info("showrunner live %s characters: %s", kind, json.dumps(characters, ensure_ascii=False))
            played = await play_book_chain(link, director.book, quests)
            results.update({f"{kind}_{k}": v for k, v in played.items()})
            await director.book.settle(fact["giver"])
            closed = bool(quests) and all(director.book.progress(q.id).state == "done" for q in quests)
            check(f"{kind}_completed", closed and all(v.startswith("done") for v in played.values()))
            if not closed or any(not v.startswith("done") for v in played.values()):
                break
            completed_lines += 1
        check("all_outcomes", outcomes == 3, f"{outcomes}/3")
        check(
            "at_least_one_line_completed", completed_lines >= 1, f"{completed_lines} completed, {lines} built"
        )
    finally:
        director.clock = real_clock
    return _logged(results)
