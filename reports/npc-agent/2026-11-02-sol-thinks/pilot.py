"""Offline Sol-thinks, Gemma-speaks pilot. Run from server/; selftest needs no network."""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import random
import re
import statistics
import sys
import time
import tomllib
from contextlib import aclosing
from copy import deepcopy
from dataclasses import asdict
from pathlib import Path
from tempfile import TemporaryDirectory
from typing import Any
from unittest.mock import patch

from gothic_ai.dialogue import _FIRST_SENTENCE, MoodTag, MoodTags, paragraph_end
from gothic_ai.llm import (
    Done,
    FakeBackend,
    FakeStep,
    Item,
    LLMBackend,
    LLMError,
    Message,
    TextDelta,
    Usage,
    make_backend,
    make_quick,
    quick_url,
)
from gothic_ai.llm.base import QUICK_SLOT
from gothic_ai.replay import _quick_context
from gothic_ai.trace import item_from_json, item_json, read

HERE = Path(__file__).resolve().parent
SEED = 20261102
TIMEOUT_S = 60
QUOTAS = {
    "BAU_4300_ADDON_CAVALORN": {"pure": 8, "soft": 7, "hard": 5},
    "NONE_100_XARDAS": {"pure": 6, "soft": 0, "hard": 4},
}
NAMES = {"BAU_4300_ADDON_CAVALORN": "Кавалорн", "NONE_100_XARDAS": "Ксардас"}
SOFT = {"set_intent", "recall", "look", "where"}
VARIANTS = ("sol", "ahead", "reply")
GROUPS = ("all", "pure", "soft", "hard")


def rows(path: Path) -> list[dict[str, Any]]:
    return read(path) if path.exists() else []


def save_rows(path: Path, records) -> None:
    """Replace a small pilot file atomically, including when a turn is redone."""
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(
        "".join(json.dumps(record, ensure_ascii=False) + "\n" for record in records), encoding="utf-8"
    )
    temporary.replace(path)


def pick(traces: list[Path], directory: Path, seed: int = SEED) -> list[dict[str, Any]]:
    candidates = {(npc, group): [] for npc, quotas in QUOTAS.items() for group in quotas}
    seen = set()
    for trace in traces:
        trace = trace.resolve()
        records = read(trace)
        instructions = {r["hash"]: r["text"] for r in records if r.get("type") == "instructions"}
        previous = {}
        for record in records:
            if record.get("type") != "turn":
                continue
            npc = record["npc"]
            branch = (npc, record.get("branch"))
            end = previous.get(branch)
            previous[branch] = record["t"] + record["total_s"]
            steps = record.get("steps")
            if (
                npc not in QUOTAS
                or record.get("trigger", {}).get("kind") != "player"
                or not steps
                or steps[0].get("llm")
                or (steps[0].get("first_token_s") or 0) < 0.3
                or record.get("instructions") not in instructions
            ):
                continue
            phrase = record["trigger"]["text"]
            identity = (npc, phrase, len(steps[0]["added"]))
            if identity in seen:
                continue
            seen.add(identity)
            calls = [call["name"] for step in steps for call in step.get("calls", [])]
            group = "pure" if not calls else "soft" if set(calls) <= SOFT else "hard"
            history, situation = _quick_context(record)
            candidates[npc, group].append(
                {
                    "id": record["id"],
                    "trace": str(trace),
                    "t": record["t"],
                    "npc": npc,
                    "group": group,
                    "calls": calls,
                    "phrase": phrase,
                    "sol": " ".join(step.get("text", "") for step in steps).strip(),
                    "instructions": instructions[record["instructions"]],
                    "history": [item_json(item) for item in history],
                    "situation": situation,
                    "window_s": None if end is None else record["t"] - end,
                }
            )
    rng = random.Random(seed)
    selected = []
    for npc, quotas in QUOTAS.items():
        for group, quota in quotas.items():
            pool = candidates[npc, group]
            if len(pool) < quota:
                print(f"{npc} {group}: only {len(pool)}/{quota} turns available", file=sys.stderr)
            selected.extend(rng.sample(pool, min(quota, len(pool))))
    selected.sort(key=lambda turn: (turn["trace"], turn["t"], turn["id"]))
    directory.mkdir(parents=True, exist_ok=True)
    save_rows(directory / "turns.jsonl", selected)
    print(f"Picked {len(selected)} turns", file=sys.stderr)
    return selected


