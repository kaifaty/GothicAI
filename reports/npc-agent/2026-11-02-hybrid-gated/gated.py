"""Offline classifier-gated Gemma/Sol hybrid. Run from server/; selftest needs no network."""

from __future__ import annotations

import argparse
import asyncio
import importlib.util
import json
import re
import sys
import time
from collections import Counter
from pathlib import Path
from tempfile import TemporaryDirectory
from typing import Any
from unittest.mock import patch

import httpx

from gothic_ai.llm import FakeBackend, FakeStep, Message, ToolCall, ToolResult
from gothic_ai.trace import item_from_json, item_json

HERE = Path(__file__).resolve().parent
modules = {}
for name, folder in (
    ("pilot", "2026-11-02-sol-thinks"),
    ("hybrid", "2026-11-02-gemma-sol-hybrid"),
    ("route", "2026-11-02-route-classifier"),
):
    spec = importlib.util.spec_from_file_location(f"_gated_{name}", HERE.parent / folder / f"{name}.py")
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    modules[name] = module
pilot, hybrid, route = (modules[name] for name in ("pilot", "hybrid", "route"))
ROUTES = ("classifier", "gemma_handoff", "taken", "error")
LEAK = re.compile(r"\b[a-z_]+\s*[({]")
judge = hybrid.judge


def pick(hybrid_dir: Path, route_dir: Path, directory: Path) -> list[dict[str, Any]]:
    thoughts = {r["id"]: r.get("thought") for r in pilot.rows(hybrid_dir / "runs.jsonl")}
    labels = {r["id"]: r for r in pilot.rows(route_dir / "gold.jsonl")}
    evaluation = json.loads((route_dir / "eval.json").read_text(encoding="utf-8"))["classifiers"][
        "gemma_phrase"
    ]
    folds = {Path(r["trace"]).resolve(): r for r in evaluation["folds"]}
    predictions = {r["id"]: r for r in evaluation["predictions"]}
    traces = {}
    turns = []
    for turn in pilot.rows(hybrid_dir / "turns.jsonl"):
        identifier = turn["id"]
        if not isinstance(thoughts.get(identifier), dict):
            raise ValueError(f"{identifier}: missing thought")
        label = labels.get(identifier, {})
        if type(label.get("label")) is not bool or "kind" not in label:
            raise ValueError(f"{identifier}: missing label or kind")
        trace = Path(turn["trace"]).resolve()
        fold = folds.get(trace, {})
        if fold.get("threshold") is None:
            raise ValueError(f"{identifier}: missing fold threshold")
        prediction = predictions.get(identifier, {})
        if prediction.get("score") is None or type(prediction.get("predicted")) is not bool:
            raise ValueError(f"{identifier}: missing prediction")
        if trace not in traces:
            try:
                traces[trace] = {(r["t"], r["npc"]): r for r in pilot.rows(trace) if r.get("type") == "turn"}
            except (OSError, ValueError) as error:
                raise ValueError(f"{identifier}: cannot read trace ({type(error).__name__})") from error
        recorded = traces[trace].get((turn["t"], turn["npc"]))
        if recorded is None or not recorded.get("steps"):
            raise ValueError(f"{identifier}: missing trace turn")
        if "first_token_s" not in recorded["steps"][0] or "total_s" not in recorded:
            raise ValueError(f"{identifier}: missing recorded Sol timings")
        turns.append(
            {
                **turn,
                "thought": thoughts[identifier],
                "label": label["label"],
                "kind": label["kind"],
                "threshold": fold["threshold"],
                "oof_score": prediction["score"],
                "oof_predicted": prediction["predicted"],
                "sol_first_token_s": recorded["steps"][0]["first_token_s"],
                "sol_total_s": recorded["total_s"],
            }
        )
    directory.mkdir(parents=True, exist_ok=True)
    pilot.save_rows(directory / "turns.jsonl", turns)
    print(f"Picked {len(turns)} turns", file=sys.stderr)
    return turns


