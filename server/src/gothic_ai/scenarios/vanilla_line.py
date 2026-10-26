"""A line beside the scripts' turnip quest, from real vanilla occasions on a save."""

from __future__ import annotations

import asyncio
import copy
import json
import logging
from collections.abc import AsyncIterator, Awaitable, Callable, Collection, Sequence
from contextlib import suppress
from typing import TYPE_CHECKING, Any

from .. import questbook
from ..attitudes import non_hostile
from ..game import GameLink
from ..llm import Done, Item, LLMEvent, Message, Tool, ToolCall, ToolChoice
from ..llm.fake import FakeBackend
from ..protocol import Session
from ..showrunner import MIN_REAL_SECONDS, OUTCOMES, occasion_members
from .common import (
    LOBART,
    RUEBEN,
    _logged,
    _save,
    _until,
    _until_async,
    wait_message,
)
from .questlog import ACCEPT, TAKE, TITLE
from .showrunner import ErrandScript
from .story import play_book_chain

if TYPE_CHECKING:
    from ..questlog import QuestLog
    from ..showrunner import Showrunner

log = logging.getLogger(__name__)

FINISH = "DIA_LOBART_RUEBENRUNNING"
REQUIRES = [{"kind": "quest", "quest": RUEBEN, "state": "success"}]
PREMISE = (
    "Герой убрал для Лобарта поле репы. Сосед увидел, что работник расторопный, "
    "и просит осмотреть дорогу, где бросили грязные остатки припасов, и принести образец. "
    "Урожай уже принят Лобартом; это отдельная просьба."
)


class VanillaLineScript:
    """The director's response is fixed; the watcher, canon and builders stay real."""

    name = "vanilla-line-script"

    def __init__(self, giver: str) -> None:
        self.giver = giver
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
        call = ToolCall(
            "propose_line",
            {
                "kind": "quest",
                "giver": self.giver,
                "place": "",
                "new_people": False,
                "premise": PREMISE,
                "reason": "сосед отвечает отдельной просьбой на работу героя у Лобарта",
                "follows": RUEBEN,
                "requires": REQUIRES,
            },
            f"vanilla_line{len(self.calls)}",
        )
        yield call
        yield Done("", (call,))