def gemma_url(explicit: str | None) -> str:
    url = quick_url(explicit or os.environ.get("GOTHIC_QUICK_LLM_URL"), os.environ.get("GOTHIC_DECIDER_URL"))
    settings_path = Path.home() / ".config/gothic-ai/settings.toml"
    if not url and settings_path.is_file():
        settings = tomllib.loads(settings_path.read_text(encoding="utf-8")).get("llm", {})
        url = quick_url(settings.get("quick_url"), settings.get("decider_url"))
    if not url:
        raise SystemExit("Set --quick-url, GOTHIC_QUICK_LLM_URL/GOTHIC_DECIDER_URL or [llm] in settings.toml")
    return url


async def measure(
    backend: LLMBackend,
    instructions: str,
    messages: list[Item],
    *,
    cache_key: str | None = None,
    prefix: list[Item] | None = None,
    slot: int = 0,
) -> dict[str, Any]:
    """One 60 s budget, including Gemma's prewarm; stream times exclude prewarm."""
    result = {"text": "", "first_token_s": None, "total_s": 0.0, "error": None}
    quick = prefix is not None
    if quick:
        result.update(prewarm_s=None, first_sentence_s=None, timings={})
    else:
        result["usage"] = None
    token = QUICK_SLOT.set(slot) if quick else None
    timings = getattr(backend, "timings", {})
    if quick:
        timings.pop(slot, None)
    started = None
    phase = "prewarm" if quick else "stream"
    try:
        async with asyncio.timeout(TIMEOUT_S):
            if quick:
                warm_started = time.monotonic()
                try:
                    await backend.prewarm(instructions=instructions, messages=prefix)
                finally:
                    result["prewarm_s"] = time.monotonic() - warm_started
            phase = "stream"
            started = time.monotonic()
            async with aclosing(
                backend.stream(
                    instructions=instructions,
                    messages=messages,
                    tools=[],
                    cache_key=cache_key,
                    **({"temperature": 0.6} if quick else {}),
                )
            ) as events:
                async for event in events:
                    elapsed = time.monotonic() - started
                    if isinstance(event, TextDelta):
                        if result["first_token_s"] is None:
                            result["first_token_s"] = elapsed
                        result["text"] += event.text
                    elif isinstance(event, Done):
                        result["text"] = event.text
                        if not quick:
                            result["usage"] = asdict(event.usage) if event.usage is not None else None
                    if quick and result["first_sentence_s"] is None:
                        # A final sentence without trailing whitespace is complete at Done.
                        text = result["text"] + (" " if isinstance(event, Done) else "")
                        if _FIRST_SENTENCE.search(text) or paragraph_end(text) is not None:
                            result["first_sentence_s"] = elapsed
    except TimeoutError:
        result["error"] = f"Timeout after {TIMEOUT_S:g} s during {phase}"
    except LLMError as error:
        result["error"] = str(error)
    finally:
        if started is not None:
            result["total_s"] = time.monotonic() - started
        if quick:
            result["timings"] = deepcopy(timings.get(slot, {}))
            QUICK_SLOT.reset(token)
    return result


