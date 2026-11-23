"""Offline Sol mind pilot. Run from server/; selftest never uses the network."""

from __future__ import annotations

import argparse
import asyncio
import importlib.util
import json
import random
import re
import sqlite3
import sys
import time
import unicodedata
from collections import Counter
from contextlib import aclosing, closing
from copy import deepcopy
from dataclasses import asdict
from pathlib import Path
from tempfile import TemporaryDirectory
from typing import Any
from unittest.mock import patch

from gothic_ai.deals import PROPOSE_TOOL, WITHDRAW_TOOL
from gothic_ai.llm import (
    Done,
    FakeBackend,
    FakeStep,
    LLMBackend,
    LLMError,
    Message,
    TextDelta,
    ToolCall,
    ToolResult,
    Usage,
)
from gothic_ai.npc_tools import TOOLS
from gothic_ai.replay import _quick_context
from gothic_ai.trace import item_from_json, item_json, read
from gothic_ai.wrath import TOOL as OFFENSE_TOOL

HERE = Path(__file__).resolve().parent
modules = {}
for name, folder in (("pilot", "2026-11-02-sol-thinks"), ("gated", "2026-11-02-hybrid-gated")):
    spec = importlib.util.spec_from_file_location(f"_mind_{name}", HERE.parent / folder / f"{name}.py")
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    modules[name] = module
pilot, gated = (modules[name] for name in ("pilot", "gated"))
HYBRID = HERE.parent / "2026-11-02-gemma-sol-hybrid"
VARIANTS = ("mind", "note")
EMPTY_MIND = "Пока пусто: разговор только начался."
TIMEOUT_S = 90
MIND_TOOLS = [tool for tool in TOOLS if tool.name in {"set_intent", "give_item", "end_conversation"}]
MIND_TOOLS += [PROPOSE_TOOL, WITHDRAW_TOOL, OFFENSE_TOOL]
FIELDS = (
    "Мнение о герое",
    "Что было между нами",
    "Чувства",
    "Желания",
    "Верю",
    "Сомневаюсь",
    "О других",
    "Нити",
    "Настроение",
    "Хочу",
)
SECTIONS = {
    "б": ("events", "Что было между нами", "было"),
    "ч": ("feelings", "Чувства", "чувство"),
    "ж": ("wishes", "Желания", "хочу"),
    "в": ("beliefs", "Верю", "верю"),
    "с": ("doubts", "Сомневаюсь", "сомневаюсь"),
    "д": ("others", "О других", "о"),
    "н": ("threads", "Нити", "нить"),
}
DURATIONS = {"час": 60, "день": 1440, "долго": 10080}
WISH_TERMS = ("сейчас", "скоро", "давно")
BELIEF_SOURCES = ("видел", "сказал", "слышал")


def new_state() -> dict[str, Any]:
    return {
        "opinion": "",
        "mood": "",
        "minute": None,
        "time": "—",
        "counters": dict.fromkeys(SECTIONS, 0),
        **{key: [] for key, _, _ in SECTIONS.values()},
    }


def render(state: dict[str, Any], ids: bool) -> str:
    if not (state["opinion"] or state["mood"] or any(state[s[0]] for s in SECTIONS.values())):
        return EMPTY_MIND
    lines = ["Мнение о герое: " + (state["opinion"] or "—")]
    for letter, (key, heading, _) in SECTIONS.items():
        lines.append(heading + ":")
        items = state[key]
        if letter == "б":
            items = sorted(
                sorted(
                    items,
                    key=lambda item: (item["importance"], item["minute"], int(item["id"][1:])),
                    reverse=True,
                )[:12],
                key=lambda item: (item["minute"], int(item["id"][1:])),
            )
        if not items:
            lines.append("- —")
        for item in items:
            prefix = item["id"] if ids else ""
            if letter == "б":
                detail = item["time"]
                if ids:
                    detail += f", важность {item['importance']}"
                prefix += f" ({detail})"
            elif letter == "ч":
                prefix += f" (сила {item['strength']}, {item['duration']})"
            elif letter == "ж":
                prefix += f" ({item['term']})"
            elif letter == "в":
                prefix += f" ({item['source']})"
            elif letter == "д":
                prefix += " " + item["name"]
            lines.append("- " + ((prefix.strip() + ": ") if prefix.strip() else "") + item["text"])
    lines.append("Настроение: " + (state["mood"] or "—"))
    return "\n".join(lines)


