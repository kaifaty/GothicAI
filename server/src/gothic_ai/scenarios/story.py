from __future__ import annotations

import asyncio
import logging
import math
import re
import time
from collections import Counter
from collections.abc import AsyncIterator, Callable, Collection, Sequence
from typing import Any

from .. import programs, questbook, triggers
from ..authoring import PROPOSE as PROPOSE_QUEST
from ..authoring import Author
from ..brain.reflex import Reflexes
from ..cast import TEMPLATES, Cast, Person, PersonTie
from ..casting import PROPOSE as PROPOSE_CHARACTER
from ..casting import Casting
from ..chains import PROPOSE as PROPOSE_CHAIN
from ..chains import ChainAuthor
from ..dialogue import DialogueManager
from ..director import PROPOSE, Director
from ..game import GameError, GameLink
from ..gamedata.storygraph import Expr
from ..llm import Done, Item, LLMEvent, Message, Tool, ToolCall, ToolChoice
from ..llm.fake import FakeBackend, FakeStep
from ..places import PlaceBook
from ..protocol import (
    DialogChosen,
    GameMessage,
    LineStarted,
    NpcDied,
    NpcLook,
    NpcSpec,
    NpcThing,
    RoutineEntry,
    SayCmd,
    Session,
)
from ..social import Rumors
from ..story import StoryPatches
from ..talk import TalkDesk
from .common import (
    HILDA,
    LOBART,
    RUEBEN,
    RUEBEN_TO_HILDA,
    ClaimScript,
    FixedDecider,
    _ByNpc,
    _logged,
    _save_or_fail,
    _until,
    _until_async,
    attach_when_free,
    wait_message,
)

log = logging.getLogger("gothic_ai.scenarios")


ORIGINAL_LINE = "DIA_XARDAS_TODO_14_01"  # Xardas: "На этот раз самим нам не справиться..."


async def graph_dialogs(
    link: GameLink, story: StoryPatches, npc: str = "NONE_100_XARDAS", slot: int = 8
) -> dict[str, str]:
    """Stage 3 check (docs/story-graph.md): the game asks the story graph which dialogs an npc offers.

    First every npc of the world builds its menu and each script condition is compared with its lifted
    form (stage 2, live). Without changes the menu is the scripts' own. A dialog can be switched off and
    on, and get a condition; the director adds a dialog with a line of its own, an original voiced line
    and an effect, and the hero chooses it. A change made before a save comes back after loading it, one
    made after it does not.
    """
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: str = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail else ''}"

    await link.wait_world()
    if link.session is None:
        await wait_message(link, Session, timeout=10)
    await asyncio.sleep(1)  # the session's patches are in the game

    live = await link.check_conditions()
    check("live_check", live.get("computed", 0) > 0 and live.get("differ") == 0, str(live))
    # the graph runs the dialogs' functions in place of the VM (docs/story-graph.md, «Исполнение без VM»):
    # every condition of the check ran as a program, none failed, none was left to the VM
    programs = live.get("programs") or {}
    check(
        "programs",
        bool(programs.get("on"))
        and programs.get("runs", 0) > 0
        and programs.get("failed") == 0
        and programs.get("vm") == 0,
        str(programs),
    )
    base = await link.offered_dialogs(npc)
    check("vanilla", bool(base), " ".join(base))
    if len(base) < 2:
        return results
    first, second = base[0], base[1]

    await story.apply({"op": "disable", "node": first})
    check("disable", first not in await link.offered_dialogs(npc), first)
    await story.apply({"op": "enable", "node": first})
    check("enable", first in await link.offered_dialogs(npc), first)

    never = ["==", ["int", 1], ["int", 0]]
    await story.apply({"op": "condition", "node": first, "expr": never})
    check("condition", first not in await link.offered_dialogs(npc), first)
    await story.apply({"op": "condition", "node": first, "expr": None})
    check("condition_dropped", first in await link.offered_dialogs(npc), first)
    try:
        await story.apply({"op": "condition", "node": first, "expr": ["var", "NO_SUCH_VARIABLE"]})
        check("bad_condition", False, "taken")
    except GameError as err:
        check("bad_condition", True, str(err))

    node = {
        "id": "DIR_M5_TEST",
        "npc": npc,
        "description": "Проверка режиссёра",
        "nr": 50,
        "condition": ["!", ["call", "NPC_ISDEAD", ["inst", "SELF"]]],
        "lines": [
            {"speaker": "other", "text": "Это проверка графа сюжета."},
            {"speaker": "self", "line": ORIGINAL_LINE},
        ],
        "effects": [{"do": "xp", "amount": 10}],
    }
    await story.apply({"op": "add_node", "node": node})
    check("add_node", "DIR_M5_TEST" in await link.offered_dialogs(npc))
    exp = ["field", ["inst", "HERO"], "EXP"]
    before = await link.evaluate(exp)
    chosen = asyncio.ensure_future(wait_message(link, DialogChosen, lambda m: m.node == "DIR_M5_TEST", 10))
    await link.choose_dialog(npc, "DIR_M5_TEST")
    await chosen
    after = await link.evaluate(exp)
    check("effects", after == before + 10, f"exp {before} -> {after}")
    check("told", "DIR_M5_TEST" not in await link.offered_dialogs(npc))

    # an original voiced line through `say`, as the director will make an npc use its own voice
    await attach_when_free(link, npc)
    started = asyncio.ensure_future(wait_message(link, LineStarted, lambda m: m.line == ORIGINAL_LINE, 15))
    await link.send(SayCmd(npc=npc, line=ORIGINAL_LINE, text=""))
    try:
        check("original_line", (await started).voiced, ORIGINAL_LINE)
    except TimeoutError:
        check("original_line", False, "did not start")
    await link.act(npc, "return_to_routine")

    await story.apply({"op": "disable", "node": second})
    await asyncio.sleep(0.5)  # the game has the memory head of that change
    await _save_or_fail(link, slot)
    await story.apply({"op": "disable", "node": first})  # after the save: gone with the load
    loaded = asyncio.ensure_future(wait_message(link, Session, lambda m: m.reason == "load"))
    await link.debug("load", slot)
    await loaded
    await link.wait_world()
    await asyncio.sleep(1)
    offered = await link.offered_dialogs(npc)
    check(
        "after_load",
        second not in offered and first in offered and "DIR_M5_TEST" not in offered,
        " ".join(offered),
    )

    for name, outcome in results.items():
        log.info("%-18s %s", name, outcome)
    return results


async def graph_programs(
    link: GameLink,
    story: StoryPatches,
    npc: str = "NONE_100_XARDAS",
    slot: int = 8,
    dialogue: DialogueManager | None = None,
) -> dict[str, str]:
    """Level 1, step 2 of «Исполнение без VM» (docs/story-graph.md): the director's programs.

    The game gives a dialog's program as JSON and takes it back unchanged. The director's program in place of
    a dialog's information runs when the hero chooses it: a line of its own, gold and experience. One in place
    of a condition hides the dialog; null brings the scripts' program back; a program the game can't run is
    rejected with the reason. A dialog of the director runs a program of its own. A change made before a save
    comes back after loading it.
    """
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: str = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail else ''}"

    await link.wait_world()
    if link.session is None:
        await wait_message(link, Session, timeout=10)
    await asyncio.sleep(1)  # the session's patches are in the game

    base = await link.offered_dialogs(npc)
    check("vanilla", len(base) >= 2, " ".join(base))
    if len(base) < 2:
        return results
    info = base[0]
    for cond in base[1:]:
        condition = await link.story_program(cond, "condition", own=True)
        if not any(e == ["sym", info] for e in programs.exprs_of(condition.get("steps"))):
            break
    else:
        check("vanilla", False, f"no offered dialog has a condition independent of {info}: {base[1:]}")
        return results

    own = await link.story_program(info)
    steps = own.get("steps") or []
    calls = [s for s in programs.steps_of(steps) if s.get("do") == "call"]
    check(
        "read",
        own.get("patched") is False and own.get("returns") is False and bool(calls),
        f"{own.get('fn')}: {len(steps)} steps",
    )
    await story.apply({"op": "program", "node": info, "steps": steps})
    same = await link.story_program(info)
    check("same", same.get("patched") is True and same.get("steps") == steps)

    gold = ["call", "NPC_HASITEMS", ["inst", "HERO"], ["sym", "ITMI_GOLD"]]
    exp = ["field", ["inst", "HERO"], "EXP"]

    async def hero() -> tuple[int, int]:
        return int(await link.evaluate(gold)), int(await link.evaluate(exp))

    def give(count: int) -> dict[str, Any]:
        made = ["call", "CREATEINVITEMS", ["inst", "OTHER"], ["sym", "ITMI_GOLD"], ["int", count]]
        return {"do": "call", "call": made}

    def xp(amount: int) -> dict[str, Any]:
        return {"do": "call", "call": ["fn", "B_GIVEPLAYERXP", ["int", amount]]}

    mine = [{"do": "say", "speaker": "self", "text": "Это говорит программа режиссёра."}, give(7), xp(10)]
    await story.apply({"op": "program", "node": info, "steps": [*mine, {"do": "return"}]})
    before = await hero()
    said = asyncio.ensure_future(
        wait_message(link, LineStarted, lambda m: m.npc == npc and m.line.startswith("AIB_G"), 30)
    )
    chosen = asyncio.ensure_future(wait_message(link, DialogChosen, lambda m: m.node == info, 10))
    await link.choose_dialog(npc, info)
    await chosen
    after = await hero()
    check("replaced", after == (before[0] + 7, before[1] + 10), f"gold, exp {before} -> {after}")
    try:
        own = await said
        check("said", True, own.line)
        # in the npc's voice (v26), when the server has one: it may come a moment after the line starts
        if dialogue is not None and dialogue.tts is not None:
            voiced = await _until(lambda: dialogue.story_voiced(own.line) is not None, 10)
            check("voiced", voiced, f"{dialogue.story_voiced(own.line)} s, voiced at the start: {own.voiced}")
    except TimeoutError:
        check("said", False, "the line of its own did not start")

    never = [{"do": "return", "value": ["int", 0]}]
    await story.apply({"op": "program", "node": cond, "part": "condition", "steps": never})
    check("condition", cond not in await link.offered_dialogs(npc), cond)
    await story.apply({"op": "program", "node": cond, "part": "condition", "steps": None})
    check("condition_back", cond in await link.offered_dialogs(npc), cond)

    await story.apply({"op": "program", "node": info, "steps": None})
    back = await link.story_program(info)
    check("restored", back.get("patched") is False and back.get("steps") == steps)
    try:
        bad = [{"do": "call", "call": ["call", "CREATEINVITEMS", ["inst", "OTHER"]]}]
        await story.apply({"op": "program", "node": info, "steps": bad})
        check("bad_program", False, "taken")
    except GameError as err:
        check("bad_program", "takes 3 arguments" in str(err), str(err))

    # a dialog of the director runs a program of its own: lines, gold, a branch
    node = {
        "id": "DIR_PROGRAM_TEST",
        "npc": npc,
        "description": "Проверка программы режиссёра",
        "nr": 50,
        "program": [
            {"do": "say", "speaker": "other", "text": "Проверка программы."},
            give(3),
            {"do": "if", "cond": [">=", gold, ["int", 3]], "then": [xp(5)], "else": [xp(1000)]},
        ],
    }
    await story.apply({"op": "add_node", "node": node})
    before = await hero()
    chosen = asyncio.ensure_future(wait_message(link, DialogChosen, lambda m: m.node == node["id"], 10))
    await link.choose_dialog(npc, "DIR_PROGRAM_TEST")
    await chosen
    after = await hero()
    check("node_program", after == (before[0] + 3, before[1] + 5), f"gold, exp {before} -> {after}")

    # choices of a dialog of the director (v24): offered after its lines, as the dialog window asks; one
    # that is picked is no longer offered and its steps run; a choice may offer others
    ask = {
        "id": "DIR_CHOICE_TEST",
        "npc": npc,
        "description": "Проверка вариантов",
        "nr": 51,
        "permanent": True,
        "program": [
            {"do": "say", "speaker": "self", "text": "Выбирай."},
            {"do": "choice", "text": "Золото", "steps": [give(2)]},
            {
                "do": "choice",
                "text": "Дальше",
                "steps": [{"do": "clear_choices"}, {"do": "choice", "text": "Опыт", "steps": [xp(3)]}],
            },
        ],
    }
    await story.apply({"op": "add_node", "node": ask})
    chosen = asyncio.ensure_future(wait_message(link, DialogChosen, lambda m: m.node == ask["id"], 10))
    await link.choose_dialog(npc, ask["id"])
    await chosen
    offered = await link.dialog_choices(npc, ask["id"])
    check(
        "choices",
        [(c["id"], c["text"]) for c in offered]
        == [("DIR_CHOICE_TEST.1", "Золото"), ("DIR_CHOICE_TEST.2", "Дальше")],
        str(offered),
    )
    before = await hero()
    await link.choose_dialog(npc, "DIR_CHOICE_TEST.1")
    after = await hero()
    # the window asks with what was picked: the choice, whose dialog offers the rest
    left = [c["text"] for c in await link.dialog_choices(npc, "DIR_CHOICE_TEST.1")]
    check(
        "choice_picked",
        after[0] == before[0] + 2 and left == ["Дальше"],
        f"gold {before[0]} -> {after[0]}, {left}",
    )
    await link.choose_dialog(npc, "DIR_CHOICE_TEST.2")
    deeper = [c["text"] for c in await link.dialog_choices(npc, "DIR_CHOICE_TEST.2")]
    check("choice_offers_more", deeper == ["Опыт"], str(deeper))

    # the same in the dialog window, as the player has it: the dialog, after its line its choices, one that
    # offers another, then the dialogs again
    async def menu(ok: Callable[[list[str]], bool], timeout: float = 30) -> list[str]:
        seen: list[str] = []
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            seen = await link.dialog_menu()
            if ok(seen):
                break
            await asyncio.sleep(0.3)
        return seen

    await link.debug("teleport", near=npc, distance=150)
    await link.start_talk(npc)
    window: list[list[str]] = [await menu(lambda m: ask["description"] in m)]
    exp_before = (await hero())[1]
    for pick, then in ((ask["description"], ["Золото", "Дальше"]), ("Дальше", ["Опыт"]), ("Опыт", None)):
        if pick not in window[-1]:
            break
        await link.pick_in_menu(pick)
        # after the last choice the window offers the dialogs again
        window.append(await menu(lambda m, then=then: m == then if then else ask["description"] in m))
    gained = (await hero())[1] - exp_before
    check(
        "window",
        window[1:3] == [["Золото", "Дальше"], ["Опыт"]] and gained == 3 and ask["description"] in window[-1],
        f"{' -> '.join(map(str, window))}; exp +{gained}",
    )
    if window[-1]:
        await link.pick_in_menu(window[-1][-1])  # the last one ends the talk
        await menu(lambda m: not m, 15)

    # a program in place of a condition, made before a save, comes back after loading it
    await story.apply({"op": "program", "node": cond, "part": "condition", "steps": never})
    await asyncio.sleep(0.5)  # the game has the memory head of that change
    await _save_or_fail(link, slot)
    loaded = asyncio.ensure_future(wait_message(link, Session, lambda m: m.reason == "load"))
    await link.debug("load", slot)
    await loaded
    await link.wait_world()
    await asyncio.sleep(1)
    offered = await link.offered_dialogs(npc)
    kept = await link.story_program(cond, "condition")
    check("after_load", cond not in offered and kept.get("patched") is True, " ".join(offered))

    live = await link.check_conditions()
    counts = live.get("programs") or {}
    check("counters", counts.get("replaced", 0) >= 1 and counts.get("failed") == 0, str(counts))

    for name, outcome in results.items():
        log.info("%-18s %s", name, outcome)
    return results


