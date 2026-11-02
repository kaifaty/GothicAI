"""Offline Gemma-speaks, Sol-decides hybrid pilot; selftest uses no network."""

from __future__ import annotations

import argparse
import asyncio
import importlib.util
import json
import random
import re
import sys
from collections import Counter
from pathlib import Path
from tempfile import TemporaryDirectory
from typing import Any

from gothic_ai.llm import FakeBackend, FakeStep, LLMBackend, LLMError, Message, TextDelta
from gothic_ai.trace import item_from_json, item_json

HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location(
    "_sol_thinks_pilot", HERE.parent / "2026-11-02-sol-thinks/pilot.py"
)
assert SPEC is not None and SPEC.loader is not None
pilot = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = pilot
SPEC.loader.exec_module(pilot)
GROUPS = pilot.GROUPS
SEED = pilot.SEED


def pick(traces: list[Path], directory: Path) -> list[dict[str, Any]]:
    turns = sorted(pilot.collect_candidates(traces), key=lambda t: (t["trace"], t["t"], t["id"]))
    directory.mkdir(parents=True, exist_ok=True)
    pilot.save_rows(directory / "turns.jsonl", turns)
    for npc in pilot.NAMES:
        counts = Counter(turn["group"] for turn in turns if turn["npc"] == npc)
        print(f"{npc}: {dict.fromkeys(GROUPS[1:], 0) | dict(counts)}", file=sys.stderr)
    print(f"Picked {len(turns)} turns", file=sys.stderr)
    return turns


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
        name: (HERE / f"{name}.md").read_text(encoding="utf-8") for name in ("think_short", "speak_hybrid")
    }
    turns = pilot.rows(directory / "turns.jsonl")
    results = {r["id"]: r for r in pilot.rows(directory / "runs.jsonl")}
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
    quick = quick if quick is not None else pilot.make_quick(pilot.gemma_url(quick_at), max_tokens=300)
    sol = sol if sol is not None else pilot.make_backend("chatgpt", reasoning_effort=effort)
    try:
        for turn in pending:
            history = [item for data in turn["history"] if (item := item_from_json(data)) is not None]
            thought = await pilot.measure(
                sol,
                turn["instructions"],
                [
                    *history,
                    Message("developer", turn["situation"]),
                    Message("developer", prompts["think_short"]),
                ],
                cache_key=f"pilot-{turn['npc']}",
            )
            handoff, stall = None, None
            if thought["error"] is not None or not thought["text"].strip():
                gemma = {
                    "text": "",
                    "error": "no thought",
                    "first_token_s": None,
                    "first_sentence_s": None,
                    "total_s": None,
                    "prewarm_s": None,
                    "timings": None,
                }
            else:
                prefix = [
                    *history,
                    Message(
                        "developer",
                        turn["situation"] + "\n\n" + prompts["speak_hybrid"].format(thought=thought["text"]),
                    ),
                ]
                gemma = await pilot.measure(
                    quick,
                    turn["instructions"],
                    [*prefix, Message("user", turn["phrase"])],
                    prefix=prefix,
                    slot=slot,
                )
                text = gemma["text"].lstrip()
                handoff = text[:5].lower() == "[sol]"
                stall = text[5:].lstrip() if handoff else None
            if turn["id"] == redo:
                # Invalidate before saving the replacement run, including after an interruption.
                path = directory / "judge.jsonl"
                if path.exists():
                    pilot.save_rows(path, (r for r in pilot.rows(path) if r["id"] != redo))
            results[turn["id"]] = {
                "id": turn["id"],
                "thought": thought,
                "gemma": gemma,
                "handoff": handoff,
                "stall": stall,
            }
            pilot.save_rows(directory / "runs.jsonl", results.values())
            errors = sum(part["error"] is not None for part in (thought, gemma))
            print(f"Ran {turn['id']}: {errors}/2 parts failed", file=sys.stderr)
    finally:
        await sol.aclose()
        await quick.aclose()


