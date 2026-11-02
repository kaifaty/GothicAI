"""Offline Sol mind pilot. Run from server/; selftest never uses the network."""

from __future__ import annotations

import argparse
import asyncio
import importlib.util
import json
import random
import re
import sys
import time
from collections import Counter
from contextlib import aclosing
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
MIND_TOOLS += [PROPOSE_TOOL, WITHDRAW_TOOL]
FIELDS = ("Мнение о герое", "Верю", "Сомневаюсь", "Хочу", "Нити", "Настроение")


def write_json(path: Path, value: Any) -> None:
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def conversations(turns: list[dict[str, Any]]) -> list[list[dict[str, Any]]]:
    groups = {}
    for turn in turns:
        groups.setdefault(turn["conv"], []).append(turn)
    return [sorted(group, key=lambda turn: turn["index"]) for group in groups.values()]


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
    sol: LLMBackend, turn: dict[str, Any], previous: str, prompt: str, deadline: float
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
                if result["note_ready_s"] is None and "[Ум]" in result["text"]:
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
    directory: Path, *, effort: str = "low", redo_conv: str | None = None, sol: LLMBackend | None = None
) -> None:
    prompt = (HERE / "mind_turn.md").read_text(encoding="utf-8")
    groups = conversations(pilot.rows(directory / "turns.jsonl"))
    if redo_conv:
        groups = [group for group in groups if group[0]["conv"] == redo_conv]
        if not groups:
            raise SystemExit("Unknown conversation")
    results = {r["id"]: r for r in pilot.rows(directory / "minds.jsonl")}
    pending = []
    for group in groups:
        restart = group[0]["conv"] == redo_conv
        for turn in group:
            record = results.get(turn["id"])
            restart = restart or record is None or record.get("error") is not None
            if restart:
                pending.append(turn)
    if not pending:
        return
    identifiers = {turn["id"] for turn in pending}
    invalidate(directory, identifiers, ("answers.jsonl", "judge_minds.jsonl", "judge_answers.jsonl"))
    results = {key: value for key, value in results.items() if key not in identifiers}
    pilot.save_rows(directory / "minds.jsonl", results.values())
    sol = sol if sol is not None else pilot.make_backend("chatgpt", reasoning_effort=effort)
    try:
        for group in groups:
            previous = EMPTY_MIND
            for turn in group:
                if turn["id"] not in identifiers:
                    previous = results[turn["id"]]["mind"]
                    continue
                started = time.monotonic()
                for attempt in range(1, 3):
                    offset = time.monotonic() - started
                    result = await mind_stream(sol, turn, previous, prompt, started + TIMEOUT_S)
                    result["attempts"] = attempt
                    for metric in ("first_token_s", "note_ready_s"):
                        if result[metric] is not None:
                            result[metric] += offset
                    result["total_s"] = time.monotonic() - started
                    if result["error"] is None:
                        break
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
                        + prompts[variant].format(thought=state["note"], mind=state["mind"]),
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