class RepairScript:
    """The cloud model of the director, scripted: first a successor that is dead (the giver himself), then,
    told why not, Hilda, with the effects the brief offers for experience, gold and the turnips."""

    name = "repair-script"

    def __init__(self, giver: str, successor: str) -> None:
        self.giver = giver
        self.successor = successor
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
        brief = next(m.content for m in messages if isinstance(m, Message))
        self.calls.append(brief)
        ending = re.search(r"^1\. (\S+)", brief, re.M)
        wanted = (
            "опыт +",
            "вариант ответа «Дай мне 5",
            "вариант ответа «Лучше продай",
            "забрать у героя ITPL_BEET",
        )
        effects = [
            m.group(1)
            for m in re.finditer(r"^\s+(E\d+): (.*)$", brief, re.M)
            if m.group(2).startswith(wanted)
        ]
        gold = next((e for e in effects if f"{e}: вариант ответа «Дай мне 5" in brief), "")
        args = {
            "kind": "handover",
            "ending": ending.group(1) if ending else "",
            "npc": self.giver if len(self.calls) == 1 else self.successor,
            "description": "Лобарт мёртв. Я собрал репу.",
            "lines": [
                {"speaker": "hero", "text": "Лобарт мёртв. Я собрал репу, как он велел."},
                {
                    "speaker": "npc",
                    "text": "Знаю. Давай её сюда. Работников кормить надо, а плату получишь, как он обещал.",
                },
            ],
            "effects": effects,
            "answers": [{"effect": gold, "text": "Держи. Пять монет, как он и обещал."}] if gold else [],
            "status": "obsolete",
            "journal_now": "Лобарт мёртв. Репу, наверное, теперь примет его жена Хильда.",
            "journal_done": "Я отдал репу Хильде, она заплатила за мужа.",
            "reason": "Хильда ведёт хозяйство вместе с мужем",
        }
        call = ToolCall(PROPOSE, args, f"r{len(self.calls)}")
        yield call
        yield Done("", (call,))


async def death_load(link: GameLink, director: Director, slot: int = 8) -> dict[str, str]:
    """Report new human deaths once after loading older world ticks, without repeating saved deaths."""
    results: dict[str, str] = {}
    first, second = "BDT_1014_BANDIT_L", "BDT_1013_BANDIT_L"
    events: list[tuple[float, NpcDied]] = []
    recorded: list[tuple[float, dict[str, Any]]] = []
    loaded = asyncio.get_running_loop().create_future()
    load_requested = False
    loaded_at: float | None = None

    def receive(message: GameMessage) -> None:
        nonlocal loaded_at
        at = time.monotonic()
        if isinstance(message, NpcDied) and message.npc in (first, second):
            events.append((at, message))
        elif isinstance(message, Session) and message.reason == "load" and load_requested:
            loaded_at = at
            if not loaded.done():
                loaded.set_result(message)

    def record(fact: dict[str, Any]) -> None:
        if fact.get("fact") == "died" and fact.get("npc") in (first, second):
            recorded.append((time.monotonic(), fact))

    def facts(npc: str) -> list[dict[str, Any]]:
        return [fact for fact in director.chronicle.facts("died") if fact.get("npc") == npc]

    def check(name: str, ok: bool, detail: str) -> None:
        results[name] = f"{'done' if ok else 'failed'} ({detail})"

    def deaths(npc: str, start: float, end: float) -> tuple[list[NpcDied], list[dict[str, Any]], int]:
        seen = [(at, event) for at, event in events if event.npc == npc and start <= at <= end]
        rows = [(at, fact) for at, fact in recorded if fact.get("npc") == npc and start <= at <= end]
        elapsed = round((max(seen[0][0], rows[0][0]) - start) * 1000) if seen and rows else -1
        return [event for _, event in seen], [fact for _, fact in rows], elapsed

    link.listeners.append(receive)
    director.chronicle.on_fact.append(record)
    try:
        await link.wait_world()
        if link.session is None:
            await wait_message(link, Session, timeout=10)
        # A fresh response proves that world ticks started; the same tick primes the death scan.
        clock_npc = "NONE_100_XARDAS"
        await link.snapshot(clock_npc, fresh=True)
        before_at = time.monotonic()
        await link.debug("kill", instance=first, by="PLAYER")
        await asyncio.sleep(max(0, before_at + 3 - time.monotonic()))
        before_events, before_rows, before_ms = deaths(first, before_at, before_at + 3)
        check(
            "before",
            len(before_events) == len(before_rows) == len(facts(first)) == 1
            and before_events[0].killer == before_rows[0].get("killer") == "PLAYER",
            f"events={len(before_events)}, facts={len(facts(first))}, "
            f"player_events={sum(event.killer == 'PLAYER' for event in before_events)}, "
            f"player_facts={sum(fact.get('killer') == 'PLAYER' for fact in before_rows)}, "
            f"latency_ms={before_ms}, limit_ms=3000",
        )

        await _save_or_fail(link, slot)
        saved_t = (await link.snapshot(clock_npc, fresh=True)).t
        async with asyncio.timeout(90):
            while (before_t := (await link.snapshot(clock_npc, fresh=True)).t) - saved_t < 30_000:  # noqa: ASYNC110
                await asyncio.sleep(0.25)
        load_requested = True
        await link.debug("load", slot)
        await asyncio.wait_for(loaded, 120)
        await link.wait_world(timeout=120)
        assert loaded_at is not None
        # The response lets the initial scan prime before a new death, even when periodic pushes stall.
        after_t = (await link.snapshot(clock_npc, fresh=True)).t
        after_at = time.monotonic()
        await link.debug("kill", instance=second, by="PLAYER")
        await asyncio.sleep(max(0, after_at + 3 - time.monotonic()))
        after_events, after_rows, after_ms = deaths(second, after_at, after_at + 3)
        # Keep both raw events and newly written facts for the next five seconds: inherited facts are old.
        quiet_at = time.monotonic()
        await asyncio.sleep(5)
        quiet_ms = round((time.monotonic() - quiet_at) * 1000)
        extra_events, extra_rows, _ = deaths(second, after_at + 3, time.monotonic())
        rewind_ms = before_t - after_t
        check(
            "rewound",
            rewind_ms >= 20_000,
            f"saved_t={saved_t}, before_t={before_t}, after_t={after_t}, rewind_ms={rewind_ms}, "
            f"required_ms=20000, advanced_ms={before_t - saved_t}",
        )
        check(
            "after",
            after_at - loaded_at <= 5
            and len(after_events) == len(after_rows) == len(facts(second)) == 1
            and after_events[0].killer == after_rows[0].get("killer") == "PLAYER"
            and not extra_events
            and not extra_rows,
            f"events={len(after_events)}, facts={len(facts(second))}, "
            f"player_events={sum(event.killer == 'PLAYER' for event in after_events)}, "
            f"player_facts={sum(fact.get('killer') == 'PLAYER' for fact in after_rows)}, "
            f"latency_ms={after_ms}, limit_ms=3000, "
            f"kill_after_load_ms={round((after_at - loaded_at) * 1000)}, "
            f"extra_events={len(extra_events)}, extra_facts={len(extra_rows)}, watch_ms={quiet_ms}",
        )
        repeated_events, repeated_rows, _ = deaths(first, loaded_at, time.monotonic())
        check(
            "no_repeat",
            not repeated_events and not repeated_rows and len(facts(first)) == 1,
            f"new_events={len(repeated_events)}, new_facts={len(repeated_rows)}, "
            f"inherited_facts={len(facts(first))}, "
            f"observed_ms={round((time.monotonic() - loaded_at) * 1000)}",
        )
        return _logged(results)
    finally:
        link.listeners.remove(receive)
        director.chronicle.on_fact.remove(record)


async def repair_after_death(
    link: GameLink, director: Director, story: StoryPatches, giver: str = LOBART, successor: str = HILDA
) -> dict[str, str]:
    """Stage 5 of the story graph on the game: a quest giver dies, the director repairs the quest, the hero
    finishes it.

    Lobart gives the hero the turnip field (the effects of his own dialog: the quest variable and the
    journal). He dies. The director finds the quest without an ending and asks its model for a repair
    (scripted unless the server runs a real one, LLM=chatgpt): first the dead Lobart himself, which the
    validator rejects, then his wife Hilda. Her dialog appears once the hero has 20 turnips; he chooses it,
    the quest ends with Lobart's reward, and the dialog is not offered again."""
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    await link.wait_world()
    if link.session is None:
        await wait_message(link, Session, timeout=10)
    await asyncio.sleep(1)
    script = None
    if isinstance(director.model.llm, FakeBackend):
        director.model.llm = script = RepairScript(giver, successor)
    facts = director.chronicle.facts
    hero = ["inst", "HERO"]

    async def var(name: str) -> int | None:
        return (await link.story_state(vars=[name])).get("vars", {}).get(name)

    async def has(item: str) -> int:
        return int(await link.evaluate(["call", "NPC_HASITEMS", hero, ["sym", item]]) or 0)

    # the quest, as Lobart gives it (his dialog's choice "Хорошо...": the variable and the journal)
    taken = [
        {"do": "set", "var": RUEBEN, "value": 1},
        {
            "do": "journal",
            "topic": "Убрать поле репы",
            "text": "Фермер Лобарт хочет, чтобы я собрал репу на поле. За это он заплатит мне золотом "
            "или продаст мне одежду по значительно сниженной цене.",
        },
    ]
    await link.story_effect(giver, taken)
    before = await director.story()
    check("quest_taken", await var(RUEBEN) == 1 and before.state.alive(giver), before.endings(RUEBEN))
    check("has_an_ending", RUEBEN not in before.dead_ends())

    # Lobart dies
    await link.debug("kill", npc=giver)
    ok = await _until(lambda: any(f.get("npc") == giver for f in facts("died")), 10)
    check("died", ok)
    ok = await _until(
        lambda: any(f.get("quest") == RUEBEN for f in facts("repair") + facts("unrepaired")), 60
    )
    dead_end = [f for f in facts("dead_end") if f.get("quest") == RUEBEN]
    check("dead_end", bool(dead_end), dead_end[-1:])
    rejected = [f for f in facts("repair_rejected") if f.get("quest") == RUEBEN]
    if script is not None:
        check("rejected", any("мёртв" in why for f in rejected for why in f["why"]), rejected[-1:])
    repair = next((f for f in facts("repair") if f.get("quest") == RUEBEN), None)
    check(
        "repaired",
        ok and repair is not None and (script is None or repair.get("npc") == successor),
        repair or facts("unrepaired"),
    )
    if repair is None or repair.get("kind") != "handover":
        return _logged(results)
    node, successor = repair["node"], repair["npc"]
    # what the repair gives: Lobart's reward when scripted, what the model chose when not
    made = next(op["node"] for op in story.ops() if op.get("op") == "add_node" and op["node"]["id"] == node)
    gives = [*made["effects"], *programs.effects(made.get("program"))]
    give_gold = sum(
        e["count"] for e in gives if e["do"] == "give" and e["item"] == "ITMI_GOLD" and e.get("to") != "self"
    )
    give_exp = sum(e["amount"] for e in made["effects"] if e["do"] == "xp")
    takes = any(e["do"] == "give" and e.get("to") == "self" for e in made["effects"])
    choices = [c["text"] for c in programs.choices(made.get("program"))]
    if script is not None:
        # Lobart's reward: the experience at once, the gold or the cheaper clothes as the hero's choice,
        # as Lobart offered them
        both = sorted(["Дай мне 5 золотых монет!", "Лучше продай одежду дешевле!"])
        reward = (give_gold, give_exp, takes, sorted(choices))
        check("lobarts_reward", reward == (5, 50, True, both), made)

    # the hero brings the turnips (Hilda's dialog wants 20)
    offered = await link.offered_dialogs(successor)
    await link.attach(successor)
    await link.act(successor, "debug_add_item", item="ITPL_BEET", count=20)
    await link.detach(successor)
    await link.story_effect(successor, [{"do": "give", "item": "ITPL_BEET", "count": 20}])
    gold, exp = await has("ITMI_GOLD"), await link.evaluate(["field", hero, "EXP"])
    now = await link.offered_dialogs(successor)
    check(
        "offered", node not in offered and node in now and await has("ITPL_BEET") >= 20, f"{offered} -> {now}"
    )

    # he talks to her and chooses it
    await link.debug("player_goto", to=successor, distance=300)
    for _ in range(40):  # her own greeting may start as he comes in
        if not (await link.story_state()).get("dialog"):
            break
        await asyncio.sleep(0.5)
    await link.choose_dialog(successor, node)
    ok = await _until_async(lambda: var(RUEBEN), lambda v: v == 2, 15)
    check("quest_done", ok, await var(RUEBEN))
    # after her lines the menu offers the hero his choices; he takes the gold
    offers = await link.dialog_choices(successor, node)
    check("choices", sorted(c["text"] for c in offers) == sorted(choices), offers)
    gold_choice = next((c["id"] for c in offers if c["text"].startswith("Дай мне 5")), None)
    if gold_choice is not None:
        await link.choose_dialog(successor, gold_choice)
    after = (await has("ITMI_GOLD"), await link.evaluate(["field", hero, "EXP"]), await has("ITPL_BEET"))
    check(
        "reward",
        after[0] >= gold + give_gold and after[1] >= exp + give_exp and (after[2] == 0 or not takes),
        f"gold, exp, turnips {(gold, exp)} -> {after}",
    )
    await asyncio.sleep(1)
    check("once", node not in await link.offered_dialogs(successor))
    check("kept", any(op.get("op") == "add_node" for op in story.ops()))
    return _logged(results)