class Turnips:
    """Prepare an isolated save, then take and turn in the scripts' own quest through its dialogs."""

    def __init__(self, link: GameLink, director: Showrunner, quests: QuestLog) -> None:
        self.link, self.director, self.quests = link, director, quests

    def event(self, state: str, after: int = 0) -> dict[str, Any] | None:
        return next(
            (
                fact
                for fact in self.director.chronicle.facts("occasion")[after:]
                if fact.get("kind") == "vanilla"
                and fact.get("vanilla", {}).get("quest") == RUEBEN
                and fact["vanilla"].get("to") == state
            ),
            None,
        )

    async def approach(self) -> None:
        await self.link.debug("skip_important", near=LOBART)
        await self.link.debug("player_goto", to=LOBART, distance=250)
        if not await _until_async(self.link.story_state, lambda s: not s.get("dialog"), 30):
            raise RuntimeError("the previous dialog did not finish")

    async def prepare(self) -> None:
        await self.link.wait_world()
        if self.link.session is None:
            await wait_message(self.link, Session, timeout=10)
        if not await _until(lambda: RUEBEN in self.quests.states, 15):
            raise RuntimeError("the vanilla watcher did not sample the loaded save")
        await self.link.debug("player_hp", hp=1_000_000)
        await self.link.story_effect(
            LOBART,
            [
                {"do": "set", "var": "KAPITEL", "value": 1},
                {"do": "set", "var": RUEBEN, "value": 0},
                {"do": "knows", "info": "DIA_LOBART_KLEIDUNG"},
            ],
        )
        await self.quests.poll()  # preparation; only the native dialogs below are measured
        await self.approach()
        service = self.director.occasions
        if not await _until(lambda: service.current is not None, 25):
            raise RuntimeError("the hero did not settle at Lobart's farm")
        if self.quests.states.get(RUEBEN) != "open" or TAKE not in await self.link.offered_dialogs(LOBART):
            raise RuntimeError(
                f"Lobart does not offer the turnip quest: {await self.link.offered_dialogs(LOBART)}"
            )

    async def take(self) -> None:
        await self.link.start_talk(LOBART)
        parent = self.quests.graph.node(TAKE).description
        answer = self.quests.graph.node(ACCEPT).description
        if not await _until_async(self.link.dialog_menu, lambda menu: parent in menu, 30):
            raise RuntimeError("Lobart's native work offer did not appear")
        await self.link.choose_dialog(LOBART, TAKE)
        if not await _until_async(self.link.dialog_menu, lambda menu: answer in menu or parent in menu, 60):
            raise RuntimeError("Lobart's work dialog did not finish")
        if parent in await self.link.dialog_menu():
            await self.link.pick_in_menu(parent)
        if not await _until_async(self.link.dialog_menu, lambda menu: answer in menu, 60):
            raise RuntimeError("Lobart's native acceptance answer did not appear")
        await self.link.pick_in_menu(answer)
        if not await _until(lambda: self.quests.states.get(RUEBEN) == "running", 20):
            raise RuntimeError("the scripts did not start the turnip quest")
        await self.link.story_effect(LOBART, [{"do": "end"}])

    async def harvest(self) -> None:
        await self.approach()
        count = int(
            await self.link.evaluate(["call", "NPC_HASITEMS", ["inst", "HERO"], ["sym", "ITPL_BEET"]])
        )
        if count < 20:
            await self.link.story_effect(
                LOBART,
                [
                    {"do": "create", "item": "ITPL_BEET", "count": 20 - count},
                    {"do": "give", "item": "ITPL_BEET", "count": 20 - count},
                ],
            )
        if FINISH not in await self.link.offered_dialogs(LOBART):
            raise RuntimeError(
                f"Lobart does not offer the native turn-in: {await self.link.offered_dialogs(LOBART)}"
            )

    async def finish(self) -> None:
        await self.link.choose_dialog(LOBART, FINISH)
        if not await _until(lambda: self.quests.states.get(RUEBEN) == "success", 30):
            raise RuntimeError("the scripts did not finish the turnip quest")
        await self.link.story_effect(LOBART, [{"do": "end"}])


def _participants(director: Showrunner, follows: str) -> set[str]:
    quest = director.author.graph.quests.get(follows)
    if quest is None:
        return set()
    return {
        director.author.graph.nodes[node].npc
        for node in quest.running | quest.success | quest.failed | quest.obsolete
        if node in director.author.graph.nodes
    }


async def _giver_ok(link: GameLink, director: Showrunner, line: dict[str, Any]) -> bool:
    giver, follows = line.get("giver", ""), line.get("follows", "")
    return bool(
        giver
        and not director.rules.plot_npc(giver)
        and follows == RUEBEN
        and follows in director.author.graph.quests
        and giver not in _participants(director, follows)
        and giver in await non_hostile(link, [giver], context=giver)
    )