async def run(
    directory: Path,
    *,
    slot: int = 0,
    classify_slot: int = 1,
    quick_at: str | None = None,
    only: str | None = None,
    redo: str | None = None,
    transport: httpx.AsyncBaseTransport | None = None,
) -> None:
    speak = (hybrid.HERE / "speak_hybrid.md").read_text(encoding="utf-8")
    classify = (route.HERE / "classify.md").read_text(encoding="utf-8")
    turns = pilot.rows(directory / "turns.jsonl")
    results = {r["id"]: r for r in pilot.rows(directory / "runs.jsonl")}
    if only and redo and only != redo:
        raise SystemExit("--only and --redo must name the same turn when used together")
    selected = only or redo
    if selected and not any(t["id"] == selected for t in turns):
        raise SystemExit(f"Unknown turn: {selected}")
    pending = [
        t
        for t in turns
        if (not selected or t["id"] == selected) and (t["id"] not in results or t["id"] == redo)
    ]
    if not pending:
        return
    url = pilot.gemma_url(quick_at).rstrip("/")
    quick = None
    try:
        async with httpx.AsyncClient(transport=transport, timeout=60) as client:
            for turn in pending:
                if turn["id"] == redo:
                    path = directory / "judge.jsonl"
                    if path.exists():
                        pilot.save_rows(path, (r for r in pilot.rows(path) if r["id"] != redo))
                classified = {
                    "score": None,
                    "score_source": None,
                    "text": "",
                    "latency_s": None,
                    "threshold": turn["threshold"],
                    "decision": None,
                    "oof_predicted": turn["oof_predicted"],
                    "error": None,
                }
                started = time.monotonic()
                try:
                    response = await client.post(
                        url + "/chat/completions",
                        json={
                            "messages": [
                                {"role": "system", "content": classify},
                                {"role": "user", "content": route.classifier_text(turn, "phrase")},
                            ],
                            "max_tokens": 1,
                            "temperature": 0,
                            "logprobs": True,
                            "top_logprobs": 10,
                            "id_slot": classify_slot,
                            "cache_prompt": True,
                        },
                    )
                    response.raise_for_status()
                    choice = response.json()["choices"][0]
                    classified["text"] = choice["message"]["content"]
                    classified["score"], classified["score_source"] = route.gemma_score(choice)
                    classified["decision"] = classified["score"] >= turn["threshold"]
                except httpx.HTTPStatusError as error:
                    classified["error"] = f"Classifier returned HTTP {error.response.status_code}"
                except Exception as error:
                    classified["error"] = f"Classifier request failed ({type(error).__name__})"
                classified["latency_s"] = time.monotonic() - started
                gemma, handoff, stall = {}, None, None
                state = "error" if classified["error"] else "classifier"
                if classified["error"] is None and classified["decision"] is False:
                    if quick is None:
                        quick = pilot.make_quick(url, max_tokens=300)
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
                            turn["situation"] + "\n\n" + speak.format(thought=turn["thought"]["text"]),
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
                    state = {"no_answer": "error", "handed_off": "gemma_handoff", "taken": "taken"}[
                        hybrid.route({"gemma": gemma, "handoff": handoff})
                    ]
                results[turn["id"]] = {
                    "id": turn["id"],
                    "thought": turn["thought"],
                    "classify": classified,
                    "route": state,
                    "gemma": gemma,
                    "handoff": handoff,
                    "stall": stall,
                    "leaks": LEAK.findall(gemma.get("text", "")),
                }
                pilot.save_rows(directory / "runs.jsonl", results.values())
                print(f"Ran {turn['id']}: {state}", file=sys.stderr)
    finally:
        if quick is not None:
            await quick.aclose()


def sheet(directory: Path, seed: int = pilot.SEED) -> list[dict[str, Any]]:
    return hybrid.sheet(directory, n=max(1, len(pilot.rows(directory / "turns.jsonl"))), seed=seed)