class AuthoredScript:
    """The director's model for its own quests, scripted: the first thing and the first place the brief
    offers, the most experience and a little gold."""

    name = "authored-script"

    def __init__(self) -> None:
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
        brief = next(m.content for m in messages if isinstance(m, Message))
        self.calls.append(brief)
        item = re.search(r"^- (\S+) — ([^,\n]+)$", brief, re.M)
        spot = re.search(r"^- (\S+) — (.+), \d+ м$", brief, re.M)
        caps = re.search(r"опыт до (\d+), золото до (\d+)", brief)
        xp, gold = (int(caps.group(1)), int(caps.group(2))) if caps else (0, 0)
        name, where = (item.group(2) if item else "вещь"), (spot.group(2) if spot else "")
        asker = re.search(r"^Кто просит: (.+) \(", brief, re.M)
        who = asker.group(1) if asker else "Хильда"
        args = {
            "title": f"Пропажа: {name}"[:40],
            "item": item.group(1) if item else "",
            "place": spot.group(1) if spot else "",
            "give_description": "Ты чем-то озабочена?",
            "give_lines": [
                {"speaker": "hero", "text": "Ты чем-то озабочена?"},
                {"speaker": "npc", "text": f"Пропала у меня одна вещь — {name}. Лежит, верно, {where}."},
                {"speaker": "npc", "text": "Принесёшь — не обижу."},
            ],
            "done_description": f"Я нашёл: {name}."[:60],
            "done_lines": [
                {"speaker": "hero", "text": "Вот, нашёл."},
                {"speaker": "npc", "text": "Ну надо же. Держи за труды."},
            ],
            "gold": min(gold, 10),
            "xp": xp,
            "journal_given": f"{who}: пропала одна вещь — {name}. Лежит, наверное, {where}.",
            "journal_done": f"Я нашёл пропажу и вернул её. {who} заплатил(а).",
            "reason": "мелкая пропажа по хозяйству",
        }
        call = ToolCall(PROPOSE_QUEST, args, f"a{len(self.calls)}")
        yield call
        yield Done("", (call,))


class CastingScript:
    """The director's model for its own characters, scripted: the first kind, head, face, clothes, points,
    states and thing the brief offers, a peddler's card, the first person of the place as an acquaintance."""

    name = "casting-script"

    def __init__(self) -> None:
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
        brief = next(m.content for m in messages if isinstance(m, Message))
        self.calls.append(brief)

        def section(header: str) -> list[str]:
            lines = brief.split("\n")
            at = next((i for i, line in enumerate(lines) if line.startswith(header)), None)
            out = []
            for line in lines[at + 1 :] if at is not None else []:
                if not line.startswith("- "):
                    break
                out.append(line[2:])
            return out

        def listed(header: str) -> list[str]:
            line = next((x for x in brief.split("\n") if x.startswith(header)), "")
            return [x.strip(" .") for x in line[len(header) :].split(",") if x.strip(" .")]

        points = section("Точки этого места")
        kinds = {k.split(" — ")[0]: k.split("; ")[1].split(", ") for k in section("Кем он может быть")}
        kind = "townsman" if "townsman" in kinds else next(iter(kinds), "")
        woman = kind == "townswoman"
        states = [x.split(" — ")[0] for x in section("Чем он занят")]
        things = [x.split(" — ")[0] for x in section("Вещи при нём")]
        people = [x.split(" — ") for x in section("Люди этого места")]
        args = {
            "kind": kind,
            "name": "Гарет" if not woman else "Мира",
            "head": (listed("Головы женщин:") if woman else listed("Головы мужчин:"))[0],
            "face": (listed("Лица женщин:") if woman else listed("Лица мужчин:"))[0],
            "armor": kinds.get(kind, [""])[0],
            "day_point": points[0] if points else "",
            "day_state": states[0] if states else "",
            "night_point": points[-1] if points else "",
            "night_state": states[-1] if states else "",
            "items": [{"item": things[0], "count": 2}] if things else [],
            "gold": 20,
            "ru": "Ты — бродячий торговец, скупой на слова, но честный в цене.",
            "en": "A taciturn peddler who keeps his prices fair.",
            "greed": 3,
            "honesty": 4,
            "courage": 2,
            "talk": 2,
            "pushy": 2,
            "strangers": 4,
            "voice": "A weathered peddler in her forties with a dry woman's voice"
            if woman
            else "A weathered peddler in his forties with a low, dry male voice",
            "past": "Ты пришёл с последним кораблём и торгуешь по фермам.",
            "knows": ["На дорогах к городу неспокойно."],
            "ties": [{"npc": people[0][0], "how": f"{people[0][1]}: покупаешь у неё молоко"}]
            if people
            else [],
            "reason": "торговец на дороге",
        }
        call = ToolCall(PROPOSE_CHARACTER, args, f"c{len(self.calls)}")
        yield call
        yield Done("", (call,))


class ChainCharacterScript(CastingScript):
    """The chain's named thief has a separate casting request, rather than an author's personality."""

    name = "chain-character-script"

    async def stream(self, **kwargs: Any) -> AsyncIterator[LLMEvent]:
        async for event in super().stream(**kwargs):
            if isinstance(event, ToolCall):
                args = {
                    **event.arguments,
                    "name": "Рудек",
                    "ru": "Ты — Рудек, мелкий вор. Добычу прячешь при себе и лишнего не болтаешь.",
                    "en": "Rudek, a furtive petty thief carrying stolen merchandise.",
                    "past": "Ты стащил товар у бродячего торговца и теперь пережидаешь на дороге.",
                    "knows": ["Ты украл товар у торговца Гарета."],
                    "ties": [],
                    "reason": "вор с пропавшим товаром Гарета",
                }
                call = ToolCall(PROPOSE_CHARACTER, args, event.call_id)
                yield call
                yield Done("", (call,))


class ChainScript:
    """Garet's two quests; first offer the camp in the farmyard so the author's retry is exercised."""

    name = "chain-script"

    def __init__(self, *, thief: str, camp: str, item: str, gold: int, xp: int) -> None:
        self.thief, self.camp, self.item = thief, camp, item
        self.gold, self.xp = gold, xp
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

        def talk(description: str, reply: str, remember: str = "") -> dict[str, Any]:
            return {
                "description": description,
                "lines": [{"speaker": "hero", "text": description}, {"speaker": "npc", "text": reply}],
                "remember": remember,
            }

        stolen = {
            "title": "Украденный товар",
            "give": talk("У тебя что-то пропало?", "Вор унёс мой товар. Найди его и принеси обратно."),
            "stages": [
                {
                    "journal": "Гарет просит найти вора на дороге.",
                    "goals": [{"kind": "reach", "npc": "thief"}],
                },
                {
                    "journal": "Я нашёл вора. Надо вернуть товар.",
                    "goals": [{"kind": "have", "npc": "thief", "item": self.item}],
                },
                {
                    "journal": "Товар у меня. Гарет ждёт его на ферме.",
                    "goals": [
                        {
                            "kind": "bring",
                            "npc": "giver",
                            "item": self.item,
                            "talk": talk(
                                "Вот твой товар.",
                                "Всё цело. Держи за труды.",
                                "Герой вернул тебе украденный товар.",
                            ),
                        }
                    ],
                },
            ],
            "done_journal": "Я вернул Гарету украденный товар и получил награду.",
            "knows": "У тебя украли товар. Вор скрывается неподалёку.",
            "asked": "Найти вора и принести украденный товар.",
            "gold": min(10, self.gold),
            "xp": self.xp,
        }
        camp = {
            "title": "Лагерь",
            "give": talk("Куда вор нёс товар?", "У него подельники в лагере. Убей их, иначе опять налетят."),
            "stages": [
                {
                    "journal": "Гарет просит покончить с бандитами лагеря.",
                    "goals": [{"kind": "kill", "npcs": ["camp"]}],
                },
                {
                    "journal": "Бандиты мертвы. Надо сказать Гарету.",
                    "goals": [
                        {
                            "kind": "talk",
                            "npc": "giver",
                            "talk": talk(
                                "Бандиты мертвы.",
                                "Теперь можно торговать спокойно. Вот награда.",
                                "Герой убил всех бандитов лагеря, ты заплатил ему.",
                            ),
                        }
                    ],
                },
            ],
            "done_journal": "Я уничтожил лагерь бандитов и рассказал Гарету. Он заплатил.",
            "knows": "Вор связан с бандитами лагеря неподалёку.",
            "asked": "Убить бандитов лагеря и сказать тебе.",
            "gold": min(15, self.gold),
            "xp": self.xp,
        }
        args = {
            "cast": [
                {
                    "role": "thief",
                    "kind": "townsman",
                    "place": self.thief,
                    "count": 1,
                    "about": "Мелкий вор Рудек, стащивший товар Гарета.",
                    "carries": [self.item],
                },
                {
                    "role": "camp",
                    "kind": "bandit",
                    "place": "FARM1_OUT" if len(self.calls) == 1 else self.camp,
                    "count": 3,
                    "about": "Подельники вора, грабящие торговцев.",
                    "carries": [],
                },
            ],
            "quests": [stolen, camp],
            "reason": "У вора украденный товар Гарета; след от него ведёт к лагерю его подельников.",
        }
        call = ToolCall(PROPOSE_CHAIN, args, f"chain{len(self.calls)}")
        yield call
        yield Done("", (call,))


async def director_casts(
    link: GameLink, casting: Casting, author: Author, story: StoryPatches, places: PlaceBook
) -> dict[str, str]:
    """Stage 2 of the director as author, layer 4 (docs/director.md): the director brings in a character of
    its own, and he gives the hero a quest of the author's. Its models are scripted unless the server runs a
    real one (LLM=chatgpt). The place: where Lobart spends most of his day. Checks:
    - cast: the character comes, in the world by his name, and is a person (the cast has his card);
    - then the quest of stage 1 with him as the giver (authored_quest): made, offered, given, found,
      after_load (he too comes back from the save), done, once, knows."""
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    await link.wait_world()
    if link.session is None:
        await wait_message(link, Session, timeout=10)
    await asyncio.sleep(1)
    if isinstance(casting.model.llm, FakeBackend):
        casting.model.llm = CastingScript()
    try:
        day = await link.routine(LOBART)
    except (GameError, TimeoutError) as err:
        return _logged({"cast": f"failed (Lobart's day: {err})"})
    homes = Counter(p.id for e in day.entries if (p := places.of(e.waypoint)) is not None)
    if not homes:
        return _logged({"cast": "failed (Lobart's day is in no place of the book)"})
    place = homes.most_common(1)[0][0]
    spec = await casting.character(place)
    person = casting.cast.person(spec.npc) if spec is not None else None
    state = (await link.story_state(npcs=[spec.npc])).get("npcs", {}).get(spec.npc, {}) if spec else {}
    check(
        "cast",
        spec is not None
        and person is not None
        and state.get("state") == "alive"
        and state.get("name") == spec.name,
        f"{spec.npc} «{spec.name}» at {spec.at} in {place}: {person.ru if person else ''}"
        if spec is not None
        else casting.chronicle.facts("cast_rejected")[-1:],
    )
    _logged(results)
    if spec is None:
        return results
    return {**results, **await authored_quest(link, author, story, giver=spec.npc)}