async def vanilla_line(
    link: GameLink, director: Showrunner, quests: QuestLog, slot: int = 7
) -> dict[str, str]:
    """A scripted response to Lobart's success; the common player finishes it, then save/load keeps it."""
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    if not isinstance(director.model.llm, FakeBackend):
        return _logged({"scripted_model": "failed (vanilla_line requires LLM=fake)"})
    turnips = Turnips(link, director, quests)
    await turnips.prepare()
    check("prepared", True)
    await turnips.take()
    await _until(lambda: turnips.event("running"), 15)
    running = turnips.event("running")
    check("vanilla_taken", running is not None and running["vanilla"].get("node") == ACCEPT, running)
    await turnips.harvest()
    await turnips.finish()
    await _until(lambda: turnips.event("success"), 15)
    occasion = turnips.event("success")
    check("vanilla_success", occasion is not None and occasion["vanilla"].get("node") == FINISH, occasion)
    if occasion is None:
        return _logged(results)
    brief = await director.brief(occasion)
    giver = None
    for npc in sorted(brief.candidates, key=lambda n: (not n.startswith("BAU_"), n)):
        if npc not in _participants(director, RUEBEN) and await director.author.brief(npc) is not None:
            giver = npc
            break
    check("neighbor", giver is not None and giver != LOBART, giver)
    if giver is None:
        return _logged(results)
    script, builder = VanillaLineScript(giver), await ErrandScript.for_giver(director, giver)
    if builder is None:
        return _logged({**results, "builder": "failed (no book quest brief)"})
    director.model.llm, director.chains.model.llm = script, builder
    line = await director.decide(occasion)
    check("line", line is not None and line.get("fact") == "line", line)
    if line is None or line.get("fact") != "line":
        return _logged(results)
    check("follows_requires", line.get("follows") == RUEBEN and line.get("requires") == REQUIRES, line)
    check("giver", line.get("giver") == giver and await _giver_ok(link, director, line), giver)
    made = [
        (await director.book.all())[qid] for qid in line.get("quests", []) if qid in await director.book.all()
    ]
    check("book", bool(made) and len(made) == len(line.get("quests", [])), [q.id for q in made])
    if not made:
        return _logged(results)
    check("quest_requires", made[0].requires == REQUIRES, made[0].requires)
    check("giver_knows", TITLE in made[0].knows and RUEBEN not in made[0].knows, made[0].knows)
    await link.debug("skip_important", near=giver)
    check("give_open", questbook.give_node(made[0]) in await link.offered_dialogs(giver))
    played = await play_book_chain(link, director.book, made)
    results.update({f"passage_{key}": value for key, value in played.items()})
    await director.book.settle(giver)
    check("completed", all(director.book.progress(q.id).state == "done" for q in made))
    lines = copy.deepcopy(director.lines)
    descriptions = [q.to_dict() for q in made]
    await asyncio.sleep(0.5)
    if await _save(link, slot) is None:
        check("after_load", False, "the game refused the scenario's save")
        return _logged(results)
    loading = asyncio.create_task(wait_message(link, Session, lambda m: m.reason == "load", timeout=60))
    await asyncio.sleep(0)
    await link.debug("load", slot)
    await loading
    await link.wait_world()
    await _until(lambda: all(q.id in director.book.all() for q in made), 15)
    check("line_after_load", director.lines == lines, director.lines)
    check(
        "book_after_load",
        [(await director.book.all())[q.id].to_dict() for q in made if q.id in await director.book.all()]
        == descriptions
        and all(director.book.progress(q.id).state == "done" for q in made),
        [q.id for q in made],
    )
    return _logged(results)


