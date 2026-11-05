"""Стенд офлайн-пилота фазы 2 общей памяти NPC (замысел и приёмка — README.md рядом).

Запуск из server/, команды и порядок — README.md, раздел «Данные и запуск». `selftest` идёт без сети.

- `mind DIR --carry DB [--conv …]` — ход Sol после каждого ответа NPC и замыкающий ход (ум v3), но память —
  пункты фазы 1: правки применяет `memory_items` теми же правилами (слот, правда, пределы, остывание);
  мелочь забывается только в замыкающем ходе. Перенос — по цепочке сохранений. Ходы — в `minds.jsonl`.
- `opening DIR --probes FILE` — открывающий ход Sol на новой встрече через час после каждого разговора из
  `ask_after`: «Голосу» к первой фразе героя и правки к копии памяти. В `openings.jsonl`.
- `answer DIR --probes FILE --from RUN1` — ответ Gemma `note` по заметке открывающего хода; ответы `bare`,
  `items`, `sol` и их суждения копируются из прогона фазы 1 без новых вызовов.
- `judge DIR [--stride N]` — судья состояний фазы 1 (judge_items.md) по ходам фазы 2 и по ходам ума v3 второго
  круга в тех же точках; судья проб (judge_probe.md) по ответам `note`.
- `sheet DIR` — слепой лист: `bare`, `note`, `sol` в перемешанном порядке, ключ отдельно.
- `summary DIR` — числа пунктов 1–6 приёмки.

Из фазы 1 и ума v3 всё берётся импортом. Скопировано с правкой: `fold_turn` — это `memory_items.fold` без
забывания мелочи в ходах посреди разговора; `sol_turn` — это `mind.mind_stream` ума v3 с готовым списком
сообщений.
"""

from __future__ import annotations

import argparse
import asyncio
import importlib.util
import json
import random
import sys
import time
from collections import Counter
from contextlib import aclosing
from copy import deepcopy
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any

from gothic_ai.llm import Done, LLMBackend, Message, TextDelta, ToolCall
from gothic_ai.trace import item_from_json


def _load(name: str, path: Path) -> Any:
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


HERE = Path(__file__).resolve().parent
PHASE1 = HERE.parent / "2026-11-04-shared-memory-phase1"
stand = _load("_p2_stand", PHASE1 / "stand.py")  # it loads memory_items and ум v3 (mind.py) itself
mi, mind, pilot = stand.mi, stand.mind, stand.pilot

TURNS_DIR, V3_DIR = stand.TURNS_DIR, stand.V3_DIR
EFFORT = stand.EFFORT  # Sol's reasoning effort, as ум v3
TIMEOUT_S = mind.TIMEOUT_S  # one budget for a turn, the retry included, as ум v3
SLOT = 2  # Gemma's slot given to this pilot
MARKER = "[Правки]"  # «Голосу» is ready when this heading comes
COPIED = ("bare", "items", "sol")  # answers of phase 1, copied with their judgments
VARIANTS = ("bare", "items", "note", "sol")
SHEET = ("bare", "note", "sol")
THOUGHTS_HEAD = "[Твои мысли — их слышишь только ты]"
HOLD = "Держись своих мыслей: в них то, что ты помнишь и как к нему относишься."
# the acceptance table of README.md
TOKENS_MEDIAN, TOKENS_MAX, TOTAL_S, FITS, NOTE_FITS = 250, 600, 7.5, 0.85, 0.90
OPENING_NOTE_S, NOTE_OF_SOL = 6.0, 0.8


def prompt(name: str) -> str:
    return (HERE / name).read_text(encoding="utf-8")