async def authored_quest(
    link: GameLink, author: Author, story: StoryPatches, giver: str = HILDA, slot: int = 7
) -> dict[str, str]:
    """Stage 1 of the director as author (docs/director.md) on the game: a quest of its own for Hilda.

    The director makes it up (its model scripted unless the server runs a real one, LLM=chatgpt): she has lost
    a thing, it lies at a place near the farm. Checks:
    - made: the quest passes the validator; its variable is in the game, 0;
    - offered: her menu has the dialog that gives it, not the one that ends it;
    - given: the hero asks her, the quest runs;
    - found: the thing lies at the place's point, the hero picks it up;
    - after_load: a save and a load in between keep the variable, and the dialog that ends it is offered;
    - done: he brings it, she takes it and pays (her gold and experience as proposed), the quest is done;
    - once: neither dialog is offered again; knows: her conversational model knows he brought it."""
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    await link.wait_world()
    if link.session is None:
        await wait_message(link, Session, timeout=10)
    await asyncio.sleep(1)
    if isinstance(author.model.llm, FakeBackend):
        author.model.llm = AuthoredScript()
    hero = ["inst", "HERO"]

    async def var(name: str) -> int | None:
        return (await link.story_state(vars=[name])).get("vars", {}).get(name)

    async def has(item: str) -> int:
        return int(await link.evaluate(["call", "NPC_HASITEMS", hero, ["sym", item]]) or 0)

    async def come_to_her() -> None:
        await link.debug("player_goto", to=giver, distance=300)
        for _ in range(40):  # her own greeting may start as he comes in
            if not (await link.story_state()).get("dialog"):
                break
            await asyncio.sleep(0.5)

    q = await author.quest(giver)
    rejected = author.chronicle.facts("authored_rejected")
    check("made", q is not None, q or rejected[-1:])
    if q is None:
        return _logged(results)
    book = author.book
    thing = q.stages[0].goals[0]
    give, done = questbook.give_node(q), questbook.done_node(q)
    point, item = thing.point, thing.item
    check("var", await var(q.id) == 0, f"{q.id} = {await var(q.id)}")
    offered = await link.offered_dialogs(giver)
    check("offered", give in offered and done not in offered, " ".join(offered))

    # the hero asks her what is wrong
    await come_to_her()
    await link.choose_dialog(giver, give)
    ok = await _until_async(lambda: var(q.id), lambda v: v == 1, 15)
    await _until(lambda: book.progress(q.id).state == "given", 5)
    check("given", ok and book.progress(q.id).state == "given", f"{q.id} = {await var(q.id)}")

    # the thing lies where she said
    before = await has(item)
    await link.debug("player_goto", waypoint=point)
    await asyncio.sleep(1)
    error = ""
    try:
        await link.debug("player_take", item=item, distance=800)
    except GameError as err:
        error = str(err)
    ok = await _until_async(lambda: has(item), lambda n: n > before, 10)
    check("found", ok, f"{item} at {point}" + (f": {error}" if error else ""))

    # a save and a load in between
    await asyncio.sleep(0.5)  # the game has the memory head of the quest
    await _save_or_fail(link, slot)
    loaded = asyncio.ensure_future(wait_message(link, Session, lambda m: m.reason == "load"))
    await link.debug("load", slot)
    await loaded
    await link.wait_world()
    await asyncio.sleep(1)
    now = await link.offered_dialogs(giver)
    kept = await var(q.id)
    check("after_load", kept == 1 and done in now and give not in now, f"{q.id} = {kept}; {' '.join(now)}")

    # he brings it
    gold, exp = await has("ITMI_GOLD"), await link.evaluate(["field", hero, "EXP"])
    await come_to_her()
    await link.choose_dialog(giver, done)
    ok = await _until_async(lambda: var(q.id), lambda v: v == 2, 15)
    after = (await has(item), await has("ITMI_GOLD"), await link.evaluate(["field", hero, "EXP"]))
    paid = after[0] == before and after[1] >= gold + q.gold and after[2] >= exp + q.xp
    check("done", ok and paid, f"thing, gold, exp {(before + 1, gold, exp)} -> {after}")
    await asyncio.sleep(1)
    left = await link.offered_dialogs(giver)
    check("once", give not in left and done not in left, " ".join(left))
    await _until(lambda: book.progress(q.id).state == "done", 5)
    check("knows", "ты заплатил" in (book.situation(giver) or ""), book.situation(giver) or "")
    return _logged(results)


class OwnRepairScript:
    """Hilda's turn-in passes to Lobart: first the dead giver is refused, then he keeps the gold reward.
    The experience is left out, so the description and the dialog must both keep the smaller reward."""

    name = "own-repair-script"

    def __init__(self) -> None:
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
        brief = next(m.content for m in messages if isinstance(m, Message))
        self.calls.append(brief)
        ending = re.search(r"^1\. (\S+)", brief, re.M)
        args = {
            "kind": "handover",
            "ending": ending.group(1) if ending else "",
            "npc": HILDA if len(self.calls) == 1 else LOBART,
            "description": "Вот что искала Хильда.",
            "lines": [
                {"speaker": "hero", "text": "Хильда просила вернуть это. Она мертва."},
                {"speaker": "npc", "text": "Знаю. Неси сюда. За её поручение я заплачу."},
            ],
            "effects": ["E1"],
            "answers": [],
            "status": "obsolete",
            "journal_now": "Хильда мертва. Её находку теперь примет Лобарт.",
            "journal_done": "Я отдал находку Лобарту. Он заплатил за поручение Хильды.",
            "reason": "Лобарт ведёт хозяйство вместе с женой и был рядом",
        }
        call = ToolCall(PROPOSE, args, f"o{len(self.calls)}")
        yield call
        yield Done("", (call,))


async def own_repair(
    link: GameLink,
    book: questbook.QuestBook,
    director: Director,
    places: PlaceBook,
    slot: int = 7,
) -> dict[str, str]:
    """Stage 3, layer 3: the three-stage quest of `stages`, with Hilda dead at stage 2 and Lobart taking
    her turn-in over, a save and a load before the hero finds the broom and brings it to him."""
    return await book_stages(link, book, places, slot, director=director)


async def own_amend(
    link: GameLink,
    book: questbook.QuestBook,
    places: PlaceBook,
    dialogue: DialogueManager,
    talk: TalkDesk,
    slot: int = 7,
) -> dict[str, str]:
    """Hilda waives the first destination in conversation; the three-stage quest continues and pays."""
    if not isinstance(dialogue.llm, FakeBackend):
        return _logged({"scripted_model": "failed (own_amend requires LLM=fake)"})
    return await book_stages(link, book, places, slot, amend=(dialogue, talk))