def prepare_state(state: dict[str, Any], situation: str) -> tuple[str, dict[str, list]]:
    changes = {"cooled": [], "cap_dropped": [], "forgotten": []}
    previous_minute = state["minute"]
    match = re.search(r"(?m)^Время: (день (\d+), (\d{2}):(\d{2}))", situation)
    if match:
        state["time"] = match[1]
        state["minute"] = int(match[2]) * 1440 + int(match[3]) * 60 + int(match[4])
    minute = state["minute"] or 0
    for item in list(state["feelings"]):
        periods = max(0, (minute - item["updated_minute"]) // DURATIONS[item["duration"]])
        item["strength"] -= periods
        item["updated_minute"] += periods * DURATIONS[item["duration"]]
        if item["strength"] <= 0:
            state["feelings"].remove(item)
            changes["cooled"].append({"id": item["id"], "text": item["text"]})
    previous = render(state, ids=True)
    elapsed = minute - previous_minute if previous_minute is not None else 0
    if elapsed > 60:
        days, hours = divmod(elapsed // 60, 24)
        parts = ([f"{days} дн."] if days else []) + ([f"{hours} ч"] if hours else [])
        previous = "С прошлого раза прошло: " + " ".join(parts) + "\n" + previous
    return previous, changes


def parse_patches(text: str) -> dict[str, Any]:
    match = re.search(r"\[Голосу\](.*?)\[Правки\](.*)", text, re.DOTALL)
    if match is None:
        return {"note": "", "patches": [], "say": [], "parse_error": True}
    rest = match[2]
    return {
        "note": match[1].strip(),
        "patches": [
            line for line in re.split(r"(?m)^\[Скажу\]", rest, maxsplit=1)[0].splitlines() if line.strip()
        ],
        "say": re.findall(r"(?m)^\[Скажу\][ \t]*(.+)$", rest),
        "parse_error": False,
    }


def patch_params(letter: str, params: str, *, adding: bool) -> dict[str, Any]:
    tokens = params.split()
    if not tokens:
        if adding and letter in "бчжвд":
            raise ValueError("syntax")
        return {}
    if letter == "б" and len(tokens) == 1 and tokens[0].isdigit() and 1 <= int(tokens[0]) <= 10:
        return {"importance": int(tokens[0])}
    if letter == "ч":
        values = {}
        if tokens[0].isdigit():
            values["strength"] = int(tokens.pop(0))
            if not 1 <= values["strength"] <= 3:
                raise ValueError("bad_param")
        if tokens and tokens[0] in DURATIONS:
            values["duration"] = tokens.pop(0)
        if not tokens and values and (not adding or len(values) == 2):
            return values
    if letter == "ж" and len(tokens) == 1 and params in WISH_TERMS:
        return {"term": params}
    if letter == "в" and len(tokens) == 1 and params in BELIEF_SOURCES:
        return {"source": params}
    if letter == "д":
        return {"name": params}
    raise ValueError("bad_param")


def normalized(text: str) -> str:
    text = " ".join(text.casefold().split())
    while text and unicodedata.category(text[-1]).startswith("P"):
        text = text[:-1].rstrip()
    return text


def enforce_caps(state: dict[str, Any], changes: dict[str, list]) -> None:
    for key, limit in (("beliefs", 10), ("doubts", 5), ("others", 8), ("threads", 8), ("wishes", 2)):
        for term in WISH_TERMS if key == "wishes" else (None,):
            items = [item for item in state[key] if term is None or item["term"] == term]
            for item in items[:-limit]:
                state[key].remove(item)
                changes["cap_dropped"].append({"id": item["id"], "text": item["text"]})


def apply_patches(state: dict[str, Any], lines: list[str], changes: dict[str, list]) -> list[dict]:
    applied = []
    for raw in lines:
        line = raw.strip()
        record = {"line": raw, "op": None, "ok": False, "error": None, "id": None}
        applied.append(record)
        try:
            if line in ("—", "–", "-"):
                record["op"] = "noop"
            elif line.startswith("="):
                record["op"] = "="
                match = re.fullmatch(r"=\s*(мнение|настроение)\s*:\s*(.+)", line)
                if match is None:
                    raise ValueError("syntax")
                state["opinion" if match[1] == "мнение" else "mood"] = match[2].strip()
            elif line.startswith("+"):
                record["op"] = "+"
                match = re.fullmatch(
                    r"\+\s*(было|чувство|хочу|верю|сомневаюсь|нить|о)(?:\s+([^:]*?))?\s*:\s*(.+)",
                    line,
                )
                if match is None:
                    raise ValueError("syntax")
                letter = next(letter for letter, s in SECTIONS.items() if s[2] == match[1])
                values = patch_params(letter, (match[2] or "").strip(), adding=True)
                values["text"] = match[3].strip()
                items = state[SECTIONS[letter][0]]
                existing = next(
                    (
                        item
                        for item in items
                        if letter == "д" and item["name"].casefold() == values["name"].casefold()
                    ),
                    None,
                )
                if existing is None and any(
                    normalized(item["text"]) == normalized(values["text"])
                    and (letter != "д" or normalized(item["name"]) == normalized(values["name"]))
                    for item in items
                ):
                    raise ValueError("duplicate")
                if existing is not None:
                    existing["text"] = values["text"]
                    record["id"] = existing["id"]
                else:
                    state["counters"][letter] += 1
                    record["id"] = letter + str(state["counters"][letter])
                    if letter == "б":
                        values.update(time=state["time"], minute=state["minute"] or 0)
                    elif letter == "ч":
                        values["updated_minute"] = state["minute"] or 0
                    items.append({"id": record["id"], **values})
            elif line.startswith(("~", "-", "–", "−")):
                record["op"] = "~" if line.startswith("~") else "-"
                pattern = (
                    r"~\s*([бчжвсдн]\d+)(?=\s|:|$)(.*)"
                    if record["op"] == "~"
                    else r"[\-–−]\s*([бчжвсдн]\d+)(.*)"
                )
                match = re.fullmatch(pattern, line)
                if match is None:
                    raise ValueError("syntax")
                identifier, tail = match[1], match[2].strip()
                record["id"] = identifier
                items = state[SECTIONS[identifier[0]][0]]
                item = next((item for item in items if item["id"] == identifier), None)
                if item is None:
                    raise ValueError("unknown_id")
                if record["op"] == "-":
                    items.remove(item)
                else:
                    params, _, text = tail.partition(":")
                    values = patch_params(identifier[0], params.strip(), adding=False)
                    if text.strip():
                        values["text"] = text.strip()
                    if not values:
                        raise ValueError("syntax")
                    if identifier[0] == "ч":
                        values["updated_minute"] = state["minute"] or 0
                    item.update(values)
            else:
                raise ValueError("syntax")
            record["ok"] = True
            enforce_caps(state, changes)
        except ValueError as error:
            record["error"] = str(error)
    return applied


def forget_events(state: dict[str, Any], changes: dict[str, list]) -> None:
    """After a conversation: trifles are forgotten, and what the npc wanted from this talk is over."""
    for key, over in (
        ("events", lambda item: item["importance"] <= 3),
        ("wishes", lambda item: item["term"] == "сейчас"),
    ):
        for item in list(state[key]):
            if over(item):
                state[key].remove(item)
                changes["forgotten"].append({"id": item["id"], "text": item["text"]})


def write_json(path: Path, value: Any) -> None:
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def conversations(turns: list[dict[str, Any]]) -> list[list[dict[str, Any]]]:
    groups = {}
    for turn in turns:
        groups.setdefault(turn["conv"], []).append(turn)
    return sorted(
        (sorted(group, key=lambda turn: turn["index"]) for group in groups.values()),
        key=lambda group: group[0]["t"],
    )


def carry_sources(groups: list[list[dict[str, Any]]], memory_db: Path | None) -> dict[str, str | None]:
    branches = {}
    if memory_db is not None:
        with closing(sqlite3.connect(memory_db.resolve().as_uri() + "?mode=ro", uri=True)) as db:
            branches = {row[0]: row[1:] for row in db.execute("SELECT id, parent, created FROM branches")}
    sources, processed = {}, []
    for group in groups:
        first = group[0]
        limits = {first["branch"]: first["t"]}
        branch = first["branch"]
        while branch in branches:
            parent, created = branches[branch]
            if parent is None:
                break
            if parent in limits:
                raise ValueError("Cycle in memory branches")
            limits[parent] = min(first["t"], created)
            branch = parent
        candidates = [
            prior
            for prior in processed
            if memory_db is not None
            and prior[0]["npc"] == first["npc"]
            and prior[0]["branch"] in limits
            and prior[-1]["t"] < limits[prior[0]["branch"]]
        ]
        prior = max(candidates, key=lambda group: group[-1]["t"], default=None)
        sources[first["conv"]] = prior[0]["conv"] if prior else None
        processed.append(group)
    return sources


def closing_turn(group: list[dict[str, Any]]) -> dict[str, Any]:
    last = group[-1]
    return {
        **last,
        "id": last["conv"] + "#end",
        "index": len(group),
        "closing": True,
        "history": [
            *last["history"],
            item_json(Message("user", last["phrase"])),
            item_json(Message("assistant", last["sol"])),
        ],
        "window_s": None,
    }


def pick(traces: list[Path], route: Path, directory: Path) -> list[dict[str, Any]]:
    evaluation = json.loads((route / "eval.json").read_text(encoding="utf-8"))
    predictions = {r["id"]: r for r in evaluation["classifiers"]["gemma_phrase"]["predictions"]}
    labels = {r["id"]: r for r in pilot.rows(route / "gold.jsonl")}
    turns = []
    for trace in dict.fromkeys(path.resolve() for path in traces):
        records = read(trace)
        instructions = {r["hash"]: r["text"] for r in records if r.get("type") == "instructions"}
        previous, starts, groups = {}, {}, {}
        for record in sorted((r for r in records if r.get("type") == "turn"), key=lambda r: r["t"]):
            npc, branch = record["npc"], record.get("branch")
            if npc not in pilot.NAMES:
                continue
            key = (npc, branch)
            prior = previous.get(key)
            if prior is None or record["t"] - prior["t"] > 300:
                starts[key] = f"{trace.stem}:{npc}:{branch}:{record['t']}"
            previous[key] = record
            steps = record.get("steps")
            if (
                record.get("trigger", {}).get("kind") != "player"
                or not steps
                or record.get("instructions") not in instructions
            ):
                continue
            identifier = record["id"]
            prediction, label = predictions.get(identifier), labels.get(identifier)
            missing = prediction is None or label is None
            history, situation = _quick_context(record)
            group = groups.setdefault(starts[key], [])
            group.append(
                {
                    "id": identifier,
                    "trace": str(trace),
                    "t": record["t"],
                    "npc": npc,
                    "branch": branch,
                    "conv": starts[key],
                    "index": len(group),
                    "phrase": record["trigger"]["text"],
                    "sol": " ".join(step.get("text", "") for step in steps).strip(),
                    "calls": [call["name"] for step in steps for call in step.get("calls", [])],
                    "history": [item_json(item) for item in history],
                    "situation": situation,
                    "instructions": instructions[record["instructions"]],
                    "window_s": None
                    if prior is None
                    else record["t"] - prior["t"] - (prior.get("total_s") or 0),
                    "route_missing": missing,
                    "oof_predicted": None if missing else prediction["predicted"],
                    "oof_score": None if missing else prediction["score"],
                    "label": None if missing else label["label"],
                    "kind": None if missing else label["kind"],
                }
            )
        turns.extend(turn for group in groups.values() if len(group) >= 2 for turn in group)
    turns.sort(key=lambda turn: (turn["trace"], turn["t"], turn["id"]))
    directory.mkdir(parents=True, exist_ok=True)
    pilot.save_rows(directory / "turns.jsonl", turns)
    convs = [
        {
            "conv": group[0]["conv"],
            "npc": group[0]["npc"],
            "name": pilot.NAMES[group[0]["npc"]],
            "turn_ids": [turn["id"] for turn in group],
        }
        for group in conversations(turns)
    ]
    write_json(directory / "conversations.json", convs)
    print(f"Picked {len(convs)} conversations, {len(turns)} turns", file=sys.stderr)
    return turns


def parse_mind(text: str) -> dict[str, Any]:
    text = text.strip()
    while pilot.starts_with_mood(text):
        text = text[text.index("]") + 1 :].lstrip()
    match = re.search(r"\[Голосу\](.*?)\[Ум\](.*)", text, re.DOTALL)
    if match is None:
        return {"note": "", "mind": None, "say": [], "parse_error": True}
    rest = match[2]
    return {
        "note": match[1].strip(),
        "mind": re.split(r"(?m)^\[Скажу\]", rest, maxsplit=1)[0].strip(),
        "say": re.findall(r"(?m)^\[Скажу\][ \t]*(.+)$", rest),
        "parse_error": False,
    }


async def mind_stream(
    sol: LLMBackend,
    turn: dict[str, Any],
    previous: str,
    prompt: str,
    deadline: float,
    *,
    patch: bool = False,
) -> dict[str, Any]:
    result = {
        "text": "",
        "calls": [],
        "usage": None,
        "error": None,
        "first_token_s": None,
        "note_ready_s": None,
        "total_s": 0.0,
    }
    history = [item for data in turn["history"] if (item := item_from_json(data)) is not None]
    messages = [
        *history,
        Message("developer", turn["situation"]),
        Message("developer", "[Твой ум до этой минуты]\n" + previous),
        Message("developer", prompt),
    ]
    started = time.monotonic()
    try:
        async with (
            asyncio.timeout_at(deadline),
            aclosing(
                sol.stream(
                    instructions=turn["instructions"],
                    messages=messages,
                    tools=MIND_TOOLS,
                    cache_key=f"mind-{turn['npc']}",
                )
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
                marker = "[Правки]" if patch else "[Ум]"
                if result["note_ready_s"] is None and marker in result["text"]:
                    result["note_ready_s"] = elapsed
    except TimeoutError:
        result["error"] = f"Timeout after {TIMEOUT_S:g} s"
    except Exception as error:
        # Provider exceptions can contain private endpoints or credentials.
        result["error"] = f"Model request failed ({type(error).__name__})"
    result["total_s"] = time.monotonic() - started
    return result


def invalidate(directory: Path, identifiers: set[str], names: tuple[str, ...]) -> None:
    for name in names:
        path = directory / name
        if path.exists():
            pilot.save_rows(path, (r for r in pilot.rows(path) if r["id"] not in identifiers))


async def mind(
    directory: Path,
    *,
    effort: str = "low",
    redo_conv: str | None = None,
    prompt_file: str | None = None,
    carry: Path | None = None,
    patch: bool = False,
    sol: LLMBackend | None = None,
) -> None:
    prompt = (HERE / (prompt_file or ("mind_turn_v3.md" if patch else "mind_turn.md"))).read_text(
        encoding="utf-8"
    )
    groups = conversations(pilot.rows(directory / "turns.jsonl"))
    if redo_conv and not any(group[0]["conv"] == redo_conv for group in groups):
        raise SystemExit("Unknown conversation")
    sources = carry_sources(groups, carry)
    results = {r["id"]: r for r in pilot.rows(directory / "minds.jsonl")}
    required = {redo_conv}
    source = sources.get(redo_conv)
    while source is not None:
        end = results.get(source + "#end")
        if end is not None and end.get("error") is None and (not patch or "state" in end):
            break
        required.add(source)
        source = sources[source]
    pending, restarted = [], set()
    for group in groups:
        conv = group[0]["conv"]
        source = sources[conv]
        if redo_conv and conv not in required and source not in restarted:
            continue
        restart = (
            conv == redo_conv
            or source in restarted
            or results.get(group[0]["id"], {}).get("carried_from") != source
            or (patch and "state" not in results.get(group[0]["id"], {}))
        )
        for turn in [*group, closing_turn(group)]:
            record = results.get(turn["id"])
            restart = restart or record is None or record.get("error") is not None
            if restart:
                pending.append(turn)
                restarted.add(conv)
    if not pending:
        return
    identifiers = {turn["id"] for turn in pending}
    invalidate(directory, identifiers, ("answers.jsonl", "judge_minds.jsonl", "judge_answers.jsonl"))
    results = {key: value for key, value in results.items() if key not in identifiers}
    pilot.save_rows(directory / "minds.jsonl", results.values())
    sol = sol if sol is not None else pilot.make_backend("chatgpt", reasoning_effort=effort)
    try:
        for group in groups:
            if group[0]["conv"] not in restarted:
                continue
            source = sources[group[0]["conv"]]
            previous = results[source + "#end"]["mind"] if source else EMPTY_MIND
            state = deepcopy(results[source + "#end"]["state"]) if patch and source else new_state()
            for turn in [*group, closing_turn(group)]:
                if turn["id"] not in identifiers:
                    previous = results[turn["id"]]["mind"]
                    if patch:
                        state = deepcopy(results[turn["id"]]["state"])
                    continue
                if patch:
                    previous, code_changes = prepare_state(state, turn["situation"])
                started = time.monotonic()
                for attempt in range(1, 3):
                    offset = time.monotonic() - started
                    result = await mind_stream(sol, turn, previous, prompt, started + TIMEOUT_S, patch=patch)
                    result["attempts"] = attempt
                    for metric in ("first_token_s", "note_ready_s"):
                        if result[metric] is not None:
                            result[metric] += offset
                    result["total_s"] = time.monotonic() - started
                    if result["error"] is None:
                        break
                if patch:
                    parsed = (
                        parse_patches(result["text"])
                        if result["error"] is None
                        else {"note": "", "patches": [], "say": [], "parse_error": False}
                    )
                    parsed["applied"] = apply_patches(state, parsed["patches"], code_changes)
                    if turn.get("closing"):
                        forget_events(state, code_changes)
                    parsed.update(
                        state=deepcopy(state),
                        code_changes=code_changes,
                        mind=render(state, ids=True),
                        mind_plain=render(state, ids=False),
                    )
                else:
                    parsed = (
                        parse_mind(result["text"])
                        if result["error"] is None
                        else {"note": "", "mind": None, "say": [], "parse_error": False}
                    )
                    if parsed["mind"] is None:
                        parsed["mind"] = previous
                result.update(
                    parsed, id=turn["id"], conv=turn["conv"], index=turn["index"], previous_mind=previous
                )
                if turn.get("closing"):
                    result["closing"] = True
                if turn["index"] == 0:
                    result["carried_from"] = source
                results[turn["id"]] = result
                pilot.save_rows(directory / "minds.jsonl", results.values())
                previous = result["mind"]
                status = result["error"] or ("parse_error" if result["parse_error"] else "ok")
                print(f"Mind {turn['id']}: {status}", file=sys.stderr)
    finally:
        await sol.aclose()


async def speak(
    directory: Path, *, slot: int = 0, quick_at: str | None = None, quick: LLMBackend | None = None
) -> None:
    prompts = {
        "mind": (HERE / "speak_mind.md").read_text(encoding="utf-8"),
        "note": (HYBRID / "speak_hybrid.md").read_text(encoding="utf-8"),
    }
    minds = {r["id"]: r for r in pilot.rows(directory / "minds.jsonl")}
    results = {(r["id"], r["variant"]): r for r in pilot.rows(directory / "answers.jsonl")}
    pending = [
        (turn, variant)
        for turn in pilot.rows(directory / "turns.jsonl")
        if turn["oof_predicted"] is False
        for variant in VARIANTS
        if (turn["id"], variant) not in results or results[turn["id"], variant].get("error") is not None
    ]
    keys = {(turn["id"], variant) for turn, variant in pending}
    path = directory / "judge_answers.jsonl"
    if path.exists() and keys:
        pilot.save_rows(path, (r for r in pilot.rows(path) if (r["id"], r["variant"]) not in keys))
    try:
        for turn, variant in pending:
            state = minds.get(turn["id"], {})
            handoff, stall = None, None
            if state.get("error") is not None or not state.get("note", "").strip():
                result = {
                    "text": "",
                    "error": "no note",
                    "first_token_s": None,
                    "first_sentence_s": None,
                    "total_s": None,
                    "prewarm_s": None,
                    "timings": None,
                }
            else:
                if quick is None:
                    quick = pilot.make_quick(pilot.gemma_url(quick_at), max_tokens=300)
                history = [
                    item
                    for data in turn["history"]
                    if (item := item_from_json(data)) is not None
                    and not isinstance(item, (ToolCall, ToolResult))
                ]
                prefix = [
                    *history,
                    Message(
                        "developer",
                        turn["situation"]
                        + "\n\n"
                        + prompts[variant].format(
                            thought=state["note"], mind=state.get("mind_plain", state["mind"])
                        ),
                    ),
                ]
                result = await pilot.measure(
                    quick,
                    turn["instructions"],
                    [*prefix, Message("user", turn["phrase"])],
                    prefix=prefix,
                    slot=slot,
                )
                if result["error"] is not None:
                    result["error"] = "Model request failed"
                elif not result["text"].strip():
                    result["error"] = "Empty answer"
                text = result["text"].lstrip()
                handoff = text[:5].lower() == "[sol]"
                stall = text[5:].lstrip() if handoff else None
            result.update(
                id=turn["id"],
                variant=variant,
                handoff=handoff,
                stall=stall,
                leaks=gated.LEAK.findall(result["text"]),
            )
            results[turn["id"], variant] = result
            pilot.save_rows(directory / "answers.jsonl", results.values())
            print(f"Spoke {turn['id']} {variant}: {result['error'] or 'ok'}", file=sys.stderr)
    finally:
        if quick is not None:
            await quick.aclose()


def history_text(turn: dict[str, Any]) -> str:
    return "\n".join(
        ("Герой: " if item["who"] == "hero" else "NPC: ") + item["text"] for item in pilot.history_tail(turn)
    )


async def judge(directory: Path, *, redo_minds: bool = False, sol: LLMBackend | None = None) -> None:
    mind_prompt = (HERE / "judge_mind.md").read_text(encoding="utf-8")
    answer_prompt = (HYBRID / "judge_hybrid.md").read_text(encoding="utf-8")
    turns = pilot.rows(directory / "turns.jsonl")
    minds = {r["id"]: r for r in pilot.rows(directory / "minds.jsonl")}
    answers = {(r["id"], r["variant"]): r for r in pilot.rows(directory / "answers.jsonl")}
    if redo_minds:
        pilot.save_rows(directory / "judge_minds.jsonl", [])
    judged_minds = {r["id"]: r for r in pilot.rows(directory / "judge_minds.jsonl")}
    judged_answers = {(r["id"], r["variant"]): r for r in pilot.rows(directory / "judge_answers.jsonl")}
    requests = []
    for group in conversations(turns):
        for turn in [*group, closing_turn(group)]:
            identifier = turn["id"]
            state = minds.get(identifier)
            if state is not None and state["error"] is None and not state["parse_error"]:
                if identifier not in judged_minds or judged_minds[identifier]["error"] is not None:
                    calls = [
                        f"{call['name']} {json.dumps(call['arguments'], ensure_ascii=False)}"
                        for call in state["calls"]
                    ]
                    calls.extend("[Скажу] " + phrase for phrase in state["say"])
                    history = []
                    for data in turn["history"]:
                        item = item_from_json(data)
                        if isinstance(item, Message):
                            prefix = {"user": "Герой: ", "assistant": "NPC: ", "developer": ""}[item.role]
                            text = pilot.spoken(item.content) if item.role == "assistant" else item.content
                            history.append(prefix + text)
                        elif isinstance(item, ToolCall):
                            history.append(
                                f"Вызов: {item.name} {json.dumps(item.arguments, ensure_ascii=False)}"
                            )
                        elif isinstance(item, ToolResult):
                            history.append("Итог вызова: " + item.output)
                    sections = {
                        "Описание персонажа": turn["instructions"],
                        "Обстановка": turn["situation"],
                        "Разговор до этой минуты": "\n".join(history),
                        "Прежний ум": state["previous_mind"],
                        "Новый ум": state["mind"],
                        "Вызовы": "\n".join(calls) or "—",
                    }
                    requests.append((identifier, None, mind_prompt, sections))
                if redo_minds or turn.get("closing"):
                    continue
                for variant in VARIANTS:
                    key = (identifier, variant)
                    answer = answers.get(key)
                    if answer is None or answer["error"] is not None:
                        continue
                    if key in judged_answers and judged_answers[key]["error"] is None:
                        continue
                    thought = state["note"] + ("\n\n" + state["mind"] if variant == "mind" else "")
                    sections = {
                        "Описание персонажа": turn["instructions"],
                        "Обстановка": turn["situation"],
                        "Конец разговора": history_text(turn),
                        "Фраза героя": turn["phrase"],
                        "Мысли NPC": thought,
                        "Реплика NPC": answer["text"],
                    }
                    requests.append((identifier, variant, answer_prompt, sections))
    if not requests:
        return
    sol = sol if sol is not None else pilot.make_backend("chatgpt", reasoning_effort="low")
    try:
        for identifier, variant, prompt, sections in requests:
            content = "\n\n".join(f"{title}\n{body}" for title, body in sections.items())
            result = await pilot.measure(sol, prompt, [Message("user", content)])
            result.update(id=identifier, raw=result.pop("text"), result=None)
            if result["error"] is not None:
                result["error"] = "Model request failed"
            else:
                try:
                    result["result"] = pilot.parse_judge(result["raw"])
                except ValueError:
                    result["error"] = "Invalid judge JSON"
            if variant is None:
                judged_minds[identifier] = result
                pilot.save_rows(directory / "judge_minds.jsonl", judged_minds.values())
            else:
                result["variant"] = variant
                judged_answers[identifier, variant] = result
                pilot.save_rows(directory / "judge_answers.jsonl", judged_answers.values())
            print(f"Judged {identifier} {variant or 'mind turn'}: {result['error'] or 'ok'}", file=sys.stderr)
    finally:
        await sol.aclose()


def taken(answer: dict[str, Any]) -> bool:
    return (
        answer.get("error") is None
        and bool(answer.get("text", "").strip())
        and answer.get("handoff") is False
    )


def verdict(record: dict[str, Any]) -> dict[str, Any]:
    return (record.get("result") or {}) if record.get("error") is None else {}


def sheet(directory: Path, seed: int = pilot.SEED) -> list[dict[str, Any]]:
    turns = pilot.rows(directory / "turns.jsonl")
    minds = {r["id"]: r for r in pilot.rows(directory / "minds.jsonl")}
    answers = {(r["id"], r["variant"]): r for r in pilot.rows(directory / "answers.jsonl")}
    judged = {r["id"]: r for r in pilot.rows(directory / "judge_minds.jsonl")}
    rng, result = random.Random(seed), []
    for turn in turns:
        if not all(taken(answers.get((turn["id"], variant), {})) for variant in VARIANTS):
            continue
        variants = [{"key": "sol", "text": pilot.spoken(turn["sol"])}]
        variants.extend(
            {"key": variant, "text": pilot.spoken(answers[turn["id"], variant]["text"])}
            for variant in VARIANTS
        )
        rng.shuffle(variants)
        result.append(
            {
                "id": turn["id"],
                "npc": pilot.NAMES[turn["npc"]],
                "history": [
                    {**item, "text": pilot.spoken(item["text"])} for item in pilot.history_tail(turn)
                ],
                "situation": turn["situation"],
                "phrase": turn["phrase"],
                "answers": variants,
            }
        )
    write_json(directory / "sheet.json", result)
    timeline = []
    for group in conversations(turns):
        items = []
        for turn in group:
            state = minds.get(turn["id"], {})
            check = verdict(judged.get(turn["id"], {}))
            texts = {}
            for variant in VARIANTS:
                answer = answers.get((turn["id"], variant), {})
                texts[variant] = answer.get("text") if answer.get("error") is None else None
            items.append(
                {
                    "id": turn["id"],
                    "index": turn["index"],
                    "phrase": turn["phrase"],
                    "sol": pilot.spoken(turn["sol"]),
                    "route": "gemma" if turn["oof_predicted"] is False else "sol",
                    "note": state.get("note", ""),
                    "mind": state.get("mind", ""),
                    "say": state.get("say", []),
                    "calls": state.get("calls", []),
                    "judge": {key: check.get(key) for key in ("false_facts", "tools", "stale", "note")}
                    if check
                    else None,
                    "answers": texts,
                    "times": {
                        "note_ready_s": state.get("note_ready_s"),
                        "total_s": state.get("total_s"),
                        "window_s": turn["window_s"],
                    },
                    "error": state.get("error") or ("parse_error" if state.get("parse_error") else None),
                    **{key: state[key] for key in ("patches", "applied", "code_changes") if key in state},
                }
            )
        timeline.append(
            {
                "conv": group[0]["conv"],
                "npc": group[0]["npc"],
                "name": pilot.NAMES[group[0]["npc"]],
                "carried_from": minds.get(group[0]["id"], {}).get("carried_from"),
                "end_mind": minds.get(group[0]["conv"] + "#end", {}).get("mind"),
                "turns": items,
            }
        )
    write_json(directory / "timeline.json", timeline)
    return result


def mind_fields(text: str) -> dict[str, str]:
    pattern = re.compile(r"(?m)^(" + "|".join(FIELDS) + r"):[ \t]*")
    matches = list(pattern.finditer(text))
    return {
        match[1]: text[match.end() : matches[i + 1].start() if i + 1 < len(matches) else len(text)].strip()
        for i, match in enumerate(matches)
    }


def distribution(values) -> dict[str, Any]:
    values = [value for value in values if value is not None]
    return {**pilot.distribution(values), "max": max(values, default=None)}


def summary(directory: Path, ratings: Path | None = None, *, emit: bool = True) -> dict[str, Any]:
    turns = pilot.rows(directory / "turns.jsonl")
    minds = {r["id"]: r for r in pilot.rows(directory / "minds.jsonl")}
    answers = {(r["id"], r["variant"]): r for r in pilot.rows(directory / "answers.jsonl")}
    judged_minds = {r["id"]: r for r in pilot.rows(directory / "judge_minds.jsonl")}
    judged_answers = {(r["id"], r["variant"]): r for r in pilot.rows(directory / "judge_answers.jsonl")}
    groups = conversations(turns)
    mind_turns = [turn for group in groups for turn in [*group, closing_turn(group)]]
    parts = [minds[turn["id"]] for turn in mind_turns if turn["id"] in minds]
    successful = [part for part in parts if part["error"] is None and not part["parse_error"]]
    windows = [turn for turn in turns if turn["window_s"] is not None]
    fits = sum(
        minds.get(turn["id"], {}).get("error") is None
        and not minds.get(turn["id"], {}).get("parse_error")
        and minds.get(turn["id"], {}).get("note_ready_s") is not None
        and minds[turn["id"]]["note_ready_s"] <= turn["window_s"]
        for turn in windows
    )
    total_fits = sum(
        minds.get(turn["id"], {}).get("error") is None
        and not minds.get(turn["id"], {}).get("parse_error")
        and minds.get(turn["id"], {}).get("total_s") is not None
        and minds[turn["id"]]["total_s"] <= turn["window_s"]
        for turn in windows
    )
    stats = {
        "n": len(parts),
        "errors": sum(part["error"] is not None for part in parts),
        "parse_errors": sum(part["parse_error"] for part in parts),
        "window_n": len(windows),
        "fits_window_n": fits,
        "fits_window_fraction": pilot.fraction(fits, len(windows)),
        "total_fits_window_n": total_fits,
        "total_fits_window_fraction": pilot.fraction(total_fits, len(windows)),
    }
    patch_parts = [part for part in parts if "state" in part]
    if patch_parts:
        applied = [item for part in patch_parts for item in part.get("applied", [])]
        errors = Counter(item["error"] for item in applied if not item["ok"])
        closing_chars = {part["conv"]: len(part["mind"]) for part in patch_parts if part.get("closing")}
        stats["patches"] = {
            "per_turn": distribution(len(part.get("patches", [])) for part in patch_parts),
            "by_op": dict(Counter(item["op"] or "unknown" for item in applied)),
            "by_error": dict(errors),
            "error_n": sum(errors.values()),
            "line_n": len(applied),
            "error_fraction": pilot.fraction(sum(errors.values()), len(applied)),
            "code_changes": {
                key: sum(len(part.get("code_changes", {}).get(key, [])) for part in patch_parts)
                for key in ("cooled", "cap_dropped", "forgotten")
            },
            "closing_mind_chars_by_conv": closing_chars,
            "closing_mind_chars_max": max(closing_chars.values(), default=None),
        }
    for metric in ("first_token_s", "note_ready_s", "total_s"):
        stats[metric] = pilot.distribution(part[metric] for part in successful if not part.get("closing"))
    stats["output_tokens"] = distribution((part["usage"] or {}).get("output_tokens") for part in successful)
    lengths, closing_lengths, chain_ends, transitions = {}, {}, {}, []
    for group in groups:
        first = group[0]
        source = minds.get(first["id"], {}).get("carried_from")
        transitions.append({"conv": first["conv"], "carried_from": source, "npc": first["npc"]})
        offset = chain_ends.get(source, -1) + 1
        chain_ends[first["conv"]] = offset + len(group)
        for turn in [*group, closing_turn(group)]:
            state = minds.get(turn["id"])
            if state is not None and state["error"] is None and not state["parse_error"]:
                bucket = closing_lengths if turn.get("closing") else lengths
                bucket.setdefault(offset + turn["index"], []).append(state)

    def length_stats(states):
        return {
            "mind_chars": distribution(len(state["mind"]) for state in states),
            "output_tokens": distribution((state["usage"] or {}).get("output_tokens") for state in states),
        }

    stats["length_by_chain_index"] = [
        {"index": index, **length_stats(states)} for index, states in sorted(lengths.items())
    ]
    closing_parts = [state for states in closing_lengths.values() for state in states]
    stats["closing"] = {
        "n": sum(bool(part.get("closing")) for part in parts),
        **length_stats(closing_parts),
        "length_by_chain_index": [
            {"index": index, **length_stats(states)} for index, states in sorted(closing_lengths.items())
        ],
    }
    changes = {}
    comparisons = [
        (mind_fields(state["previous_mind"]), mind_fields(state["mind"]))
        for state in successful
        if state["previous_mind"] != EMPTY_MIND
    ]
    for field in FIELDS:
        common = [(old[field], new[field]) for old, new in comparisons if field in old and field in new]
        changed = sum(old != new for old, new in common)
        changes[field] = {
            "n": len(common),
            "changed_n": changed,
            "fraction": pilot.fraction(changed, len(common)),
        }
    call_counts, justified_counts, call_items = Counter(), Counter(), []
    for turn in mind_turns:
        state = minds.get(turn["id"], {})
        checks = list(verdict(judged_minds.get(turn["id"], {})).get("tools", []))
        for call in state.get("calls", []):
            call_counts[call["name"]] += 1
            position = next((i for i, check in enumerate(checks) if check["name"] == call["name"]), None)
            check = checks.pop(position) if position is not None else {}
            justified_counts[call["name"]] += check.get("justified") is True
            call_items.append(
                {
                    "id": turn["id"],
                    **call,
                    "say": state.get("say", []),
                    "why": check.get("why"),
                    "justified": check.get("justified"),
                }
            )
    judge_stats = {}
    for field in ("false_facts", "stale"):
        items = [
            {"id": turn["id"], field: check[field]}
            for turn in mind_turns
            if (check := verdict(judged_minds.get(turn["id"], {}))).get(field)
        ]
        judge_stats[field] = len(items)
        judge_stats[field + "_items"] = items
    variants = {}
    for variant in VARIANTS:
        chosen = [(turn, answers[turn["id"], variant]) for turn in turns if (turn["id"], variant) in answers]
        accepted = [(turn, answer) for turn, answer in chosen if taken(answer)]
        violations = []
        for turn, answer in accepted:
            check = verdict(judged_answers.get((turn["id"], variant), {}))
            if check.get("unauthorized") or check.get("commits"):
                violations.append(
                    {
                        "id": turn["id"],
                        "phrase": turn["phrase"],
                        "text": answer["text"],
                        "unauthorized": check.get("unauthorized", []),
                        "commits": check.get("commits", []),
                    }
                )
        variants[variant] = {
            "n": len(chosen),
            "taken_n": len(accepted),
            "handoff_n": sum(answer["error"] is None and answer["handoff"] is True for _, answer in chosen),
            "errors": sum(answer["error"] is not None for _, answer in chosen),
            "leaks": sum(bool(answer["leaks"]) for _, answer in chosen),
            "violations": violations,
            "violations_n": len(violations),
            "first_sentence_s": pilot.distribution(answer["first_sentence_s"] for _, answer in accepted),
        }
    costs = []
    for group in conversations(turns):
        current = len(group)
        with_mind = current + 1 + sum(turn["oof_predicted"] is not False for turn in group)
        costs.append(
            {
                "conv": group[0]["conv"],
                "current": current,
                "with_mind": with_mind,
                "ratio": pilot.fraction(with_mind, current),
            }
        )
    current, with_mind = sum(item["current"] for item in costs), sum(item["with_mind"] for item in costs)
    result = {
        "mind": stats,
        "transitions": transitions,
        "changes": changes,
        "calls": {
            "by_name": dict(call_counts),
            "justified_by_name": dict(justified_counts),
            "n": sum(call_counts.values()),
            "justified_n": sum(justified_counts.values()),
            "items": call_items,
            "say_n": sum(len(part["say"]) for part in parts),
        },
        "judge": judge_stats,
        "variants": variants,
        "cost": {
            "conversations": costs,
            "current": current,
            "with_mind": with_mind,
            "ratio": pilot.fraction(with_mind, current),
        },
        "ratings": None,
        "acceptance": {"criterion_2": None},
    }
    if ratings is not None:
        scores = {"bad": 0, "ok": 1, "good": 2}
        identifiers = {turn["id"] for turn in turns}
        rated = {
            (r["id"], r["key"]): scores[r["rating"]]
            for r in json.loads(ratings.read_text(encoding="utf-8"))
            if r.get("key") in ("sol", *VARIANTS) and r["id"] in identifiers
        }
        rating_stats = {
            "by_key": {
                key: {
                    name: sum(
                        value == score for (identifier, variant), value in rated.items() if variant == key
                    )
                    for name, score in scores.items()
                }
                for key in ("sol", *VARIANTS)
            }
        }
        fractions = []
        for other in ("note", "sol"):
            paired = [
                identifier
                for identifier in identifiers
                if (identifier, "mind") in rated and (identifier, other) in rated
            ]
            better = sum(rated[identifier, "mind"] >= rated[identifier, other] for identifier in paired)
            fraction = pilot.fraction(better, len(paired))
            rating_stats["at_least_" + other] = {"n": len(paired), "at_least_n": better, "fraction": fraction}
            fractions.append(fraction)
        values = [value for (_, key), value in rated.items() if key == "mind"]
        bad = pilot.fraction(values.count(0), len(values))
        rating_stats.update(mind_bad_fraction=bad, mind_rated_n=len(values))
        eligible = [
            turn
            for turn in turns
            if all(taken(answers.get((turn["id"], variant), {})) for variant in VARIANTS)
        ]
        complete = bool(eligible) and all(
            (turn["id"], key) in rated for turn in eligible for key in ("sol", *VARIANTS)
        )
        result["ratings"] = rating_stats
        result["acceptance"]["criterion_2"] = {
            "eligible_n": len(eligible),
            "complete": complete,
            "passed": all(value >= 0.6 for value in fractions) and bad <= 0.15
            if complete and all(value is not None for value in fractions) and bad is not None
            else None,
        }
    write_json(directory / "summary.json", result)
    if emit:
        print(f"Mind: {stats['n']} turns, {stats['errors']} errors, {stats['parse_errors']} parse errors")
        print(
            f"Note fits: {fits}/{len(windows)}; tools: {sum(call_counts.values())}; "
            f"Sol calls: {current} -> {with_mind}"
        )
        for variant, counts in variants.items():
            print(
                f"{variant}: {counts['taken_n']}/{counts['n']} taken, "
                f"{counts['handoff_n']} handoffs, {counts['errors']} errors"
            )
    return result


async def selftest() -> None:
    def mind_text(opinion="Opinion A", trust="Trust A", threads="- Question\n- Item", mood="Calm"):
        return (
            f"Мнение о герое: {opinion}\nВерю: {trust}\nСомневаюсь: —\nХочу: Talk\n"
            f"Нити:\n{threads}\nНастроение: {mood}"
        )

    def reply(text, *, say=False):
        return (
            "[neutral]\n[Голосу]\nVoice note.\n[Ум]\n"
            + text
            + ("\n[Скажу] Listen.\n[Скажу] Wait." if say else "")
        )

    class ScriptedSol(FakeBackend):
        def __init__(self, *args, failures=(), **kwargs):
            super().__init__(*args, **kwargs)
            self.failures = set(failures)
            self.requests = []

        async def stream(self, **kwargs):
            self.requests.append(kwargs)
            if len(self.requests) in self.failures:
                raise LLMError("Synthetic model failure")
            async for event in super().stream(**kwargs):
                if isinstance(event, Done):
                    event = Done(event.text, event.items, Usage(output_tokens=123))
                yield event

    def situation_at(minute):
        day, within_day = divmod(minute, 1440)
        hour, minute = divmod(within_day, 60)
        return f"Время: день {day}, {hour:02}:{minute:02}. Ты у точки TEST_WAYPOINT."

    state = new_state()
    assert render(state, True) == render(state, False) == EMPTY_MIND
    previous, changes = prepare_state(state, "Время: день 1, 08:20, утро.")
    assert previous == EMPTY_MIND and state["minute"] == 1940
    parsed = parse_patches(
        "[Голосу]\nVoice note.\n[Правки]\n\n+ было 8: Helped.\n—\n[Скажу] Listen.\n[Скажу] Wait."
    )
    assert parsed == {
        "note": "Voice note.",
        "patches": ["+ было 8: Helped.", "—"],
        "say": ["Listen.", "Wait."],
        "parse_error": False,
    }
    assert parse_patches("[Голосу] Note without patches.")["parse_error"]
    assert parse_patches("[Правки]\n+ было 8: Helped.")["parse_error"]
    added = apply_patches(
        state,
        [
            "+ было 8: Helped.",
            "+ чувство 2 долго: Grateful.",
            "+ хочу сейчас: Talk.",
            "+ верю сказал: A promise.",
            "+ сомневаюсь: His story.",
            "+ о Old Merchant: Honest.",
            "+ нить: A letter.",
            "= мнение: Helpful.",
            "= настроение: Calm.",
            "—",
        ],
        changes,
    )
    assert all(item["ok"] and item["error"] is None for item in added)
    assert {item["op"] for item in added} == {"+", "=", "noop"}
    assert state["events"][0] == {
        "id": "б1",
        "time": "день 1, 08:20",
        "minute": 1940,
        "importance": 8,
        "text": "Helped.",
    }
    assert state["feelings"][0]["updated_minute"] == 1940
    changed = apply_patches(
        state,
        [
            "~ б1 9: Helped again.",
            "~ ч1 3 час",
            "~ ж1 скоро",
            "~ в1 видел: Saw it.",
            "~ с1: A different story.",
            "~ н1: Another letter.",
            "+ о old merchant: Reliable.",
        ],
        changes,
    )
    assert all(item["ok"] for item in changed)
    assert state["events"][0]["importance"] == 9
    assert state["feelings"][0]["strength"] == 3
    assert state["feelings"][0]["duration"] == "час"
    assert state["wishes"][0]["term"] == "скоро"
    assert state["beliefs"][0]["source"] == "видел"
    assert state["others"][0]["id"] == "д1" and len(state["others"]) == 1
    assert state["others"][0]["text"] == "Reliable."
    assert apply_patches(state, ["+ о Another Merchant: Reliable."], changes)[0]["id"] == "д2"
    assert len(state["others"]) == 2
    rejected = apply_patches(
        state,
        ["garbage", "+ было 11: Invalid.", "~ ч99 2", "+ верю сказал:  SAW   IT!!!"],
        changes,
    )
    assert [item["error"] for item in rejected] == ["syntax", "bad_param", "unknown_id", "duplicate"]
    assert all(not item["ok"] for item in rejected)
    plain = render(state, False)
    assert not re.search(r"[бчжвсдн]\d+", plain) and "важность" not in plain
    assert "(день 1, 08:20)" in plain and "(сила 3, час)" in plain
    assert "(скоро)" in plain and "(видел)" in plain and "Old Merchant" in plain
    assert "Мнение о герое: Helpful." in plain and "Настроение: Calm." in plain
    removed = apply_patches(state, ["- с1—closed", "– н1.done", "− ж1 ignored tail"], changes)
    assert all(item["ok"] and item["op"] == "-" for item in removed)
    assert not state["doubts"] and not state["threads"] and not state["wishes"]
    assert apply_patches(state, ["+ нить: New letter."], changes)[0]["id"] == "н2"
    assert "Сомневаюсь:\n- —" in render(state, True)

    for duration, interval in (("час", 60), ("день", 1440), ("долго", 10080)):
        cooling = new_state()
        _, cooling_changes = prepare_state(cooling, situation_at(1440))
        apply_patches(cooling, [f"+ чувство 3 {duration}: Anger."], cooling_changes)
        prepare_state(cooling, situation_at(1440 + 2 * interval + 17))
        feeling = cooling["feelings"][0]
        assert feeling["strength"] == 1 and feeling["updated_minute"] == 1440 + 2 * interval
        _, cooling_changes = prepare_state(cooling, situation_at(1440 + 3 * interval))
        assert not cooling["feelings"] and cooling_changes["cooled"] == [{"id": "ч1", "text": "Anger."}]
    refreshing = new_state()
    _, refreshing_changes = prepare_state(refreshing, situation_at(1440))
    apply_patches(refreshing, ["+ чувство 3 час: Anger."], refreshing_changes)
    prepare_state(refreshing, situation_at(1510))
    apply_patches(refreshing, ["~ ч1: Renewed anger."], refreshing_changes)
    assert refreshing["feelings"][0]["updated_minute"] == 1510
    assert not prepare_state(refreshing, situation_at(1570))[0].startswith("С прошлого раза")
    elapsed, _ = prepare_state(refreshing, situation_at(1570 + 2 * 1440 + 3 * 60))
    assert elapsed.startswith("С прошлого раза прошло: 2 дн. 3 ч\n")
    old_minute = refreshing["minute"]
    prepare_state(refreshing, "No recorded time.")
    assert refreshing["minute"] == old_minute

    capped = new_state()
    _, cap_changes = prepare_state(capped, situation_at(1440))
    for section, prefix, cap in (
        ("beliefs", "+ верю слышал:", 10),
        ("doubts", "+ сомневаюсь:", 5),
        ("threads", "+ нить:", 8),
        ("others", "+ о Person", 8),
    ):
        lines = [
            f"{prefix} {index}: Item {index}." if section == "others" else f"{prefix} Item {index}."
            for index in range(cap + 1)
        ]
        assert all(item["ok"] for item in apply_patches(capped, lines, cap_changes))
        assert len(capped[section]) == cap and capped[section][0]["text"] == "Item 1."
    for term in ("сейчас", "скоро", "давно"):
        apply_patches(capped, [f"+ хочу {term}: {term} {i}." for i in range(3)], cap_changes)
        assert sum(item["term"] == term for item in capped["wishes"]) == 2
    assert len(cap_changes["cap_dropped"]) == 7

    events = new_state()
    for index in range(15):
        _, event_changes = prepare_state(events, situation_at(1440 + index))
        importance = 10 if index == 0 else 4
        apply_patches(events, [f"+ было {importance}: Event {index}."], event_changes)
    event_lines = render(events, True).splitlines()
    assert [re.match(r"- (б\d+) ", line)[1] for line in event_lines if line.startswith("- б")] == [
        "б1",
        *[f"б{i}" for i in range(5, 16)],
    ]
    apply_patches(
        events, ["+ было 3: Greeting.", "+ хочу сейчас: Talk.", "+ хочу скоро: Rest."], event_changes
    )
    forget_events(events, event_changes)
    assert event_changes["forgotten"] == [{"id": "б16", "text": "Greeting."}, {"id": "ж1", "text": "Talk."}]
    assert len(events["events"]) == 15 and [item["id"] for item in events["wishes"]] == ["ж2"]

    with (
        TemporaryDirectory(prefix="sol-mind-") as temporary,
        patch.object(pilot, "make_backend", side_effect=AssertionError("Selftest must use a fake Sol")),
        patch.object(pilot, "make_quick", side_effect=AssertionError("Selftest must use a fake Gemma")),
    ):
        root = Path(temporary)
        trace, route_dir, directory = root / "trace.jsonl", root / "route", root / "mind"
        route_dir.mkdir()
        npc = "BAU_4300_ADDON_CAVALORN"
        records = [{"type": "instructions", "hash": "persona", "text": "Recorded NPC description."}]
        old_call = ToolCall("look", {"target": "hero"}, "old-call")
        old_result = ToolResult("old-call", "Recorded tool output.")
        remark = "[Event: the hero stole a purse.]"
        history = [
            Message("user", "Earlier hero."),
            Message("assistant", "[neutral] Earlier NPC."),
            Message("developer", remark),
            old_call,
            old_result,
        ]

        def record(identifier, at, *, kind="player", character=npc, branch="branch"):
            phrase = (
                "Repeated phrase." if identifier in ("turn-0", "turn-1") else f"Hero phrase {identifier}."
            )
            return {
                "type": "turn",
                "id": identifier,
                "npc": character,
                "branch": branch,
                "t": at,
                "total_s": 2,
                "instructions": "persona",
                "trigger": {"kind": kind, "text": phrase},
                "steps": [
                    {
                        "first_token_s": 0.1,
                        "llm": "quick",
                        "text": "[neutral] Recorded answer.",
                        "added": [
                            item_json(item)
                            for item in [
                                *history,
                                Message("developer", "Recorded situation."),
                                Message("user", phrase),
                            ]
                        ],
                        "calls": [],
                    },
                    {"text": "Another sentence.", "calls": [{"name": "look"}]},
                ],
            }

        records += [
            record("ambient-0", 0, kind="event"),
            record("turn-0", 10),
            record("single", 20, character="NONE_100_XARDAS"),
            record("other-branch", 40, branch="other"),
            record("ambient-1", 310, kind="event"),
            record("turn-1", 330),
            {**record("empty-steps", 340), "steps": []},
            record("turn-2", 350),
            {**record("no-instructions", 360), "instructions": "absent"},
            record("turn-3", 370),
            record("turn-4", 800),
            record("turn-5", 820),
        ]
        # Input order is immaterial; non-player turns bridge the first conversation.
        pilot.save_rows(trace, [records[0], *reversed(records[1:])])
        write_json(
            route_dir / "eval.json",
            {
                "classifiers": {
                    "gemma_phrase": {
                        "predictions": [
                            {"id": f"turn-{i}", "predicted": i == 2, "score": 0.9 if i == 2 else 0.1}
                            for i in range(5)
                        ]
                    }
                }
            },
        )
        pilot.save_rows(
            route_dir / "gold.jsonl",
            [
                {"id": f"turn-{i}", "label": i == 2, "kind": "decision" if i == 2 else "chat"}
                for i in range(6)
            ],
        )
        turns = pick([trace, trace], route_dir, directory)
        assert [turn["id"] for turn in turns] == [f"turn-{i}" for i in range(6)]
        convs = json.loads((directory / "conversations.json").read_text())
        assert len(convs) == 2 and [len(conv["turn_ids"]) for conv in convs] == [4, 2]
        assert [turn["index"] for turn in turns] == [0, 1, 2, 3, 0, 1]
        assert convs[0]["conv"] == f"trace:{npc}:branch:0"
        assert [turn["window_s"] for turn in turns] == [8, 18, 8, 8, 428, 18]
        assert turns[0]["calls"] == ["look"] and turns[0]["sol"].endswith("Another sentence.")
        assert turns[0]["oof_predicted"] is False and turns[2]["oof_predicted"] is True
        assert turns[2]["oof_score"] == 0.9 and turns[2]["label"] is True and turns[2]["kind"] == "decision"
        assert turns[5]["route_missing"] is True
        assert all(turns[5][key] is None for key in ("oof_predicted", "oof_score", "label", "kind"))
        assert all(
            turn["instructions"] == "Recorded NPC description." and Path(turn["trace"]).is_absolute()
            for turn in turns
        )

        a = mind_text()
        b = mind_text(trust="Trust B", threads="- Item")
        e = mind_text(opinion="Opinion E", mood="Curious")
        sol = ScriptedSol(
            script=[
                FakeStep(reply(a, say=True), [ToolCall("set_intent", {"goal": "talk"})]),
                FakeStep("Missing headings."),
                FakeStep(reply(b)),
                FakeStep(reply(b)),
                FakeStep(reply(a)),
                FakeStep(reply(e)),
                FakeStep(reply(e)),
            ],
            failures={3, 5, 6},
            delay=0,
            chunk=1,
        )
        with patch.object(asyncio, "timeout_at", wraps=asyncio.timeout_at) as deadlines:
            await mind(directory, sol=sol)
        assert len(deadlines.call_args_list) == 10
        assert deadlines.call_args_list[2] == deadlines.call_args_list[3]
        assert deadlines.call_args_list[4] == deadlines.call_args_list[5]
        states = {r["id"]: r for r in pilot.rows(directory / "minds.jsonl")}
        assert len(states) == 8 and len(sol.requests) == 10
        assert states["turn-0"]["note"] == "Voice note." and states["turn-0"]["mind"] == a
        assert states["turn-0"]["calls"] == [{"name": "set_intent", "arguments": {"goal": "talk"}}]
        assert states["turn-0"]["say"] == ["Listen.", "Wait."]
        assert states["turn-0"]["usage"]["output_tokens"] == 123
        assert states["turn-1"]["parse_error"] is True and states["turn-1"]["error"] is None
        assert states["turn-1"]["mind"] == a and states["turn-1"]["note"] == ""
        assert states["turn-2"]["attempts"] == 2 and states["turn-2"]["error"] is None
        assert states["turn-3"]["attempts"] == 2 and states["turn-3"]["error"] is not None
        assert states["turn-3"]["mind"] == b and states["turn-3"]["note"] == ""
        assert states["turn-4"]["previous_mind"] == EMPTY_MIND
        end_ids = {conv["conv"] + "#end" for conv in convs}
        for group, request in zip(conversations(turns), (sol.requests[6], sol.requests[9]), strict=True):
            end = states[group[0]["conv"] + "#end"]
            assert end["closing"] is True and end["index"] == len(group)
            assert request["messages"][-5:-3] == [
                Message("user", group[-1]["phrase"]),
                Message("assistant", group[-1]["sol"]),
            ]
            assert request["messages"][-3] == Message("developer", group[-1]["situation"])
        assert all(states[group[0]["id"]]["carried_from"] is None for group in conversations(turns))
        assert all("state" not in state and "mind_plain" not in state for state in states.values())
        assert sol.requests[0]["messages"][-2] == Message(
            "developer", "[Твой ум до этой минуты]\n" + EMPTY_MIND
        )
        assert (
            sol.requests[1]["messages"][-2]
            == sol.requests[2]["messages"][-2]
            == Message("developer", "[Твой ум до этой минуты]\n" + a)
        )
        assert sol.requests[2]["messages"] == sol.requests[3]["messages"]
        assert sol.requests[4]["messages"] == sol.requests[5]["messages"]
        assert sol.requests[0]["messages"][-1] == Message("developer", (HERE / "mind_turn.md").read_text())
        assert sol.requests[0]["messages"][-3] == Message("developer", "Recorded situation.")
        assert item_from_json(item_json(old_call)) in sol.requests[0]["messages"]
        assert old_result in sol.requests[0]["messages"]
        assert not any(
            isinstance(item, Message) and item.content == turns[0]["phrase"]
            for item in sol.requests[0]["messages"]
        )
        assert {tool.name for tool in sol.requests[0]["tools"]} == {
            "set_intent",
            "give_item",
            "end_conversation",
            "propose_deal",
            "withdraw_deal",
            "take_offense",
        }
        assert all(request["cache_key"] == f"mind-{npc}" for request in sol.requests)
        for state in states.values():
            if state["error"] is None and not state["parse_error"]:
                assert 0 <= state["first_token_s"] < state["note_ready_s"] <= state["total_s"]
        assert parse_mind("[Голосу] Note only.")["parse_error"] is True

        quick = FakeBackend(
            script=[
                FakeStep("[friendly] First answer."),
                FakeStep("[neutral] Second answer."),
                FakeStep("  [SoL]  [neutral] Let me think."),
                FakeStep("[neutral] give_item({})"),
            ],
            delay=0,
        )
        await speak(directory, quick=quick, slot=3)
        answers = {(r["id"], r["variant"]): r for r in pilot.rows(directory / "answers.jsonl")}
        assert len(answers) == 8 and len(quick.calls) == len(quick.prewarms) == 4
        assert {identifier for identifier, _ in answers} == {"turn-0", "turn-1", "turn-3", "turn-4"}
        assert all(
            answers[identifier, variant]["error"] == "no note"
            for identifier in ("turn-1", "turn-3")
            for variant in VARIANTS
        )
        assert answers["turn-4", "mind"]["handoff"] is True
        assert answers["turn-4", "mind"]["stall"] == "[neutral] Let me think."
        assert answers["turn-4", "note"]["leaks"] == ["give_item("]
        assert quick.slot_ids == quick.prewarm_slot_ids == [3] * 4
        for i, ((_, prefix), (instructions, messages, tools)) in enumerate(
            zip(quick.prewarms, quick.calls, strict=True)
        ):
            assert messages[:-1] == prefix and messages[-1].role == "user" and not tools
            assert instructions == turns[0]["instructions"]
            assert not any(isinstance(item, (ToolCall, ToolResult)) for item in messages)
            assert "Voice note." in messages[-2].content
            assert (a in messages[-2].content) is (i % 2 == 0)
        assert quick.temperatures == [0.6] * 4
        await speak(directory, quick=quick)
        assert len(quick.calls) == 4

        clean = {
            "false_facts": [],
            "tools": [{"name": "set_intent", "justified": True, "why": "Reason."}],
            "stale": [],
            "note": "",
            "commits": [],
            "unauthorized": [],
        }
        referee = FakeBackend("```json\n" + json.dumps(clean) + "\n```", delay=0)
        await judge(directory, sol=referee)
        await judge(directory, sol=referee)
        jm, ja = pilot.rows(directory / "judge_minds.jsonl"), pilot.rows(directory / "judge_answers.jsonl")
        assert len(jm) == 6 and len(ja) == 4 and len(referee.calls) == 10
        assert end_ids <= {row["id"] for row in jm}
        assert not end_ids & {row["id"] for row in ja}
        closing_history = next(
            messages[0].content
            for _, messages, _ in referee.calls
            if "Герой: " + turns[3]["phrase"] in messages[0].content
        )
        assert "NPC: " + pilot.spoken(turns[3]["sol"]) in closing_history
        assert {(r["id"], r["variant"]) for r in ja} == {
            (identifier, variant) for identifier in ("turn-0", "turn-4") for variant in VARIANTS
        }
        content = referee.calls[0][1][0]
        assert content.role == "user"
        assert all(
            title + "\n" in content.content
            for title in (
                "Описание персонажа",
                "Обстановка",
                "Разговор до этой минуты",
                "Прежний ум",
                "Новый ум",
                "Вызовы",
            )
        )
        assert "Прежний ум\n" + EMPTY_MIND in content.content and "[Скажу] Listen." in content.content
        assert 'set_intent {"goal": "talk"}' in content.content and turns[0]["phrase"] not in content.content
        recorded_history = "\n".join(
            [
                "Герой: Earlier hero.",
                "NPC: Earlier NPC.",
                remark,
                'Вызов: look {"target": "hero"}',
                "Итог вызова: Recorded tool output.",
            ]
        )
        assert "Разговор до этой минуты\n" + recorded_history + "\n\nПрежний ум\n" in content.content
        later = next(
            messages[0].content for _, messages, _ in referee.calls if "Новый ум\n" + b in messages[0].content
        )
        assert "Разговор до этой минуты\n" + recorded_history + "\n\nПрежний ум\n" in later
        assert "Вызовы\n—" in later
        assert "Мысли NPC\nVoice note.\n\n" + a in referee.calls[1][1][0].content
        assert "Мысли NPC\nVoice note.\n\nРеплика NPC" in referee.calls[2][1][0].content
        jm[0]["error"] = ja[0]["error"] = "Synthetic judge failure"
        pilot.save_rows(directory / "judge_minds.jsonl", jm)
        pilot.save_rows(directory / "judge_answers.jsonl", ja)
        await judge(directory, sol=referee)
        assert len(referee.calls) == 12
        jm, ja = pilot.rows(directory / "judge_minds.jsonl"), pilot.rows(directory / "judge_answers.jsonl")
        ja[0]["error"] = "Synthetic judge failure"
        pilot.save_rows(directory / "judge_answers.jsonl", ja)
        answer_judgments = (directory / "judge_answers.jsonl").read_bytes()
        await judge(directory, redo_minds=True, sol=referee)
        assert len(referee.calls) == 18
        assert (directory / "judge_answers.jsonl").read_bytes() == answer_judgments
        jm = pilot.rows(directory / "judge_minds.jsonl")
        assert len(jm) == 6 and all(row["error"] is None for row in jm)
        ja[0]["error"] = None
        jm[0]["result"]["false_facts"] = ["Unsupported fact."]
        jm[0]["result"]["stale"] = ["Closed thread."]
        ja[0]["result"]["unauthorized"] = ["Unsupported commitment."]
        ja[0]["result"]["commits"] = ["Commitment."]
        pilot.save_rows(directory / "judge_minds.jsonl", jm)
        pilot.save_rows(directory / "judge_answers.jsonl", ja)

        comparison = sheet(directory)
        assert comparison == sheet(directory) and len(comparison) == 1
        assert comparison[0]["id"] == "turn-0" and comparison[0]["npc"] == "Кавалорн"
        assert {answer["key"] for answer in comparison[0]["answers"]} == {"sol", "mind", "note"}
        assert all("[" not in answer["text"] for answer in comparison[0]["answers"])
        assert comparison[0]["history"] == [
            {"who": "hero", "text": "Earlier hero."},
            {"who": "npc", "text": "Earlier NPC."},
        ]
        timeline = json.loads((directory / "timeline.json").read_text())
        assert len(timeline) == 2 and [len(conv["turns"]) for conv in timeline] == [4, 2]
        assert timeline[0]["turns"][0]["judge"]["false_facts"] == ["Unsupported fact."]
        assert timeline[0]["turns"][1]["answers"] == {"mind": None, "note": None}
        assert timeline[1]["turns"][1]["route"] == "sol"
        assert timeline[0]["turns"][0]["times"]["window_s"] == 8
        assert timeline[0]["turns"][1]["error"] == "parse_error"
        assert timeline[0]["carried_from"] is None and timeline[0]["end_mind"] == b
        assert timeline[1]["end_mind"] == e
        ratings = root / "ratings.json"
        write_json(
            ratings,
            [{"id": "turn-0", "key": key, "rating": "good"} for key in ("sol", *VARIANTS)]
            + [{"id": "turn-1", "rating": "unkeyed row is skipped"}],
        )
        states["turn-1"]["usage"]["output_tokens"] = 999
        pilot.save_rows(directory / "minds.jsonl", states.values())
        report = summary(directory, ratings, emit=False)
        assert (
            report["mind"]["n"] == 8 and report["mind"]["errors"] == 1 and report["mind"]["parse_errors"] == 1
        )
        assert report["mind"]["output_tokens"] == {"n": 6, "median": 123, "p90": 123, "max": 123}
        assert report["mind"]["note_ready_s"]["n"] == 4
        assert report["mind"]["fits_window_n"] == 4 and report["mind"]["window_n"] == 6
        assert report["changes"]["Мнение о герое"] == {"n": 4, "changed_n": 1, "fraction": 0.25}
        assert report["changes"]["Верю"]["fraction"] == report["changes"]["Нити"]["fraction"] == 0.25
        assert report["changes"]["Хочу"]["fraction"] == 0
        assert report["calls"]["by_name"] == {"set_intent": 1} and report["calls"]["justified_n"] == 1
        assert report["calls"]["say_n"] == 2 and report["calls"]["items"][0]["why"] == "Reason."
        assert report["judge"]["false_facts"] == report["judge"]["stale"] == 1
        assert report["variants"]["mind"]["taken_n"] == 1 and report["variants"]["mind"]["handoff_n"] == 1
        assert report["variants"]["mind"]["errors"] == 2 and report["variants"]["note"]["leaks"] == 1
        assert report["variants"]["mind"]["violations_n"] == 1
        assert report["variants"]["mind"]["first_sentence_s"]["n"] == 1
        assert report["cost"]["current"] == 6 and report["cost"]["with_mind"] == 10
        assert report["cost"]["ratio"] == 10 / 6
        assert [cost["with_mind"] for cost in report["cost"]["conversations"]] == [6, 4]
        assert report["ratings"]["by_key"]["mind"] == {"good": 1, "ok": 0, "bad": 0}
        assert (
            report["ratings"]["at_least_note"]["fraction"]
            == report["ratings"]["at_least_sol"]["fraction"]
            == 1
        )
        assert report["acceptance"]["criterion_2"]["passed"] is True
        write_json(
            ratings,
            [
                {"id": "turn-0", "key": key, "rating": "bad" if key == "mind" else "good"}
                for key in ("sol", *VARIANTS)
            ],
        )
        assert summary(directory, ratings, emit=False)["acceptance"]["criterion_2"]["passed"] is False
        write_json(ratings, [{"id": "turn-0", "key": "mind", "rating": "good"}])
        assert summary(directory, ratings, emit=False)["acceptance"]["criterion_2"]["passed"] is None

        # A failed Gemma row is replaced and its old judge row is invalidated.
        answers["turn-0", "mind"]["error"] = "Synthetic request failure"
        pilot.save_rows(directory / "answers.jsonl", answers.values())
        empty_quick = FakeBackend("", delay=0)
        await speak(directory, quick=empty_quick)
        assert len(empty_quick.calls) == 1
        assert (
            next(
                r
                for r in pilot.rows(directory / "answers.jsonl")
                if (r["id"], r["variant"]) == ("turn-0", "mind")
            )["error"]
            == "Empty answer"
        )
        retry_quick = FakeBackend("[neutral] Recovered answer.", delay=0)
        await speak(directory, quick=retry_quick)
        assert len(retry_quick.calls) == 1
        assert ("turn-0", "mind") not in {
            (r["id"], r["variant"]) for r in pilot.rows(directory / "judge_answers.jsonl")
        }

        before_recovery = pilot.rows(directory / "minds.jsonl")
        next(state for state in before_recovery if state["id"] == "turn-2")["error"] = (
            "Synthetic model failure"
        )
        pilot.save_rows(directory / "minds.jsonl", before_recovery)
        recovery = FakeBackend(reply(b), delay=0)
        await mind(directory, sol=recovery)
        recovered = {r["id"]: r for r in pilot.rows(directory / "minds.jsonl")}
        assert len(recovery.calls) == 3 and all(state["error"] is None for state in recovered.values())
        assert recovered["turn-0"] == states["turn-0"] and recovered["turn-4"] == states["turn-4"]
        assert recovered["turn-1"] == states["turn-1"]
        for name in ("answers.jsonl", "judge_minds.jsonl", "judge_answers.jsonl"):
            assert not {"turn-2", "turn-3", convs[0]["conv"] + "#end"} & {
                r["id"] for r in pilot.rows(directory / name)
            }
        await mind(directory, sol=recovery)
        assert len(recovery.calls) == 3
        assert {r["id"]: r for r in pilot.rows(directory / "minds.jsonl")}["turn-1"] == states["turn-1"]
        await speak(directory, quick=retry_quick)
        assert len(retry_quick.calls) == 3
        await judge(directory, sol=referee)
        assert len(pilot.rows(directory / "judge_minds.jsonl")) == 7
        assert len(pilot.rows(directory / "judge_answers.jsonl")) == 6

        before_redo = pilot.rows(directory / "minds.jsonl")
        unrelated = next(state for state in before_redo if state["id"] == "turn-5")
        unrelated["error"] = "Synthetic failure in another conversation"
        pilot.save_rows(directory / "minds.jsonl", before_redo)
        redo = FakeBackend(reply(a), delay=0)
        await mind(directory, redo_conv=convs[0]["conv"], sol=redo)
        assert len(redo.calls) == 5 and redo.calls[0][1][-2].content.endswith(EMPTY_MIND)
        redone = {r["id"]: r for r in pilot.rows(directory / "minds.jsonl")}
        assert len(redone) == 8 and redone["turn-4"] == states["turn-4"] and redone["turn-5"] == unrelated
        for name in ("answers.jsonl", "judge_minds.jsonl", "judge_answers.jsonl"):
            assert {r["id"] for r in pilot.rows(directory / name)} <= {
                "turn-4",
                "turn-5",
                convs[1]["conv"] + "#end",
            }
        assert all(
            (directory / name).is_file()
            for name in (
                "turns.jsonl",
                "conversations.json",
                "minds.jsonl",
                "answers.jsonl",
                "judge_minds.jsonl",
                "judge_answers.jsonl",
                "sheet.json",
                "timeline.json",
                "summary.json",
            )
        )

        v2 = (
            a + "\nЧто было между нами:\n- noon: helped\n- earlier: met\n"
            "Чувства:\n- gratitude, strength 2: helped\n"
            "Желания:\n- now: talk\n- soon: rest\n- long ago: return\n"
            "О других:\n- Merchant: honest"
        )
        v2_end = v2.replace("Opinion A", "Opinion Z")
        fields = mind_fields(v2)
        assert set(fields) == set(FIELDS)
        assert fields["Что было между нами"] == "- noon: helped\n- earlier: met"
        assert fields["Чувства"] == "- gratitude, strength 2: helped"
        assert fields["Желания"] == "- now: talk\n- soon: rest\n- long ago: return"
        assert fields["О других"] == "- Merchant: honest"
        assert fields["Настроение"] == "Calm" and fields["Хочу"] == "Talk"
        assert report["changes"]["Чувства"] == {"n": 0, "changed_n": 0, "fraction": None}

        memory_db = root / "memory.sqlite3"
        with closing(sqlite3.connect(memory_db)) as db:
            db.execute("CREATE TABLE branches (id TEXT, parent TEXT, parent_seq INTEGER, created REAL)")
            db.executemany(
                "INSERT INTO branches VALUES (?, ?, 0, ?)",
                [
                    ("root", None, 0),
                    ("child", "root", 50),
                    ("grandchild", "child", 75),
                    ("sibling", "root", 5),
                ],
            )
            db.commit()
        db_bytes = memory_db.read_bytes()

        def meeting(conv, branch, at, *, character=npc):
            return [
                {
                    **turns[i],
                    "id": f"{conv}-{i}",
                    "conv": conv,
                    "branch": branch,
                    "t": at + i,
                    "index": i,
                    "npc": character,
                }
                for i in range(2)
            ]

        meetings = [
            meeting("early", "root", 10),
            meeting("late", "root", 60),
            meeting("child-first", "child", 70),
            meeting("child-next", "child", 80),
            meeting("sibling", "sibling", 90),
            meeting("unknown", "missing", 100),
            meeting("unknown-next", "missing", 110),
            meeting("grandchild", "grandchild", 120),
        ]
        expected_sources = {
            "early": None,
            "late": "early",
            "child-first": "early",
            "child-next": "child-first",
            "sibling": None,
            "unknown": None,
            "unknown-next": "unknown",
            "grandchild": "child-first",
        }
        assert carry_sources(meetings, memory_db) == expected_sources
        assert set(carry_sources(meetings, None).values()) == {None}
        # Neither an overlapping conversation nor a different NPC can precede the child.
        overlap = meeting("overlap", "root", 40)
        overlap[-1]["t"] = 200
        boundary = meeting("boundary", "root", 49)
        another = meeting("other-npc", "child", 65, character="NONE_100_XARDAS")
        candidates = conversations(
            [turn for group in [*meetings, overlap, boundary, another] for turn in group]
        )
        assert carry_sources(candidates, memory_db)["child-first"] == "early"

        carried = root / "carried"
        carried.mkdir()
        # Deliberately reverse the file order: the first-turn timestamp determines processing order.
        pilot.save_rows(carried / "turns.jsonl", reversed([turn for group in meetings for turn in group]))
        carrier = ScriptedSol(
            script=[
                FakeStep(reply(text))
                for position in range(len(meetings))
                for text in (a if position == 0 else v2, v2, v2_end)
            ],
            delay=0,
        )
        await mind(carried, carry=memory_db, prompt_file="mind_turn_v2.md", sol=carrier)
        carried_states = {r["id"]: r for r in pilot.rows(carried / "minds.jsonl")}
        assert len(carrier.requests) == len(carried_states) == 24
        assert memory_db.read_bytes() == db_bytes
        for position, group in enumerate(meetings):
            source = expected_sources[group[0]["conv"]]
            previous = carried_states[source + "#end"]["mind"] if source else EMPTY_MIND
            assert carried_states[group[0]["id"]]["carried_from"] == source
            assert carrier.requests[position * 3]["messages"][-2] == Message(
                "developer", "[Твой ум до этой минуты]\n" + previous
            )
        assert carrier.requests[0]["messages"][-1] == Message(
            "developer", (HERE / "mind_turn_v2.md").read_text()
        )
        await mind(carried, carry=memory_db, sol=carrier)
        assert len(carrier.requests) == 24
        carried_states["early#end"]["usage"]["output_tokens"] = 700
        pilot.save_rows(carried / "minds.jsonl", carried_states.values())
        sheet(carried)
        carried_timeline = json.loads((carried / "timeline.json").read_text())
        assert carried_timeline[2]["carried_from"] == "early"
        assert carried_timeline[2]["end_mind"] == v2_end
        growth = summary(carried, emit=False)
        assert growth["transitions"] == [
            {"conv": group[0]["conv"], "carried_from": expected_sources[group[0]["conv"]], "npc": npc}
            for group in meetings
        ]
        assert growth["changes"]["Чувства"] == {"n": 20, "changed_n": 0, "fraction": 0}
        assert growth["changes"]["Мнение о герое"] == {"n": 21, "changed_n": 13, "fraction": 13 / 21}
        lengths = growth["mind"]["length_by_chain_index"]
        assert [item["index"] for item in lengths] == [0, 1, 3, 4, 6, 7]
        assert lengths[0]["mind_chars"] == {"n": 3, "median": len(v2), "p90": len(v2), "max": len(v2)}
        assert lengths[-1]["output_tokens"]["median"] == lengths[-1]["output_tokens"]["max"] == 123
        assert growth["mind"]["output_tokens"]["max"] == 700
        ends = growth["mind"]["closing"]
        assert ends["n"] == 8 and ends["mind_chars"]["median"] == ends["mind_chars"]["max"] == len(v2)
        assert ends["output_tokens"]["median"] == 123 and ends["output_tokens"]["max"] == 700
        assert [item["index"] for item in ends["length_by_chain_index"]] == [2, 5, 8]
        assert growth["mind"]["window_n"] == 16

        # A missing closing row resumes at that row and restarts every dependent meeting.
        pilot.save_rows(
            carried / "minds.jsonl", (r for r in carried_states.values() if r["id"] != "early#end")
        )
        resume = FakeBackend(reply(v2), delay=0)
        await mind(carried, carry=memory_db, redo_conv="child-first", sol=resume)
        assert len(resume.calls) == 13
        completed = {r["id"]: r for r in pilot.rows(carried / "minds.jsonl")}
        assert completed["early-0"] == carried_states["early-0"]
        assert completed["sibling-0"] == carried_states["sibling-0"]
        for name in ("answers.jsonl", "judge_minds.jsonl", "judge_answers.jsonl"):
            pilot.save_rows(carried / name, ({"id": identifier} for identifier in completed))
        redo_carried = FakeBackend(reply(v2), delay=0)
        await mind(carried, carry=memory_db, redo_conv="child-first", sol=redo_carried)
        assert len(redo_carried.calls) == 9
        assert redo_carried.calls[0][1][-2] == Message("developer", "[Твой ум до этой минуты]\n" + v2)
        for name in ("answers.jsonl", "judge_minds.jsonl", "judge_answers.jsonl"):
            remaining = {r["id"] for r in pilot.rows(carried / name)}
            assert "early#end" in remaining and "sibling-0" in remaining
            assert not {"child-first-0", "child-next-1", "grandchild#end"} & remaining

        patched = root / "patched"
        patched.mkdir()
        patch_turns = [
            {**turn, "situation": situation_at(minute) if minute is not None else "No recorded time."}
            for turn, minute in zip([*meetings[0], *meetings[2]], [1940, 1941, 2062, None], strict=True)
        ]
        pilot.save_rows(patched / "turns.jsonl", patch_turns)

        def patch_reply(lines):
            return "[Голосу]\nVoice note.\n[Правки]\n" + lines

        patch_sol = ScriptedSol(
            script=[
                FakeStep(
                    patch_reply(
                        "+ было 8: Shared help.\n+ чувство 2 час: Gratitude.\n+ нить: Letter.\n"
                        "+ было 3: Greeting.\n+ верю сказал: Promise.\n"
                        "= мнение: Helpful.\n= настроение: Calm."
                    )
                ),
                FakeStep(
                    patch_reply(
                        "- н1\n+ нить: New question.\n+ верю сказал: PROMISE!!!\n~ ч99 2\n"
                        "+ было 11: Invalid.\ngarbage\n~ ч1 3\n~ в1 видел: Saw help."
                    )
                ),
                FakeStep(patch_reply("+ нить: End question.\n+ было 2: Waited.")),
                FakeStep("[Голосу] Missing patch marker.\n+ было 10: Must not apply."),
                FakeStep(patch_reply("—")),
                FakeStep(patch_reply("+ было 1: Farewell.")),
            ],
            delay=0,
            chunk=1,
        )
        await mind(patched, patch=True, carry=memory_db, sol=patch_sol)
        patch_rows = {r["id"]: r for r in pilot.rows(patched / "minds.jsonl")}
        assert len(patch_rows) == len(patch_sol.requests) == 6
        assert patch_sol.requests[0]["messages"][-1].content == (HERE / "mind_turn_v3.md").read_text()
        assert patch_sol.requests[0]["messages"][-2].content.endswith(EMPTY_MIND)
        first_end = patch_rows["early#end"]
        next_first = patch_rows["child-first-0"]
        assert first_end["state"]["events"] == patch_rows["early-0"]["state"]["events"][:1]
        assert first_end["state"]["counters"]["б"] == 3
        assert first_end["code_changes"]["forgotten"] == [
            {"id": "б2", "text": "Greeting."},
            {"id": "б3", "text": "Waited."},
        ]
        assert next_first["carried_from"] == "early" and next_first["parse_error"]
        assert next_first["patches"] == next_first["applied"] == []
        assert next_first["state"]["feelings"][0]["strength"] == 1
        assert next_first["state"]["feelings"][0]["updated_minute"] == 2061
        assert next_first["state"]["threads"] == first_end["state"]["threads"]
        assert next_first["previous_mind"].startswith("С прошлого раза прошло: 2 ч\n")
        assert patch_rows["child-first#end"]["state"]["counters"]["б"] == 4
        for row, request in zip(patch_rows.values(), patch_sol.requests, strict=True):
            assert request["messages"][-2].content == "[Твой ум до этой минуты]\n" + row["previous_mind"]
            assert row["mind"] == render(row["state"], True)
            assert row["mind_plain"] == render(row["state"], False)
            if not row["parse_error"]:
                assert 0 <= row["first_token_s"] < row["note_ready_s"] <= row["total_s"]
        assert next_first["note_ready_s"] is None
        await mind(patched, patch=True, carry=memory_db, sol=patch_sol)
        assert len(patch_sol.requests) == 6 and memory_db.read_bytes() == db_bytes

        patch_quick = FakeBackend("[neutral] Understood.", delay=0)
        await speak(patched, quick=patch_quick)
        assert patch_rows["early-0"]["mind_plain"] in patch_quick.calls[0][1][-2].content
        assert "важность" not in patch_quick.calls[0][1][-2].content
        sheet(patched)
        timeline = json.loads((patched / "timeline.json").read_text())
        for key in ("patches", "applied", "code_changes"):
            assert timeline[0]["turns"][0][key] == patch_rows["early-0"][key]

        patch_rows["early-0"]["total_s"] = patch_turns[0]["window_s"]
        patch_rows["early-1"]["total_s"] = patch_turns[1]["window_s"] + 1
        patch_rows["child-first-1"]["total_s"] = patch_turns[3]["window_s"]
        pilot.save_rows(patched / "minds.jsonl", patch_rows.values())
        patch_stats = summary(patched, emit=False)["mind"]
        assert patch_stats["total_fits_window_n"] == 2
        assert patch_stats["total_fits_window_fraction"] == 0.5
        assert "patches" not in report["mind"]
        stats = patch_stats["patches"]
        assert stats["per_turn"] == {"n": 6, "median": 1.5, "p90": 7.5, "max": 8}
        assert stats["by_op"] == {"+": 11, "=": 2, "-": 1, "~": 3, "unknown": 1, "noop": 1}
        assert stats["by_error"] == {"syntax": 1, "bad_param": 1, "unknown_id": 1, "duplicate": 1}
        assert stats["error_n"] == 4 and stats["line_n"] == 19 and stats["error_fraction"] == 4 / 19
        assert stats["code_changes"] == {"cooled": 0, "cap_dropped": 0, "forgotten": 3}
        assert stats["closing_mind_chars_by_conv"] == {
            conv: len(patch_rows[conv + "#end"]["mind"]) for conv in ("early", "child-first")
        }
        assert stats["closing_mind_chars_max"] == max(stats["closing_mind_chars_by_conv"].values())

        override = ScriptedSol(patch_reply("—"), delay=0)
        await mind(
            patched,
            patch=True,
            prompt_file="mind_turn_v2.md",
            carry=memory_db,
            redo_conv="child-first",
            sol=override,
        )
        assert len(override.requests) == 3
        assert override.requests[0]["messages"][-1].content == (HERE / "mind_turn_v2.md").read_text()
    print("Selftest passed (no network)")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    pick_parser = commands.add_parser("pick")
    pick_parser.add_argument("traces", nargs="+", type=Path)
    pick_parser.add_argument("--route", required=True, type=Path)
    pick_parser.add_argument("--out", required=True, type=Path)
    for name in ("mind", "speak", "judge", "sheet", "summary"):
        command = commands.add_parser(name)
        command.add_argument("directory", type=Path)
        if name == "mind":
            command.add_argument("--effort", default="low")
            command.add_argument("--redo-conv")
            command.add_argument("--prompt-file")
            command.add_argument("--carry", type=Path)
            command.add_argument("--patch", action="store_true")
        elif name == "speak":
            command.add_argument("--slot", type=int, default=0)
            command.add_argument("--quick-at")
        elif name == "judge":
            command.add_argument("--redo-minds", action="store_true")
        elif name == "sheet":
            command.add_argument("--seed", type=int, default=pilot.SEED)
        elif name == "summary":
            command.add_argument("--ratings", type=Path)
    commands.add_parser("selftest")
    args = vars(parser.parse_args())
    command = args.pop("command")
    if command == "pick":
        pick(args["traces"], args["route"], args["out"])
    elif command in ("mind", "speak", "judge", "selftest"):
        asyncio.run(globals()[command](**args))
    else:
        globals()[command](**args)


if __name__ == "__main__":
    main()