async def run(
    directory: Path,
    *,
    only: str | None = None,
    redo: str | None = None,
    slot: int = 0,
    quick_at: str | None = None,
    effort: str = "low",
    sol: LLMBackend | None = None,
    quick: LLMBackend | None = None,
) -> None:
    prompts = {
        name: (HERE / f"{name}.md").read_text(encoding="utf-8")
        for name in ("think_ahead", "think_reply", "speak")
    }
    turns = read(directory / "turns.jsonl")
    results = {r["id"]: r for r in rows(directory / "runs.jsonl")}
    if only and redo and only != redo:
        raise SystemExit("--only and --redo must name the same turn when used together")
    selected = only or redo
    if selected and not any(turn["id"] == selected for turn in turns):
        raise SystemExit(f"Unknown turn: {selected}")
    pending = [
        turn
        for turn in turns
        if (not selected or turn["id"] == selected) and (turn["id"] not in results or turn["id"] == redo)
    ]
    if not pending:
        return
    quick = quick if quick is not None else make_quick(gemma_url(quick_at), max_tokens=300)
    sol = sol if sol is not None else make_backend("chatgpt", reasoning_effort=effort)
    try:
        for turn in pending:
            history = [item for data in turn["history"] if (item := item_from_json(data)) is not None]
            result = {"id": turn["id"]}
            for variant in ("ahead", "reply"):
                messages = [*history, Message("developer", turn["situation"])]
                if variant == "reply":
                    messages.append(Message("user", turn["phrase"]))
                messages.append(Message("developer", prompts[f"think_{variant}"]))
                thought = await measure(sol, turn["instructions"], messages, cache_key=f"pilot-{turn['npc']}")
                result[f"thought_{variant}"] = thought
                prefix = [
                    *history,
                    Message(
                        "developer",
                        turn["situation"] + "\n\n" + prompts["speak"].format(thought=thought["text"]),
                    ),
                ]
                result[f"gemma_{variant}"] = await measure(
                    quick,
                    turn["instructions"],
                    [*prefix, Message("user", turn["phrase"])],
                    prefix=prefix,
                    slot=slot,
                )
            if turn["id"] == redo:
                # Do this before replacing the run, so an interruption cannot leave stale judgments.
                path = directory / "judge.jsonl"
                if path.exists():
                    save_rows(path, (r for r in rows(path) if r["id"] != redo or r["key"] == "sol"))
            results[turn["id"]] = result
            save_rows(directory / "runs.jsonl", results.values())
            errors = sum(part["error"] is not None for key, part in result.items() if key != "id")
            print(f"Ran {turn['id']}: {errors}/4 parts failed", file=sys.stderr)
    finally:
        await sol.aclose()
        await quick.aclose()


def history_tail(turn: dict[str, Any]) -> list[dict[str, str]]:
    return [
        {"who": "hero" if item["role"] == "user" else "npc", "text": item["content"]}
        for item in turn["history"]
        if item.get("role") in ("user", "assistant")
    ][-8:]


def answers(turn: dict[str, Any], run: dict[str, Any]) -> dict[str, tuple[str, str]]:
    result = {"sol": (turn["sol"], "нет")}
    for variant in ("ahead", "reply"):
        if f"gemma_{variant}" in run:
            result[variant] = (run[f"gemma_{variant}"]["text"], run[f"thought_{variant}"]["text"])
    return result


async def judge(directory: Path, *, sol: LLMBackend | None = None) -> None:
    prompt = (HERE / "judge.md").read_text(encoding="utf-8")
    runs = {r["id"]: r for r in rows(directory / "runs.jsonl")}
    results = {(r["id"], r["key"]): r for r in rows(directory / "judge.jsonl")}
    pending = [
        (turn, key, text, thought)
        for turn in read(directory / "turns.jsonl")
        for key, (text, thought) in answers(turn, runs.get(turn["id"], {})).items()
        if (turn["id"], key) not in results
    ]
    if not pending:
        return
    sol = sol if sol is not None else make_backend("chatgpt", reasoning_effort="low")
    try:
        for turn, key, text, thought in pending:
            history = "\n".join(
                ("Герой: " if item["who"] == "hero" else "NPC: ") + item["text"]
                for item in history_tail(turn)
            )
            sections = {
                "Описание персонажа": turn["instructions"],
                "Обстановка": turn["situation"],
                "Конец разговора": history,
                "Фраза героя": turn["phrase"],
                "Мысли NPC": thought,
                "Реплика NPC": text,
            }
            content = "\n\n".join(f"{title}\n{body}" for title, body in sections.items())
            result = await measure(sol, prompt, [Message("user", content)])
            result.update(id=turn["id"], key=key, raw=result.pop("text"), result=None)
            if result["error"] is None:
                try:
                    raw = result["raw"].strip()
                    fenced = re.fullmatch(r"```(?:json)?\s*(.*?)\s*```", raw, re.DOTALL | re.IGNORECASE)
                    verdict = json.loads(fenced[1] if fenced else raw)
                    if not isinstance(verdict, dict):
                        raise ValueError("Judge response is not a JSON object")
                    result["result"] = verdict
                except ValueError as error:
                    result["error"] = f"Invalid judge JSON: {error}"
            results[turn["id"], key] = result
            save_rows(directory / "judge.jsonl", results.values())
            print(f"Judged {turn['id']} {key}: {result['error'] or 'ok'}", file=sys.stderr)
    finally:
        await sol.aclose()