async def judge(directory: Path, *, sol: LLMBackend | None = None) -> None:
    prompt = (HERE / "judge_hybrid.md").read_text(encoding="utf-8")
    runs = {r["id"]: r for r in pilot.rows(directory / "runs.jsonl")}
    results = {r["id"]: r for r in pilot.rows(directory / "judge.jsonl")}
    pending = [
        turn
        for turn in pilot.rows(directory / "turns.jsonl")
        if turn["id"] not in results and runs.get(turn["id"], {}).get("gemma", {}).get("text", "").strip()
    ]
    if not pending:
        return
    sol = sol if sol is not None else pilot.make_backend("chatgpt", reasoning_effort="low")
    try:
        for turn in pending:
            record = runs[turn["id"]]
            history = "\n".join(
                ("Герой: " if item["who"] == "hero" else "NPC: ") + item["text"]
                for item in pilot.history_tail(turn)
            )
            sections = {
                "Описание персонажа": turn["instructions"],
                "Обстановка": turn["situation"],
                "Конец разговора": history,
                "Фраза героя": turn["phrase"],
                "Мысли NPC": record["thought"]["text"],
                "Реплика NPC": record["gemma"]["text"],
            }
            content = "\n\n".join(f"{title}\n{body}" for title, body in sections.items())
            result = await pilot.measure(sol, prompt, [Message("user", content)])
            result.update(id=turn["id"], raw=result.pop("text"), result=None)
            if result["error"] is None:
                try:
                    result["result"] = pilot.parse_judge(result["raw"])
                except ValueError as error:
                    result["error"] = f"Invalid judge JSON: {error}"
            results[turn["id"]] = result
            pilot.save_rows(directory / "judge.jsonl", results.values())
            print(f"Judged {turn['id']}: {result['error'] or 'ok'}", file=sys.stderr)
    finally:
        await sol.aclose()


def route(record: dict[str, Any]) -> str:
    gemma = record.get("gemma", {})
    if gemma.get("error") is not None or not gemma.get("text", "").strip():
        return "no_answer"
    if record.get("handoff") is True:
        return "handed_off"
    return "taken" if record.get("handoff") is False else "no_answer"