async def book_stages(
    link: GameLink,
    book: questbook.QuestBook,
    places: PlaceBook,
    slot: int = 7,
    *,
    director: Director | None = None,
    amend: tuple[DialogueManager, TalkDesk] | None = None,
) -> dict[str, str]:
    """Stage 3 of the director as author, layer 2 (docs/director.md): a quest as an object, of three stages.

    The quest is written here, not by a model: Hilda asks the hero to look who walks about a place near the
    farm (reach), then to get a thing that lies at another one (have, put there as the stage begins), then
    to bring it to her (bring, the turn-in). The book checks it, builds it of the graph's primitives and
    makes it.
    Checks:
    - made: the book takes it; its status is in the game, 0, and her menu offers the dialog that gives it;
    - given: the hero asks her, the quest runs at stage 1;
    - reach: he comes to the place, its trigger and the stage's fire: stage 2;
    - after_load: a save and a load keep the stage, and the triggers are back with their counts;
    - have: he picks the thing up where it was put: stage 3;
    - done: he brings it, she takes it and pays; the quest is done, no dialog of it is offered any more;
    - knows: her conversational model knows he did it and she paid."""
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    await link.wait_world()
    if link.session is None:
        await wait_message(link, Session, timeout=10)
    await asyncio.sleep(1)
    hero = ["inst", "HERO"]
    successor = HILDA
    if director is not None:
        director.model.llm = OwnRepairScript()

    async def vars(*names: str) -> dict[str, int]:
        return dict((await link.story_state(vars=list(names))).get("vars", {}))

    async def has(item: str) -> int:
        return int(await link.evaluate(["call", "NPC_HASITEMS", hero, ["sym", item]]) or 0)

    async def come_to_her() -> None:
        await link.debug("player_goto", to=successor, distance=300)
        for _ in range(40):  # her own greeting may start as he comes in
            if not (await link.story_state()).get("dialog"):
                break
            await asyncio.sleep(0.5)

    # two places near her own: one to come to, one where the thing lies
    routine = await link.routine(HILDA)
    home = places.of(*(e.waypoint for e in routine.entries))
    if home is None or home.center is None:
        return _logged({"made": "failed (no place of Hilda's in the book)"})
    spots = [
        (p, sorted(w for w in p.points if not w.startswith("FP_")))
        for p, _ in places.near(home.center, 4000, 8)
        if p.id != home.id
    ]
    spots = [(p, points) for p, points in spots if points]
    if director is not None:
        # This repair always starts in the farmyard and ends with the same broom, on the supplied save.
        yard = places.get("FARM1_OUT")
        yard_points = sorted(w for w in yard.points if not w.startswith("FP_")) if yard else []
        if yard is None or not yard_points:
            return _logged({"made": "failed (no farmyard in the book)"})
        spots = [(yard, yard_points), *(s for s in spots if s[0].id != yard.id)]
    if len(spots) < 2:
        return _logged({"made": f"failed (places near {home.id}: {len(spots)})"})
    (look, look_at), (lies, lies_at) = spots[0], spots[1]
    to, at = look_at[len(look_at) // 2], lies_at[len(lies_at) // 2]
    thing = None
    things = (
        ("ITMI_BROOM",)
        if director is not None
        else ("ITMI_BROOM", "ITMI_RAKE", "ITMI_SAW", "ITMI_SCOOP", "ITMI_BRUSH")
    )
    for candidate in things:
        if await has(candidate) == 0:
            thing = candidate
            break
    if thing is None:
        return _logged({"made": "failed (the hero has every thing of the list)"})

    def talk(said: str, answer: str, remember: str = "") -> questbook.Talk:
        return questbook.Talk(said, [{"speaker": "self", "text": answer}], remember)

    q = questbook.Quest(
        id=book.next_id(),
        giver=HILDA,
        title="Чужак у фермы",
        give=talk("Что-то случилось?", f"Кто-то бродит {look.where_ru}. Глянь, а? Заплачу."),
        stages=[
            questbook.Stage(
                f"Хильда видела чужака {look.where_ru}. Надо посмотреть.", [questbook.Goal("reach", point=to)]
            ),
            questbook.Stage(
                f"Чужака нет, но он что-то бросил {lies.where_ru}.",
                [questbook.Goal("have", item=thing, point=at, journal="Я подобрал то, что он бросил.")],
            ),
            questbook.Stage(
                "Надо отнести находку Хильде.",
                [
                    questbook.Goal(
                        "bring",
                        npc=HILDA,
                        item=thing,
                        talk=talk(
                            "Вот что он бросил.",
                            "Так это наша! Держи за труды.",
                            "Герой принёс то, что бросил чужак.",
                        ),
                    )
                ],
            ),
        ],
        done_journal="Я отдал Хильде находку.",
        asked=f"посмотреть, кто бродит {look.where_ru}.",
        gold=10,
        xp=20,
        names={HILDA: "Хильда", LOBART: "Лобарт"} if director is not None else {HILDA: "Хильда"},
        reason="e2e: три стадии",
    )
    errors = await book.make(q)
    status, stage = q.id, questbook.stage_var(q)
    offered = await link.offered_dialogs(HILDA) if not errors else []
    give, done = questbook.give_node(q), questbook.done_node(q)
    now = await vars(status, stage)
    check(
        "made",
        not errors and now.get(status) == 0 and give in offered and done not in offered,
        errors or f"{now}; {' '.join(offered)}",
    )
    if errors:
        return _logged(results)
    original_reward = q.gold, q.xp
    original = next(
        op["node"] for op in book.patches.ops() if op.get("op") == "add_node" and op["node"]["id"] == done
    )

    await come_to_her()
    await link.choose_dialog(HILDA, give)
    ok = await _until_async(lambda: vars(status, stage), lambda v: v == {status: 1, stage: 1}, 15)
    check("given", ok and book.progress(q.id).state == "given", await vars(status, stage))

    if amend is None:
        await link.debug("player_goto", waypoint=to)
        ok = await _until_async(lambda: vars(stage), lambda v: v.get(stage) == 2, 10)
        await _until(lambda: book.progress(q.id).stage == 2, 5)
        check("reach", ok and book.progress(q.id).stage == 2, f"{to} ({look.id}): {await vars(stage)}")
    else:
        dialogue, talk = amend
        gid = questbook.goal_id(q, 1, 1)
        facts = book.chronicle.facts
        why = "Не нужно никуда идти: чужак уже ушёл."
        await come_to_her()
        distance = await link.evaluate(["call", "NPC_GETDISTTOWP", hero, ["str", to]])
        before = await vars(status, stage, gid)
        unvisited = before == {status: 1, stage: 1, gid: 0} and distance >= q.stages[0].goals[0].radius
        check("unvisited", unvisited, f"{before}; {to}: {distance} cm")
        if not unvisited:
            return _logged(results)
        script = ClaimScript(
            said=[("не нужно никуда идти", FakeStep("Хорошо, туда идти не надо. Найди то, что он бросил.", [
                ToolCall("amend_quest", {"quest": q.id, "drop": [gid], "why": why}),
            ]))],
            told=[],
            default="Хм.",
        )
        dialogue.llm = _ByNpc({"Хильда": script}, FakeBackend("Хм.", delay=0))
        talk.reflexes = Reflexes(FixedDecider({"posture": "listen"}))
        nearby = await _until(lambda: HILDA in talk.people and talk.people[HILDA].distance <= 600, 20)
        check("roster", nearby, HILDA)
        if not nearby:
            return _logged(results)
        conv = dialogue.conversations.get(HILDA)
        before_reply = conv.reply if conv else None
        talk.hear("Хильда, не нужно никуда идти: чужак уже ушёл.", HILDA, {"via": "scenario"})

        def answered() -> bool:
            conv = dialogue.conversations.get(HILDA)
            return bool(conv and conv.reply and conv.reply is not before_reply and conv.reply.done())

        replied = await _until(answered, 60)
        advanced = await _until_async(lambda: vars(stage), lambda v: v.get(stage) == 2, 10)
        await _until(lambda: book.progress(q.id).stage == 2, 5)
        terms = [f for f in facts("quest_terms") if f.get("quest") == q.id]
        stages = [f for f in facts("story_quest_stage") if f.get("id") == q.id]
        reached = [f for f in facts("triggered") if f.get("id") == gid]
        situation = book.situation(HILDA) or ""
        accepted = bool(
            replied and advanced and book.progress(q.id).stage == 2
            and any(f.get("drop") == [gid] and f.get("why") == why for f in terms)
            and any(f.get("stage") == 2 for f in stages) and not reached
            and "Снято тобой:" in situation and why in situation
        )
        check(
            "amended", accepted,
            f"terms={terms}; stages={stages}; reach={reached}; {await vars(stage, gid)}",
        )
        talk.end("scenario")
        released = await _until(lambda: HILDA not in talk.attached, 20)
        check("released", released, HILDA)
        if not accepted or not released:
            return _logged(results)

    if director is not None:
        facts = director.chronicle.facts
        await link.debug("npc_goto", npc=LOBART, to=HILDA, distance=300)
        await link.debug("kill", npc=HILDA)
        died = await _until(lambda: any(f.get("npc") == HILDA for f in facts("died")), 10)
        check("died", died)
        await _until(lambda: any(f.get("quest") == q.id for f in facts("repair") + facts("unrepaired")), 60)
        dead_ends = [f for f in facts("dead_end") if f.get("quest") == q.id]
        check("dead_end", bool(dead_ends), dead_ends[-1:])
        rejected = [f for f in facts("repair_rejected") if f.get("quest") == q.id]
        check("rejected", any("мёртв" in why for f in rejected for why in f["why"]), rejected[-1:])
        repaired = next((f for f in facts("repair") if f.get("quest") == q.id), None)
        accepted = (
            repaired is not None and repaired.get("kind") == "handover" and repaired.get("npc") == LOBART
        )
        check("repaired", accepted, repaired or facts("unrepaired"))
        if not accepted:
            return _logged(results)
        successor = LOBART
        q = book.all()[q.id]
        made = next(
            op["node"]
            for op in reversed(book.patches.ops())
            if op.get("op") == "add_node" and op["node"]["id"] == done
        )
        mandatory = [
            e
            for e in original["effects"]
            if e["do"] not in ("xp", "journal") and not (e["do"] == "give" and e.get("item") == "ITMI_GOLD")
        ]
        check(
            "same_dialog",
            made["id"] == original["id"] == done
            and made["npc"] == successor
            and made["condition"] == original["condition"]
            and all(e in made["effects"] for e in mandatory)
            and q.turn_in() is not None
            and q.turn_in().npc == successor,
            made,
        )
        gold = sum(e["count"] for e in made["effects"] if e["do"] == "give" and e.get("item") == "ITMI_GOLD")
        xp = sum(e["amount"] for e in made["effects"] if e["do"] == "xp")
        check(
            "reward_cap",
            (gold, xp) == (q.gold, q.xp) == (original_reward[0], 0)
            and 0 <= gold <= original_reward[0]
            and 0 <= xp <= original_reward[1],
            f"{original_reward} -> {(gold, xp)}",
        )
        situation = book.situation(successor) or ""
        check("successor_knows", "принял" in situation.lower() and "Хильд" in situation, situation)

    await _save_or_fail(link, slot)
    loaded = asyncio.ensure_future(wait_message(link, Session, lambda m: m.reason == "load"))
    await link.debug("load", slot)
    await loaded
    await link.wait_world()
    await asyncio.sleep(2)
    counts = {t["id"]: t["count"] for t in await link.triggers()}
    mine = {k: v for k, v in counts.items() if k.startswith(f"{q.id}_")}
    check(
        "after_load",
        await vars(status, stage) == {status: 1, stage: 2}
        and mine == {f"{q.id}_S1G1": 1, f"{q.id}_S1": 1, f"{q.id}_S2G1": 0, f"{q.id}_S2": 0},
        f"{await vars(status, stage)}; {mine}",
    )
    if amend is not None:
        terms = [f for f in book.chronicle.facts("quest_terms") if f.get("quest") == q.id]
        situation = book.situation(HILDA) or ""
        check(
            "terms_after_load",
            any(f.get("drop") == [gid] and f.get("why") == why for f in terms)
            and "Снято тобой:" in situation and why in situation,
            f"terms={terms}; {situation}",
        )
    if director is not None:
        q = book.all()[q.id]
        nodes = [
            op["node"] for op in book.patches.ops() if op.get("op") == "add_node" and op["node"]["id"] == done
        ]
        check(
            "repair_after_load",
            bool(nodes)
            and nodes[-1]["npc"] == successor
            and q.turn_in() is not None
            and q.turn_in().npc == successor
            and len([f for f in director.chronicle.facts("repair") if f.get("quest") == q.id]) == 1,
            nodes[-1] if nodes else "no turn-in",
        )

    await link.debug("player_goto", waypoint=at)
    await asyncio.sleep(1)
    error = ""
    try:
        await link.debug("player_take", item=thing, distance=800)
    except GameError as err:
        error = str(err)
    ok = await _until_async(lambda: vars(stage), lambda v: v.get(stage) == 3, 10)
    check("have", ok, f"{thing} at {at} ({lies.id}): {await vars(stage)}" + (f"; {error}" if error else ""))

    gold, exp = await has("ITMI_GOLD"), await link.evaluate(["field", hero, "EXP"])
    await come_to_her()
    offered = await link.offered_dialogs(successor)
    if director is not None:
        check("successor_offers", done in offered and done not in await link.offered_dialogs(HILDA), offered)
    try:
        await link.choose_dialog(successor, done)
    except GameError as err:
        check("done", False, f"{err}; {' '.join(offered)}")
        return _logged(results)
    ok = await _until_async(lambda: vars(status), lambda v: v.get(status) == 2, 15)
    after = (await has(thing), await has("ITMI_GOLD"), await link.evaluate(["field", hero, "EXP"]))
    await asyncio.sleep(1)
    left = await link.offered_dialogs(successor)
    paid = after[0] == 0 and after[1] >= gold + q.gold and after[2] >= exp + q.xp
    if director is not None:
        paid = paid and after[1] - gold <= original_reward[0] and after[2] - exp <= original_reward[1]
    check(
        "done",
        ok and paid and give not in left and done not in left,
        f"thing, gold, exp {(1, gold, exp)} -> {after}; {await vars(status, stage)}",
    )
    await _until(lambda: book.progress(q.id).state == "done", 5)
    situation = book.situation(successor) or ""
    check("knows", "ты заплатил" in situation, situation)
    if amend is not None:
        outcomes = [f for f in book.chronicle.facts("quest_outcome") if f.get("quest") == q.id]
        check(
            "outcome", any(f.get("how") == "amended" and f.get("pay") == "full" for f in outcomes), outcomes,
        )
    if director is not None:
        left = [*left, *await link.offered_dialogs(HILDA)]
        check("once", not any(n.startswith(f"{q.id}_") for n in left), left)
        check("book_done", book.progress(q.id).state == "done" and "герой выполнил" in situation, situation)
    return _logged(results)


async def book_requires(link: GameLink, book: questbook.QuestBook, slot: int = 7) -> dict[str, str]:
    """Stage 5, layer 2: Hilda's book quest opens after Lobart's turnips, then waits for her pan quest.

    No model: vanilla effects take and finish the turnips; the native shopping dialog gives the other
    quest. A save and load while its vanilla goal waits must keep both the stage and its unfired trigger.
    """
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    await link.wait_world()
    if link.session is None:
        await wait_message(link, Session, timeout=10)
    await asyncio.sleep(1)
    pan = "MIS_HILDA_PFANNEKAUFEN"
    shopping = "DIA_HILDA_EINKAUFEN"
    hero = ["inst", "HERO"]

    async def vars(*names: str) -> dict[str, int]:
        return dict((await link.story_state(vars=list(names))).get("vars", {}))

    async def come_to_her() -> None:
        await link.debug("player_goto", to=HILDA, distance=300)
        for _ in range(40):  # her native greeting may start as he comes in
            if not (await link.story_state()).get("dialog"):
                break
            await asyncio.sleep(0.5)

    def talk(said: str, answer: str) -> questbook.Talk:
        return questbook.Talk(said, [{"speaker": "self", "text": answer}])

    initial = await vars(RUEBEN, pan, "KAPITEL")
    ready = initial.get(RUEBEN) == 0 and initial.get(pan) == 0 and initial.get("KAPITEL") == 1
    check("prepared", ready, initial)
    if not ready:
        return _logged(results)

    q = questbook.Quest(
        id=book.next_id(),
        giver=HILDA,
        title="Помощник по хозяйству",
        give=talk("Лобарт доволен моей работой. Есть ещё дело?", "Есть. Поговорим о покупках для кухни."),
        stages=[
            questbook.Stage(
                "После работы у Лобарта Хильда хочет поговорить о покупках.",
                [
                    questbook.Goal(
                        "talk",
                        npc=HILDA,
                        talk=talk("Что тебе нужно для кухни?", "Нужна сковорода. Возьмись за моё поручение."),
                    )
                ],
            ),
            questbook.Stage(
                "Нужно взять у Хильды поручение «Сковорода для Хильды».",
                [questbook.Goal("vanilla", quest=pan, state="running")],
            ),
            questbook.Stage(
                "Поручение о сковороде взято. Можно сказать Хильде, что я готов помочь.",
                [
                    questbook.Goal(
                        "talk",
                        npc=HILDA,
                        talk=talk(
                            "Я взялся за твою сковороду.", "Вот тебе за готовность. Саму сковороду ещё жду."
                        ),
                    )
                ],
            ),
        ],
        done_journal="Хильда заплатила за готовность помогать. Сковороду ещё нужно принести.",
        asked="поговорить о покупках и взять поручение о сковороде",
        gold=5,
        xp=10,
        names={HILDA: "Хильда"},
        requires=[{"kind": "quest", "quest": RUEBEN, "state": "success"}],
        reason="e2e: открытие и цель по состоянию ванильных квестов",
    )
    errors = await book.make(q)
    status, stage = q.id, questbook.stage_var(q)
    give, done = questbook.give_node(q), questbook.done_node(q)
    now = await vars(status, stage)
    offered = await link.offered_dialogs(HILDA) if not errors else []
    check(
        "locked",
        not errors
        and now == {status: 0, stage: 0}
        and book.progress(q.id).state == "open"
        and give not in offered
        and done not in offered,
        errors or f"{now}; {' '.join(offered)}",
    )
    if errors:
        return _logged(results)

    # The vanilla setup has the same quest variable and journal effects as Lobart's own dialogs.
    await link.story_effect(
        LOBART,
        [
            {"do": "set", "var": RUEBEN, "value": 1},
            {
                "do": "journal",
                "topic": "Убрать поле репы",
                "text": "Лобарт поручил мне собрать репу на поле.",
                "status": "LOG_RUNNING",
            },
        ],
    )
    offered = await link.offered_dialogs(HILDA)
    check(
        "running_locked",
        (await vars(RUEBEN)).get(RUEBEN) == 1 and give not in offered and book.progress(q.id).state == "open",
        offered,
    )
    await link.story_effect(
        LOBART,
        [
            {"do": "set", "var": RUEBEN, "value": 2},
            {
                "do": "journal",
                "topic": "Убрать поле репы",
                "text": "Я собрал репу для Лобарта.",
                "status": "LOG_SUCCESS",
            },
        ],
    )
    # Delivering the harvest is the prerequisite of Hilda's native offer to shop for a pan.
    await link.story_effect(
        HILDA,
        [
            {"do": "set", "var": RUEBEN_TO_HILDA, "value": 2},
            {
                "do": "journal",
                "topic": "Отнести репу Хильде",
                "text": "Я доставил Хильде собранную репу.",
                "status": "LOG_SUCCESS",
            },
        ],
    )
    offered = await link.offered_dialogs(HILDA)
    check(
        "unlocked",
        (await vars(RUEBEN)).get(RUEBEN) == 2
        and give in offered
        and shopping in offered
        and book.progress(q.id).state == "open",
        offered,
    )

    await come_to_her()
    await link.choose_dialog(HILDA, give)
    ok = await _until_async(lambda: vars(status, stage), lambda v: v == {status: 1, stage: 1}, 15)
    await _until(lambda: book.progress(q.id).state == "given", 5)
    check("given", ok and book.progress(q.id).state == "given", await vars(status, stage))
    await link.choose_dialog(HILDA, questbook.goal_node(questbook.goal_id(q, 1, 1)))
    ok = await _until_async(lambda: vars(stage), lambda v: v.get(stage) == 2, 15)
    await _until(lambda: book.progress(q.id).stage == 2, 5)
    check(
        "waiting",
        ok
        and book.progress(q.id).stage == 2
        and (await vars(pan)).get(pan) == 0
        and done not in await link.offered_dialogs(HILDA),
        await vars(status, stage, pan),
    )

    await _save_or_fail(link, slot)
    loaded = asyncio.ensure_future(wait_message(link, Session, lambda m: m.reason == "load"))
    await link.debug("load", slot)
    await loaded
    await link.wait_world()
    await asyncio.sleep(2)
    counts = {t["id"]: t["count"] for t in await link.triggers()}
    mine = {k: v for k, v in counts.items() if k.startswith(f"{q.id}_")}
    restored = book.all().get(q.id)
    check(
        "after_load",
        await vars(status, stage, pan) == {status: 1, stage: 2, pan: 0}
        and book.progress(q.id).stage == 2
        and restored is not None
        and restored.requires == q.requires
        and restored.stages[1].goals[0] == q.stages[1].goals[0]
        and mine == {f"{q.id}_S1": 1, f"{q.id}_S2G1": 0, f"{q.id}_S2": 0}
        and done not in await link.offered_dialogs(HILDA),
        f"{await vars(status, stage, pan)}; {mine}",
    )

    await come_to_her()
    offered = await link.offered_dialogs(HILDA)
    if shopping not in offered:
        check("vanilla", False, offered)
        return _logged(results)
    await link.choose_dialog(HILDA, shopping)
    ok = await _until_async(lambda: vars(stage, pan), lambda v: v == {stage: 3, pan: 1}, 20)
    await _until(lambda: book.progress(q.id).stage == 3, 5)
    check(
        "vanilla",
        ok and book.progress(q.id).stage == 3 and done in await link.offered_dialogs(HILDA),
        await vars(status, stage, pan),
    )

    gold = int(await link.evaluate(["call", "NPC_HASITEMS", hero, ["sym", "ITMI_GOLD"]]) or 0)
    exp = int(await link.evaluate(["field", hero, "EXP"]) or 0)
    await link.choose_dialog(HILDA, done)
    ok = await _until_async(lambda: vars(status, stage), lambda v: v == {status: 2, stage: 4}, 15)
    await _until(lambda: book.progress(q.id).state == "done", 5)
    left = await link.offered_dialogs(HILDA)
    after_gold = int(await link.evaluate(["call", "NPC_HASITEMS", hero, ["sym", "ITMI_GOLD"]]) or 0)
    after_exp = int(await link.evaluate(["field", hero, "EXP"]) or 0)
    check(
        "done",
        ok
        and book.progress(q.id).state == "done"
        and after_gold == gold + q.gold
        and after_exp == exp + q.xp
        and await vars(RUEBEN, pan) == {RUEBEN: 2, pan: 1}
        and not any(n.startswith(f"{q.id}_") for n in left),
        f"{await vars(status, stage, RUEBEN, pan)}; gold, exp {(gold, exp)} -> {(after_gold, after_exp)}",
    )
    return _logged(results)


async def author_chain(
    link: GameLink, chains: ChainAuthor, places: PlaceBook, slot: int = 7
) -> dict[str, str]:
    """Stage 3, layer 4: Garet's chain, built by the author, then played from the book's descriptions.

    With a scripted model the thief holds his stolen merchandise and three bandits wait at their camp.
    The first proposal puts the bandits in a farmyard and must be refused before anybody is introduced.
    With a live model the same player handles whatever valid two or three quests it invents.
    """
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    await link.wait_world()
    if link.session is None:
        await wait_message(link, Session, timeout=10)
    await asyncio.sleep(1)
    # v37 has no god-mode command. A large health reserve protects the hero while reach goals can send
    # him near a live hostile role; kill goals themselves work remotely, before he approaches the camp.
    await link.debug("player_hp", hp=1_000_000)
    found = await _peddler(link, chains.casting.cast)
    if isinstance(found, str):
        return _logged({"giver": f"failed ({found})"})
    spec, _ = found
    made, why = await chains.casting.cast.introduce(spec, GARET)
    check("giver", made is not None, made or why)
    if made is None:
        return _logged(results)
    giver = spec.npc
    script: ChainScript | None = None
    characters_script: ChainCharacterScript | None = None
    if isinstance(chains.model.llm, FakeBackend):
        brief = await chains.brief(giver)
        if brief is None:
            return _logged({**results, "made": "failed (no author's brief for Garet)"})
        camps = [p for p in brief.places.values() if brief.bandit_ok(p.id) and len(p.points) >= 3]
        if not camps:
            return _logged({**results, "made": "failed (no unoccupied camp in the author's brief)"})
        camp = camps[0]
        thief = next((p for p in brief.places.values() if p.id != camp.id), camp)
        item = next(iter(brief.base.items))
        for candidate in brief.base.items:
            if not await link.evaluate(["call", "NPC_HASITEMS", ["inst", "HERO"], ["sym", candidate]]):
                item = candidate
                break
        chains.model.llm = script = ChainScript(
            thief=thief.id, camp=camp.id, item=item, gold=brief.base.gold, xp=brief.base.xp
        )
        chains.casting.model.llm = characters_script = ChainCharacterScript()

    quests = await chains.chain(giver, new_people=True)  # a chain of the director's own people
    facts = chains.chronicle.facts
    rejected = facts("chain_rejected")
    if script is not None:
        refusal = [str(why) for f in rejected for why in f.get("why", f.get("errors", []))]
        check("rejected", len(script.calls) == 2 and bool(refusal), rejected)
        check("retry_reasons", bool(refusal) and all(why in script.calls[-1] for why in refusal), refusal)
    check(
        "made",
        quests is not None and 2 <= len(quests) <= 3 and all(q.id in chains.book.all() for q in quests),
        [q.id for q in quests]
        if quests
        else {"rejected": rejected, "proposal": script.calls if script else "see chain model log"},
    )
    if not quests:
        return _logged(results)
    if script is not None:
        check(
            "garet_chain",
            [q.title for q in quests] == ["Украденный товар", "Лагерь"],
            [q.title for q in quests],
        )
        check(
            "named_cast_only",
            characters_script is not None and len(characters_script.calls) == 1,
            len(characters_script.calls) if characters_script else 0,
        )
    chain_fact = next((f for f in reversed(facts("chain")) if f.get("quests") == [q.id for q in quests]), {})
    roles = chain_fact.get("roles", {})
    npcs = [npc for group in roles.values() for npc in group]
    world = (await link.story_state(npcs=npcs)).get("npcs", {})
    # asked for someone new: the cast is not empty, and all of it is in the world
    check("roles", bool(npcs) and all(world.get(n, {}).get("state") == "alive" for n in npcs), roles)
    held = []
    for q in quests:
        for stage in q.stages:
            for goal in stage.goals:
                if goal.kind == questbook.HAVE and goal.npc:
                    count = await link.evaluate(
                        ["call", "NPC_HASITEMS", ["inst", "SELF"], ["sym", goal.item]], npc=goal.npc
                    )
                    held.append((goal.npc, goal.item, count, goal.count))
    check("carries", all(count >= needed for _, _, count, needed in held), held)
    check(
        "linear",
        all(q.after == quests[k - 1].id for k, q in enumerate(quests) if k),
        [q.after for q in quests],
    )
    locked = [questbook.give_node(q) not in await link.offered_dialogs(giver) for q in quests[1:]]
    check("locked", all(locked), locked)

    async def save_and_load(q: questbook.Quest, dead: str, rest: list[str]) -> None:
        names = [v for quest in quests for v in (quest.id, questbook.stage_var(quest))]
        before = (await link.story_state(vars=names)).get("vars", {})
        before_counts = {
            t["id"]: t["count"]
            for t in await link.triggers()
            if any(t["id"].startswith(f"{quest.id}_") for quest in quests)
        }
        await _save_or_fail(link, slot)
        loaded = asyncio.ensure_future(wait_message(link, Session, lambda m: m.reason == "load"))
        await link.debug("load", slot)
        await loaded
        await link.wait_world()
        await asyncio.sleep(2)
        after = (await link.story_state(vars=names, npcs=[dead, *rest])).copy()
        after_counts = {
            t["id"]: t["count"]
            for t in await link.triggers()
            if any(t["id"].startswith(f"{quest.id}_") for quest in quests)
        }
        actors = after.get("npcs", {})
        check(
            "after_load",
            before == after.get("vars", {})
            and before_counts == after_counts
            and actors.get(dead, {}).get("state") == "dead"
            and all(actors.get(n, {}).get("state") == "alive" for n in rest)
            and all(quest.id in chains.book.all() for quest in quests),
            f"{q.id}: {before} -> {after.get('vars')}; counts {before_counts} -> {after_counts}; {actors}",
        )

    played = await play_book_chain(link, chains.book, quests, after_first_kill=save_and_load)
    results.update(played)
    if script is not None and "after_load" not in results:
        check("after_load", False, "the scripted chain never reached its group kill checkpoint")
    await chains.book.settle(giver)
    situation = chains.book.situation(giver) or ""
    check(
        "knows",
        all(q.title in situation and chains.book.progress(q.id).state == "done" for q in quests)
        and situation.count("герой выполнил") == len(quests),
        situation,
    )
    snap = await link.snapshot(giver, fresh=True)
    check(
        "hero_alive",
        snap.player is not None and snap.player.hp > 0,
        snap.player.hp if snap.player else "no player",
    )
    return _logged(results)


async def play_book_chain(
    link: GameLink,
    book: questbook.QuestBook,
    quests: list[questbook.Quest],
    *,
    after_first_kill: Callable[[questbook.Quest, str, list[str]], Any] | None = None,
) -> dict[str, str]:
    """A debug player for any book description: no Garet ids, items, stages or dialogs are hardcoded."""
    results: dict[str, str] = {}
    saved = False

    async def vars(*names: str) -> dict[str, int]:
        return dict((await link.story_state(vars=list(names))).get("vars", {}))

    async def has(item: str, npc: str = "HERO") -> int:
        return int(await link.evaluate(["call", "NPC_HASITEMS", ["inst", npc], ["sym", item]]) or 0)

    async def pick(item: str) -> None:
        deadline = time.monotonic() + 5
        while True:
            try:
                await link.debug("player_take", item=item, distance=800)
                return
            except GameError as err:
                if "the hero could not pick it up now" not in str(err) or time.monotonic() >= deadline:
                    raise
                await asyncio.sleep(0.25)

    async def idle() -> None:
        for _ in range(40):
            if not (await link.story_state()).get("dialog"):
                return
            await asyncio.sleep(0.5)
        raise RuntimeError("the previous dialog did not finish")

    async def choose(npc: str, node: str) -> None:
        await link.debug("player_goto", to=npc, distance=300)
        await idle()
        if node not in await link.offered_dialogs(npc):
            raise RuntimeError(f"{npc} does not offer {node}: {await link.offered_dialogs(npc)}")
        await link.choose_dialog(npc, node)

    for qi, q in enumerate(quests):
        status, stage_var = q.id, questbook.stage_var(q)
        try:
            if qi:
                results[f"opened_{qi + 1}"] = (
                    "done" if questbook.give_node(q) in await link.offered_dialogs(q.giver) else "failed"
                )
            await choose(q.giver, questbook.give_node(q))
            if not await _until_async(
                lambda status=status: vars(status), lambda v, status=status: v.get(status) == 1, 15
            ):
                raise RuntimeError(f"quest was not given: {await vars(status, stage_var)}")
            for si, stage in enumerate(q.stages, 1):
                last = si == len(q.stages)
                # World goals first: the last dialog is the turn-in and must wait for all others.
                ordered = sorted(enumerate(stage.goals, 1), key=lambda x: x[1].kind in questbook.TALKS)
                for gi, goal in ordered:
                    await idle()
                    gid = questbook.goal_id(q, si, gi)

                    async def counts(gid: str = gid) -> int:
                        return next((t["count"] for t in await link.triggers() if t["id"] == gid), 0)

                    if goal.kind not in questbook.TALKS and await counts() >= 1:
                        continue
                    if goal.kind == questbook.REACH:
                        if goal.point:
                            await link.debug("player_goto", waypoint=goal.point)
                        else:
                            await link.debug("teleport", near=goal.npc, distance=max(50, goal.radius // 2))
                            await asyncio.sleep(0.5)
                    elif goal.kind == questbook.HAVE:
                        if goal.npc:
                            await link.debug("kill", npc=goal.npc)
                            await link.debug("teleport", near=goal.npc, distance=100)
                            await asyncio.sleep(0.5)
                            before = await has(goal.item, goal.npc)
                            # v37 only picks things off the ground. Use the existing inventory transfer
                            # for debug looting, then drop/pick the real transferred item through player_take.
                            await link.story_effect(
                                goal.npc, [{"do": "give", "item": goal.item, "count": goal.count}], check=True
                            )
                            after = await has(goal.item, goal.npc)
                            if before - after != goal.count:
                                raise RuntimeError(f"looting {goal.npc}: {goal.item} {before} -> {after}")
                            for _ in range(goal.count):
                                await link.debug("player_drop", item=goal.item)
                                await asyncio.sleep(2)
                        elif goal.point:
                            await link.debug("player_goto", waypoint=goal.point)
                        for _ in range(max(0, goal.count - await has(goal.item))):
                            await pick(goal.item)
                    elif goal.kind == questbook.KILL:
                        states = (await link.story_state(npcs=goal.npcs)).get("npcs", {})
                        alive = [n for n in goal.npcs if states.get(n, {}).get("state") == "alive"]
                        for ni, npc in enumerate(alive):
                            await link.debug("kill", npc=npc)
                            if not saved and ni == 0 and len(alive) >= 2 and after_first_kill is not None:
                                await asyncio.sleep(1)
                                await after_first_kill(q, npc, alive[1:])
                                saved = True
                    elif goal.kind in questbook.TALKS:
                        if goal.kind == questbook.BRING and goal.point and await has(goal.item) < goal.count:
                            await link.debug("player_goto", waypoint=goal.point)
                            for _ in range(goal.count - await has(goal.item)):
                                await pick(goal.item)
                        if qi + 1 < len(quests) and last:
                            next_q = quests[qi + 1]
                            results[f"locked_until_done_{qi + 2}"] = (
                                "done"
                                if questbook.give_node(next_q) not in await link.offered_dialogs(next_q.giver)
                                else "failed"
                            )
                        node = questbook.done_node(q) if last else questbook.goal_node(gid)
                        await choose(goal.npc, node)
                    elif goal.kind == questbook.WAIT:
                        for _ in range(goal.days):
                            await link.debug("time", hour=23, minute=0)
                            await link.debug("time", hour=0, minute=0)
                    elif goal.kind == questbook.VANILLA:
                        target_state = questbook.VANILLA_STATES[goal.state]
                        if not await _until_async(
                            lambda quest=goal.quest: vars(quest),
                            lambda v, quest=goal.quest, target=target_state: v.get(quest) == target,
                            15,
                        ):
                            raise RuntimeError(
                                f"vanilla quest {goal.quest} has not reached {goal.state}: "
                                f"{await vars(goal.quest)}"
                            )
                    if goal.kind not in questbook.TALKS:
                        if not await _until_async(counts, lambda value: value >= 1, 15):
                            raise RuntimeError(f"goal {gid} did not fire: {await vars(status, stage_var)}")
                target = len(q.stages) + 1 if last else si + 1
                if not await _until_async(
                    lambda stage_var=stage_var: vars(stage_var),
                    lambda v, stage_var=stage_var, target=target: v.get(stage_var) == target,
                    15,
                ):
                    raise RuntimeError(f"stage {si} did not end: {await vars(status, stage_var)}")
            done = (await vars(status)).get(status) == 2
            results[f"quest_{qi + 1}"] = f"{'done' if done else 'failed'} ({q.id} «{q.title}»)"
        except (GameError, TimeoutError, RuntimeError) as err:
            results[f"quest_{qi + 1}"] = f"failed ({q.id} «{q.title}»: {err})"
            return results
    return results


async def _peddler(link: GameLink, actors: Cast) -> tuple[NpcSpec, str] | str:
    """Garet, a peddler at Lobart's farm made like a nameless townsman, standing by day at the farm's point
    nearest Hilda's day and by night at the next one; and his point by day. Or why there is none."""
    people = {p["npc"]: p for p in await link.story_people()}
    towns = sorted(n for n in people if n.startswith("VLK_") and TEMPLATES.fullmatch(n))
    hilda = people.get(HILDA)
    if not towns or hilda is None:
        return f"no townsman {towns} or no Hilda"
    # two points of the farm near Hilda's day, none of hers: he stands at one by day and at the other by night
    home = next((e["pos"] for e in hilda.get("routine", []) if e.get("pos")), None)
    hers = {e["waypoint"] for e in hilda.get("routine", [])}
    points = [w for w in await link.waypoints() if not w.get("fp") and w["name"].startswith("NW_FARM1_")]
    if home is None or len(points) < 2:
        return "no points of the farm"
    by_day, by_night = [
        w["name"]
        for w in sorted(points, key=lambda w: math.dist((w["x"], w["y"], w["z"]), home))
        if w["name"] not in hers
    ][:2]
    spec = NpcSpec(
        npc=actors.next_id(),
        like=towns[0],
        at=by_day,
        name="Гарет",
        guild="GIL_BAU",
        look=NpcLook(
            gender="MALE", head="Hum_Head_Pony", face="FACE_N_NORMAL05", skin="BODYTEX_N", armor="ITAR_BAU_M"
        ),
        items=[NpcThing(item="ITMI_GOLD", count=40), NpcThing(item="ITFO_APPLE", count=5)],
        routine=[
            RoutineEntry(start="06:00", end="21:00", state="ZS_STAND_ARMSCROSSED", waypoint=by_day),
            RoutineEntry(start="21:00", end="06:00", state="ZS_STAND_WP", waypoint=by_night),
        ],
    )
    return spec, by_day


async def cast_character(link: GameLink, actors: Cast, slot: int = 7) -> dict[str, str]:
    """Stage 2 of the director as author, layer 2 (docs/director.md): a character of the director's, on the
    game.

    A peddler at Lobart's farm, made like a nameless townsman: his own name, guild, look, things and day.
    A save before he comes (`slot`), one after (`slot` + 1). Checks:
    - rejected: a description with a person of the plot as the template and a known face is refused, and
      nothing is made;
    - made: he is in the world by his name, at his point;
    - guild, look, things: of the guild, in the clothes and with the things he was given;
    - day: his routine is the one given, not the template's;
    - again: asked for once more, the game leaves him as he is (`there`), still one;
    - after_load: after a load of the save made after he came, the server asks for him again and the game
      has him from the save: one, the same name, guild, clothes, things and day;
    - before: after a load of the save made before he came, he is not in the world and not on the branch."""
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    await link.wait_world()
    if link.session is None:
        await wait_message(link, Session, timeout=10)
    await asyncio.sleep(1)

    found = await _peddler(link, actors)
    if isinstance(found, str):
        return _logged({"made": f"failed ({found})"})
    spec, by_day = found
    me = spec.npc

    async def save(at: int) -> None:
        await _save_or_fail(link, at)

    async def load(at: int) -> None:
        loaded = asyncio.ensure_future(wait_message(link, Session, lambda m: m.reason == "load"))
        await link.debug("load", at)
        await loaded
        await link.wait_world()
        await asyncio.sleep(2)  # the server asks the game for the branch's characters again

    async def seen() -> tuple[str, str]:
        n = (await link.story_state(npcs=[me])).get("npcs", {}).get(me, {})
        return n.get("state", "absent"), n.get("name", "")

    async def count() -> int:
        return sum(1 for p in await link.story_people() if p["npc"] == me)

    async def his() -> dict[str, Any]:
        self_ = ["inst", "SELF"]
        armor = ["call", "HLP_GETINSTANCEID", ["call", "NPC_GETEQUIPPEDARMOR", self_]]
        return {
            "guild": await link.evaluate(["==", ["field", self_, "GUILD"], ["var", "GIL_BAU"]], npc=me),
            "armor": await link.evaluate(["==", armor, ["sym", "ITAR_BAU_M"]], npc=me),
            "gold": await link.evaluate(["call", "NPC_HASITEMS", self_, ["sym", "ITMI_GOLD"]], npc=me),
            "apples": await link.evaluate(["call", "NPC_HASITEMS", self_, ["sym", "ITFO_APPLE"]], npc=me),
        }

    def minutes(clock: str) -> int:
        return int(clock[:2]) * 60 + int(clock[3:])

    given = [(minutes(e.start), minutes(e.end), e.state, e.waypoint) for e in spec.routine]

    async def day() -> list[tuple[Any, ...]]:
        person = next((p for p in await link.story_people() if p["npc"] == me), {})
        return [(e["start"], e["end"], e["state"], e["waypoint"]) for e in person.get("routine", [])]

    def dressed(h: dict[str, Any]) -> bool:
        return bool(h["guild"]) and bool(h["armor"]) and h["gold"] >= 40 and h["apples"] >= 5

    await save(slot)  # before he comes

    assert spec.look is not None
    # a person of the plot to be made like, the face of one: refused before the game hears of it
    bad = spec.model_copy(
        update={"like": "BAU_950_LOBART", "look": spec.look.model_copy(update={"face": "FACE_N_LEE"})}
    )
    refused, why = await actors.introduce(bad)
    check("rejected", refused is None and len(why) == 2 and await seen() == ("absent", ""), "; ".join(why))

    made, why = await actors.introduce(spec)
    check(
        "made",
        made is not None and made.get("state") == "made" and await seen() == ("alive", "Гарет"),
        made or why,
    )
    if made is None:
        return _logged(results)
    await link.debug("player_goto", waypoint=by_day)
    await asyncio.sleep(1)
    near = (await link.story_state(npcs=[me], near="PC_HERO")).get("npcs", {}).get(me, {})
    check("at", near.get("distance", 1e9) < 1500, f"{near} from {by_day}")
    ok = await _until_async(his, dressed, 10)  # the clothes go on with his first steps (AI_EquipArmor)
    h = await his()
    check("look", ok, h)
    now = await day()
    check("day", now == given, f"{now} for {given}")
    again = await link.story_npc(spec)
    check("again", again.get("state") == "there" and await count() == 1, again)

    await save(slot + 1)
    await load(slot + 1)
    h, now, n = await his(), await day(), await count()
    check(
        "after_load",
        await seen() == ("alive", "Гарет")
        and n == 1
        and dressed(h)
        and now == given
        and actors.characters() == [spec],
        f"{n} of him; {h}; {now}",
    )

    await load(slot)
    check(
        "before",
        await seen() == ("absent", "") and actors.characters() == [],
        f"{await seen()}; {[c.npc for c in actors.characters()]}",
    )
    return _logged(results)


# Garet as a person (layer 3): a card, a voice, a past, what he knows, whom he knows
GARET = Person(
    ru="Ты — Гарет, бродячий торговец. Скуп на слова, но честен в цене и не любит, когда торгуются зря.",
    en="A taciturn peddler who keeps his prices fair.",
    greed=3,
    honesty=4,
    courage=2,
    talk=2,
    pushy=3,
    strangers=4,
    voice="A weathered peddler in his forties with a low, dry male voice, speaking unhurriedly",
    past="Ты пришёл из Венгарда с последним кораблём и с тех пор торгуешь по фермам.",
    knows=("У каменных ступеней к башне Ксардаса засели бандиты.",),
    ties=(PersonTie(npc=HILDA, how="Хильда с фермы Лобарта: берёшь у неё молоко и сыр"),),
)


async def cast_person(
    link: GameLink, actors: Cast, dialogue: DialogueManager, rumors: Rumors, slot: int = 7
) -> dict[str, str]:
    """Stage 2 of the director as author, layer 3 (docs/director.md): a character of the director's is a
    person, on the game. Garet the peddler (as in `cast`) comes with who he is (GARET). Checks:
    - persona: his persona is his card, past, knowledge and acquaintances, in a voice of his own;
    - talk: the hero speaks to him, and his model has all that in front of it;
    - knows_hilda: Hilda and he are acquainted (the rumors' ties), and with him beside her she calls him by
      name, not «незнакомец»;
    - after_load: after a save and a load he is the same person, in the same voice."""
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    await link.wait_world()
    if link.session is None:
        await wait_message(link, Session, timeout=10)
    await asyncio.sleep(1)
    found = await _peddler(link, actors)
    if isinstance(found, str):
        return _logged({"made": f"failed ({found})"})
    spec, by_day = found
    me = spec.npc
    made, why = await actors.introduce(spec, GARET)
    check("made", made is not None and made.get("state") == "made", made or why)
    if made is None:
        return _logged(results)

    persona = dialogue.personas.get(me, spec.name)
    parts = (GARET.ru, GARET.past, GARET.knows[0], GARET.ties[0].how)
    check(
        "persona",
        all(t in persona.instructions for t in parts)
        and persona.voice == GARET.voice
        and "~" in persona.voice_id,
        persona.voice_id,
    )

    def told(llm: FakeBackend) -> str:
        return "\n".join([llm.calls[0][0], *(str(getattr(m, "content", "")) for m in llm.calls[0][1])])

    # the hero speaks to him
    await link.debug("player_goto", waypoint=by_day)
    await asyncio.sleep(1)
    dialogue.llm = llm = FakeBackend("Гарет я. Торгую понемногу.", delay=0)
    await link.debug("type", text="Гарет, кто ты такой?")
    ok = await _until(lambda: bool(llm.calls), 20)
    prompt = told(llm) if ok else ""
    check("talk", ok and all(t in prompt for t in parts), f"{len(llm.calls)} call(s)")
    await _until(lambda: not dialogue.speaking(me), 15)

    # Hilda knows him: beside her, he is Garet to her
    ok = await _until(lambda: rumors.social is not None and me in rumors.social.ties.get(HILDA, {}), 15)
    bonds = rumors.social.ties[HILDA][me].bonds if ok and rumors.social is not None else []
    check("tie", ok and bonds == [GARET.ties[0].how], bonds)
    await link.debug("player_goto", to=HILDA, distance=250)
    # the talk held him to listen: free, he steps beside her
    for _ in range(20):
        try:
            await link.detach(me)
        except GameError:
            pass
        try:
            await link.debug("npc_goto", npc=me, to=HILDA, distance=150, face=HILDA)
            break
        except GameError:
            await asyncio.sleep(0.5)
    await asyncio.sleep(2)
    for _ in range(40):  # her own greeting may start as he comes in
        if not (await link.story_state()).get("dialog"):
            break
        await asyncio.sleep(0.5)
    dialogue.llm = llm = FakeBackend("Это Гарет, торговец.", delay=0)
    await link.debug("type", text="Хильда, кто это рядом с тобой?")
    ok = await _until(lambda: bool(llm.calls), 20)
    prompt = told(llm) if ok else ""
    seen = [line for line in prompt.split("\n") if "Гарет" in line or "незнакомец" in line]
    check("knows_hilda", ok and any("Гарет" in line for line in seen), seen[:4])
    await _until(lambda: not dialogue.speaking(HILDA), 15)

    # a save and a load
    await _save_or_fail(link, slot)
    loaded = asyncio.ensure_future(wait_message(link, Session, lambda m: m.reason == "load"))
    await link.debug("load", slot)
    await loaded
    await link.wait_world()
    await asyncio.sleep(2)
    again = dialogue.personas.get(me, spec.name)
    check(
        "after_load",
        actors.person(me) == GARET
        and again.voice_id == persona.voice_id
        and again.instructions == persona.instructions,
        again.voice_id,
    )
    return _logged(results)


async def own_symbols(link: GameLink, slot: int = 7) -> dict[str, str]:
    """Stage 2 of the director as author, layer 1 (docs/director.md): script symbols of our own on the game.

    A story variable, an npc of the director's made like a nameless farmer of the scripts, another variable
    after it; then two saves and loads, a variable changed in between. Checks:
    - made: the npc stands in front of the hero, by the farmer's name, a symbol of the npc kind;
    - like: its day and its clothes are the farmer's (the farmer's code made it, on it and not on the farmer),
      and the farmer is still one of his own;
    - after_load, again: after each load the npc is there once, where it stood, bound to its own symbol
      (Hlp_GetInstanceID of it), and the variables have the values they had at the save;
    - vanilla: Hilda and the farmer are where and who they were."""
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    await link.wait_world()
    if link.session is None:
        await wait_message(link, Session, timeout=10)
    await asyncio.sleep(1)
    own, first, second = "DIR_NPC_E2E", "DIR_E2E_A", "DIR_E2E_B"

    people = {p["npc"]: p for p in await link.story_people()}
    farmers = sorted(n for n in people if re.fullmatch(r"BAU_\d+_BAUER", n))
    if not farmers:
        return _logged({"made": "failed (no nameless farmer in the world)"})
    like = farmers[0]

    # in the order of the save's entry: a variable, the npc, a variable
    await link.patch_graph([{"op": "vars", "vars": {first: 7}}])
    try:
        made = await link.own_npc(own, like, distance=300)
    except GameError as err:
        return _logged({"made": f"failed ({err})"})
    await link.patch_graph([{"op": "vars", "vars": {second: 9}}])
    # their values the save keeps, not the first ones a patch would make again
    values = {first: 17, second: 19}
    await link.story_effect(HILDA, [{"do": "set", "var": k, "value": v} for k, v in values.items()])

    async def state() -> dict[str, Any]:
        return await link.story_state(
            vars=list(values), npcs=[own, like, HILDA], symbols=[own], near="PC_HERO"
        )

    async def bound() -> Any:
        return await link.evaluate(["call", "HLP_GETINSTANCEID", ["inst", "SELF"]], npc=own)

    def shown(s: dict[str, Any], npc: str) -> tuple[str, str]:
        n = s.get("npcs", {}).get(npc, {})
        return n.get("state", "absent"), n.get("name", "")

    before = await state()
    me = before.get("npcs", {}).get(own, {})
    check(
        "made",
        shown(before, own) == ("alive", people[like]["name"])
        and me.get("distance", 1e9) < 600
        and before.get("symbols", {}).get(own) == "npc",
        f"{made}; {me}",
    )

    def day(person: dict[str, Any]) -> list[dict[str, Any]]:
        keys = ("start", "end", "state", "waypoint")
        return [{k: e.get(k) for k in keys} for e in person.get("routine", [])]

    now = await link.story_people()
    mine = [p for p in now if p["npc"] == own]
    theirs = [p for p in now if p["npc"] == like]
    same = bool(mine) and day(mine[0]) == day(people[like])
    # the farmer's code dressed this one, not the farmer again
    armor = ["call", "NPC_HASEQUIPPEDARMOR", ["inst", "SELF"]]
    dressed = (await link.evaluate(armor, npc=own), await link.evaluate(armor, npc=like))
    check(
        "like",
        same and len(theirs) == 1 and shown(before, like)[0] == "alive" and dressed[0] == dressed[1],
        f"{len(theirs)} {like}; armor {dressed}",
    )
    vanilla = (shown(before, HILDA), shown(before, like))

    async def save_and_load() -> None:
        await _save_or_fail(link, slot)
        loaded = asyncio.ensure_future(wait_message(link, Session, lambda m: m.reason == "load"))
        await link.debug("load", slot)
        await loaded
        await link.wait_world()
        await asyncio.sleep(1)

    for name in ("after_load", "again"):
        if name == "again":
            # the next save writes what is now, not what came from the last one
            values[first] += 10
            await link.story_effect(HILDA, [{"do": "set", "var": first, "value": values[first]}])
        at = (await state()).get("npcs", {}).get(own, {}).get("distance", 0)
        await save_and_load()
        after = await state()
        me = after.get("npcs", {}).get(own, {})
        count = sum(1 for p in await link.story_people() if p["npc"] == own)
        try:
            index = await bound()
        except GameError as err:
            index = str(err)
        kept = {k: after.get("vars", {}).get(k) for k in values}
        check(
            name,
            me.get("state") == "alive"
            and count == 1
            and abs(me.get("distance", 1e9) - at) < 300
            and index == made.get("index")
            and kept == values,
            f"{me}; {count} of it; Hlp_GetInstanceID {index}, made {made.get('index')}; {kept}",
        )
    last = await state()
    check(
        "vanilla",
        (shown(last, HILDA), shown(last, like)) == vanilla,
        f"{vanilla} -> {(shown(last, HILDA), shown(last, like))}",
    )
    return _logged(results)


async def story_triggers(link: GameLink, story: StoryPatches, slot: int = 7) -> dict[str, str]:
    """Stage 3 of the director as author, layer 1 (docs/director.md): the triggers of the bridge.

    A trigger for every event, put into the branch like the director's other changes, with a save and a load
    in the middle. Checks:
    - start: a trigger without a condition fires at once, once: it puts a thing at a point nearby;
    - item: the hero picks it up, and a trigger on his having it fires, its self the hero;
    - place: one that repeats fires on every coming into a place, not while he stays or is away;
    - dialog: one on a dialog of the director's fires when it is chosen, its self the npc;
    - after_load: after a save and a load every count is what it was, and none fired again: the first one
      not, though its condition holds, the place's not, though the hero loaded inside it;
    - place_again: coming into the place again after the load counts on;
    - hour: one on the time of day fires when the clock gets there;
    - death: one on the death of an npc of the director's fires when the hero kills it, its self the dead
      and its other the killer."""
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    await link.wait_world()
    if link.session is None:
        await wait_message(link, Session, timeout=10)
    await asyncio.sleep(1)
    own = "DIR_NPC_T"
    start, item_t, place_t, hour_t, death_t, dialog_t = (
        "DIR_E2E_START",
        "DIR_E2E_ITEM",
        "DIR_E2E_PLACE",
        "DIR_E2E_HOUR",
        "DIR_E2E_DEATH",
        "DIR_E2E_DIALOG",
    )
    counts = [start, item_t, place_t, hour_t, death_t, dialog_t]
    marks = ["DIR_E2E_STARTS", "DIR_E2E_GOT", "DIR_E2E_TOLD", "DIR_E2E_DEAD", "DIR_E2E_KILLER"]

    def own_id(of: str) -> Expr:
        return ["call", "HLP_GETINSTANCEID", ["inst", of]]

    def mark(var: str, value: Expr) -> dict[str, Any]:
        return {"do": "set", "target": ["var", var], "value": value}

    def plus(var: str) -> dict[str, Any]:
        return mark(var, ["+", ["var", var], ["int", 1]])

    # the points: a waypoint for the thing, another for the place, apart from each other and from the hero
    snap = await link.snapshot(HILDA, fresh=True)
    if snap.player is None:
        return _logged({"start": "failed (no hero in the snapshot)"})
    hero_at = snap.player.pos

    def far(p: dict[str, Any], q: Sequence[float], cm: float) -> bool:
        return math.dist((p["x"], p["z"]), (q[0], q[2])) >= cm

    points = sorted(
        (w for w in await link.waypoints() if not w.get("fp")),
        key=lambda w: math.dist((w["x"], w["z"]), (hero_at[0], hero_at[2])),
    )
    thing_at = next((w for w in points if far(w, hero_at, 700)), None)
    place_at = (
        next(
            (w for w in points if far(w, hero_at, 1500) and far(w, (thing_at["x"], 0, thing_at["z"]), 1500)),
            None,
        )
        if thing_at
        else None
    )
    if thing_at is None or place_at is None:
        return _logged({"start": "failed (no waypoints around the hero)"})
    a, b = thing_at["name"], place_at["name"]
    # a thing the hero has none of
    thing = None
    for candidate in ("ITMI_SAW", "ITMI_BROOM", "ITMI_RAKE", "ITMI_SCOOP", "ITMI_BRUSH", "ITMI_PLIERS"):
        if await link.evaluate(triggers.hero_has(candidate)) == 0:
            thing = candidate
            break
    if thing is None:
        return _logged({"start": "failed (the hero has every thing of the list)"})
    hour = int((await link.story_state()).get("hour", 8))
    at = ((hour + 6) % 24, (hour + 7) % 24)

    try:
        farmers = sorted(
            p["npc"] for p in await link.story_people() if re.fullmatch(r"BAU_\d+_BAUER", p["npc"])
        )
        await link.own_npc(own, farmers[0], distance=400)
    except (GameError, IndexError) as err:
        return _logged({"start": f"failed (no npc of the director's: {err})"})

    talk = "DIR_E2E_TALK"
    ops: list[dict[str, Any]] = [
        {"op": "vars", "vars": dict.fromkeys(marks, 0)},
        {
            "op": "add_node",
            "node": {
                "id": talk,
                "npc": HILDA,
                "description": "Как дела на ферме?",
                "permanent": True,
                "lines": [{"speaker": "self", "text": "Работы много, как всегда."}],
            },
        },
        triggers.trigger(
            start,
            triggers.STATE,
            [
                {"do": "call", "call": ["call", "WLD_INSERTITEM", ["sym", thing], ["str", a]]},
                plus("DIR_E2E_STARTS"),
            ],
        ),
        triggers.trigger(
            item_t, triggers.STATE, [mark("DIR_E2E_GOT", own_id("SELF"))], when=triggers.hero_has(thing)
        ),
        # the count is all these two do
        triggers.trigger(place_t, triggers.STATE, [], when=triggers.hero_near_point(b, 400), repeat=True),
        triggers.trigger(
            dialog_t, triggers.DIALOG, [mark("DIR_E2E_TOLD", own_id("SELF"))], npc=HILDA, node=talk
        ),
        triggers.trigger(hour_t, triggers.STATE, [], when=triggers.time_between((at[0], 0), (at[1], 0))),
        triggers.trigger(
            death_t,
            triggers.DEATH,
            [mark("DIR_E2E_DEAD", own_id("SELF")), mark("DIR_E2E_KILLER", own_id("OTHER"))],
            npc=own,
        ),
    ]
    try:
        for op in ops:
            await story.apply(op)
    except GameError as err:
        return _logged({"start": f"failed ({err})"})

    async def values() -> dict[str, int]:
        state = await link.story_state(vars=[*counts, *marks])
        return {k: int(v) for k, v in state.get("vars", {}).items()}

    async def goto(point: str) -> None:
        await link.debug("player_goto", waypoint=point)
        await asyncio.sleep(1.0)  # four times a second

    hero = await link.evaluate(own_id("HERO"))
    hilda = await link.evaluate(own_id("SELF"), npc=HILDA)
    mine = await link.evaluate(own_id("SELF"), npc=own)

    await asyncio.sleep(1.0)
    v = await values()
    check("start", v.get(start) == 1 and v.get("DIR_E2E_STARTS") == 1, f"{v.get(start)}×, at {a}")

    await goto(a)
    try:
        await link.debug("player_take", item=thing, distance=500)
    except GameError as err:
        check("item", False, f"no {thing} at {a}: {err}")
    else:
        await asyncio.sleep(1.0)
        v = await values()
        check(
            "item",
            v.get(item_t) == 1 and v.get("DIR_E2E_GOT") == hero,
            f"{v.get(item_t)}×, self {v.get('DIR_E2E_GOT')}, hero {hero}",
        )

    seen = []
    for point in (b, a, b):
        await goto(point)
        seen.append((await values()).get(place_t))
    check("place", seen == [1, 1, 2], f"{b}: {seen}")

    try:
        await link.choose_dialog(HILDA, talk)
        await asyncio.sleep(1.0)
    except GameError as err:
        check("dialog", False, err)
    else:
        v = await values()
        check(
            "dialog",
            v.get(dialog_t) == 1 and v.get("DIR_E2E_TOLD") == hilda,
            f"{v.get(dialog_t)}×, self {v.get('DIR_E2E_TOLD')}, Hilda {hilda}",
        )

    before = await values()
    await _save_or_fail(link, slot)
    loaded = asyncio.ensure_future(wait_message(link, Session, lambda m: m.reason == "load"))
    await link.debug("load", slot)
    await loaded
    await link.wait_world()
    await asyncio.sleep(3.0)  # the branch's changes come back, the triggers count again
    after = await values()
    listed = {t["id"]: t for t in await link.triggers()}
    broken = [t["id"] for t in listed.values() if t.get("broken")]
    check(
        "after_load",
        after == before and set(counts) <= set(listed) and not broken,
        f"{before} -> {after}; listed {sorted(listed)}, broken {broken}",
    )

    seen = []
    for point in (a, b):
        await goto(point)
        seen.append((await values()).get(place_t))
    check("place_again", seen == [2, 3], seen)

    await link.debug("time", hour=at[0], minute=30)
    await asyncio.sleep(1.0)
    v = await values()
    check("hour", v.get(hour_t) == 1, f"{v.get(hour_t)}× at {at[0]}:30")

    try:
        await link.debug("kill", instance=own)
        await asyncio.sleep(1.5)  # deaths are looked for twice a second
    except GameError as err:
        check("death", False, err)
    else:
        v = await values()
        check(
            "death",
            v.get(death_t) == 1 and v.get("DIR_E2E_DEAD") == mine and v.get("DIR_E2E_KILLER") == hero,
            f"{v.get(death_t)}×, self {v.get('DIR_E2E_DEAD')} ({mine}), "
            f"other {v.get('DIR_E2E_KILLER')} ({hero})",
        )
    return _logged(results)