def spoken(text: str) -> str:
    tags = MoodTags()
    return "".join(part for part in [*tags.feed(text), *tags.flush()] if isinstance(part, str)).strip()


def sheet(directory: Path) -> list[dict[str, Any]]:
    runs = {r["id"]: r for r in rows(directory / "runs.jsonl")}
    result = []
    for turn in read(directory / "turns.jsonl"):
        variants = [
            {"key": key, "text": spoken(text)}
            for key, (text, _) in answers(turn, runs.get(turn["id"], {})).items()
        ]
        random.Random(turn["id"]).shuffle(variants)
        result.append(
            {
                "id": turn["id"],
                "npc": NAMES[turn["npc"]],
                "group": turn["group"],
                "history": [{**item, "text": spoken(item["text"])} for item in history_tail(turn)],
                "situation": turn["situation"],
                "phrase": turn["phrase"],
                "answers": variants,
            }
        )
    (directory / "sheet.json").write_text(
        json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    return result


def distribution(values) -> dict[str, Any]:
    values = sorted(value for value in values if value is not None)
    if not values:
        return {"n": 0, "median": None, "p90": None}
    index = (len(values) - 1) * 0.9
    low = int(index)
    p90 = values[low] + (values[min(low + 1, len(values) - 1)] - values[low]) * (index - low)
    return {"n": len(values), "median": statistics.median(values), "p90": p90}


def fraction(numerator: int, denominator: int) -> float | None:
    return numerator / denominator if denominator else None


def starts_with_mood(text: str) -> bool:
    tag = re.match(r"\[[^\]]+\]", text.lstrip())
    return bool(tag and any(isinstance(part, MoodTag) for part in MoodTags().feed(tag[0])))


def summary(directory: Path, ratings: Path | None = None, *, emit: bool = True) -> dict[str, Any]:
    turns = read(directory / "turns.jsonl")
    runs = {r["id"]: r for r in rows(directory / "runs.jsonl")}
    judged = {(r["id"], r["key"]): r for r in rows(directory / "judge.jsonl")}
    result = {"variants": {}, "thoughts": {}, "judge": {}}
    for variant in VARIANTS:
        result["variants"][variant] = {}
        result["judge"][variant] = {}
        if variant != "sol":
            result["thoughts"][variant] = {}
        for group in GROUPS:
            chosen = [turn for turn in turns if group == "all" or turn["group"] == group]
            parts = (
                [
                    runs[turn["id"]][f"gemma_{variant}"]
                    for turn in chosen
                    if f"gemma_{variant}" in runs.get(turn["id"], {})
                ]
                if variant != "sol"
                else []
            )
            stats = {
                "n": len(chosen) if variant == "sol" else len(parts),
                "errors": sum(part["error"] is not None for part in parts),
            }
            if variant != "sol":
                successful = [part for part in parts if part["error"] is None]
                for metric in ("prewarm_s", "first_token_s", "first_sentence_s", "total_s"):
                    stats[metric] = distribution(part[metric] for part in successful)
                stats["prompt_n"] = distribution(part["timings"].get("prompt_n") for part in successful)
                stats["mood_at_start"] = fraction(
                    sum(starts_with_mood(p["text"]) for p in successful), len(successful)
                )
                thoughts = [
                    (turn, runs[turn["id"]][f"thought_{variant}"])
                    for turn in chosen
                    if f"thought_{variant}" in runs.get(turn["id"], {})
                ]
                ready = [(turn, part) for turn, part in thoughts if part["error"] is None]
                thought_stats = {
                    "n": len(thoughts),
                    "errors": len(thoughts) - len(ready),
                    "total_s": distribution(part["total_s"] for _, part in ready),
                }
                if variant == "ahead":
                    windows = [(turn, part) for turn, part in thoughts if turn["window_s"] is not None]
                    fits = sum(
                        part["error"] is None and part["total_s"] < turn["window_s"] for turn, part in windows
                    )
                    thought_stats.update(
                        window_n=len(windows),
                        fits_window_n=fits,
                        fits_window_fraction=fraction(fits, len(windows)),
                    )
                result["thoughts"][variant][group] = thought_stats
            result["variants"][variant][group] = stats
            checks = [judged[turn["id"], variant] for turn in chosen if (turn["id"], variant) in judged]
            verdicts = [check["result"] for check in checks if check["error"] is None]
            counts = {"n": len(checks), "errors": len(checks) - len(verdicts)}
            for field in ("unauthorized", "invented"):
                counts[field] = sum(bool(verdict.get(field)) for verdict in verdicts)
                counts[field + "_items"] = sum(len(verdict.get(field, [])) for verdict in verdicts)
            for field in ("answers", "in_character"):
                counts[field + "_false"] = sum(verdict.get(field) is False for verdict in verdicts)
            result["judge"][variant][group] = counts
    if ratings is not None:
        scores = {"bad": 0, "ok": 1, "good": 2}
        rated = {
            (r["id"], r["key"]): scores[r["rating"]] for r in json.loads(ratings.read_text(encoding="utf-8"))
        }
        acceptance = {}
        for variant in ("ahead", "reply"):
            chosen = [turn for turn in turns if turn["group"] in ("pure", "soft")]
            paired = [
                turn for turn in chosen if (turn["id"], variant) in rated and (turn["id"], "sol") in rated
            ]
            values = [rated[turn["id"], variant] for turn in chosen if (turn["id"], variant) in rated]
            better = sum(rated[turn["id"], variant] >= rated[turn["id"], "sol"] for turn in paired)
            bad = sum(value == 0 for value in values)
            acceptance[variant] = {
                "criterion_1": {
                    "total_n": len(chosen),
                    "paired_n": len(paired),
                    "at_least_sol_n": better,
                    "at_least_sol_fraction": fraction(better, len(paired)),
                    "rated_n": len(values),
                    "bad_n": bad,
                    "bad_fraction": fraction(bad, len(values)),
                    "passed": (better / len(paired) >= 0.6 and bad / len(values) <= 0.15)
                    if paired and len(paired) == len(chosen)
                    else None,
                }
            }
            hard = [turn for turn in turns if turn["group"] == "hard"]
            hard_values = [rated[turn["id"], variant] for turn in hard if (turn["id"], variant) in rated]
            acceptance[variant]["criterion_3"] = {
                "total_n": len(hard),
                "rated_n": len(hard_values),
                "ratings": {name: hard_values.count(score) for name, score in scores.items()},
                "judge": result["judge"][variant]["hard"],
                "passed": None,
                "note": "Manual review required: ratings do not identify uncovered requests or stalling.",
            }
        result["acceptance"] = acceptance
    text = json.dumps(result, ensure_ascii=False, indent=2) + "\n"
    (directory / "summary.json").write_text(text, encoding="utf-8")
    if emit:
        print(text, end="")
    return result


async def selftest() -> None:
    with TemporaryDirectory(prefix="sol-thinks-") as temporary:
        root = Path(temporary)
        trace = root / "trace.jsonl"
        records = [{"type": "instructions", "hash": "persona", "text": "Recorded NPC instructions."}]
        for index, calls in enumerate(([], ["look", "recall"], ["look", "give_item"])):
            phrase = f"Hero phrase {index}"
            history = [Message("developer", "Earlier note.")]
            history += [Message(role, f"Earlier {i}") for i in range(10) for role in ("user", "assistant")]
            records.append(
                {
                    "type": "turn",
                    "id": f"turn-{index}",
                    "npc": "BAU_4300_ADDON_CAVALORN",
                    "branch": "branch",
                    "t": 10.0 + index * 20,
                    "total_s": 2,
                    "instructions": "persona",
                    "trigger": {"kind": "player", "text": phrase},
                    "steps": [
                        {
                            "first_token_s": 0.5,
                            "added": [
                                item_json(item)
                                for item in [
                                    *history,
                                    Message("developer", "Recorded situation."),
                                    Message("user", phrase),
                                ]
                            ],
                            "text": "[neutral] Recorded answer.",
                            "calls": [],
                        },
                        {"text": "Another sentence.", "calls": [{"name": name} for name in calls]},
                    ],
                }
            )
        save_rows(trace, records)
        directory = root / "pilot"
        turns = pick([trace], directory)
        assert [turn["group"] for turn in turns] == ["pure", "soft", "hard"]
        assert [turn["window_s"] for turn in turns] == [None, 18.0, 18.0]
        assert all(Path(turn["trace"]).is_absolute() for turn in turns)
        assert turns[1]["calls"] == ["look", "recall"]
        assert turns[0]["sol"] == "[neutral] Recorded answer. Another sentence."
        sol = FakeBackend(script=[FakeStep("A thought."), FakeStep("")], delay=0)
        quick = FakeBackend("[friendly] First sentence. [nods] Second sentence.", delay=0)
        await run(directory, only="turn-0", sol=sol, quick=quick, slot=2)
        assert len(rows(directory / "runs.jsonl")) == 1 and len(sol.calls) == 2
        await run(directory, sol=sol, quick=quick, slot=2)
        assert len(sol.calls) == len(quick.calls) == len(quick.prewarms) == 6
        assert quick.slot_ids == quick.prewarm_slot_ids == [2] * 6
        assert all(not tools for _, _, tools in sol.calls + quick.calls)
        assert sol.cache_keys == ["pilot-BAU_4300_ADDON_CAVALORN"] * 6
        assert quick.temperatures == [0.6] * 6
        assert sol.calls[0][1][-1] == Message("developer", (HERE / "think_ahead.md").read_text())
        assert not any(
            isinstance(item, Message) and item.content == turns[0]["phrase"] for item in sol.calls[0][1]
        )
        assert sol.calls[1][1][-2] == Message("user", turns[0]["phrase"])
        for (_, prefix), (_, request, _) in zip(quick.prewarms, quick.calls, strict=True):
            assert request[:-1] == prefix and request[-1].role == "user"
        runs = rows(directory / "runs.jsonl")
        assert len(runs) == 3
        for record in runs:
            for variant in ("ahead", "reply"):
                thought, reply = record[f"thought_{variant}"], record[f"gemma_{variant}"]
                assert "usage" in thought and thought["error"] is None
                assert reply["text"].endswith("Second sentence.") and reply["error"] is None
                assert 0 <= reply["first_token_s"] <= reply["first_sentence_s"] <= reply["total_s"]
                assert reply["prewarm_s"] >= 0 and reply["timings"] == {}
        assert runs[0]["thought_reply"]["text"] == ""
        assert runs[0]["thought_reply"]["first_token_s"] is None
        await run(directory, sol=sol, quick=quick)
        assert len(sol.calls) == len(quick.calls) == 6
        verdict = {
            "commits": [],
            "unauthorized": [],
            "invented": [],
            "answers": True,
            "in_character": True,
            "note": "",
        }
        referee = FakeBackend("```json\n" + json.dumps(verdict) + "\n```", delay=0)
        await judge(directory, sol=referee)
        await judge(directory, sol=referee)
        assert len(referee.calls) == len(rows(directory / "judge.jsonl")) == 9
        assert all(record["result"] == verdict for record in rows(directory / "judge.jsonl"))
        assert len(referee.calls[0][1]) == 1 and referee.calls[0][1][0].role == "user"
        assert "Мысли NPC\nнет" in referee.calls[0][1][0].content
        await run(directory, redo="turn-0", sol=sol, quick=quick)
        assert len(sol.calls) == len(quick.calls) == 8 and len(rows(directory / "runs.jsonl")) == 3
        assert len(rows(directory / "judge.jsonl")) == 7
        await judge(directory, sol=referee)
        assert len(referee.calls) == 11
        comparison = sheet(directory)
        assert comparison == sheet(directory) and len(comparison) == 3
        assert all(len(turn["history"]) == 8 and len(turn["answers"]) == 3 for turn in comparison)
        assert all("[" not in answer["text"] for turn in comparison for answer in turn["answers"])
        ratings = root / "ratings.json"
        ratings.write_text(
            json.dumps(
                [{"id": turn["id"], "key": key, "rating": "good"} for turn in turns for key in VARIANTS]
            )
        )
        report = summary(directory, ratings, emit=False)
        assert report["variants"]["ahead"]["all"]["n"] == 3
        assert report["variants"]["ahead"]["all"]["mood_at_start"] == 1
        assert report["thoughts"]["ahead"]["all"]["fits_window_fraction"] == 1
        assert report["acceptance"]["ahead"]["criterion_1"]["passed"] is True
        assert all(
            (directory / name).is_file()
            for name in ("turns.jsonl", "runs.jsonl", "judge.jsonl", "sheet.json", "summary.json")
        )

        async def failed_stream(**kwargs):
            yield TextDelta("Partial")
            raise LLMError("Synthetic model error")

        broken = FakeBackend(delay=0)
        broken.stream = failed_stream
        await run(directory, redo="turn-1", sol=broken, quick=quick)
        failed = next(record for record in rows(directory / "runs.jsonl") if record["id"] == "turn-1")
        assert failed["thought_ahead"]["text"] == "Partial"
        assert failed["thought_ahead"]["error"] == "Synthetic model error"
        assert failed["gemma_reply"]["error"] is None

        slow = FakeBackend(first=1, delay=0)
        with patch(__name__ + ".TIMEOUT_S", 0.01):
            timeout = await measure(slow, "", [], prefix=[], slot=2)
        assert "Timeout" in timeout["error"] and timeout["prewarm_s"] >= 0
        assert timeout["text"] == "" and 0 < timeout["total_s"] < 0.5
        assert QUICK_SLOT.get() is None

        async def timed_stream(**kwargs):
            quick.timings[2] = {"prompt_n": 17}
            yield TextDelta("One sentence.")
            yield Done("One sentence.", usage=Usage(input_tokens=17, output_tokens=3))

        quick.timings = {}
        quick.stream = timed_stream
        measured = await measure(quick, "", [], prefix=[], slot=2)
        quick.timings[2]["prompt_n"] = 99
        assert measured["timings"] == {"prompt_n": 17} and measured["first_sentence_s"] is not None
        measured = await measure(quick, "", [])
        assert measured["usage"]["input_tokens"] == 17
        invalid = FakeBackend("invalid JSON", delay=0)
        await judge(directory, sol=invalid)
        assert any(
            record["raw"] == "invalid JSON" and record["error"] for record in rows(directory / "judge.jsonl")
        )
        print("Selftest passed: 3 groups; pick/run/judge/sheet/summary; resume, redo, errors, empty output.")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_subparsers(dest="mode", required=True)
    command = modes.add_parser("pick")
    command.add_argument("traces", type=Path, nargs="+")
    command.add_argument("--out", type=Path, required=True)
    command.add_argument("--seed", type=int, default=SEED)
    command = modes.add_parser("run")
    command.add_argument("directory", type=Path)
    command.add_argument("--only")
    command.add_argument("--redo")
    command.add_argument("--slot", type=int, default=0)
    command.add_argument("--quick-url")
    command.add_argument("--effort", default="low")
    for mode in ("judge", "sheet", "summary"):
        command = modes.add_parser(mode)
        command.add_argument("directory", type=Path)
        if mode == "summary":
            command.add_argument("--ratings", type=Path)
    modes.add_parser("selftest")
    args = parser.parse_args()
    if args.mode == "pick":
        pick(args.traces, args.out, args.seed)
    elif args.mode == "run":
        asyncio.run(
            run(
                args.directory,
                only=args.only,
                redo=args.redo,
                slot=args.slot,
                quick_at=args.quick_url,
                effort=args.effort,
            )
        )
    elif args.mode == "judge":
        asyncio.run(judge(args.directory))
    elif args.mode == "sheet":
        sheet(args.directory)
    elif args.mode == "summary":
        summary(args.directory, args.ratings)
    else:
        asyncio.run(selftest())


if __name__ == "__main__":
    main()