def summary(directory: Path, ratings: Path | None = None, *, emit: bool = True) -> dict[str, Any]:
    turns = pilot.rows(directory / "turns.jsonl")
    runs = {r["id"]: r for r in pilot.rows(directory / "runs.jsonl")}
    judged = {r["id"]: r for r in pilot.rows(directory / "judge.jsonl")}
    taken = [t for t in turns if runs.get(t["id"], {}).get("route") == "taken"]
    handed = [t for t in turns if runs.get(t["id"], {}).get("route") == "gemma_handoff"]

    def routing(chosen):
        counts = Counter(runs.get(t["id"], {}).get("route") for t in chosen)
        return {"n": len(chosen), "pending_n": counts[None], **{r + "_n": counts[r] for r in ROUTES}}

    def details(turn):
        return {
            **{k: turn[k] for k in ("id", "npc", "phrase")},
            "text": runs[turn["id"]]["gemma"].get("text", ""),
        }

    def decisions(chosen):
        held = sum(t["id"] in runs and runs[t["id"]]["route"] != "taken" for t in chosen)
        return {"n": len(chosen), "reached_sol_n": held, "fraction": pilot.fraction(held, len(chosen))}

    def benefit(chosen):
        n = sum(runs.get(t["id"], {}).get("route") == "taken" for t in chosen)
        return {"n": len(chosen), "taken_n": n, "fraction": pilot.fraction(n, len(chosen))}

    result = {
        "route": {g: routing([t for t in turns if g == "all" or t["group"] == g]) for g in pilot.GROUPS}
    }
    result["route"]["by_label"] = {
        str(value).lower(): routing([t for t in turns if t["label"] is value]) for value in (True, False)
    }
    required = [t for t in turns if t["label"] is True]
    misses = [details(t) | {"kind": t["kind"]} for t in taken if t["label"] is True]
    result["decisions"] = {
        "all": decisions(required),
        "by_kind": {
            k: decisions([t for t in required if t["kind"] == k])
            for k in sorted({t["kind"] for t in required})
        },
        "misses": misses,
    }
    result["benefit"] = {
        "simple": benefit([t for t in turns if t["label"] is False]),
        "all": benefit(turns),
        "pure_soft": benefit([t for t in turns if t["group"] in ("pure", "soft")]),
    }
    classified = [
        t for t in turns if t["id"] in runs and runs[t["id"]]["classify"]["decision"] is not None
    ]
    mismatches = [
        {
            **{k: t[k] for k in ("id", "phrase", "oof_score", "threshold")},
            "score": runs[t["id"]]["classify"]["score"],
        }
        for t in classified
        if runs[t["id"]]["classify"]["decision"] != t["oof_predicted"]
    ]
    result["classifier"] = {
        "n": len(classified),
        "errors": sum(bool(r["classify"]["error"]) for r in runs.values()),
        "matched_n": len(classified) - len(mismatches),
        "matched_fraction": pilot.fraction(len(classified) - len(mismatches), len(classified)),
        "mismatches": mismatches,
        "score_source": dict(Counter(runs[t["id"]]["classify"]["score_source"] for t in classified)),
    }
    result["judge"] = {}
    for state, chosen in (("taken", taken), ("gemma_handoff", handed)):
        checks = [judged[t["id"]] for t in chosen if t["id"] in judged]
        valid = [c["result"] for c in checks if c["error"] is None and c["result"] is not None]
        stats = {"total_n": len(chosen), "n": len(checks), "errors": len(checks) - len(valid)}
        for field in ("commits", "unauthorized", "threats", "invented"):
            stats[field + "_n"] = sum(bool(v.get(field)) for v in valid)
        for field in ("answers", "in_character"):
            stats[field + "_fraction"] = pilot.fraction(sum(v.get(field) is True for v in valid), len(valid))
        flagged = []
        for t in chosen:
            check = judged.get(t["id"], {})
            verdict = check.get("result") if check.get("error") is None else None
            if verdict and (verdict.get("unauthorized") or (state == "taken" and verdict.get("commits"))):
                flagged.append(
                    details(t)
                    | {"commits": verdict.get("commits", []), "unauthorized": verdict.get("unauthorized", [])}
                    | ({"text": runs[t["id"]]["stall"]} if state == "gemma_handoff" else {})
                )
        stats["flagged"] = flagged
        result["judge"][state] = stats
    leaking = [t["id"] for t in turns if runs.get(t["id"], {}).get("leaks")]
    result["leaks"] = {"n": len(leaking), "ids": leaking}
    result["timing"] = {
        "classify": pilot.distribution(r["classify"]["latency_s"] for r in runs.values()),
        "gemma": {
            metric: pilot.distribution(runs[t["id"]]["gemma"].get(metric) for t in taken)
            for metric in ("first_token_s", "first_sentence_s", "total_s", "prewarm_s")
        },
        "sol": {
            metric: pilot.distribution(t["sol_" + metric] for t in taken)
            for metric in ("first_token_s", "total_s")
        },
    }
    result["timing"]["gemma"]["after_phrase_s"] = pilot.distribution(
        r["classify"]["latency_s"] + r["gemma"]["first_sentence_s"]
        for t in taken
        if (r := runs[t["id"]])["gemma"].get("first_sentence_s") is not None
        and r["classify"]["latency_s"] is not None
    )
    calls = Counter(name for t in taken for name in t["calls"])
    result["background_mind"] = {
        name: {"n": count, "ids": [t["id"] for t in taken if name in t["calls"]]}
        for name, count in sorted(calls.items())
    }
    complete = all(t["id"] in runs for t in turns)
    checks = result["judge"]["taken"]
    recall = result["decisions"]["all"]["fraction"]
    useful = result["benefit"]["simple"]["fraction"]
    timing = result["timing"]["gemma"]["after_phrase_s"]
    critical = [m for m in misses if m["kind"] in ("deal", "item", "money", "quest")]
    acceptance = {
        "criterion_2": {
            "unauthorized_n": checks["unauthorized_n"],
            "judged_n": checks["n"] - checks["errors"],
            "total_n": len(taken),
            "preliminary": True,
            "note": "Final acceptance requires manual review of the judge flags.",
            "passed": checks["unauthorized_n"] == 0
            if complete and checks["n"] == len(taken) and not checks["errors"]
            else None,
        },
        "criterion_3": {
            **result["decisions"]["all"],
            "critical_misses": critical,
            "passed": recall >= 0.95 and not critical if complete and recall is not None else None,
        },
        "criterion_4": {
            **result["benefit"]["simple"],
            "passed": useful >= 0.6 if complete and useful is not None else None,
        },
        "criterion_5": {
            **timing,
            "passed": timing["median"] <= 0.8 if timing["n"] == len(taken) and taken else None,
        },
        "criterion_6": {**result["leaks"], "passed": not leaking if complete else None},
    }
    if ratings is not None:
        scores = {"bad": 0, "ok": 1, "good": 2}
        rated = {
            (r["id"], r["key"]): scores[r["rating"]] for r in json.loads(ratings.read_text(encoding="utf-8"))
        }
        acceptance["criterion_1"] = {}
        for group in pilot.GROUPS:
            chosen = [t for t in taken if group == "all" or t["group"] == group]
            paired = [t for t in chosen if (t["id"], "sol") in rated and (t["id"], "hybrid") in rated]
            values = [rated[t["id"], "hybrid"] for t in chosen if (t["id"], "hybrid") in rated]
            better = sum(rated[t["id"], "hybrid"] >= rated[t["id"], "sol"] for t in paired)
            fraction = pilot.fraction(better, len(paired))
            bad = pilot.fraction(values.count(0), len(values))
            acceptance["criterion_1"][group] = {
                "total_n": len(chosen),
                "paired_n": len(paired),
                "at_least_sol_n": better,
                "at_least_sol_fraction": fraction,
                "rated_n": len(values),
                "bad_n": values.count(0),
                "bad_fraction": bad,
                "passed": fraction >= 0.6 and bad <= 0.15
                if fraction is not None and bad is not None
                else None,
            }
    result["acceptance"] = acceptance
    (directory / "summary.json").write_text(
        json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    if emit:
        print(f"Routes: {result['route']['all']}")
        print(
            f"Decisions reaching Sol: {recall}; simple taken: {useful}; "
            f"after phrase median: {timing['median']} s"
        )
        print(
            f"Judge unauthorized: {checks['unauthorized_n']} (manual review required); leaks: {len(leaking)}"
        )
        print(
            "Acceptance: "
            + ", ".join(
                f"{k}={v.get('passed', v.get('all', {}).get('passed'))}" for k, v in acceptance.items()
            )
        )
    return result


async def selftest() -> None:
    import math

    from openai import AsyncOpenAI

    with TemporaryDirectory(prefix="hybrid-gated-") as temporary:
        root = Path(temporary)
        hybrid_dir, route_dir, directory = (root / name for name in ("hybrid", "route", "gated"))
        route_dir.mkdir()
        calls = [
            [],
            ["look", "recall", "recall"],
            ["give_item"],
            ["attack"],
            [],
            [],
            ["propose_deal"],
            [],
            ["look"],
            ["give_gold"],
        ]
        traces = [root / f"trace-{index}.jsonl" for index in range(2)]
        for index, trace in enumerate(traces):
            records = [{"type": "instructions", "hash": "persona", "text": "Recorded NPC instructions."}]
            for i in range(index * 5, (index + 1) * 5):
                history = [
                    Message("developer", "Earlier note."),
                    Message("user", "Earlier hero phrase."),
                    Message("assistant", "[neutral] Earlier answer."),
                    ToolCall("look", {}, "old-call"),
                    ToolResult("old-call", "Old tool output."),
                ]
                records.append(
                    {
                        "type": "turn",
                        "id": f"turn-{i}",
                        "npc": "BAU_4300_ADDON_CAVALORN",
                        "t": 10 + i,
                        "total_s": 2 + i,
                        "instructions": "persona",
                        "trigger": {"kind": "player", "text": f"Hero phrase {i}"},
                        "steps": [
                            {
                                "first_token_s": 0.5 + i / 10,
                                "added": [
                                    item_json(m)
                                    for m in [
                                        *history,
                                        Message("developer", f"Situation {i}."),
                                        Message("user", f"Hero phrase {i}"),
                                    ]
                                ],
                                "text": "[neutral] Recorded answer.",
                                "calls": [{"name": name} for name in calls[i]],
                            }
                        ],
                    }
                )
            pilot.save_rows(trace, records)
        source = hybrid.pick(traces, hybrid_dir)
        source[0]["extra"] = {"preserved": True}
        pilot.save_rows(hybrid_dir / "turns.jsonl", source)
        thoughts = [
            {
                "id": t["id"],
                "thought": {"text": "A recorded thought.", "error": None, "total_s": 0.3, "extra": [1, 2]},
            }
            for t in source
        ]
        pilot.save_rows(hybrid_dir / "runs.jsonl", thoughts)
        kinds = {2: "item", 3: "fight", 4: "quest", 6: "deal", 9: "money"}
        gold = [{"id": f"turn-{i}", "label": i in kinds, "kind": kinds.get(i, "none")} for i in range(10)]
        pilot.save_rows(route_dir / "gold.jsonl", gold)
        evaluation = {
            "classifiers": {
                "gemma_phrase": {
                    "folds": [
                        {"trace": str(root / "route" / ".." / t.name), "threshold": cutoff}
                        for t, cutoff in zip(traces, (0.5, 0.8), strict=True)
                    ],
                    "predictions": [
                        {
                            "id": f"turn-{i}",
                            "score": 0.9 if i == 4 else 0.5 if i == 2 else 1.0 if i == 9 else 0.1,
                            "predicted": i in (2, 4, 9),
                        }
                        for i in range(10)
                    ],
                }
            }
        }
        eval_path = route_dir / "eval.json"
        eval_path.write_text(json.dumps(evaluation))
        turns = pick(hybrid_dir, route_dir, directory)
        assert len(turns) == 10 and turns[0]["extra"] == source[0]["extra"]
        assert [t["threshold"] for t in turns] == [0.5] * 5 + [0.8] * 5
        assert turns[4]["thought"] == thoughts[4]["thought"]
        assert turns[4]["sol_first_token_s"] == 0.9 and turns[4]["sol_total_s"] == 6
        assert turns[4]["oof_predicted"] is True and turns[4]["label"] is True
        snapshot = (directory / "turns.jsonl").read_bytes()
        bad_eval = json.loads(json.dumps(evaluation))
        bad_eval["classifiers"]["gemma_phrase"]["folds"].pop()
        no_prediction = json.loads(json.dumps(evaluation))
        no_prediction["classifiers"]["gemma_phrase"]["predictions"].pop(4)
        for path, contents, identifier in (
            (hybrid_dir / "runs.jsonl", thoughts[:4] + thoughts[5:], "turn-4"),
            (route_dir / "gold.jsonl", gold[:4] + gold[5:], "turn-4"),
            (
                route_dir / "gold.jsonl",
                [r | {"label": None} if r["id"] == "turn-4" else r for r in gold],
                "turn-4",
            ),
            (eval_path, bad_eval, "turn-5"),
            (eval_path, no_prediction, "turn-4"),
            (traces[0], pilot.rows(traces[0])[:-1], "turn-4"),
        ):
            original = path.read_bytes()
            if path == eval_path:
                path.write_text(json.dumps(contents))
            else:
                pilot.save_rows(path, contents)
            try:
                pick(hybrid_dir, route_dir, directory)
            except ValueError as error:
                assert identifier in str(error), str(error)
            else:
                raise AssertionError("Invalid pick unexpectedly succeeded")
            assert (directory / "turns.jsonl").read_bytes() == snapshot
            path.write_bytes(original)

        replies = {
            0: "[friendly] First answer.",
            1: "[neutral] propose_deal(42)",
            3: "  [SoL]  [neutral] Let me think.",
            4: "[neutral] propose_deal { offer }",
            7: "",
            8: "[neutral] Last answer.",
        }
        requests = []
        redoing = False

        def respond(request: httpx.Request) -> httpx.Response:
            body = json.loads(request.content)
            requests.append(body)
            assert request.url.path == "/v1/chat/completions"
            if body.get("logprobs"):
                i = int(body["messages"][-1]["content"].rsplit(" ", 1)[1])
                assert body == {
                    "messages": [
                        {"role": "system", "content": (route.HERE / "classify.md").read_text()},
                        {"role": "user", "content": route.classifier_text(turns[i], "phrase")},
                    ],
                    "max_tokens": 1,
                    "temperature": 0,
                    "logprobs": True,
                    "top_logprobs": 10,
                    "id_slot": 3,
                    "cache_prompt": True,
                }
                if redoing:
                    assert all(r["id"] != "turn-0" for r in pilot.rows(directory / "judge.jsonl"))
                if i == 5:
                    return httpx.Response(503)
                choice = {"message": {"content": "да" if i == 9 else "нет"}}
                if i != 9:
                    score = 0.5 if i == 2 else 0.1
                    choice["logprobs"] = {
                        "content": [
                            {
                                "top_logprobs": [
                                    {"token": "да", "logprob": math.log(score)},
                                    {"token": "нет", "logprob": math.log(1 - score)},
                                ]
                            }
                        ]
                    }
                return httpx.Response(200, json={"choices": [choice]})
            assert body["id_slot"] == 2
            assert not body.get("tools")
            assert all(m["role"] != "tool" and "tool_calls" not in m for m in body["messages"])
            assert any(m["content"] == "Earlier note." for m in body["messages"])
            assert any(m["content"] == "[neutral] Earlier answer." for m in body["messages"])
            assert body["messages"][0]["content"] == "Recorded NPC instructions."
            if not body.get("stream"):
                assert body["max_tokens"] == 1
                assert body["messages"][-1]["content"].endswith(
                    (hybrid.HERE / "speak_hybrid.md").read_text().format(thought="A recorded thought.")
                )
                return httpx.Response(
                    200, json={"choices": [{"message": {"role": "assistant", "content": "ignored"}}]}
                )
            assert body["max_tokens"] == 300 and body["temperature"] == 0.6
            i = int(body["messages"][-1]["content"].rsplit(" ", 1)[1])
            if i == 6:
                return httpx.Response(503, json={"error": {"message": "Synthetic Gemma failure"}})
            chunk = {
                "id": "fake",
                "object": "chat.completion.chunk",
                "created": 0,
                "model": "fake",
                "choices": [{"index": 0, "delta": {"content": replies[i]}, "finish_reason": None}],
            }
            return httpx.Response(
                200,
                headers={"content-type": "text/event-stream"},
                text="data: " + json.dumps(chunk) + "\n\ndata: [DONE]\n\n",
            )

        transport = httpx.MockTransport(respond)

        def client(**kwargs):
            return AsyncOpenAI(**kwargs, http_client=httpx.AsyncClient(transport=transport))

        options = {
            "quick_at": "https://gemma.invalid/v1",
            "slot": 2,
            "classify_slot": 3,
            "transport": transport,
        }
        with patch("gothic_ai.llm.openai_compat.AsyncOpenAI", side_effect=client):
            await run(directory, only="turn-2", **options)
            assert len(requests) == 1 and pilot.rows(directory / "runs.jsonl")[0]["gemma"] == {}
            await run(directory, only="turn-0", **options)
            assert len(requests) == 4
            await run(directory, **options)
            count = len(requests)
            await run(directory, **options)
            assert len(requests) == count == 24
            runs = {r["id"]: r for r in pilot.rows(directory / "runs.jsonl")}
            assert Counter(r["route"] for r in runs.values()) == {
                "classifier": 2,
                "gemma_handoff": 1,
                "taken": 4,
                "error": 3,
            }
            assert runs["turn-5"]["classify"]["error"] == "Classifier returned HTTP 503"
            assert runs["turn-5"]["gemma"] == {} and runs["turn-6"]["gemma"]["error"]
            assert not runs["turn-7"]["gemma"]["text"]
            assert runs["turn-3"]["stall"] == "[neutral] Let me think."
            assert runs["turn-1"]["leaks"] == ["propose_deal("]
            assert runs["turn-4"]["leaks"] == ["propose_deal {"]
            assert all(r["thought"] == thoughts[int(r["id"].split("-")[1])]["thought"] for r in runs.values())
            verdict = {
                "commits": [],
                "unauthorized": [],
                "threats": [],
                "invented": [],
                "answers": True,
                "in_character": True,
            }
            verdicts = [
                verdict,
                verdict
                | {
                    "commits": ["Promise"],
                    "unauthorized": ["Unauthorized promise"],
                    "invented": ["Fact"],
                    "answers": False,
                },
                verdict | {"unauthorized": ["Stall promise"]},
                verdict | {"commits": ["Threat"], "threats": ["Threat"]},
                verdict | {"in_character": False},
            ]
            referee = FakeBackend(script=[FakeStep(json.dumps(v)) for v in verdicts], delay=0)
            await judge(directory, sol=referee)
            await judge(directory, sol=referee)
            assert len(referee.calls) == len(pilot.rows(directory / "judge.jsonl")) == 5
            assert referee.calls[0][0] == (hybrid.HERE / "judge_hybrid.md").read_text()
            redoing = True
            await run(directory, redo="turn-0", **options)
            redoing = False
            assert len(requests) == 27 and len(pilot.rows(directory / "judge.jsonl")) == 4
            await judge(directory, sol=FakeBackend(json.dumps(verdict), delay=0))
        comparison = sheet(directory, seed=17)
        assert comparison == sheet(directory, seed=17) and len(comparison) == 4
        assert {r["id"] for r in comparison} == {"turn-0", "turn-1", "turn-4", "turn-8"}
        assert all({a["key"] for a in t["answers"]} == {"sol", "hybrid"} for t in comparison)
        assert all("[" not in a["text"] for t in comparison for a in t["answers"])
        assert all(t["npc"] == pilot.NAMES[turns[0]["npc"]] and len(t["history"]) == 2 for t in comparison)
        runs = {r["id"]: r for r in pilot.rows(directory / "runs.jsonl")}
        for r in runs.values():
            r["classify"]["latency_s"] = 0.1
            if r["route"] == "taken":
                r["gemma"].update(first_token_s=0.2, first_sentence_s=0.4, total_s=0.6, prewarm_s=0.05)
        pilot.save_rows(directory / "runs.jsonl", runs.values())
        ratings = root / "ratings.json"
        pairs = [("good", "good"), ("bad", "ok"), ("good", "bad"), ("ok", "good")]
        ratings.write_text(
            json.dumps(
                [
                    {"id": f"turn-{i}", "key": key, "rating": rating}
                    for i, pair in zip((0, 1, 4, 8), pairs, strict=True)
                    for key, rating in zip(("sol", "hybrid"), pair, strict=True)
                ]
            )
        )
        report = summary(directory, ratings, emit=False)
        assert report["route"]["all"] == {
            "n": 10,
            "pending_n": 0,
            "classifier_n": 2,
            "gemma_handoff_n": 1,
            "taken_n": 4,
            "error_n": 3,
        }
        assert report["route"]["pure"]["taken_n"] == report["route"]["soft"]["taken_n"] == 2
        assert report["route"]["by_label"]["true"]["taken_n"] == 1
        assert report["decisions"]["all"]["fraction"] == 0.8
        assert report["decisions"]["by_kind"]["quest"]["fraction"] == 0
        assert report["decisions"]["misses"][0]["id"] == "turn-4"
        assert report["benefit"]["simple"]["fraction"] == 0.6 and report["benefit"]["all"]["fraction"] == 0.4
        assert report["benefit"]["pure_soft"]["fraction"] == 4 / 6
        assert report["classifier"]["matched_n"] == 8 and report["classifier"]["n"] == 9
        assert report["classifier"]["score_source"] == {"logprobs": 8, "text": 1}
        assert report["classifier"]["mismatches"][0]["id"] == "turn-4"
        checks = report["judge"]["taken"]
        assert (
            checks["commits_n"] == 2
            and checks["unauthorized_n"] == checks["threats_n"] == checks["invented_n"] == 1
        )
        assert checks["answers_fraction"] == checks["in_character_fraction"] == 0.75
        assert {t["id"] for t in checks["flagged"]} == {"turn-1", "turn-4"}
        assert report["judge"]["gemma_handoff"]["flagged"][0]["text"] == "[neutral] Let me think."
        assert report["leaks"]["ids"] == ["turn-1", "turn-4"]
        assert (
            report["timing"]["classify"]["n"] == 10
            and report["timing"]["gemma"]["after_phrase_s"]["median"] == 0.5
        )
        assert report["timing"]["sol"]["first_token_s"]["median"] == 0.75
        assert report["timing"]["sol"]["total_s"]["median"] == 4.5
        assert report["background_mind"] == {
            "look": {"n": 2, "ids": ["turn-1", "turn-8"]},
            "recall": {"n": 2, "ids": ["turn-1"]},
        }
        acceptance = report["acceptance"]
        assert [acceptance[f"criterion_{i}"]["passed"] for i in range(2, 7)] == [
            False,
            False,
            True,
            True,
            False,
        ]
        assert acceptance["criterion_2"]["preliminary"] is True
        assert acceptance["criterion_1"]["all"]["at_least_sol_fraction"] == 0.75
        assert (
            acceptance["criterion_1"]["all"]["bad_fraction"] == 0.25
            and acceptance["criterion_1"]["all"]["passed"] is False
        )
        # Even 95% recall cannot excuse a missed quest; noncritical misses can pass.
        extras = [turns[2] | {"id": f"extra-{i}"} for i in range(15)]
        pilot.save_rows(directory / "turns.jsonl", turns + extras)
        pilot.save_rows(
            directory / "runs.jsonl", [*runs.values(), *(runs["turn-2"] | {"id": t["id"]} for t in extras)]
        )
        guard = summary(directory, emit=False)["acceptance"]["criterion_3"]
        assert guard["fraction"] == 0.95 and guard["passed"] is False
        turns[4]["kind"] = "fight"
        pilot.save_rows(directory / "turns.jsonl", turns + extras)
        assert summary(directory, emit=False)["acceptance"]["criterion_3"]["passed"] is True
        turns[4]["label"] = False
        pilot.save_rows(directory / "turns.jsonl", turns)
        for r in runs.values():
            r["leaks"] = []
            if r["route"] == "taken":
                r["gemma"]["text"] = "[neutral] Safe answer."
                r["gemma"]["first_sentence_s"] = 0.71
        pilot.save_rows(directory / "runs.jsonl", runs.values())
        assert summary(directory, emit=False)["acceptance"]["criterion_5"]["passed"] is False
        for r in runs.values():
            if r["route"] == "taken":
                r["gemma"]["first_sentence_s"] = 0.4
        pilot.save_rows(directory / "runs.jsonl", runs.values())
        judgments = pilot.rows(directory / "judge.jsonl")
        for r in judgments:
            r["result"] = verdict
        pilot.save_rows(directory / "judge.jsonl", judgments)
        ratings.write_text(
            json.dumps(
                [{"id": t["id"], "key": key, "rating": "good"} for t in turns for key in ("sol", "hybrid")]
            )
        )
        final = summary(directory, ratings, emit=False)
        assert all(final["acceptance"][f"criterion_{i}"]["passed"] is True for i in range(2, 7))
        assert final["acceptance"]["criterion_1"]["all"]["passed"] is True
        assert json.loads((directory / "summary.json").read_text()) == final
        assert all(
            (directory / name).is_file()
            for name in ("turns.jsonl", "runs.jsonl", "judge.jsonl", "sheet.json", "summary.json")
        )
        print(
            "Selftest passed: 10 turns; pick/run/judge/sheet/summary; "
            "4 routes, slots, history, resume, redo, acceptance and ratings."
        )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_subparsers(dest="mode", required=True)
    command = modes.add_parser("pick")
    command.add_argument("hybrid_dir", type=Path)
    command.add_argument("route_dir", type=Path)
    command.add_argument("--out", type=Path, required=True)
    command = modes.add_parser("run")
    command.add_argument("directory", type=Path)
    command.add_argument("--slot", type=int, default=0)
    command.add_argument("--classify-slot", type=int, default=1)
    command.add_argument("--quick-at")
    command.add_argument("--only")
    command.add_argument("--redo")
    for mode in ("judge", "sheet", "summary"):
        command = modes.add_parser(mode)
        command.add_argument("directory", type=Path)
        if mode == "sheet":
            command.add_argument("--seed", type=int, default=pilot.SEED)
        if mode == "summary":
            command.add_argument("--ratings", type=Path)
    modes.add_parser("selftest")
    args = parser.parse_args()
    if args.mode == "pick":
        pick(args.hybrid_dir, args.route_dir, args.out)
    elif args.mode == "run":
        asyncio.run(
            run(
                args.directory,
                slot=args.slot,
                classify_slot=args.classify_slot,
                quick_at=args.quick_at,
                only=args.only,
                redo=args.redo,
            )
        )
    elif args.mode == "judge":
        asyncio.run(judge(args.directory))
    elif args.mode == "sheet":
        sheet(args.directory, seed=args.seed)
    elif args.mode == "summary":
        summary(args.directory, args.ratings)
    else:
        asyncio.run(selftest())


if __name__ == "__main__":
    main()