async def judge(directory: Path, *, sol: LLMBackend | None = None) -> None:
    mind_prompt = (HERE / "judge_mind.md").read_text(encoding="utf-8")
    answer_prompt = (HYBRID / "judge_hybrid.md").read_text(encoding="utf-8")
    turns = pilot.rows(directory / "turns.jsonl")
    minds = {r["id"]: r for r in pilot.rows(directory / "minds.jsonl")}
    answers = {(r["id"], r["variant"]): r for r in pilot.rows(directory / "answers.jsonl")}
    judged_minds = {r["id"]: r for r in pilot.rows(directory / "judge_minds.jsonl")}
    judged_answers = {(r["id"], r["variant"]): r for r in pilot.rows(directory / "judge_answers.jsonl")}
    requests = []
    for group in conversations(turns):
        prior = []
        for turn in group:
            identifier = turn["id"]
            state = minds.get(identifier)
            if state is not None and state["error"] is None and not state["parse_error"]:
                if identifier not in judged_minds or judged_minds[identifier]["error"] is not None:
                    calls = [
                        f"{call['name']} {json.dumps(call['arguments'], ensure_ascii=False)}"
                        for call in state["calls"]
                    ]
                    calls.extend("[Скажу] " + phrase for phrase in state["say"])
                    sections = {
                        "Описание персонажа": turn["instructions"],
                        "Обстановка": turn["situation"],
                        "Разговор до этой минуты": "\n".join(prior),
                        "Прежний ум": state["previous_mind"],
                        "Новый ум": state["mind"],
                        "Вызовы": "\n".join(calls) or "—",
                    }
                    requests.append((identifier, None, mind_prompt, sections))
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
            prior.extend(("Герой: " + turn["phrase"], "NPC: " + pilot.spoken(turn["sol"])))
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
                }
            )
        timeline.append(
            {
                "conv": group[0]["conv"],
                "npc": group[0]["npc"],
                "name": pilot.NAMES[group[0]["npc"]],
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


def summary(directory: Path, ratings: Path | None = None, *, emit: bool = True) -> dict[str, Any]:
    turns = pilot.rows(directory / "turns.jsonl")
    minds = {r["id"]: r for r in pilot.rows(directory / "minds.jsonl")}
    answers = {(r["id"], r["variant"]): r for r in pilot.rows(directory / "answers.jsonl")}
    judged_minds = {r["id"]: r for r in pilot.rows(directory / "judge_minds.jsonl")}
    judged_answers = {(r["id"], r["variant"]): r for r in pilot.rows(directory / "judge_answers.jsonl")}
    parts = [minds[turn["id"]] for turn in turns if turn["id"] in minds]
    successful = [part for part in parts if part["error"] is None and not part["parse_error"]]
    windows = [turn for turn in turns if turn["window_s"] is not None]
    fits = sum(
        minds.get(turn["id"], {}).get("error") is None
        and not minds.get(turn["id"], {}).get("parse_error")
        and minds.get(turn["id"], {}).get("note_ready_s") is not None
        and minds[turn["id"]]["note_ready_s"] <= turn["window_s"]
        for turn in windows
    )
    stats = {
        "n": len(parts),
        "errors": sum(part["error"] is not None for part in parts),
        "parse_errors": sum(part["parse_error"] for part in parts),
        "window_n": len(windows),
        "fits_window_n": fits,
        "fits_window_fraction": pilot.fraction(fits, len(windows)),
    }
    for metric in ("first_token_s", "note_ready_s", "total_s"):
        stats[metric] = pilot.distribution(part[metric] for part in successful)
    stats["output_tokens"] = pilot.distribution(
        (part["usage"] or {}).get("output_tokens") for part in successful
    )
    changes = {}
    comparisons = []
    for group in conversations(turns):
        previous = EMPTY_MIND
        for turn in group:
            state = minds.get(turn["id"])
            if state is not None:
                if state["error"] is None and not state["parse_error"] and previous != EMPTY_MIND:
                    comparisons.append((mind_fields(previous), mind_fields(state["mind"])))
                previous = state["mind"]
    for field in FIELDS:
        changed = sum(old.get(field, "") != new.get(field, "") for old, new in comparisons)
        changes[field] = {
            "n": len(comparisons),
            "changed_n": changed,
            "fraction": pilot.fraction(changed, len(comparisons)),
        }
    call_counts, justified_counts, call_items = Counter(), Counter(), []
    for turn in turns:
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
            for turn in turns
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
        with_mind = current + sum(turn["oof_predicted"] is not False for turn in group)
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
        old_call = ToolCall("look", {}, "old-call")
        old_result = ToolResult("old-call", "Recorded tool output.")
        history = [
            Message("user", "Earlier hero."),
            Message("assistant", "[neutral] Earlier NPC."),
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
                FakeStep(reply(a)),
                FakeStep(reply(e)),
            ],
            failures={3, 5, 6},
            delay=0,
            chunk=1,
        )
        with patch.object(asyncio, "timeout_at", wraps=asyncio.timeout_at) as deadlines:
            await mind(directory, sol=sol)
        assert len(deadlines.call_args_list) == 8
        assert deadlines.call_args_list[2] == deadlines.call_args_list[3]
        assert deadlines.call_args_list[4] == deadlines.call_args_list[5]
        states = {r["id"]: r for r in pilot.rows(directory / "minds.jsonl")}
        assert len(states) == 6 and len(sol.requests) == 8
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
        assert len(jm) == len(ja) == 4 and len(referee.calls) == 8
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
        later = next(
            messages[0].content for _, messages, _ in referee.calls if "Новый ум\n" + b in messages[0].content
        )
        assert f"Герой: {turns[0]['phrase']}\nNPC: {pilot.spoken(turns[0]['sol'])}" in later
        assert later.count("Герой: ") == 2 and "Вызовы\n—" in later
        assert "Мысли NPC\nVoice note.\n\n" + a in referee.calls[1][1][0].content
        assert "Мысли NPC\nVoice note.\n\nРеплика NPC" in referee.calls[2][1][0].content
        jm[0]["error"] = ja[0]["error"] = "Synthetic judge failure"
        pilot.save_rows(directory / "judge_minds.jsonl", jm)
        pilot.save_rows(directory / "judge_answers.jsonl", ja)
        await judge(directory, sol=referee)
        assert len(referee.calls) == 10
        jm, ja = pilot.rows(directory / "judge_minds.jsonl"), pilot.rows(directory / "judge_answers.jsonl")
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
        ratings = root / "ratings.json"
        write_json(
            ratings,
            [{"id": "turn-0", "key": key, "rating": "good"} for key in ("sol", *VARIANTS)]
            + [{"id": "turn-1", "rating": "unkeyed row is skipped"}],
        )
        report = summary(directory, ratings, emit=False)
        assert (
            report["mind"]["n"] == 6 and report["mind"]["errors"] == 1 and report["mind"]["parse_errors"] == 1
        )
        assert report["mind"]["output_tokens"]["median"] == 123
        assert report["mind"]["fits_window_n"] == 4 and report["mind"]["window_n"] == 6
        assert report["changes"]["Мнение о герое"] == {"n": 2, "changed_n": 1, "fraction": 0.5}
        assert report["changes"]["Верю"]["fraction"] == report["changes"]["Нити"]["fraction"] == 0.5
        assert report["changes"]["Хочу"]["fraction"] == 0
        assert report["calls"]["by_name"] == {"set_intent": 1} and report["calls"]["justified_n"] == 1
        assert report["calls"]["say_n"] == 2 and report["calls"]["items"][0]["why"] == "Reason."
        assert report["judge"]["false_facts"] == report["judge"]["stale"] == 1
        assert report["variants"]["mind"]["taken_n"] == 1 and report["variants"]["mind"]["handoff_n"] == 1
        assert report["variants"]["mind"]["errors"] == 2 and report["variants"]["note"]["leaks"] == 1
        assert report["variants"]["mind"]["violations_n"] == 1
        assert report["variants"]["mind"]["first_sentence_s"]["n"] == 1
        assert report["cost"]["current"] == 6 and report["cost"]["with_mind"] == 8
        assert report["cost"]["ratio"] == 8 / 6
        assert [cost["with_mind"] for cost in report["cost"]["conversations"]] == [5, 3]
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
        assert len(recovery.calls) == 2 and all(state["error"] is None for state in recovered.values())
        assert recovered["turn-0"] == states["turn-0"] and recovered["turn-4"] == states["turn-4"]
        assert recovered["turn-1"] == states["turn-1"]
        for name in ("answers.jsonl", "judge_minds.jsonl", "judge_answers.jsonl"):
            assert not {"turn-2", "turn-3"} & {r["id"] for r in pilot.rows(directory / name)}
        await mind(directory, sol=recovery)
        assert len(recovery.calls) == 2
        assert {r["id"]: r for r in pilot.rows(directory / "minds.jsonl")}["turn-1"] == states["turn-1"]
        await speak(directory, quick=retry_quick)
        assert len(retry_quick.calls) == 3
        await judge(directory, sol=referee)
        assert len(pilot.rows(directory / "judge_minds.jsonl")) == 5
        assert len(pilot.rows(directory / "judge_answers.jsonl")) == 6

        before_redo = pilot.rows(directory / "minds.jsonl")
        unrelated = next(state for state in before_redo if state["id"] == "turn-5")
        unrelated["error"] = "Synthetic failure in another conversation"
        pilot.save_rows(directory / "minds.jsonl", before_redo)
        redo = FakeBackend(reply(a), delay=0)
        await mind(directory, redo_conv=convs[0]["conv"], sol=redo)
        assert len(redo.calls) == 4 and redo.calls[0][1][-2].content.endswith(EMPTY_MIND)
        redone = {r["id"]: r for r in pilot.rows(directory / "minds.jsonl")}
        assert len(redone) == 6 and redone["turn-4"] == states["turn-4"] and redone["turn-5"] == unrelated
        for name in ("answers.jsonl", "judge_minds.jsonl", "judge_answers.jsonl"):
            assert {r["id"] for r in pilot.rows(directory / name)} <= {"turn-4", "turn-5"}
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
        elif name == "speak":
            command.add_argument("--slot", type=int, default=0)
            command.add_argument("--quick-at")
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