def elapsed_text(minutes: int) -> str:
    """«1 ч», «2 дн. 3 ч», as render_for_sol writes the time since the last fold."""
    if minutes < 60:
        return f"{max(0, minutes)} мин"
    days, hours = divmod(minutes // 60, 24)
    return " ".join(([f"{days} дн."] if days else []) + ([f"{hours} ч"] if hours else []))


def with_elapsed(situation: str, minutes: int) -> str:
    """The situation with «С прошлого раза прошло: …» after its clock."""
    lines = situation.splitlines()
    at = next((n + 1 for n, line in enumerate(lines) if line.startswith("Время:")), len(lines))
    return "\n".join([*lines[:at], "С прошлого раза прошло: " + elapsed_text(minutes), *lines[at:]])


def save(path: Path, records: dict[Any, dict[str, Any]], order: dict[Any, int]) -> None:
    pilot.save_rows(path, sorted(records.values(), key=lambda r: order.get(_key(r), len(order))))


def _key(record: dict[str, Any]) -> Any:
    return record.get("id") or record.get("conv")


def steps_of(group: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """A talk's mind turns: one before each phrase of the hero (after the npc's last answer), the closing
    one."""
    return [*group, mind.closing_turn(group)]


def turn_order(world: Any) -> dict[str, int]:
    ids = [turn["id"] for group in world.groups for turn in steps_of(group)]
    return {identifier: n for n, identifier in enumerate(ids)}


def conv_order(world: Any) -> dict[str, int]:
    return {conv: n for n, conv in enumerate(world.by_conv)}


# --- what a mind turn sees ---------------------------------------------------------------------------


@dataclass
class View:
    """The talk's numbered lines a mind turn sees: those before the hero's phrase of the turn (all of them
    in the closing turn), the chronicle facts and the branch's facts before it."""

    lines: list[Any]
    chronicle: list[Any]
    entries: list[tuple[int, dict[str, Any]]]
    hero_turns: list[tuple[int, str, str]]
    found: bool = True  # the hero's line of the turn is in the talk

    @property
    def line_map(self) -> dict[int, str]:
        return {line.number: line.turn for line in self.lines if line.turn is not None}

    @property
    def remark_map(self) -> dict[int, int]:
        return {line.number: line.fact for line in self.chronicle}

    def text(self) -> str:
        return "\n".join(f"{line.number}. {line.text}" for line in self.lines)

    def chronicle_text(self) -> str:
        return "\n".join(f"{line.number}. {line.text}" for line in self.chronicle)


def view(talk: Any, turn: dict[str, Any]) -> View:
    """The numbering is the talk's, as the fold of phase 1 has it: the numbers do not change from turn to
    turn, a later turn sees more of them."""
    said = [line for line in talk.lines if line.fact is None]
    facts = [line for line in talk.lines if line.fact is not None]
    if turn.get("closing"):
        return View(said, facts, list(talk.entries), list(talk.hero_turns))
    phrase = turn["phrase"].strip()
    hero = [
        line
        for line in said
        if line.turn == turn["id"]
        and line.text.startswith("Герой: ")
        and line.text.removeprefix("Герой: ").strip() == phrase
    ]
    if not hero:
        return View(said, facts, list(talk.entries), list(talk.hero_turns), found=False)
    seq = next((s for s, identifier, _ in talk.hero_turns if identifier == turn["id"]), None)
    return View(
        [line for line in said if line.number < hero[-1].number],
        [line for line in facts if seq is None or line.fact < seq],
        [(s, f) for s, f in talk.entries if seq is None or s < seq],
        [h for h in talk.hero_turns if seq is None or h[0] < seq],
    )


def mind_messages(turn: dict[str, Any], seen: View, memory: str, text: str) -> list[Any]:
    """ум v3's request (mind.mind_stream) with the talk's numbered lines and the chronicle before the mind."""
    history = [item for data in turn["history"] if (item := item_from_json(data)) is not None]
    head = (
        "[Весь этот разговор — строки с номерами]"
        if turn.get("closing")
        else "[Этот разговор до этой минуты — строки с номерами]"
    )
    messages = [
        *history,
        Message("developer", turn["situation"]),
        Message("developer", head + "\n" + (seen.text() or "—")),
    ]
    if seen.chronicle:
        messages.append(
            Message("developer", "[Хроника игры о тебе — строки с номерами]\n" + seen.chronicle_text())
        )
    messages += [Message("developer", "[Твой ум до этой минуты]\n" + memory), Message("developer", text)]
    return messages


def opening_messages(ctx: Any, situation: str, memory: str, text: str) -> list[Any]:
    """The history the game gives at the new meeting (summary, ≤ 40 lines, «Новая встреча»), the calm
    situation an hour later, the memory after the closing turn."""
    return [
        *ctx.history,
        Message("developer", situation),
        Message("developer", "[Твоя память до этой встречи]\n" + memory),
        Message("developer", text),
    ]


def note_messages(ctx: Any, phrase: str, note: str) -> list[Message]:
    """Gemma's `note`: the history, the brief calm situation, the thoughts of the opening turn, the phrase."""
    hint = stand.prompt("speak_probe.md").rstrip() + "\n" + HOLD
    body = ctx.brief + "\n\n" + THOUGHTS_HEAD + "\n" + note.strip() + "\n\n" + hint
    return [*ctx.history, Message("developer", body), Message("user", phrase)]


def chars(messages: list[Any]) -> int:
    return sum(len(item.content) for item in messages if isinstance(item, Message))


# --- Sol ---------------------------------------------------------------------------------------------


async def sol_turn(
    sol: LLMBackend,
    instructions: str,
    messages: list[Any],
    *,
    tools: list[Any],
    cache_key: str,
    deadline: float,
) -> dict[str, Any]:
    """One request (mind.mind_stream with given messages): text, calls, usage, first token, «Голосу» ready."""
    result: dict[str, Any] = {
        "text": "",
        "calls": [],
        "usage": None,
        "error": None,
        "first_token_s": None,
        "note_ready_s": None,
        "total_s": 0.0,
    }
    started = time.monotonic()
    try:
        async with (
            asyncio.timeout_at(deadline),
            aclosing(
                sol.stream(instructions=instructions, messages=messages, tools=tools, cache_key=cache_key)
            ) as events,
        ):
            async for event in events:
                elapsed = time.monotonic() - started
                if isinstance(event, TextDelta):
                    if result["first_token_s"] is None:
                        result["first_token_s"] = elapsed
                    result["text"] += event.text
                elif isinstance(event, ToolCall):
                    result["calls"].append({"name": event.name, "arguments": event.arguments})
                elif isinstance(event, Done):
                    result["text"] = event.text
                    result["usage"] = asdict(event.usage) if event.usage is not None else None
                if result["note_ready_s"] is None and MARKER in result["text"]:
                    result["note_ready_s"] = elapsed
    except TimeoutError:
        result["error"] = f"Timeout after {TIMEOUT_S:g} s"
    except Exception as error:  # provider errors may carry private endpoints: only the type is kept
        result["error"] = f"Model request failed ({type(error).__name__})"
    result["total_s"] = time.monotonic() - started
    return result


async def sol_call(
    sol: LLMBackend, instructions: str, messages: list[Any], *, tools: list[Any], cache_key: str
) -> dict[str, Any]:
    """Up to two attempts in one budget, as ум v3: the times count from the start of the first."""
    started = time.monotonic()
    result: dict[str, Any] = {}
    for attempt in (1, 2):
        offset = time.monotonic() - started
        result = await sol_turn(
            sol, instructions, messages, tools=tools, cache_key=cache_key, deadline=started + TIMEOUT_S
        )
        result["attempts"] = attempt
        for metric in ("first_token_s", "note_ready_s"):
            if result[metric] is not None:
                result[metric] += offset
        result["total_s"] = time.monotonic() - started
        if result["error"] is None:
            break
    return result


def parse(result: dict[str, Any]) -> dict[str, Any]:
    """«Голосу», the patch lines and [Скажу] as ум v3 reads them (mind.parse_patches)."""
    if result["error"] is not None:
        return {"note": "", "patches": [], "say": [], "parse_error": False}
    return mind.parse_patches(result["text"])


def fold_turn(
    state: dict[str, Any],
    text: str,
    *,
    names: Any,
    lines: dict[int, str],
    remarks: dict[int, int],
    minute: int,
    entries: list[tuple[int, dict[str, Any]]],
    closing: bool,
) -> dict[str, Any]:
    """memory_items.fold for one mind turn: parse, apply, caps, collisions. Trifles and «сейчас» wishes are
    forgotten only in the closing turn, as ум v3 forgets them (mind.forget_events)."""
    shown = [mi._shown(state, c["ids"]) for c in state["collisions"]]
    parsed = mi.parse_patches(text, names=names, lines=lines, remarks=remarks)
    applied = mi.apply_patches(state, parsed, minute=minute, names=names, entries=entries)
    touched = [mi.find_item(state, i) for i in dict.fromkeys(r["id"] for r in applied if r["ok"] and r["id"])]
    parsed["stats"]["unbound_hearsay"] = sum(
        1
        for item in touched
        if item and any(s["kind"] == "сказал" and s["fact"] is None for s in (item["source"], *item["also"]))
    )
    forgotten = mi.forget(state, minute) if closing else []
    again = [pair["ids"] for pair in shown]
    state["collisions_seen"] = [p for p in state["collisions_seen"] if p["ids"] not in again] + shown
    state["collisions"] = mi.find_collisions(state)
    state["folded"] = minute
    return {
        "stats": parsed["stats"],
        "applied": applied,
        "errors": sum(1 for r in applied if not r["ok"]),
        "forgotten": forgotten,
        "collisions": state["collisions"],
    }


# --- mind --------------------------------------------------------------------------------------------


async def think(
    sol: LLMBackend, talk: Any, turn: dict[str, Any], state: dict[str, Any], text: str, names: Any
) -> dict[str, Any]:
    """prepare → Sol → fold_turn → apply_truth for one mind turn; the state changes in place."""
    seen = view(talk, turn)
    minute = mi.minute_of(turn["situation"])
    if minute is None:
        minute = state["minute"] if state["minute"] is not None else talk.minute
    prepared = mi.prepare(state, minute)
    memory = mi.render_for_sol(state)
    messages = mind_messages(turn, seen, memory, text)
    result = await sol_call(
        sol, turn["instructions"], messages, tools=mind.MIND_TOOLS, cache_key=f"mind-{turn['npc']}"
    )
    parsed = parse(result)
    closing = bool(turn.get("closing"))
    report = fold_turn(
        state,
        "\n".join(parsed["patches"]),
        names=names,
        lines=seen.line_map,
        remarks=seen.remark_map,
        minute=minute,
        entries=seen.entries,
        closing=closing,
    )
    truth = mi.apply_truth({talk.npc: state}, seen.entries, minute=minute, hero_turns=seen.hero_turns)
    return {
        "id": turn["id"],
        "conv": turn["conv"],
        "index": turn["index"],
        "closing": closing,
        "npc": turn["npc"],
        "minute": minute,
        "time": mi.time_text(minute),
        "window_s": turn["window_s"],
        **result,
        **parsed,
        "prepare": prepared,
        "memory_before": memory,
        "fold": report,
        "truth": truth,
        "state": deepcopy(state),
        "mind": mi.render_for_sol(state),
        "seen": {"lines": len(seen.lines), "facts": len(seen.chronicle), "found": seen.found},
        "prompt_chars": chars(messages),
    }


def _drop(path: Path, drop) -> None:
    if path.exists():
        pilot.save_rows(path, [r for r in pilot.rows(path) if not drop(r)])


def forget_after(directory: Path, convs: set[str]) -> None:
    """What was built on the redone talks: their openings, the note answers after them, the judgments."""
    _drop(directory / "openings.jsonl", lambda r: r["conv"] in convs)
    _drop(directory / "judge_states.jsonl", lambda r: r["system"] == "items" and r["conv"] in convs)
    for name in ("answers.jsonl", "judge_answers.jsonl"):
        _drop(directory / name, lambda r: r["variant"] == "note" and r["ask_after"] in convs)


async def mind_run(
    directory: Path,
    *,
    carry: Path,
    conv: list[str] | None = None,
    redo: bool = False,
    turns: Path = TURNS_DIR,
    effort: str = EFFORT,
    sol: LLMBackend | None = None,
) -> None:
    stand.start(directory, {"turns": str(turns.resolve()), "carry": str(carry.resolve())})  # noqa: ASYNC240
    world = stand.World(turns, carry)
    path = directory / "minds.jsonl"
    records = {r["id"]: r for r in pilot.rows(path)}
    chosen = set(world.select(conv))
    named = {world.match(part) for part in conv or ()} if redo else set()
    plan: dict[str, int] = {}  # talk → the first turn to think again
    for group in world.groups:  # in time order: a source comes before the talks carried from it
        c, source = group[0]["conv"], world.sources[group[0]["conv"]]
        steps = steps_of(group)
        first = records.get(steps[0]["id"])
        if c not in chosen:  # a talk thought before from a source thought again is stale
            if source in plan and first is not None:
                plan[c] = 0
            continue
        if c in named or source in plan or first is None or first.get("carried_from") != source:
            plan[c] = 0
            continue
        start = next(
            (n for n, t in enumerate(steps) if (records.get(t["id"]) or {}).get("error", "-") is not None),
            None,
        )
        if start is not None:
            plan[c] = start
    if not plan:
        print("Nothing to think", file=sys.stderr)
        world.close()
        return
    forget_after(directory, set(plan))
    order = turn_order(world)
    for group in world.groups:
        if group[0]["conv"] in plan:
            for turn in steps_of(group)[plan[group[0]["conv"]] :]:
                records.pop(turn["id"], None)
    save(path, records, order)
    names = world.names()
    text = prompt("mind_turn_items.md")
    sol = sol if sol is not None else pilot.make_backend("chatgpt", reasoning_effort=effort)
    try:
        for group in world.groups:
            c = group[0]["conv"]
            if c not in plan:
                continue
            source, steps = world.sources[c], steps_of(group)
            if plan[c] > 0:
                state = deepcopy(records[steps[plan[c] - 1]["id"]]["state"])
            elif source is None:
                state = mi.new_state(group[0]["npc"])
            elif (records.get(source + "#end") or {}).get("state") is not None:
                state = deepcopy(records[source + "#end"]["state"])
            else:
                print(f"Skip {c}: its carry source has no closing state", file=sys.stderr)
                continue
            talk = world.talk(c)
            for turn in steps[plan[c] :]:
                record = await think(sol, talk, turn, state, text, names)
                if turn is steps[0]:
                    record["carried_from"] = source
                records[turn["id"]] = record
                save(path, records, order)
                status = record["error"] or ("parse_error" if record["parse_error"] else "ok")
                print(
                    f"Mind {turn['id'][-12:]}: {status}, {record['fold']['stats']['lines']} lines, "
                    f"{record['fold']['errors']} rejected",
                    file=sys.stderr,
                )
    finally:
        await sol.aclose()
        world.close()


# --- opening ----------------------------------------------------------------------------------------


def wanted(probes: Path) -> list[str]:
    listed, _ = stand.load_probes(probes)
    return list(dict.fromkeys(c for p in listed for c in p["ask_after"]))


async def opening(
    directory: Path, *, probes: Path, effort: str = EFFORT, sol: LLMBackend | None = None
) -> None:
    world = stand.World.of(directory)
    minds = {r["id"]: r for r in pilot.rows(directory / "minds.jsonl")}
    asked_after = set(wanted(probes))
    ready = [c for c in world.by_conv if c in asked_after and (minds.get(c + "#end") or {}).get("state")]
    path = directory / "openings.jsonl"
    records = {r["conv"]: r for r in pilot.rows(path)}
    pending = [c for c in ready if (records.get(c) or {}).get("error", "-") is not None]
    if not pending:
        print(f"Nothing to open ({len(ready)} ready)", file=sys.stderr)
        world.close()
        return
    for name in ("answers.jsonl", "judge_answers.jsonl"):
        _drop(directory / name, lambda r: r["variant"] == "note" and r["ask_after"] in pending)
    names = world.names()
    text = prompt("opening_turn.md")
    order = conv_order(world)
    sol = sol if sol is not None else pilot.make_backend("chatgpt", reasoning_effort=effort)
    try:
        for c in pending:
            ctx = stand.asked(world, c)
            state = deepcopy(minds[c + "#end"]["state"])
            since = state["minute"] if state["minute"] is not None else ctx.talk.minute
            prepared = mi.prepare(state, ctx.minute)  # an hour later: feelings have cooled
            situation = with_elapsed(ctx.situation, ctx.minute - since)
            memory = mi.render_for_sol(state)
            messages = opening_messages(ctx, situation, memory, text)
            result = await sol_call(
                sol, ctx.talk.instructions, messages, tools=[], cache_key=f"opening-{ctx.talk.npc}"
            )
            parsed = parse(result)
            report = fold_turn(  # a copy of the memory: the closing state stays as it was
                state,
                "\n".join(parsed["patches"]),
                names=names,
                lines={},
                remarks={},
                minute=ctx.minute,
                entries=ctx.talk.entries,
                closing=False,
            )
            records[c] = {
                "conv": c,
                "npc": ctx.talk.npc,
                "name": ctx.talk.name,
                "minute": ctx.minute,
                "time": mi.time_text(ctx.minute),
                "elapsed_min": ctx.minute - since,
                **result,
                **parsed,
                "prepare": prepared,
                "situation": situation,
                "memory_before": memory,
                "fold": report,
                "state": deepcopy(state),
                "mind": mi.render_for_sol(state),
                "prompt_chars": chars(messages),
            }
            save(path, records, order)
            status = result["error"] or ("parse_error" if parsed["parse_error"] else "ok")
            print(f"Opening after {c.split(':')[2][:8]}: {status}", file=sys.stderr)
    finally:
        await sol.aclose()
        world.close()


# --- answers ----------------------------------------------------------------------------------------


def _answer_key(record: dict[str, Any]) -> tuple[str, str, str]:
    return record["probe"], record["ask_after"], record["variant"]


async def answer(
    directory: Path,
    *,
    probes: Path,
    source: Path,
    quick: LLMBackend | None = None,
) -> None:
    world = stand.World.of(directory)
    notes = {
        r["conv"]: r
        for r in pilot.rows(directory / "openings.jsonl")
        if r.get("error") is None and r.get("note", "").strip()
    }
    listed, where = stand.load_probes(probes)
    plan, skipped = stand.schedule(listed, set(notes))
    stand.write_json(
        directory / "schedule.json",
        {
            "probes": where,
            "from": str(source),
            "asked": [[p["id"], c] for p, c in plan],
            "skipped": skipped,
        },
    )
    path, judged_path = directory / "answers.jsonl", directory / "judge_answers.jsonl"
    results = {_answer_key(r): r for r in pilot.rows(path)}
    judged = {_answer_key(r): r for r in pilot.rows(judged_path)}
    theirs = {_answer_key(r): r for r in pilot.rows(source / "answers.jsonl")}
    their_judged = {_answer_key(r): r for r in pilot.rows(source / "judge_answers.jsonl")}
    missing = []
    for probe, c in plan:
        for variant in COPIED:
            key = (probe["id"], c, variant)
            if key not in theirs:
                missing.append(list(key))
                continue
            results[key] = {**theirs[key], "copied_from": str(source)}
            if key in their_judged:
                judged[key] = {**their_judged[key], "copied_from": str(source)}
    order = {(p["id"], c): n for n, (p, c) in enumerate(plan)}

    def keep(store: dict, file: Path) -> None:
        rows = sorted(
            store.values(),
            key=lambda r: (order.get((r["probe"], r["ask_after"]), len(order)), VARIANTS.index(r["variant"])),
        )
        pilot.save_rows(file, rows)

    keep(results, path)
    keep(judged, judged_path)
    if missing:
        print(f"Not in phase 1, not copied: {missing}", file=sys.stderr)
    pending = [
        (p, c) for p, c in plan if (results.get((p["id"], c, "note")) or {}).get("error", "-") is not None
    ]
    for p, c in pending:
        judged.pop((p["id"], c, "note"), None)
    keep(judged, judged_path)
    if not pending:
        print(f"Nothing to answer ({len(plan)} asked, {len(skipped)} skipped)", file=sys.stderr)
        world.close()
        return
    own = quick is None
    if quick is None:
        quick = pilot.make_quick(pilot.gemma_url(None), max_tokens=300)
    try:
        for probe, c in pending:
            ctx = stand.asked(world, c)
            note = notes[c]["note"]
            messages = note_messages(ctx, probe["phrase"], note)
            result = await pilot.measure(
                quick, ctx.talk.instructions, messages, prefix=messages[:-1], slot=SLOT
            )
            timings = result.get("timings") or {}
            result["prompt_tokens"] = (timings.get("prompt_n") or 0) + (timings.get("cache_n") or 0) or None
            if result["error"] is None and not result["text"].strip():
                result["error"] = "Empty answer"
            text = result["text"].lstrip()
            handoff = text[:5].lower() == "[sol]"
            results[probe["id"], c, "note"] = {
                "probe": probe["id"],
                "ask_after": c,
                "variant": "note",
                "npc": probe["npc"],
                "kind": probe["kind"],
                "calls": [],
                **result,
                "handoff": handoff,
                "stall": text[5:].lstrip() if handoff else None,
                "note": note,
                "prompt_chars": chars(messages),
            }
            keep(results, path)
            print(
                f"Answered {probe['id']} after {c.split(':')[2][:8]} note: {result['error'] or 'ok'}",
                file=sys.stderr,
            )
    finally:
        if own:
            await quick.aclose()
        world.close()


# --- judge ------------------------------------------------------------------------------------------


def point_sections(talk: Any, turn: dict[str, Any], seen: View, before: str, after: str) -> dict[str, str]:
    """The judge of states (judge_items.md of phase 1) at one mind turn: what was there by then."""
    closing = bool(turn.get("closing"))
    return {
        "Описание персонажа": turn["instructions"],
        ("Обстановка в конце разговора" if closing else "Обстановка на этой минуте разговора"): turn[
            "situation"
        ],
        ("Разговор по строкам" if closing else "Разговор по строкам до этой минуты"): seen.text() or "—",
        "Хроника": seen.chronicle_text() or "—",
        "Память до этого хода": before,
        "Память после этого хода": after,
    }


def judged_here(index: int, closing: bool, stride: int) -> bool:
    return closing or (index + 1) % stride == 0


async def judge(
    directory: Path,
    *,
    v3: Path = V3_DIR,
    stride: int = 1,
    sol: LLMBackend | None = None,
    record_chars: int = stand.RECORD_CHARS,
) -> None:
    """The states after the mind turns of phase 2 and of ум v3 (second round) at the same points: every
    `stride`-th turn of a talk and its closing turn (all with 1). The note answers to the probes."""
    world = stand.World.of(directory)
    names = world.names()
    minds = {r["id"]: r for r in pilot.rows(directory / "minds.jsonl")}
    theirs = {r["id"]: r for r in pilot.rows(v3 / "minds.jsonl")}
    states_path, answers_path = directory / "judge_states.jsonl", directory / "judge_answers.jsonl"
    judged_states = {(r["id"], r["system"]): r for r in pilot.rows(states_path)}
    judged_answers = {_answer_key(r): r for r in pilot.rows(answers_path)}
    requests: list[tuple[str, tuple, str, dict[str, str], dict[str, Any]]] = []
    for group in world.groups:
        c = group[0]["conv"]
        if group[0]["id"] not in minds:
            continue
        talk, source = world.talk(c), world.sources[c]
        carried = (minds.get(f"{source}#end") or {}).get("state") if source else None
        before = carried or mi.new_state(group[0]["npc"])
        for turn in steps_of(group):
            record = minds.get(turn["id"]) or {}
            meta = {"conv": c, "index": turn["index"], "closing": bool(turn.get("closing"))}
            if judged_here(turn["index"], bool(turn.get("closing")), stride):
                seen = view(talk, turn)
                ok = record.get("state") is not None and record["error"] is None and not record["parse_error"]
                if ok and (judged_states.get((turn["id"], "items")) or {}).get("error", "-") is not None:
                    sections = point_sections(
                        talk,
                        turn,
                        seen,
                        stand.judge_view(before, names),
                        stand.judge_view(record["state"], names),
                    )
                    requests.append(
                        ("state", (turn["id"], "items"), stand.prompt("judge_items.md"), sections, meta)
                    )
                other = theirs.get(turn["id"]) or {}
                ok = (
                    other.get("mind") is not None
                    and other.get("error") is None
                    and not other.get("parse_error")
                )
                if ok and (judged_states.get((turn["id"], "v3")) or {}).get("error", "-") is not None:
                    sections = point_sections(talk, turn, seen, other["previous_mind"], other["mind"])
                    requests.append(
                        ("state", (turn["id"], "v3"), stand.prompt("judge_items.md"), sections, meta)
                    )
            if record.get("state") is not None:
                before = record["state"]
    plan = stand.read_json(directory / "schedule.json")
    probes = {p["id"]: p for p in stand.load_probes(Path(plan["probes"]))[0]} if plan else {}
    turns = {turn["id"]: turn for turn in world.turns}
    for record in pilot.rows(directory / "answers.jsonl"):
        key = _answer_key(record)
        if record["variant"] != "note" or record.get("error") is not None:
            continue
        if (judged_answers.get(key) or {}).get("error", "-") is None or record["probe"] not in probes:
            continue
        probe = probes[record["probe"]]
        ctx = stand.asked(world, record["ask_after"])
        evidence = {turns[i]["conv"] for i in probe.get("evidence") or [] if i in turns}
        history, meta = stand.branch_record(
            world, record["ask_after"], evidence | {record["ask_after"]}, record_chars
        )
        sections = {
            "Описание персонажа": ctx.talk.instructions,
            "Запись ветки: что было с этим NPC в этой линии сохранений до встречи, по разговорам": history,
            "Обстановка, в которой NPC отвечал": ctx.situation,
            "Вид вопроса": probe["kind"],
            "Фраза героя": probe["phrase"],
            "Ожидаемый ответ (подсказка)": probe["expect"],
            "Опора (подсказка: реплики из записи, на которых держится ожидаемый ответ)": stand.evidence_text(
                turns, probe.get("evidence") or []
            ),
            "Ответ NPC": stand.answer_text(record),
        }
        requests.append(("answer", key, stand.prompt("judge_probe.md"), sections, {"record": meta}))
    if not requests:
        print("Nothing to judge", file=sys.stderr)
        world.close()
        return
    order = turn_order(world)
    sol = sol if sol is not None else pilot.make_backend("chatgpt", reasoning_effort=EFFORT)
    try:
        for kind, key, text, sections, meta in requests:
            content = "\n\n".join(f"{title}\n{body}" for title, body in sections.items())
            cache = f"judge-{key[1]}" if kind == "answer" else None
            result = await stand.sol_stream(sol, text, [Message("user", content)], cache_key=cache)
            result.update(raw=result.pop("text"), result=None)
            if result["error"] is None:
                try:
                    reader = stand.parse_state_verdict if kind == "state" else stand.parse_probe_verdict
                    result["result"] = reader(result["raw"])
                except (ValueError, json.JSONDecodeError) as error:
                    result["error"] = f"Invalid judge JSON: {error}"
            if kind == "state":
                judged_states[key] = {"id": key[0], "system": key[1], **meta, **result}
                pilot.save_rows(
                    states_path,
                    sorted(
                        judged_states.values(), key=lambda r: (order.get(r["id"], len(order)), r["system"])
                    ),
                )
            else:
                judged_answers[key] = {
                    "probe": key[0],
                    "ask_after": key[1],
                    "variant": key[2],
                    **meta,
                    **result,
                }
                pilot.save_rows(answers_path, judged_answers.values())
            print(f"Judged {kind} {key[0][-12:]} {key[-1]}: {result['error'] or 'ok'}", file=sys.stderr)
    finally:
        await sol.aclose()
        world.close()


# --- blind sheet ------------------------------------------------------------------------------------


def sheet(directory: Path) -> list[dict[str, Any]]:
    """As phase 1's sheet: three answers to a probe, A–C in a shuffled order; the key is a separate file."""
    plan = stand.read_json(directory / "schedule.json")
    if plan is None:
        raise SystemExit(f"{directory}: no schedule.json — run answer first")
    probes = {p["id"]: p for p in stand.load_probes(Path(plan["probes"]))[0]}
    answers = {_answer_key(r): r for r in pilot.rows(directory / "answers.jsonl")}
    rng, entries, key = random.Random(stand.SEED), [], {}
    for probe_id, conv in plan["asked"]:
        trio = {v: answers.get((probe_id, conv, v)) for v in SHEET}
        if any(a is None or a.get("error") is not None for a in trio.values()):
            continue
        order = list(SHEET)
        rng.shuffle(order)
        name = f"{probe_id}@{conv.split(':')[2][:8]}"
        probe = probes[probe_id]
        entries.append(
            {
                "key": name,
                "npc": pilot.NAMES.get(probe["npc"], probe["npc"]),
                "kind": probe["kind"],
                "phrase": probe["phrase"],
                "expect": probe["expect"],
                "answers": [
                    {"label": label, "text": stand.shown(trio[v])}
                    for label, v in zip("ABC", order, strict=True)
                ],
            }
        )
        key[name] = dict(zip("ABC", order, strict=True))
    stand.write_json(directory / "sheet.json", entries)
    stand.write_json(directory / "sheet_key.json", {"seed": stand.SEED, "key": key})
    return entries


# --- summary ----------------------------------------------------------------------------------------


distribution, verdict = stand.distribution, stand.verdict


def turn_stats(records: list[dict[str, Any]], windows: list[dict[str, Any]]) -> dict[str, Any]:
    """Item 1 as ум v3 counts it: tokens over all good turns, times over the turns before a phrase, the
    share of turns whose «Голосу» and whole turn end before the hero's phrase (the window)."""
    by_id = {r["id"]: r for r in records}
    good = [r for r in records if r.get("error") is None and not r.get("parse_error")]
    talk_turns = [r for r in good if not r.get("closing")]

    def fits(metric: str) -> int:
        return sum(
            (r := by_id.get(t["id"]) or {}).get("error") is None
            and not r.get("parse_error")
            and r.get(metric) is not None
            and r[metric] <= t["window_s"]
            for t in windows
        )

    note_fits, total_fits = fits("note_ready_s"), fits("total_s")
    return {
        "n": len(records),
        "closing_n": sum(bool(r.get("closing")) for r in records),
        "errors": sum(r.get("error") is not None for r in records),
        "parse_errors": sum(bool(r.get("parse_error")) for r in records),
        "attempts": sum(r.get("attempts") or 0 for r in records),
        "output_tokens": distribution((r.get("usage") or {}).get("output_tokens") for r in good),
        "input_tokens": distribution((r.get("usage") or {}).get("input_tokens") for r in good),
        "first_token_s": distribution(r.get("first_token_s") for r in talk_turns),
        "note_ready_s": distribution(r.get("note_ready_s") for r in talk_turns),
        "total_s": distribution(r.get("total_s") for r in talk_turns),
        "window_n": len(windows),
        "note_fits_n": note_fits,
        "note_fits_fraction": pilot.fraction(note_fits, len(windows)),
        "fits_n": total_fits,
        "fits_fraction": pilot.fraction(total_fits, len(windows)),
    }


def tally(chosen: list[dict[str, Any]], judged: dict[tuple, dict[str, Any]]) -> dict[str, Any]:
    """Verdicts of the probe judge, as phase 1 counts them."""
    counts = Counter(v for a in chosen if (v := verdict(judged.get(_answer_key(a))).get("verdict")))
    n = sum(counts.values())
    return {
        "n": len(chosen),
        "judged": n,
        **{v: counts.get(v, 0) for v in stand.VERDICTS},
        "correct_fraction": pilot.fraction(counts.get("correct", 0), n),
    }


def summary(directory: Path, *, v3: Path = V3_DIR, emit: bool = True) -> dict[str, Any]:
    world = stand.World.of(directory)
    minds = {r["id"]: r for r in pilot.rows(directory / "minds.jsonl")}
    theirs = {r["id"]: r for r in pilot.rows(v3 / "minds.jsonl")}
    openings = {r["conv"]: r for r in pilot.rows(directory / "openings.jsonl")}
    answers = pilot.rows(directory / "answers.jsonl")
    judged_states = {(r["id"], r["system"]): r for r in pilot.rows(directory / "judge_states.jsonl")}
    judged_answers = {_answer_key(r): r for r in pilot.rows(directory / "judge_answers.jsonl")}
    groups = [g for g in world.groups if g[0]["id"] in minds]
    ids = [t["id"] for g in groups for t in steps_of(g)]
    windows = [t for g in groups for t in g if t["window_s"] is not None]

    # 1. the mind turn, against ум v3 (second round) on the same turns
    ours = turn_stats([minds[i] for i in ids if i in minds], windows)
    v3_stats = turn_stats([theirs[i] for i in ids if i in theirs], windows)
    parsing = Counter()
    closed = Counter()
    for g in groups:
        source = minds.get(f"{world.sources[g[0]['conv']]}#end") or {}
        log = len((source.get("state") or {}).get("log", [])) if world.sources[g[0]["conv"]] else 0
        for turn in steps_of(g):
            record = minds.get(turn["id"])
            if record is None:
                continue
            for name, value in record["fold"]["stats"].items():
                parsing[name] += value
            parsing["rejected"] += record["fold"]["errors"]
            fresh = record["state"]["log"][log:]
            closed.update(e["by"] for e in fresh if e["op"] == "close" and e["by"] != "sol")
            log = len(record["state"]["log"])
    mind_turn = {
        "phase2": ours,
        "v3": v3_stats,
        "parsing": dict(parsing),
        "closed_by_code": dict(closed),
        "calls": Counter(c["name"] for i in ids for c in (minds.get(i) or {}).get("calls", [])),
        "say_n": sum(len((minds.get(i) or {}).get("say", [])) for i in ids),
    }

    # 2. clean memory over all judged points, one judge for both
    systems = {}
    fields = ("false_facts", "stale", "wrong_source", "name_confusion")
    for system in ("items", "v3"):
        found = {
            k[0]: verdict(r) for k, r in judged_states.items() if k[1] == system and r.get("error") is None
        }
        systems[system] = {
            "points": len(found),
            **{f: sum(len(v.get(f, [])) for v in found.values()) for f in fields},
            "found": {f: [{"id": i, f: v[f]} for i, v in found.items() if v.get(f)] for f in fields},
        }
    both = [
        i
        for i in ids
        if all((judged_states.get((i, s)) or {}).get("error", "-") is None for s in ("items", "v3"))
    ]
    stale_pair = {
        s: sum(len(verdict(judged_states[i, s]).get("stale", [])) for i in both) for s in ("items", "v3")
    }

    # 3. what Cavalorn carries to the next meeting: the closing memory and the opening note
    transitions = []
    for g in groups:
        first = minds.get(g[0]["id"]) or {}
        source = first.get("carried_from")
        end = minds.get(f"{source}#end") or {}
        if g[0]["npc"] != mi.CAVALORN or source is None or end.get("state") is None:
            continue
        lines = [mi.item_line(i, section=True) for i in end["state"]["items"]]
        note = (openings.get(source) or {}).get("note", "")
        transitions.append(
            {
                "from": source,
                "to": g[0]["conv"],
                "topics": {t: [x for x in lines if p.search(x)] for t, p in stand.TOPICS.items()},
                "opening_note": note,
                "note_topics": {t: bool(p.search(note)) for t, p in stand.TOPICS.items()},
            }
        )

    # 4. the probes: bare, items and sol of phase 1 with their judgments, note of this one
    plan = stand.read_json(directory / "schedule.json")
    kinds = {p["id"]: p["kind"] for p in stand.load_probes(Path(plan["probes"]))[0]} if plan else {}
    for a in answers:  # the probes file is the truth: the lead may relabel probes after the answers
        a["kind"] = kinds.get(a["probe"], a["kind"])
    probes = {}
    for variant in VARIANTS:
        chosen = [a for a in answers if a["variant"] == variant and a.get("error") is None]
        probes[variant] = {
            **tally(chosen, judged_answers),
            "errors": sum(a["variant"] == variant and a.get("error") is not None for a in answers),
            "handoffs": sum(bool(a.get("handoff")) for a in chosen),
            "invented_claims": sum(
                len(verdict(judged_answers.get(_answer_key(a))).get("invented", [])) for a in chosen
            ),
            "first_sentence_s": distribution(a.get("first_sentence_s") for a in chosen),
            "by_kind": {
                k: tally([a for a in chosen if a["kind"] == k], judged_answers)
                for k in sorted(set(kinds.values()))
            },
            "not_to_know": tally([a for a in chosen if a["kind"] in stand.NOT_TO_KNOW], judged_answers),
            "by_npc": {
                pilot.NAMES.get(npc, npc): tally([a for a in chosen if a["npc"] == npc], judged_answers)
                for npc in sorted({a["npc"] for a in answers})
            },
        }

    # 5. the opening turn
    opened = list(openings.values())
    good = [r for r in opened if r.get("error") is None and not r.get("parse_error")]
    opening_stats = {
        "n": len(opened),
        "errors": sum(r.get("error") is not None for r in opened),
        "parse_errors": sum(bool(r.get("parse_error")) for r in opened),
        "attempts": sum(r.get("attempts") or 0 for r in opened),
        "first_token_s": distribution(r.get("first_token_s") for r in good),
        "note_ready_s": distribution(r.get("note_ready_s") for r in good),
        "total_s": distribution(r.get("total_s") for r in good),
        "output_tokens": distribution((r.get("usage") or {}).get("output_tokens") for r in good),
        "patch_lines": sum(r["fold"]["stats"]["lines"] for r in opened),
        "rejected": sum(r["fold"]["errors"] for r in opened),
        "cooled": sum(len(r["prepare"]["cooled"]) for r in opened),
    }

    # 6. Sol calls for a phrase of the hero: now Sol answers each; ум v3 adds a turn per phrase, the closing
    # one and keeps Sol's answers where the router sends the phrase to it; phase 2 adds the opening turn
    talks = []
    for g in groups:
        n = len(g)
        routed = sum(t.get("oof_predicted") is not False for t in g)
        talks.append({"conv": g[0]["conv"], "phrases": n, "routed_to_sol": routed})
    phrases = sum(t["phrases"] for t in talks)
    routed = sum(t["routed_to_sol"] for t in talks)
    calls = {
        "phrases": phrases,
        "now": phrases,
        "v3": phrases + len(talks) + routed,
        "phase2_gemma_answers_all": phrases + 2 * len(talks),
        "phase2_router_as_v3": phrases + 2 * len(talks) + routed,
        "retries_mind": ours["attempts"] - ours["n"],
        "talks": talks,
    }
    for name in ("now", "v3", "phase2_gemma_answers_all", "phase2_router_as_v3"):
        calls[name + "_per_phrase"] = pilot.fraction(calls[name], phrases)

    made = [r for r in judged_answers.values() if not r.get("copied_from")]
    sol_calls = {
        "mind": ours["attempts"],
        "opening": opening_stats["attempts"],
        "judge_states": len(judged_states),
        "judge_answers": len(made),
    }
    sol_calls["total"] = sum(sol_calls.values())

    note_median = opening_stats["note_ready_s"]["median"]
    tokens = ours["output_tokens"]
    acceptance = {
        "1_mind_turn": {
            "tokens_median": tokens["median"] is not None and tokens["median"] <= TOKENS_MEDIAN,
            "tokens_max": tokens["max"] is not None and tokens["max"] <= TOKENS_MAX,
            "total_s_median": ours["total_s"]["median"] is not None and ours["total_s"]["median"] <= TOTAL_S,
            "fits": (ours["fits_fraction"] or 0) >= FITS,
            "note_fits": (ours["note_fits_fraction"] or 0) >= NOTE_FITS,
        }
        if ours["n"]
        else None,
        "2_clean": {
            "false_facts_0": systems["items"]["false_facts"] == 0,
            "name_confusion_0": systems["items"]["name_confusion"] == 0,
            "wrong_source_0": systems["items"]["wrong_source"] == 0,
            "stale_less_than_v3": stale_pair["items"] < stale_pair["v3"] if both else None,
            "points": len(both),
        }
        if systems["items"]["points"]
        else None,
        "3_remembers": None,  # the lead reads `transitions`
        "4_probes": {
            "note_beats_bare": probes["note"]["correct"] > probes["bare"]["correct"],
            "note_invents_not_more_than_bare": probes["note"]["invented"] <= probes["bare"]["invented"],
            "note_at_least_80pct_of_sol": probes["note"]["correct"] >= NOTE_OF_SOL * probes["sol"]["correct"],
        }
        if probes["note"]["judged"]
        else None,
        "5_opening": note_median <= OPENING_NOTE_S if note_median is not None else None,
        "6_cost": None,  # the user decides
    }
    result = {
        "acceptance": acceptance,
        "mind_turn": mind_turn,
        "judge": systems,
        "stale_same_points": {**stale_pair, "points": both},
        "transitions": transitions,
        "probes": probes,
        "opening": opening_stats,
        "cost": calls,
        "sol_calls": sol_calls,
    }
    stand.write_json(directory / "summary.json", result)
    world.close()
    if emit:
        for name, s in (("phase 2", ours), ("v3", v3_stats)):
            print(
                f"1. {name}: {s['n']} turns ({s['closing_n']} closing), {s['errors']} errors, "
                f"{s['parse_errors']} parse errors; output tokens median {s['output_tokens']['median']} "
                f"/ max {s['output_tokens']['max']}; turn {s['total_s']['median']} s, "
                f"«Голосу» {s['note_ready_s']['median']} s; fits {s['fits_n']}/{s['window_n']}, "
                f"«Голосу» fits {s['note_fits_n']}/{s['window_n']}"
            )
        print(f"   parsing {dict(parsing)}; closed by code {dict(closed)}")
        for system, s in systems.items():
            print(
                f"2. {system}: {s['points']} points; false facts {s['false_facts']}, stale {s['stale']}, "
                f"wrong source {s['wrong_source']}, name confusion {s['name_confusion']}"
            )
        print(
            f"   stale at the same {len(both)} points: phase 2 {stale_pair['items']}, v3 {stale_pair['v3']}"
        )
        for t in transitions:
            print(
                f"3. {t['from'].split(':')[2][:8]} → {t['to'].split(':')[2][:8]}: "
                + "; ".join(
                    f"{k} {len(v)} (note {'да' if t['note_topics'][k] else 'нет'})"
                    for k, v in t["topics"].items()
                )
            )
        for variant in VARIANTS:
            p = probes[variant]
            print(f"4. {variant}: " + ", ".join(f"{v} {p[v]}" for v in stand.VERDICTS) + f" of {p['judged']}")
        print(
            f"5. opening: {opening_stats['n']} turns, «Голосу» {opening_stats['note_ready_s']['median']} s "
            f"median, turn {opening_stats['total_s']['median']} s"
        )
        print(
            "6. Sol calls per phrase: now 1, v3 {v3_per_phrase:.2f}, phase 2 "
            "{phase2_gemma_answers_all_per_phrase:.2f} (Gemma answers all), "
            "{phase2_router_as_v3_per_phrase:.2f} (router as v3)".format(**calls)
            if phrases
            else "6. no phrases"
        )
        print(f"Sol calls of this run: {sol_calls}")
        print(f"Acceptance: {json.dumps(acceptance, ensure_ascii=False)}")
    return result


# --- selftest ---------------------------------------------------------------------------------------


_VOICE = (
    "[Голосу]\nТон: настороженно.\nХочу: вернуть своё.\nПомню: обокрал меня.\nНельзя: верить.\nЕсли он:\n"
    "- извинится → посмотрим\n"
)


async def _selftest_run(check: Any, root: Path) -> None:
    from gothic_ai.llm import FakeBackend, FakeStep

    fx = stand._fixture(root)
    out = root / "run"
    v3 = fx["v3"]
    conv1, conv2 = fx["conv1"], fx["conv2"]

    # the mind turns: two before the phrases of talk 1, its closing, one before the phrase of talk 2, its
    # closing
    sol = FakeBackend(
        script=[
            FakeStep(
                _VOICE
                + "[Правки]\n+ было 2: поздоровался [стр: 1]\n+ хочу сейчас: узнать, чего он хочет — гость"
            ),
            FakeStep(
                "[neutral]\n"
                + _VOICE
                + "[Правки]\n+ было 8: герой обокрал меня посреди разговора → не доверяю [стр: 5]\n"
                "+ верю герой: письмо у Ватраса [о: Ватрас; стр: 2, 9]\n= мнение: вор [стр: 5]\n"
                "[Скажу] Стой!",
                [ToolCall("set_intent", {"goal": "fight"})],
            ),
            FakeStep(
                _VOICE + "[Правки]\n+ нить: идти бить бандитов [стр: 6-8; о: Браго]\n"
                "+ чувство 2 час: злость к герою: обокрал [стр: 5]"
            ),
            FakeStep("без заголовков"),
            FakeStep(_VOICE + "[Правки]\n—"),
        ],
        delay=0,
    )
    await mind_run(out, carry=fx["db"], turns=fx["turns"], sol=sol)
    records = {r["id"]: r for r in pilot.rows(out / "minds.jsonl")}
    check("one Sol call a mind turn", len(sol.calls) == 5 and len(records) == 5, list(records))
    instructions, messages, tools = sol.calls[0]
    body = "\n".join(m.content for m in messages if isinstance(m, Message))
    check(
        "mind prompt: situation, lines before the phrase, memory, prompt, tools",
        instructions == "Ты — Кавалорн."
        and messages[-1].content == prompt("mind_turn_items.md")
        and messages[-2].content == "[Твой ум до этой минуты]\nПока пусто."
        and messages[-3].content
        == "[Этот разговор до этой минуты — строки с номерами]\n1. [Событие: гоблин напал на тебя]"
        and "Время: день 1, 10:00" in messages[-4].content
        and {t.name for t in tools} == {t.name for t in mind.MIND_TOOLS},
        body,
    )
    second = sol.calls[1][1]
    check(
        "second turn: the lines before its phrase, the memory with numbers",
        second[-3].content.endswith("5. [Герой посреди разговора обокрал тебя]")
        and "6. Герой" not in second[-3].content
        and "- б1 (день 1, 10:00, важность 2): поздоровался" in second[-2].content,
        (second[-3].content, second[-2].content),
    )
    closing_body = "\n".join(m.content for m in sol.calls[2][1] if isinstance(m, Message))
    check(
        "closing turn: all lines, the chronicle",
        "[Весь этот разговор — строки с номерами]" in closing_body
        and "12. Вызов: set_intent" in closing_body
        and "[Хроника игры о тебе — строки с номерами]\n13. Хроника:" in closing_body,
        closing_body[-600:],
    )
    a2 = records.get("a2") or {}
    state = a2.get("state") or mi.new_state("x")
    belief = mi.find_item(state, "в1") or {}
    check(
        "«Голосу», patches and actions in the grammar of items",
        a2.get("note", "").startswith("Тон: настороженно.")
        and a2.get("say") == ["Стой!"]
        and [c["name"] for c in a2.get("calls", [])] == ["set_intent"]
        and a2.get("fold", {}).get("errors") == 0
        and (mi.find_item(state, "б2") or {}).get("episodes") == ["a2"]
        and belief.get("source", {}).get("kind") == "герой"
        and mi.VATRAS in belief.get("about", [])
        and a2["fold"]["stats"]["foreign_lines"] == 1  # line 9 comes after the phrase: not seen yet
        and a2.get("note_ready_s") is not None
        and a2.get("window_s") is None,
        a2.get("fold"),
    )
    end1 = records.get(conv1 + "#end") or {}
    check(
        "trifles and «сейчас» are forgotten only in the closing turn",
        (mi.find_item(state, "б1") or {}).get("closed_by") is None
        and sorted(end1.get("fold", {}).get("forgotten", [])) == ["б1", "ж1"],
        end1.get("fold"),
    )
    c1, end2 = records.get("c1") or {}, records.get(conv2 + "#end") or {}
    check(
        "carry by the save chain; a parse error leaves the memory",
        c1.get("carried_from") == conv1
        and c1.get("parse_error") is True
        and c1["memory_before"].startswith("С прошлого раза прошло: 1 ч\nМнение о герое: вор")
        and [i["id"] for i in c1["state"]["items"]] == [i["id"] for i in end1["state"]["items"]]
        and end2.get("closing") is True,
        c1.get("memory_before"),
    )
    await mind_run(out, carry=fx["db"], turns=fx["turns"], sol=sol)
    check("mind resumes", len(sol.calls) == 5)

    # the opening turn: one per talk asked after; the patches go to a copy
    opener = FakeBackend(_VOICE + "[Правки]\n- н1\n= настроение: остыл", delay=0)
    await opening(out, probes=fx["probes"], sol=opener)
    opened = {r["conv"]: r for r in pilot.rows(out / "openings.jsonl")}
    check("one opening per talk asked after", len(opener.calls) == 2 and set(opened) == {conv1, conv2})
    instructions, messages, tools = opener.calls[0]
    check(
        "opening prompt: history to the new meeting, calm situation with the time since, memory, prompt",
        instructions == "Ты — Кавалорн."
        and not tools
        and messages[-1].content == prompt("opening_turn.md")
        and messages[-2].content.startswith("[Твоя память до этой встречи]\nМнение о герое: вор")
        and "- н1: идти бить бандитов" in messages[-2].content
        and "Время: день 1, 11:30, день.\nС прошлого раза прошло: 1 ч" in messages[-3].content
        and stand.CALM_HERO in messages[-3].content
        and messages[-4].content.startswith("[Новая встреча с героем, день 1, 11:30]"),
        [getattr(m, "content", m) for m in messages[-4:]],
    )
    first = opened.get(conv1) or {}
    check(
        "opening: note, patches on a copy",
        first.get("note", "").startswith("Тон:")
        and (mi.find_item(first["state"], "н1") or {}).get("closed_by") == "sol"
        and (mi.find_item(end1["state"], "н1") or {}).get("closed_by") is None
        and first.get("fold", {}).get("errors") == 0,
        first.get("fold"),
    )

    # answers: note by Gemma, bare / items / sol copied from phase 1 with their judgments
    phase1 = root / "phase1"
    phase1.mkdir()
    theirs = [
        {
            "probe": "p1",
            "ask_after": c,
            "variant": v,
            "npc": mi.CAVALORN,
            "kind": "feeling",
            "text": f"[angry] {v} {c[-4:]}",
            "error": None,
            "handoff": False,
            "calls": [],
        }
        for c in (conv1, conv2)
        for v in COPIED
    ]
    pilot.save_rows(phase1 / "answers.jsonl", theirs)
    pilot.save_rows(
        phase1 / "judge_answers.jsonl",
        [
            {
                "probe": r["probe"],
                "ask_after": r["ask_after"],
                "variant": r["variant"],
                "error": None,
                "result": {"verdict": "correct", "invented": []},
            }
            for r in theirs
        ],
    )
    quick = FakeBackend("[angry] Ты меня обокрал.", delay=0)
    await answer(out, probes=fx["probes"], source=phase1, quick=quick)
    got = {_answer_key(r): r for r in pilot.rows(out / "answers.jsonl")}
    judged = {_answer_key(r): r for r in pilot.rows(out / "judge_answers.jsonl")}
    check(
        "answer copies phase 1 and asks Gemma only for note",
        len(quick.calls) == 2
        and len(got) == 8
        and got[("p1", conv1, "sol")]["text"] == f"[angry] sol {conv1[-4:]}"
        and got[("p1", conv1, "sol")]["copied_from"] == str(phase1)
        and got[("p1", conv2, "note")]["text"] == "[angry] Ты меня обокрал."
        and set(judged) == {k for k in got if k[2] != "note"},
        sorted(got),
    )
    gemma = quick.calls[0][1]
    check(
        "Gemma note prompt: history, brief situation, the thoughts, the hint, the phrase",
        gemma[-1].content == "Кавалорн, ты на меня зол?"
        and THOUGHTS_HEAD + "\nТон: настороженно." in gemma[-2].content
        and gemma[-2].content.endswith(HOLD)
        and stand.prompt("speak_probe.md").strip() in gemma[-2].content
        and "Твой инвентарь" not in gemma[-2].content
        and gemma[-3].content.startswith("[Новая встреча с героем")
        and quick.prewarm_slot_ids == [SLOT, SLOT],
        gemma[-2].content,
    )

    # judge: phase 2 and ум v3 at the same points, the note answers only
    pilot.save_rows(
        v3 / "minds.jsonl",
        [
            {
                "id": i,
                "previous_mind": mind.EMPTY_MIND,
                "mind": "Мнение о герое: вор",
                "error": None,
                "parse_error": False,
            }
            for i in ("a1", "a2", conv1 + "#end", "c1", conv2 + "#end")
        ],
    )
    referee = stand._Judge()
    await judge(out, v3=v3, stride=2, sol=referee)
    states = {(r["id"], r["system"]): r for r in pilot.rows(out / "judge_states.jsonl")}
    probes_judged = [r for r in pilot.rows(out / "judge_answers.jsonl") if not r.get("copied_from")]
    check(
        "judge: every 2nd turn and the closing ones, both systems; note answers",
        set(states) == {(i, s) for i in ("a2", conv1 + "#end", conv2 + "#end") for s in ("items", "v3")}
        and len(probes_judged) == 2
        and all(r["variant"] == "note" for r in probes_judged)
        and len(referee.calls) == 8,
        sorted(states),
    )
    point = next(
        (
            c[1][0].content
            for c in referee.calls
            if "Разговор по строкам до этой минуты" in c[1][0].content
            and "Память до этого хода\n- б1" in c[1][0].content
        ),
        "",
    )
    check(
        "judge of a state: lines and memory by then",
        "Разговор по строкам до этой минуты\n1. [Событие" in point
        and "6. Герой" not in point
        and "Память до этого хода\n- б1 [было; видел сам; важность 2] поздоровался" in point,
        point[:800],
    )

    # the sheet: bare, note, sol shuffled, the key apart
    entries = sheet(out)
    key = stand.read_json(out / "sheet_key.json")["key"]
    check(
        "sheet: three answers, the key apart",
        len(entries) == 2
        and all(len(e["answers"]) == 3 and "variant" not in json.dumps(e) for e in entries)
        and all(sorted(v.values()) == sorted(SHEET) for v in key.values()),
        entries,
    )
    result = summary(out, v3=v3, emit=False)
    check(
        "summary: the numbers of items 1–6",
        result["mind_turn"]["phase2"]["n"] == 5
        and result["probes"]["note"]["judged"] == 2
        and result["probes"]["bare"]["correct"] == 2
        and result["judge"]["items"]["stale"] == 3
        and result["opening"]["n"] == 2
        and result["cost"]["phrases"] == 3
        and result["sol_calls"]["total"] == 5 + 2 + 6 + 2
        and len(result["transitions"]) == 1,
        result["sol_calls"],
    )


async def selftest() -> int:
    from tempfile import TemporaryDirectory

    check = mi._Check()
    with TemporaryDirectory() as root:
        try:
            await _selftest_run(check, Path(root))
        except Exception:  # a crash fails the selftest with its trace
            import traceback

            traceback.print_exc()
            check("crash", False)
    print(f"selftest: {check.passed} passed, {len(check.failed)} failed")
    return 1 if check.failed else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest="command", required=True)
    command = commands.add_parser("mind")
    command.add_argument("directory", type=Path)
    command.add_argument("--carry", type=Path, required=True, help="memory.sqlite3 of the recorded games")
    command.add_argument("--conv", action="append", help="a talk (its id or a unique part); repeatable")
    command.add_argument("--redo", action="store_true", help="think the named talks again from their start")
    command.add_argument("--turns", type=Path, default=TURNS_DIR)
    command = commands.add_parser("opening")
    command.add_argument("directory", type=Path)
    command.add_argument("--probes", type=Path, required=True)
    command = commands.add_parser("answer")
    command.add_argument("directory", type=Path)
    command.add_argument("--probes", type=Path, required=True)
    command.add_argument("--from", dest="source", type=Path, required=True, help="the phase 1 run")
    command = commands.add_parser("judge")
    command.add_argument("directory", type=Path)
    command.add_argument("--v3", type=Path, default=V3_DIR)
    command.add_argument(
        "--stride", type=int, default=1, help="judge every N-th turn of a talk and its closing"
    )
    command = commands.add_parser("sheet")
    command.add_argument("directory", type=Path)
    command = commands.add_parser("summary")
    command.add_argument("directory", type=Path)
    command.add_argument("--v3", type=Path, default=V3_DIR)
    commands.add_parser("selftest")
    args = vars(parser.parse_args())
    name = args.pop("command")
    if name == "selftest":
        return asyncio.run(selftest())
    if name in ("sheet", "summary"):
        globals()[name](**args)
    else:
        asyncio.run({"mind": mind_run, "opening": opening, "answer": answer, "judge": judge}[name](**args))
    return 0


if __name__ == "__main__":
    sys.exit(main())