def sheet(
    directory: Path, n: int = 30, seed: int = SEED, exclude: Path | None = None
) -> list[dict[str, Any]]:
    if n < 1:
        raise SystemExit("--n must be positive")
    runs = {r["id"]: r for r in pilot.rows(directory / "runs.jsonl")}
    eligible = [
        turn for turn in pilot.rows(directory / "turns.jsonl") if route(runs.get(turn["id"], {})) == "taken"
    ]
    excluded = {r["id"] for r in pilot.rows(exclude)} if exclude is not None else set()
    fresh = [turn for turn in eligible if turn["id"] not in excluded]
    size = min(n, len(eligible))
    # Excluded turns can enter the pool only when all fresh turns fit.
    pool = fresh if len(fresh) >= size else eligible
    counts = Counter(turn["group"] for turn in eligible)
    available = Counter(turn["group"] for turn in pool)
    quotas = Counter(turn["group"] for turn in fresh) if len(fresh) < size else Counter()
    # Group minima must fit the pool and preserve fresh turns when backfilling.
    for group in counts:
        if available[group] and not quotas[group] and sum(quotas.values()) < size:
            quotas[group] = 1
    while sum(quotas.values()) < size:
        group = max(
            (group for group in GROUPS[1:] if quotas[group] < available[group]),
            key=lambda group: size * counts[group] / len(eligible) - quotas[group],
        )
        quotas[group] += 1
    rng = random.Random(seed)
    chosen = []
    for group in GROUPS[1:]:
        group_pool = [turn for turn in pool if turn["group"] == group]
        if len(fresh) < size:
            new = [turn for turn in group_pool if turn["id"] not in excluded]
            old = [turn for turn in group_pool if turn["id"] in excluded]
            chosen.extend(new + rng.sample(old, quotas[group] - len(new)))
        else:
            chosen.extend(rng.sample(group_pool, quotas[group]))
    rng.shuffle(chosen)
    result = []
    for turn in chosen:
        variants = [
            {"key": "sol", "text": pilot.spoken(turn["sol"])},
            {"key": "hybrid", "text": pilot.spoken(runs[turn["id"]]["gemma"]["text"])},
        ]
        rng.shuffle(variants)
        result.append(
            {
                "id": turn["id"],
                "npc": pilot.NAMES[turn["npc"]],
                "group": turn["group"],
                "history": [
                    {**item, "text": pilot.spoken(item["text"])} for item in pilot.history_tail(turn)
                ],
                "situation": turn["situation"],
                "phrase": turn["phrase"],
                "answers": variants,
            }
        )
    (directory / "sheet.json").write_text(
        json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    return result


def thought_fits(turn: dict[str, Any], record: dict[str, Any]) -> bool:
    thought = record.get("thought", {})
    return (
        turn["window_s"] is not None
        and thought.get("error") is None
        and bool(thought.get("text", "").strip())
        and thought.get("total_s") is not None
        and thought["total_s"] < turn["window_s"]
    )


def summary(directory: Path, ratings: Path | None = None, *, emit: bool = True) -> dict[str, Any]:
    turns = pilot.rows(directory / "turns.jsonl")
    runs = {r["id"]: r for r in pilot.rows(directory / "runs.jsonl")}
    judged = {r["id"]: r for r in pilot.rows(directory / "judge.jsonl")}
    result = {"thought": {}, "gemma": {}, "route": {}, "needs_decision": {}, "judge": {}}

    def routing(chosen):
        counts = Counter(route(runs.get(turn["id"], {})) for turn in chosen)
        return {"n": len(chosen), **{key + "_n": counts[key] for key in ("taken", "handed_off", "no_answer")}}

    for group in GROUPS:
        chosen = [turn for turn in turns if group == "all" or turn["group"] == group]
        thoughts = [runs[turn["id"]]["thought"] for turn in chosen if turn["id"] in runs]
        ready = [p for p in thoughts if p["error"] is None and p["text"].strip()]
        windows = [turn for turn in chosen if turn["window_s"] is not None]
        fits = sum(thought_fits(turn, runs.get(turn["id"], {})) for turn in windows)
        stats = {
            "n": len(thoughts),
            "errors": sum(p["error"] is not None for p in thoughts),
            "empty_n": sum(not p["text"].strip() for p in thoughts),
            "window_n": len(windows),
            "fits_window_n": fits,
            "fits_window_fraction": pilot.fraction(fits, len(windows)),
        }
        for metric in ("total_s", "first_token_s"):
            stats[metric] = pilot.distribution(p[metric] for p in ready)
        stats["output_tokens"] = pilot.distribution(
            (p.get("usage") or {}).get("output_tokens") for p in ready
        )
        result["thought"][group] = stats
        parts = [runs[t["id"]]["gemma"] for t in chosen if t["id"] in runs]
        answered = [t for t in chosen if route(runs.get(t["id"], {})) != "no_answer"]
        successful = [runs[t["id"]]["gemma"] for t in answered]
        stats = {
            "n": len(parts),
            "answered_n": len(successful),
            "errors": sum(p["error"] is not None for p in parts),
        }
        for metric in ("prewarm_s", "first_token_s", "first_sentence_s", "total_s"):
            stats[metric] = pilot.distribution(p[metric] for p in successful)
        stats["prompt_n"] = pilot.distribution((p["timings"] or {}).get("prompt_n") for p in successful)
        stats["mood_at_start"] = pilot.fraction(
            sum(
                pilot.starts_with_mood(
                    runs[t["id"]]["stall"] if runs[t["id"]]["handoff"] else runs[t["id"]]["gemma"]["text"]
                )
                for t in answered
            ),
            len(answered),
        )
        for state, field in (("taken", "full_words"), ("handed_off", "stall_words")):
            stats[field] = pilot.distribution(
                len(
                    pilot.spoken(
                        runs[t["id"]]["stall"] if state == "handed_off" else runs[t["id"]]["gemma"]["text"]
                    ).split()
                )
                for t in answered
                if route(runs[t["id"]]) == state
            )
        result["gemma"][group] = stats
        timely = sum(
            route(runs.get(t["id"], {})) == "taken" and thought_fits(t, runs[t["id"]]) for t in windows
        )
        result["route"][group] = {
            **routing(chosen),
            "window_n": len(windows),
            "timely_taken_n": timely,
            "timely_taken_fraction": pilot.fraction(timely, len(windows)),
        }
        for state in ("all", "taken", "handed_off"):
            state_turns = [
                t
                for t in chosen
                if route(runs.get(t["id"], {})) != "no_answer"
                and (state == "all" or route(runs[t["id"]]) == state)
            ]
            checks = [judged[t["id"]] for t in state_turns if t["id"] in judged]
            verdicts = [c["result"] for c in checks if c["error"] is None and c["result"] is not None]
            counts = {"total_n": len(state_turns), "n": len(checks), "errors": len(checks) - len(verdicts)}
            for field in ("unauthorized", "threats", "invented"):
                counts[field] = sum(bool(v.get(field)) for v in verdicts)
                counts[field + "_items"] = sum(len(v.get(field, [])) for v in verdicts)
            for field in ("answers", "in_character"):
                counts[field + "_false"] = sum(v.get(field) is False for v in verdicts)
            result["judge"].setdefault(state, {})[group] = counts
    for value in (True, False):
        chosen = [
            t
            for t in turns
            if (check := judged.get(t["id"]))
            and check["error"] is None
            and (check["result"] or {}).get("needs_decision") is value
        ]
        result["needs_decision"][str(value).lower()] = routing(chosen)
    stalls = Counter(
        re.sub(r"[\W_]+$", "", pilot.spoken(r["stall"]).casefold())
        for turn in turns
        if (r := runs.get(turn["id"]))
        if route(r) == "handed_off"
    )
    stall_n = sum(stalls.values())
    result["stalls"] = {
        "n": stall_n,
        "unique_n": len(stalls),
        "most_common_fraction": pilot.fraction(max(stalls.values(), default=0), stall_n),
    }
    hard = result["route"]["hard"]
    conversational_n = sum(result["route"][g]["n"] for g in ("pure", "soft"))
    taken_n = sum(result["route"][g]["taken_n"] for g in ("pure", "soft"))
    unauthorized = result["judge"]["taken"]["all"]
    coverage = unauthorized["n"] == unauthorized["total_n"] and not unauthorized["errors"]
    acceptance = {
        "criterion_2": {
            **unauthorized,
            "preliminary": True,
            "passed": unauthorized["unauthorized"] == 0 if coverage else None,
        },
        "criterion_3": {
            "handed_off_n": hard["handed_off_n"],
            "total_n": hard["n"],
            "fraction": pilot.fraction(hard["handed_off_n"], hard["n"]),
        },
        "criterion_4": {
            "taken_n": taken_n,
            "total_n": conversational_n,
            "fraction": pilot.fraction(taken_n, conversational_n),
        },
        "criterion_5": {
            "fits_n": result["thought"]["all"]["fits_window_n"],
            "total_n": result["thought"]["all"]["window_n"],
            "fraction": result["thought"]["all"]["fits_window_fraction"],
        },
        "criterion_6": {**result["gemma"]["all"]["first_sentence_s"]},
    }
    for number, threshold in ((3, 0.9), (4, 0.6), (5, 0.9)):
        stats = acceptance[f"criterion_{number}"]
        stats["passed"] = stats["fraction"] >= threshold if stats["fraction"] is not None else None
    median = acceptance["criterion_6"]["median"]
    acceptance["criterion_6"]["passed"] = median <= 0.8 if median is not None else None
    if ratings is not None:
        scores = {"bad": 0, "ok": 1, "good": 2}
        rated = {
            (r["id"], r["key"]): scores[r["rating"]] for r in json.loads(ratings.read_text(encoding="utf-8"))
        }
        acceptance["criterion_1"] = {}
        for group in GROUPS:
            chosen = [
                t
                for t in turns
                if (group == "all" or t["group"] == group) and route(runs.get(t["id"], {})) == "taken"
            ]
            paired = [t for t in chosen if (t["id"], "sol") in rated and (t["id"], "hybrid") in rated]
            values = [rated[t["id"], "hybrid"] for t in chosen if (t["id"], "hybrid") in rated]
            better = sum(rated[t["id"], "hybrid"] >= rated[t["id"], "sol"] for t in paired)
            same_or_better = pilot.fraction(better, len(paired))
            bad = pilot.fraction(values.count(0), len(values))
            acceptance["criterion_1"][group] = {
                "total_n": len(chosen),
                "paired_n": len(paired),
                "at_least_sol_n": better,
                "at_least_sol_fraction": same_or_better,
                "rated_n": len(values),
                "bad_n": values.count(0),
                "bad_fraction": bad,
                "passed": same_or_better >= 0.6 and bad <= 0.15
                if same_or_better is not None and bad is not None
                else None,
            }
    result["acceptance"] = acceptance
    text = json.dumps(result, ensure_ascii=False, indent=2) + "\n"
    (directory / "summary.json").write_text(text, encoding="utf-8")
    if emit:
        print(text, end="")
    return result


async def selftest() -> None:
    with TemporaryDirectory(prefix="gemma-sol-hybrid-") as temporary:
        root = Path(temporary)
        trace = root / "trace.jsonl"
        records = [{"type": "instructions", "hash": "persona", "text": "Recorded NPC instructions."}]
        for index, calls in enumerate(([], [], ["look"], ["recall"], ["give_item"], ["follow"], ["attack"])):
            phrase = f"Hero phrase {index}"
            history = [Message("developer", "Earlier note.")]
            history += [
                Message(role, f"[neutral] Earlier {i}") for i in range(5) for role in ("user", "assistant")
            ]
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
                            "calls": [{"name": name} for name in calls],
                        }
                    ],
                }
            )
        # The repeated input must not create repeated candidates.
        records.append(records[-1])
        pilot.save_rows(trace, records)
        directory = root / "pilot"
        turns = pick([trace, trace], directory)
        assert len(turns) == 7
        assert [t["group"] for t in turns] == ["pure", "pure", "soft", "soft", "hard", "hard", "hard"]
        assert [t["window_s"] for t in turns] == [None] + [18.0] * 6
        assert all(Path(t["trace"]).is_absolute() for t in turns)
        sol = FakeBackend(script=[FakeStep("" if i == 3 else "A short thought.") for i in range(7)], delay=0)
        replies = [
            "[friendly] First answer.",
            "[neutral] Second answer.",
            "[angry] Another answer.",
            "  [SoL]  [neutral] Let me think...",
            "[sol][friendly] LET ME THINK!",
            "[neutral] Last answer.",
        ]
        quick = FakeBackend(script=[FakeStep(text) for text in replies], delay=0)
        await run(directory, only="turn-0", sol=sol, quick=quick, slot=2)
        assert len(pilot.rows(directory / "runs.jsonl")) == len(sol.calls) == len(quick.calls) == 1
        await run(directory, sol=sol, quick=quick, slot=2)
        await run(directory, sol=sol, quick=quick, slot=2)
        assert len(sol.calls) == 7 and len(quick.calls) == len(quick.prewarms) == 6
        assert quick.slot_ids == quick.prewarm_slot_ids == [2] * 6
        assert all(not tools for _, _, tools in sol.calls + quick.calls)
        assert sol.cache_keys == ["pilot-BAU_4300_ADDON_CAVALORN"] * 7
        assert quick.temperatures == [0.6] * 6
        assert sol.calls[0][1][-2:] == [
            Message("developer", turns[0]["situation"]),
            Message("developer", (HERE / "think_short.md").read_text()),
        ]
        assert not any(isinstance(m, Message) and m.content == turns[0]["phrase"] for m in sol.calls[0][1])
        assert quick.prewarms[0][1][-1] == Message(
            "developer",
            turns[0]["situation"]
            + "\n\n"
            + (HERE / "speak_hybrid.md").read_text().format(thought="A short thought."),
        )
        for (_, prefix), (_, request, _) in zip(quick.prewarms, quick.calls, strict=True):
            assert request[:-1] == prefix and request[-1].role == "user"
        runs = {r["id"]: r for r in pilot.rows(directory / "runs.jsonl")}
        assert runs["turn-3"]["thought"]["text"] == "" and runs["turn-3"]["thought"]["error"] is None
        assert runs["turn-3"]["gemma"]["error"] == "no thought" and runs["turn-3"]["handoff"] is None
        assert runs["turn-4"]["handoff"] is True and runs["turn-4"]["stall"] == "[neutral] Let me think..."
        assert runs["turn-0"]["handoff"] is False and runs["turn-0"]["stall"] is None
        verdict = {
            "needs_decision": False,
            "commits": [],
            "unauthorized": [],
            "threats": [],
            "invented": [],
            "answers": True,
            "in_character": True,
            "note": "",
        }
        referee = FakeBackend(
            script=[
                FakeStep("```json\n" + json.dumps(verdict | {"needs_decision": i in (0, 4, 5)}) + "\n```")
                for i in (0, 1, 2, 4, 5, 6)
            ],
            delay=0,
        )
        await judge(directory, sol=referee)
        await judge(directory, sol=referee)
        judgments = pilot.rows(directory / "judge.jsonl")
        assert len(referee.calls) == len(judgments) == 6
        assert all(set(verdict) <= set(r["result"]) and r["error"] is None for r in judgments)
        assert all("raw" in r and "total_s" in r and "first_token_s" in r for r in judgments)
        assert referee.calls[0][0] == (HERE / "judge_hybrid.md").read_text()
        assert "Мысли NPC\nA short thought." in referee.calls[0][1][0].content
        assert "Реплика NPC\n  [SoL]" in referee.calls[3][1][0].content
        exclude = root / "exclude.jsonl"
        pilot.save_rows(exclude, [turns[0]])
        comparison = sheet(directory, n=3, exclude=exclude)
        assert comparison == sheet(directory, n=3, exclude=exclude)
        assert {t["id"] for t in comparison} == {"turn-1", "turn-2", "turn-6"}
        assert {t["group"] for t in comparison} == {"pure", "soft", "hard"}
        assert len(sheet(directory, n=4, exclude=exclude)) == 4
        assert {t["id"] for t in sheet(directory, n=4, exclude=exclude)} == {
            "turn-0",
            "turn-1",
            "turn-2",
            "turn-6",
        }
        comparison = sheet(directory, n=3)
        assert Counter(t["group"] for t in comparison) == {"pure": 1, "soft": 1, "hard": 1}
        assert all(
            len(t["history"]) == 8 and {a["key"] for a in t["answers"]} == {"sol", "hybrid"}
            for t in comparison
        )
        assert all("[" not in a["text"] for t in comparison for a in t["answers"])
        proportional = root / "proportional"
        proportional.mkdir()
        pool = [turns[0] | {"id": f"pure-{i}"} for i in range(9)]
        pool += [turns[2] | {"id": f"soft-{i}"} for i in range(3)]
        pilot.save_rows(proportional / "turns.jsonl", pool)
        pilot.save_rows(proportional / "runs.jsonl", (runs["turn-0"] | {"id": t["id"]} for t in pool))
        pilot.save_rows(exclude, pool[:6])
        comparison = sheet(proportional, n=4, exclude=exclude)
        assert Counter(t["group"] for t in comparison) == {"pure": 3, "soft": 1}
        assert not {t["id"] for t in comparison} & {t["id"] for t in pool[:6]}
        for sizes, blocked in (
            ((20, 15, 1), {"hard-0"}),
            ((29, 1, 1), {"soft-0", "hard-0"}),
        ):
            pool = [
                turns[index] | {"id": f"{group}-{i}"}
                for index, group, count in zip((0, 2, 6), GROUPS[1:], sizes, strict=True)
                for i in range(count)
            ]
            pilot.save_rows(proportional / "turns.jsonl", pool)
            pilot.save_rows(proportional / "runs.jsonl", (runs["turn-0"] | {"id": t["id"]} for t in pool))
            pilot.save_rows(exclude, ({"id": identifier} for identifier in blocked))
            comparison = sheet(proportional, n=30, exclude=exclude)
            assert len(comparison) == 30
            assert len({t["id"] for t in comparison} - blocked) == min(30, len(pool) - len(blocked))
            assert json.loads((proportional / "sheet.json").read_text()) == comparison
        # Fixed timings make arithmetic independent of the machine running this test.
        for index, record in enumerate(runs.values()):
            if index == 3:
                continue
            record["thought"].update(total_s=1.0, first_token_s=0.1, usage={"output_tokens": 10 + index})
            record["gemma"].update(
                prewarm_s=0.2,
                first_token_s=0.3,
                first_sentence_s=0.4,
                total_s=0.5,
                timings={"prompt_n": 100 + index},
            )
        pilot.save_rows(directory / "runs.jsonl", runs.values())
        ratings = root / "ratings.json"
        scores = [("good", "good"), ("bad", "ok"), ("good", "bad"), ("ok", "good")]
        ratings.write_text(
            json.dumps(
                [
                    {"id": f"turn-{index}", "key": key, "rating": rating}
                    for index, pair in zip((0, 1, 2, 6), scores, strict=True)
                    for key, rating in zip(("sol", "hybrid"), pair, strict=True)
                ]
            )
        )
        report = summary(directory, ratings, emit=False)
        assert report["route"]["all"]["taken_n"] == 4 and report["route"]["all"]["handed_off_n"] == 2
        assert report["route"]["all"]["no_answer_n"] == 1 and report["route"]["all"]["timely_taken_n"] == 3
        assert (
            report["needs_decision"]["true"]["taken_n"] == 1
            and report["needs_decision"]["true"]["handed_off_n"] == 2
        )
        assert report["needs_decision"]["false"]["taken_n"] == 3
        assert report["thought"]["all"]["output_tokens"]["median"] == 13
        assert report["thought"]["all"]["total_s"]["p90"] == 1
        assert report["gemma"]["all"]["prompt_n"]["median"] == 103
        assert report["gemma"]["all"]["mood_at_start"] == 1
        assert report["gemma"]["all"]["full_words"]["median"] == 2
        assert report["gemma"]["all"]["stall_words"]["median"] == 3
        assert report["stalls"] == {"n": 2, "unique_n": 1, "most_common_fraction": 1}
        acceptance = report["acceptance"]
        assert acceptance["criterion_1"]["all"]["at_least_sol_fraction"] == 0.75
        assert (
            acceptance["criterion_1"]["all"]["bad_fraction"] == 0.25
            and acceptance["criterion_1"]["all"]["passed"] is False
        )
        assert acceptance["criterion_1"]["pure"]["passed"] is True
        assert (
            acceptance["criterion_2"]["unauthorized"] == 0
            and acceptance["criterion_2"]["preliminary"] is True
        )
        assert acceptance["criterion_3"]["fraction"] == 2 / 3 and acceptance["criterion_3"]["passed"] is False
        assert acceptance["criterion_4"]["fraction"] == 0.75 and acceptance["criterion_4"]["passed"] is True
        assert acceptance["criterion_5"]["fraction"] == 5 / 6 and acceptance["criterion_5"]["passed"] is False
        assert acceptance["criterion_6"]["median"] == 0.4 and acceptance["criterion_6"]["passed"] is True
        judgments[0]["result"].update(
            unauthorized=["One", "Two"],
            threats=["Threat"],
            invented=["Fact"],
            answers=False,
            in_character=False,
        )
        pilot.save_rows(directory / "judge.jsonl", judgments)
        report = summary(directory, emit=False)
        assert report["judge"]["taken"]["pure"]["unauthorized"] == 1
        assert report["judge"]["taken"]["pure"]["unauthorized_items"] == 2
        assert (
            report["judge"]["taken"]["pure"]["threats"] == report["judge"]["taken"]["pure"]["invented"] == 1
        )
        assert (
            report["judge"]["taken"]["pure"]["answers_false"]
            == report["judge"]["taken"]["pure"]["in_character_false"]
            == 1
        )
        assert report["judge"]["handed_off"]["all"]["unauthorized"] == 0
        assert report["acceptance"]["criterion_2"]["passed"] is False
        await run(
            directory,
            redo="turn-0",
            sol=FakeBackend("New thought.", delay=0),
            quick=FakeBackend("[neutral] New answer.", delay=0),
        )
        assert (
            len(pilot.rows(directory / "runs.jsonl")) == 7 and len(pilot.rows(directory / "judge.jsonl")) == 5
        )
        invalid = FakeBackend("invalid JSON", delay=0)
        await judge(directory, sol=invalid)
        await judge(directory, sol=invalid)
        assert len(invalid.calls) == 1
        assert any(r["raw"] == "invalid JSON" and r["error"] for r in pilot.rows(directory / "judge.jsonl"))
        assert summary(directory, emit=False)["judge"]["taken"]["all"]["errors"] == 1

        async def failed_stream(**kwargs):
            yield TextDelta("Partial")
            raise LLMError("Synthetic model error")

        broken = FakeBackend(delay=0)
        broken.stream = failed_stream
        untouched = FakeBackend(delay=0)
        await run(directory, redo="turn-1", sol=broken, quick=untouched)
        failed = next(r for r in pilot.rows(directory / "runs.jsonl") if r["id"] == "turn-1")
        assert (
            failed["thought"]["text"] == "Partial" and failed["thought"]["error"] == "Synthetic model error"
        )
        assert failed["gemma"]["error"] == "no thought" and failed["handoff"] is None and not untouched.calls
        await run(
            directory, redo="turn-2", sol=FakeBackend("Thought.", delay=0), quick=FakeBackend("", delay=0)
        )
        empty = next(r for r in pilot.rows(directory / "runs.jsonl") if r["id"] == "turn-2")
        assert empty["gemma"]["text"] == "" and empty["gemma"]["error"] is None
        assert empty["handoff"] is False and route(empty) == "no_answer"
        await run(directory, redo="turn-6", sol=FakeBackend("Thought.", delay=0), quick=broken)
        failed = next(r for r in pilot.rows(directory / "runs.jsonl") if r["id"] == "turn-6")
        assert failed["gemma"]["text"] == "Partial" and failed["gemma"]["error"] == "Synthetic model error"
        assert all(
            (directory / name).is_file()
            for name in ("turns.jsonl", "runs.jsonl", "judge.jsonl", "sheet.json", "summary.json")
        )
        print(
            "Selftest passed: 7 turns, 3 groups; pick/run/judge/sheet/summary; "
            "resume, redo, errors, empty output, acceptance 1-6."
        )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_subparsers(dest="mode", required=True)
    command = modes.add_parser("pick")
    command.add_argument("traces", type=Path, nargs="+")
    command.add_argument("--out", type=Path, required=True)
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
        if mode == "sheet":
            command.add_argument("--n", type=int, default=30)
            command.add_argument("--seed", type=int, default=SEED)
            command.add_argument("--exclude", type=Path)
        if mode == "summary":
            command.add_argument("--ratings", type=Path)
    modes.add_parser("selftest")
    args = parser.parse_args()
    if args.mode == "pick":
        pick(args.traces, args.out)
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
        sheet(args.directory, args.n, args.seed, args.exclude)
    elif args.mode == "summary":
        summary(args.directory, args.ratings)
    else:
        asyncio.run(selftest())


if __name__ == "__main__":
    main()