async def vanilla_line_live(link: GameLink, director: Showrunner, quests: QuestLog) -> dict[str, str]:
    """The real consumer responds to taking and completing the scripts' quest, with live builders."""
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    if director.model.llm.name != "chatgpt":
        return _logged({"live_model": "failed (vanilla_line_live requires LLM=chatgpt)"})
    turnips = Turnips(link, director, quests)
    await turnips.prepare()
    check("prepared", True)

    async def resolve(state: str, trigger: Callable[[], Awaitable[None]]) -> dict[str, Any] | None:
        service = director.occasions
        await service.poll()
        if service._raw_place is not None and not await _until(
            lambda: service.current is not None and service.current.id == service._raw_place, 25
        ):
            check(f"{state}_setup", False, "the hero's place did not settle")
            return None
        await service.poll()
        async with service._lock:
            service.pending.clear()
            service._events.clear()
        before = len(director.chronicle.facts("occasion"))
        loop = asyncio.create_task(director.run())

        def stop_on_outcome(fact: dict[str, Any]) -> None:
            if fact.get("fact") in OUTCOMES - {"line_rejected"} and any(
                event.get("kind") == "vanilla"
                and event.get("vanilla", {}).get("quest") == RUEBEN
                and event["vanilla"].get("to") == state
                for event in occasion_members(fact.get("occasion", {}))
            ):
                loop.cancel()

        director.chronicle.on_fact.append(stop_on_outcome)
        try:
            await trigger()
            if not await _until(lambda: turnips.event(state, before), 45):
                check(f"{state}_occasion", False, "the watcher did not record the vanilla occasion")
                return None
            event = turnips.event(state, before)
            check(f"{state}_occasion", True, event)
            log.info("vanilla line live %s event: %s", state, json.dumps(event, ensure_ascii=False))

            def outcome() -> dict[str, Any] | None:
                return next(
                    (
                        fact
                        for fact in reversed(director.chronicle.facts())
                        if fact.get("fact") in OUTCOMES
                        and event in occasion_members(fact.get("occasion", {}))
                        and (fact["fact"] != "line_rejected" or director._work is None)
                    ),
                    None,
                )

            if not await _until(outcome, 300):
                check(f"{state}_outcome", False, "no outcome in the chronicle within 300 s")
                return None
            fact = outcome()
            assert fact is not None
            log.info("vanilla line live %s response: %s", state, json.dumps(fact, ensure_ascii=False))
            check(
                f"{state}_outcome",
                fact["fact"] == "line" or (fact["fact"] == "line_none" and bool(fact.get("reason"))),
                fact,
            )
            return fact
        finally:
            director.chronicle.on_fact.remove(stop_on_outcome)
            loop.cancel()
            with suppress(asyncio.CancelledError):
                await loop

    real_clock = director.clock
    wall = [real_clock()]
    director.clock = lambda: wall[0]
    outcomes = lines = completed = 0
    try:
        for index, (state, trigger) in enumerate((("running", turnips.take), ("success", turnips.finish))):
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
                check("success_day", elapsed >= 1440, elapsed)
                wall[0] += MIN_REAL_SECONDS + 1
                await turnips.harvest()
            fact = await resolve(state, trigger)
            if fact is None or fact["fact"] not in {"line", "line_none"}:
                break
            outcomes += 1
            if fact["fact"] == "line_none":
                continue
            lines += 1
            safe = await _giver_ok(link, director, fact)
            check(f"{state}_giver", safe, {"giver": fact.get("giver"), "follows": fact.get("follows")})
            if not safe:
                break
            made = [(await director.book.all())[qid] for qid in fact["quests"]]
            for q in made:
                log.info("vanilla line live %s built: %s", state, json.dumps(q.to_dict(), ensure_ascii=False))
            characters = [
                spec.model_dump()
                for spec in await director.casting.cast.characters()
                if spec.npc in fact["characters"]
            ]
            log.info("vanilla line live %s characters: %s", state, json.dumps(characters, ensure_ascii=False))
            await link.debug("skip_important", near=fact["giver"])
            played = await play_book_chain(link, director.book, made)
            results.update({f"{state}_{key}": value for key, value in played.items()})
            await director.book.settle(fact["giver"])
            passed = (
                bool(made)
                and all(director.book.progress(q.id).state == "done" for q in made)
                and all(value.startswith("done") for value in played.values())
            )
            log.info("vanilla line live %s passage: %s", state, json.dumps(played, ensure_ascii=False))
            check(f"{state}_completed", passed)
            if not passed:
                break
            completed += 1
        check("all_outcomes", outcomes == 2, f"{outcomes}/2")
        check("at_least_one_line", lines >= 1, f"{lines}/2")
        check("all_lines_completed", completed == lines, f"{completed}/{lines}")
    finally:
        director.clock = real_clock
    return _logged(results)
