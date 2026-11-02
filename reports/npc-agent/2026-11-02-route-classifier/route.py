"""Offline route classification benchmark. Run from server/; selftest uses no network."""

from __future__ import annotations

import argparse
import asyncio
import base64
import importlib.util
import json
import math
import os
import re
import sys
import time
import tomllib
from collections import Counter
from pathlib import Path
from tempfile import TemporaryDirectory
from typing import Any
from unittest.mock import patch

import httpx
import numpy as np

from gothic_ai.cli import embed_url
from gothic_ai.embeddings import EmbedClient
from gothic_ai.llm import FakeBackend, FakeStep, LLMBackend, LLMError, Message, make_backend
from gothic_ai.replay import _quick_context
from gothic_ai.trace import item_json, read

HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location(
    "_route_sol_thinks_pilot", HERE.parent / "2026-11-02-sol-thinks/pilot.py"
)
assert SPEC is not None and SPEC.loader is not None
pilot = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = pilot
SPEC.loader.exec_module(pilot)

RULES = re.compile(
    r"\b(?:давай|пош(?:ли|ёл|ел)|пойд|ид(?:ём|ем|и)|веди|впер[её]д|за\s+мной|"
    r"дай|дашь|отдай|верни|возьми|бери|держи|забери|прими|помоги|помоч|выручай|"
    r"спас|защити|прикрой|бей|убей|ударь|атак|мочи|вали|въеб|ебаш|купи|прода|"
    r"обмен|сделк|контракт|договор|услови|плат|золот|деньг|монет|квест|задани|"
    r"награ|опыт|уходи|проваливай)",
    re.IGNORECASE,
)
NON_ACTION = {"recall", "look", "where", "take_offense", "check_claim", "expose_lie"}
KINDS = {"none", "deal", "item", "money", "quest", "go", "follow", "fight", "leave"}
VARIANTS = ("ctx", "phrase")
TARGET_RECALL = 0.95


def write_json(path: Path, value: Any) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n")
    temporary.replace(path)


def collect(traces: list[Path], directory: Path) -> list[dict[str, Any]]:
    turns = []
    seen = set()
    for trace in traces:
        trace = trace.resolve()
        records = read(trace)
        for record in records:
            steps = record.get("steps")
            if (
                record.get("type") != "turn"
                or record.get("trigger", {}).get("kind") != "player"
                or not steps
            ):
                continue
            history_items, _ = _quick_context(record)
            prev_npc = next(
                (
                    pilot.spoken(item.content)
                    for item in reversed(history_items)
                    if isinstance(item, Message) and item.role == "assistant"
                ),
                "",
            )
            history = [
                {**item, "text": pilot.spoken(item["text"])}
                for item in pilot.history_tail({"history": [item_json(item) for item in history_items]})[-6:]
            ]
            phrase, npc = record["trigger"]["text"], record["npc"]
            identity = (npc, phrase, prev_npc)
            if identity in seen:
                continue
            seen.add(identity)
            calls = [call["name"] for step in steps for call in step.get("calls", [])]
            turns.append(
                {
                    "id": record["id"],
                    "trace": str(trace),
                    "t": record["t"],
                    "npc": npc,
                    "phrase": phrase,
                    "calls": calls,
                    "group": "pure" if not calls else "soft" if set(calls) <= pilot.SOFT else "hard",
                    "history": history,
                    "prev_npc": prev_npc,
                    "action": bool(set(calls) - NON_ACTION),
                }
            )
    turns.sort(key=lambda turn: (turn["trace"], turn["t"], turn["id"]))
    if len({turn["id"] for turn in turns}) != len(turns):
        raise ValueError("Turn ids must be unique across traces")
    directory.mkdir(parents=True, exist_ok=True)
    pilot.save_rows(directory / "turns.jsonl", turns)
    for trace in dict.fromkeys(str(trace.resolve()) for trace in traces):
        print(f"Trace {trace}: {sum(turn['trace'] == trace for turn in turns)} turns")
    for npc, count in sorted(Counter(turn["npc"] for turn in turns).items()):
        print(f"NPC {npc}: {count} turns")
    print(f"Collected {len(turns)} turns")
    return turns


async def label(directory: Path, *, effort: str = "low", sol: LLMBackend | None = None) -> None:
    prompt = (HERE / "label.md").read_text(encoding="utf-8")
    results = {r["id"]: r for r in pilot.rows(directory / "labels.jsonl")}
    pending = [
        turn
        for turn in read(directory / "turns.jsonl")
        if turn["id"] not in results or results[turn["id"]]["error"] is not None
    ]
    if not pending:
        return
    sol = sol if sol is not None else make_backend("chatgpt", reasoning_effort=effort)
    try:
        for turn in pending:
            history = "\n".join(
                ("Герой: " if item["who"] == "hero" else "NPC: ") + item["text"] for item in turn["history"]
            )
            sections = {
                "NPC": pilot.NAMES.get(turn["npc"], turn["npc"]),
                "Конец разговора": history,
                "Фраза героя": turn["phrase"],
            }
            content = "\n\n".join(f"{title}\n{body}" for title, body in sections.items())
            started = time.monotonic()
            result = {"id": turn["id"], "raw": "", "result": None, "error": None}
            try:
                measured = await pilot.measure(sol, prompt, [Message("user", content)])
                result.update(
                    raw=measured["text"],
                    total_s=measured["total_s"],
                    first_token_s=measured["first_token_s"],
                )
                if measured["error"] is not None:
                    # Backend exception strings can contain private endpoints or credentials.
                    result["error"] = "Sol request failed"
                else:
                    verdict = pilot.parse_judge(result["raw"])
                    if (
                        type(verdict.get("needs_decision")) is not bool
                        or verdict.get("kind") not in KINDS
                        or not isinstance(verdict.get("note"), str)
                    ):
                        raise ValueError("Invalid route label")
                    result["result"] = verdict
            except Exception as error:
                result["error"] = f"Sol label failed ({type(error).__name__})"
            result.setdefault("total_s", time.monotonic() - started)
            result.setdefault("first_token_s", None)
            results[turn["id"]] = result
            pilot.save_rows(directory / "labels.jsonl", results.values())
            print(f"Labeled {turn['id']}: {result['error'] or 'ok'}")
    finally:
        await sol.aclose()


def gold(directory: Path, overrides: Path | None = None) -> list[dict[str, Any]]:
    corrections = json.loads(overrides.read_text()) if overrides else {}
    if not isinstance(corrections, dict) or any(type(v) is not bool for v in corrections.values()):
        raise ValueError("Overrides must be a JSON object mapping turn ids to booleans")
    labels = {r["id"]: r for r in pilot.rows(directory / "labels.jsonl")}
    result, disputes = [], []
    for turn in read(directory / "turns.jsonl"):
        judged = labels.get(turn["id"], {})
        verdict = (judged.get("result") or {}) if not judged.get("error") else {}
        decision = verdict.get("needs_decision")
        if turn["id"] in corrections:
            value, source = corrections[turn["id"]], "override"
        elif type(decision) is bool and decision == turn["action"]:
            value, source = decision, "agree"
        else:
            value, source = None, "disputed"
        result.append({"id": turn["id"], "label": value, "source": source, "kind": verdict.get("kind")})
        if source == "disputed":
            dispute = {key: turn[key] for key in ("id", "npc", "prev_npc", "phrase", "calls")}
            dispute.update({key: verdict.get(key) for key in ("needs_decision", "kind", "note")})
            disputes.append(dispute)
            print(f"Disputed {turn['id']} {turn['npc']}: {turn['phrase']} ({decision}; {turn['calls']})")
    pilot.save_rows(directory / "gold.jsonl", result)
    write_json(directory / "disputes.json", disputes)
    print(f"Gold: {len(result) - len(disputes)}/{len(result)} labeled, {len(disputes)} disputed")
    return result


def classifier_text(turn: dict[str, Any], variant: str) -> str:
    prefix = f"NPC: {turn['prev_npc']}\n" if variant == "ctx" and turn["prev_npc"] else ""
    return prefix + f"Герой: {turn['phrase']}"


def gemma_score(choice: dict[str, Any]) -> tuple[float, str]:
    content = (choice.get("logprobs") or {}).get("content") or []
    candidates = content[0].get("top_logprobs", []) if content else []
    yes, no = [], []
    for candidate in candidates:
        token = candidate["token"].strip().lower()
        if token.startswith(("да", "нет")) and math.isfinite(candidate["logprob"]):
            (yes if token.startswith("да") else no).append(candidate["logprob"])
    if yes or no:
        largest = max(yes + no)
        yes_p = sum(math.exp(p - largest) for p in yes)
        no_p = sum(math.exp(p - largest) for p in no)
        return yes_p / (yes_p + no_p), "logprobs"
    text = choice["message"]["content"].strip().lower()
    if text.startswith(("да", "нет")):
        return float(text.startswith("да")), "text"
    raise ValueError("Gemma returned neither yes/no probabilities nor yes/no text")


async def gemma(
    directory: Path,
    *,
    variant: str = "ctx",
    slot: int = 1,
    quick_at: str | None = None,
    transport: httpx.AsyncBaseTransport | None = None,
) -> None:
    prompt = (HERE / "classify.md").read_text(encoding="utf-8")
    path = directory / f"scores_gemma_{variant}.jsonl"
    results = {r["id"]: r for r in pilot.rows(path)}
    pending = [
        turn
        for turn in read(directory / "turns.jsonl")
        if turn["id"] not in results or results[turn["id"]]["error"] is not None
    ]
    if not pending:
        return
    url = pilot.gemma_url(quick_at).rstrip("/") + "/chat/completions"
    async with httpx.AsyncClient(transport=transport, timeout=60) as client:
        for turn in pending:
            result = {
                "id": turn["id"],
                "score": None,
                "score_source": None,
                "text": "",
                "error": None,
            }
            started = time.monotonic()
            try:
                response = await client.post(
                    url,
                    json={
                        "messages": [
                            {"role": "system", "content": prompt},
                            {"role": "user", "content": classifier_text(turn, variant)},
                        ],
                        "max_tokens": 1,
                        "temperature": 0,
                        "logprobs": True,
                        "top_logprobs": 10,
                        "id_slot": slot,
                        "cache_prompt": True,
                    },
                )
                response.raise_for_status()
                choice = response.json()["choices"][0]
                result["text"] = choice["message"]["content"]
                result["score"], result["score_source"] = gemma_score(choice)
            except httpx.HTTPStatusError as error:
                result["error"] = f"Gemma returned HTTP {error.response.status_code}"
            except Exception as error:
                result["error"] = f"Gemma request failed ({type(error).__name__})"
            result["latency_s"] = time.monotonic() - started
            results[turn["id"]] = result
            pilot.save_rows(path, results.values())
            print(f"Gemma {variant} {turn['id']}: {result['error'] or result['score']}")


def frida_url(explicit: str | None) -> str:
    if explicit:
        return explicit
    settings_path = Path.home() / ".config/gothic-ai/settings.toml"
    settings = (
        tomllib.loads(settings_path.read_text(encoding="utf-8")).get("llm", {})
        if settings_path.is_file()
        else {}
    )
    url = embed_url(
        explicit or os.environ.get("GOTHIC_EMBED_URL") or settings.get("embed_url"),
        os.environ.get("GOTHIC_DECIDER_URL") or settings.get("decider_url"),
    )
    if not url:
        raise SystemExit("Set --embed-url, GOTHIC_EMBED_URL/GOTHIC_DECIDER_URL or [llm] in settings.toml")
    return url


def load_vectors(path: Path) -> dict[str, np.ndarray]:
    if not path.exists():
        return {}
    with np.load(path, allow_pickle=False) as archive:
        return {key: archive[key] for key in archive.files}


def save_vectors(path: Path, vectors: dict[str, np.ndarray]) -> None:
    temporary = path.with_suffix(".tmp.npz")
    np.savez_compressed(temporary, **vectors)
    temporary.replace(path)


async def frida(
    directory: Path,
    *,
    variant: str = "ctx",
    embed_at: str | None = None,
    transport: httpx.AsyncBaseTransport | None = None,
) -> None:
    turns = read(directory / "turns.jsonl")
    path = directory / f"frida_{variant}.npz"
    vectors = load_vectors(path)
    pending = [turn for turn in turns if turn["id"] not in vectors]
    latency_path = directory / "frida_latency.json"
    timings = json.loads(latency_path.read_text()) if latency_path.exists() else {}
    samples = {sample["index"]: sample for sample in timings.get(variant, [])}
    pending_samples = [i for i in range(20) if i not in samples or samples[i]["error"] is not None]
    if not pending and (not turns or not pending_samples):
        return
    client = EmbedClient(frida_url(embed_at), transport=transport)
    try:
        for offset in range(0, len(pending), 64):
            batch = pending[offset : offset + 64]
            try:
                embedded = await client.embed([classifier_text(t, variant) for t in batch], kind="query")
                for turn, vector in zip(batch, embedded, strict=True):
                    vectors[turn["id"]] = vector
                save_vectors(path, vectors)
                print(f"FRIDA {variant}: {len(vectors)}/{len(turns)} vectors")
            except Exception as error:
                for turn in batch:
                    print(
                        json.dumps(
                            {
                                "id": turn["id"],
                                "error": f"FRIDA request failed ({type(error).__name__})",
                            }
                        ),
                        file=sys.stderr,
                    )
        for index in pending_samples if turns else []:
            started = time.monotonic()
            sample = {"index": index, "id": turns[0]["id"], "error": None}
            try:
                await client.embed([classifier_text(turns[0], variant)], kind="query")
            except Exception as error:
                sample["error"] = f"FRIDA latency request failed ({type(error).__name__})"
            sample["latency_s"] = time.monotonic() - started
            samples[index] = sample
            timings[variant] = [samples[i] for i in sorted(samples)]
            write_json(latency_path, timings)
            print(f"FRIDA {variant} latency {index + 1}/20: {sample['error'] or 'ok'}")
    finally:
        await client.close()


def threshold(scores: np.ndarray, labels: np.ndarray) -> float:
    positive = np.sort(scores[labels])
    if not len(positive):
        raise ValueError("No positive training labels for threshold selection")
    index = len(positive) - math.ceil(TARGET_RECALL * len(positive))
    return float(positive[index])


def sigmoid(values: np.ndarray) -> np.ndarray:
    return 1 / (1 + np.exp(-np.clip(values, -40, 40)))


def logistic_scores(
    train_x: np.ndarray,
    train_y: np.ndarray,
    test_x: np.ndarray,
) -> tuple[np.ndarray, np.ndarray]:
    """Standardize on training data; minimize mean log loss + ||weights||² / (2*n)."""
    mean, scale = train_x.mean(axis=0), train_x.std(axis=0)
    scale[scale < 1e-12] = 1
    train_x, test_x = (train_x - mean) / scale, (test_x - mean) / scale
    # The dual coefficients keep iterations small even for wide FRIDA vectors.
    n = len(train_y)
    gram = train_x @ train_x.T
    rate = 1 / (0.25 * float(np.linalg.eigvalsh(gram + 1)[-1]) / n + 1 / n)
    coefficients = np.zeros(n)
    intercept = 0.0
    for _ in range(2000):
        residual = (sigmoid(gram @ coefficients + intercept) - train_y) / n
        change = rate * (residual + coefficients / n)
        bias_change = rate * float(residual.sum())
        coefficients -= change
        intercept -= bias_change
        if max(float(np.max(np.abs(change))), abs(bias_change)) < 1e-8:
            break
    return (
        sigmoid(gram @ coefficients + intercept),
        sigmoid((test_x @ train_x.T) @ coefficients + intercept),
    )


def roc_auc(labels: np.ndarray, scores: np.ndarray) -> float | None:
    positive, negative = scores[labels], scores[~labels]
    if not len(positive) or not len(negative):
        return None
    differences = positive[:, None] - negative
    return float(np.mean((differences > 0) + 0.5 * (differences == 0)))


def metrics(turns: list[dict[str, Any]], predictions: list[dict[str, Any]]) -> dict[str, Any]:
    by_id = {turn["id"]: turn for turn in turns}
    chosen = [by_id[p["id"]] for p in predictions]
    labels = np.array([turn["label"] for turn in chosen], dtype=bool)
    predicted = np.array([p["predicted"] for p in predictions], dtype=bool)
    scores = np.array([p["score"] for p in predictions])
    tp = int((labels & predicted).sum())
    tn = int((~labels & ~predicted).sum())
    kinds = sorted({turn["kind"] for turn in chosen if turn["label"] and turn["kind"] is not None})
    return {
        "n": len(chosen),
        "missing_n": len(turns) - len(chosen),
        "true_n": int(labels.sum()),
        "false_n": int((~labels).sum()),
        "true_positive_n": tp,
        "true_negative_n": tn,
        "recall": pilot.fraction(tp, int(labels.sum())),
        "false_left_fraction": pilot.fraction(tn, int((~labels).sum())),
        "precision": pilot.fraction(tp, int(predicted.sum())),
        "roc_auc": roc_auc(labels, scores),
        "recall_by_kind": {
            kind: {
                "n": sum(t["label"] and t["kind"] == kind for t in chosen),
                "recall": pilot.fraction(
                    sum(
                        t["label"] and t["kind"] == kind and p["predicted"]
                        for t, p in zip(chosen, predictions, strict=True)
                    ),
                    sum(t["label"] and t["kind"] == kind for t in chosen),
                ),
            }
            for kind in kinds
        },
        "misses": [
            {key: turn[key] for key in ("id", "npc", "prev_npc", "phrase", "kind")}
            for turn, prediction in zip(chosen, predictions, strict=True)
            if turn["label"] and not prediction["predicted"]
        ],
        "missing_ids": [turn["id"] for turn in turns if turn["id"] not in {p["id"] for p in predictions}],
    }


def evaluate(directory: Path, *, emit: bool = True) -> dict[str, Any]:
    labels = {r["id"]: r for r in read(directory / "gold.jsonl") if r["label"] is not None}
    turns = [turn | labels[turn["id"]] for turn in read(directory / "turns.jsonl") if turn["id"] in labels]
    traces = sorted({turn["trace"] for turn in turns})
    if len(traces) < 2:
        raise ValueError("Evaluation needs labeled turns from at least two traces")
    score_rows = {variant: pilot.rows(directory / f"scores_gemma_{variant}.jsonl") for variant in VARIANTS}
    inputs = {
        "rules": {t["id"]: float(bool(RULES.search(t["phrase"]))) for t in turns},
        **{
            f"gemma_{variant}": {
                r["id"]: r["score"]
                for r in score_rows[variant]
                if r["error"] is None and r["score"] is not None
            }
            for variant in VARIANTS
        },
        **{f"frida_{v}": load_vectors(directory / f"frida_{v}.npz") for v in VARIANTS},
    }
    methods = [*inputs, "or_gemma_frida"]
    predictions = {method: [] for method in methods}
    folds = {method: [] for method in methods}
    for trace in traces:
        fold_predictions = {}
        for method, values in inputs.items():
            training = [t for t in turns if t["trace"] != trace and t["id"] in values]
            testing = [t for t in turns if t["trace"] == trace and t["id"] in values]
            fold = {
                "trace": trace,
                "train_traces": sorted({t["trace"] for t in training}),
                "train_n": len(training),
                "test_n": len(testing),
                "threshold": None,
                "error": None,
            }
            folds[method].append(fold)
            if not testing:
                fold["error"] = "No test scores"
                continue
            try:
                train_y = np.array([t["label"] for t in training], dtype=bool)
                train_x = np.array([values[t["id"]] for t in training], dtype=float)
                test_x = np.array([values[t["id"]] for t in testing], dtype=float)
                if method.startswith("frida_"):
                    if len(fold["train_traces"]) < 2:
                        raise ValueError("FRIDA threshold needs at least two training traces")
                    train_traces = np.array([t["trace"] for t in training])
                    train_scores = np.empty(len(training))
                    for inner_trace in fold["train_traces"]:
                        held_out = train_traces == inner_trace
                        _, train_scores[held_out] = logistic_scores(
                            train_x[~held_out],
                            train_y[~held_out],
                            train_x[held_out],
                        )
                    cutoff = threshold(train_scores, train_y)
                    _, test_scores = logistic_scores(train_x, train_y, test_x)
                else:
                    train_scores, test_scores = train_x, test_x
                    cutoff = 0.5 if method == "rules" else threshold(train_scores, train_y)
                if method != "rules":
                    fold["threshold"] = cutoff
                    fold["training_recall"] = float(np.mean(train_scores[train_y] >= cutoff))
                predicted = [
                    {
                        "id": turn["id"],
                        "trace": trace,
                        "score": float(score),
                        "threshold": None if method == "rules" else cutoff,
                        "predicted": bool(score >= cutoff),
                    }
                    for turn, score in zip(testing, test_scores, strict=True)
                ]
                predictions[method].extend(predicted)
                fold_predictions[method] = {p["id"]: p for p in predicted}
            except ValueError as error:
                fold["error"] = str(error)
        combined = []
        for turn in (t for t in turns if t["trace"] == trace):
            identifier = turn["id"]
            left = fold_predictions.get("gemma_ctx", {}).get(identifier)
            right = fold_predictions.get("frida_ctx", {}).get(identifier)
            if left is not None and right is not None:
                decision = left["predicted"] or right["predicted"]
                combined.append(
                    {
                        "id": identifier,
                        "trace": trace,
                        "score": float(decision),
                        "threshold": None,
                        "predicted": decision,
                    }
                )
        predictions["or_gemma_frida"].extend(combined)
        folds["or_gemma_frida"].append(
            {
                "trace": trace,
                "test_n": len(combined),
                "thresholds": {m: folds[m][-1]["threshold"] for m in ("gemma_ctx", "frida_ctx")},
            }
        )
    latency_path = directory / "frida_latency.json"
    frida_timings = json.loads(latency_path.read_text()) if latency_path.exists() else {}
    result = {
        "n": len(turns),
        "traces": traces,
        "target_recall": TARGET_RECALL,
        "classifiers": {
            method: metrics(turns, predictions[method])
            | {
                "folds": folds[method],
                "predictions": predictions[method],
            }
            for method in methods
        },
        "latency_s": {
            **{
                f"gemma_{v}": pilot.distribution(
                    r["latency_s"] for r in score_rows[v][1:] if r["error"] is None
                )
                for v in VARIANTS
            },
            **{
                f"frida_{v}": pilot.distribution(
                    r["latency_s"] for r in frida_timings.get(v, []) if r["error"] is None
                )
                for v in VARIANTS
            },
        },
    }
    write_json(directory / "eval.json", result)
    if emit:
        print("Classifier           n    recall  false left  precision  ROC AUC")
        for method, values in result["classifiers"].items():
            numbers = [
                "-" if values[k] is None else f"{values[k]:.3f}"
                for k in ("recall", "false_left_fraction", "precision", "roc_auc")
            ]
            print(f"{method:20} {values['n']:4} " + "  ".join(f"{v:>9}" for v in numbers))
            if values["missing_n"]:
                print(f"  Missing scores: {values['missing_n']}/{len(turns)}")
        for method, timing in result["latency_s"].items():
            print(f"{method} latency_s: n={timing['n']} median={timing['median']} p90={timing['p90']}")
    return result


async def selftest() -> None:
    with TemporaryDirectory(prefix="route-classifier-") as temporary:
        root = Path(temporary)
        traces = []
        npcs = [*pilot.NAMES, "OTHER_NPC"]
        for trace_index in range(3):
            trace = root / f"trace-{trace_index}.jsonl"
            traces.append(trace)
            records = [{"type": "instructions", "hash": "persona", "text": "Recorded instructions."}]
            phrases = ["Привет", "Дай меч", "Да, согласен", "Зачем ты здесь?", "Ты глупец"]
            for index, phrase in enumerate(phrases):
                phrase = f"{phrase} ({trace_index})"
                history = [
                    Message(role, f"[neutral] Earlier {i}")
                    for i in range(5)
                    for role in ("user", "assistant")
                ]
                history.append(Message("assistant", f"[friendly] Заключим сделку {trace_index}-{index}?"))
                if (trace_index, index) == (0, 0):
                    history = []
                if (trace_index, index) == (2, 0):
                    history += [Message("user", f"Earlier hero {i}") for i in range(9)]
                calls = [
                    [],
                    ["look", "give_item"],
                    ["trade"],
                    ["look", "recall"],
                    ["take_offense", "check_claim", "expose_lie"],
                ][index]
                records.append(
                    {
                        "type": "turn",
                        "id": f"turn-{trace_index}-{index}",
                        "npc": npcs[trace_index],
                        "t": index,
                        "instructions": "persona",
                        "trigger": {"kind": "player", "text": phrase},
                        "steps": [
                            {
                                "first_token_s": 0.5,
                                "calls": [],
                                "added": [
                                    item_json(item)
                                    for item in [
                                        *history,
                                        Message("developer", "Recorded situation."),
                                        Message("user", phrase),
                                    ]
                                ],
                            },
                            {"calls": [{"name": name} for name in calls]},
                        ],
                    }
                )
            records[2]["steps"][0]["llm"] = "gemma-4-12b"
            records[3]["steps"][0]["first_token_s"] = 0.1
            records[4]["instructions"] = "missing"
            original = records[1]
            # Repeated requests with different history lengths still deduplicate by the last NPC line.
            duplicate = json.loads(json.dumps(records[2]))
            duplicate["id"] = f"duplicate-{trace_index}"
            duplicate["steps"][0]["added"].insert(0, item_json(Message("user", "Old extra line")))
            records.append(duplicate)
            for index, changes in enumerate(
                (
                    {"trigger": {"kind": "event"}},
                    {"steps": []},
                    {"type": "event"},
                )
            ):
                records.append(original | changes | {"id": f"filtered-{trace_index}-{index}"})
            pilot.save_rows(trace, records)
        directory = root / "data"
        turns = collect(traces, directory)
        assert len(turns) == 15 and turns == collect(traces, directory)
        assert {"turn-0-1", "turn-0-2", "turn-0-3"} <= {turn["id"] for turn in turns}
        assert {t["group"] for t in turns} == {"pure", "soft", "hard"}
        assert {t["npc"] for t in turns} == set(npcs)
        assert all(len(t["history"]) <= 6 and "[" not in t["prev_npc"] for t in turns)
        assert turns[0]["prev_npc"] == "" and turns[0]["history"] == []
        assert turns[10]["prev_npc"] == "Заключим сделку 2-0?"
        assert all(t["action"] == (t["id"][-1] in "12") for t in turns)
        assert turns[1]["calls"] == ["look", "give_item"]
        judgments = []
        for turn in turns:
            decision = turn["action"]
            if turn["id"] in ("turn-0-1", "turn-1-0"):
                decision = not decision
            verdict = {
                "needs_decision": decision,
                "kind": "item"
                if turn["id"].endswith("1")
                else "deal"
                if turn["id"].endswith("2")
                else "none",
                "note": "",
            }
            judgments.append(FakeStep("```json\n" + json.dumps(verdict) + "\n```"))
        valid_last = judgments[-1]
        judgments[-1] = FakeStep("invalid JSON")
        sol = FakeBackend(script=judgments, delay=0)
        with patch(__name__ + ".make_backend", return_value=sol) as factory:
            await label(directory, effort="low")
            factory.assert_called_once_with("chatgpt", reasoning_effort="low")
        assert len(sol.calls) == 15 and pilot.rows(directory / "labels.jsonl")[-1]["error"]
        assert sol.calls[0][0] == (HERE / "label.md").read_text()
        assert "NPC\nКавалорн" in sol.calls[0][1][0].content
        assert "NPC\nOTHER_NPC" in sol.calls[10][1][0].content
        assert "Конец разговора\n" in sol.calls[1][1][0].content
        assert "Фраза героя\nДай меч (0)" in sol.calls[1][1][0].content
        assert all(not tools for _, _, tools in sol.calls)

        async def failed_stream(**kwargs):
            raise LLMError("Synthetic model failure")
            yield  # Make this an async generator, like the backend interface.

        broken = FakeBackend(delay=0)
        broken.stream = failed_stream
        await label(directory, sol=broken)
        assert pilot.rows(directory / "labels.jsonl")[-1]["error"] == "Sol request failed"
        recovery = FakeBackend(script=[valid_last], delay=0)
        await label(directory, sol=recovery)
        await label(directory, sol=recovery)
        assert len(recovery.calls) == 1 and len(pilot.rows(directory / "labels.jsonl")) == 15
        initial_gold = gold(directory)
        assert sum(t["source"] == "disputed" for t in initial_gold) == 2
        disputes = json.loads((directory / "disputes.json").read_text())
        assert {t["id"] for t in disputes} == {"turn-0-1", "turn-1-0"}
        overrides = root / "overrides.json"
        write_json(overrides, {"turn-0-1": True, "turn-0-0": False})
        final_gold = gold(directory, overrides)
        assert [t["source"] for t in final_gold[:2]] == ["override", "override"]
        assert final_gold[1]["kind"] == "item" and final_gold[5]["label"] is None
        assert len(json.loads((directory / "disputes.json").read_text())) == 1

        lookup = {t["phrase"]: t for t in turns}
        scores = {t["id"]: (0.8 if t["action"] else 0.2) for t in turns}
        scores.update(
            {
                "turn-0-1": 1.0,
                "turn-0-2": 0.7,
                "turn-1-1": 0.9,
                "turn-1-2": 0.05,
                "turn-2-1": 0.6,
                "turn-0-4": 0.0,
            }
        )
        gemma_requests = []
        gemma_failed = False

        def gemma_response(request: httpx.Request) -> httpx.Response:
            nonlocal gemma_failed
            body = json.loads(request.content)
            gemma_requests.append(body)
            assert request.url.path == "/v1/chat/completions"
            assert (
                body
                | {
                    "max_tokens": 1,
                    "temperature": 0,
                    "logprobs": True,
                    "top_logprobs": 10,
                    "id_slot": 1,
                    "cache_prompt": True,
                }
                == body
            )
            assert body["messages"][0] == {"role": "system", "content": (HERE / "classify.md").read_text()}
            text = body["messages"][1]["content"]
            turn = lookup[text.rsplit("Герой: ", 1)[1]]
            if turn["id"] == "turn-2-4" and not gemma_failed:
                gemma_failed = True
                return httpx.Response(503)
            score = scores[turn["id"]]
            if turn["id"] in ("turn-0-1", "turn-0-4"):
                choice = {"message": {"content": " ДА" if score else " нЕТ"}}
            else:
                choice = {
                    "message": {"content": "нет"},
                    "logprobs": {
                        "content": [
                            {
                                "top_logprobs": [
                                    {"token": " ДА", "logprob": math.log(score * 0.2)},
                                    {"token": "давай", "logprob": math.log(score * 0.4)},
                                    {"token": " Нет", "logprob": math.log((1 - score) * 0.6)},
                                    {"token": "other", "logprob": math.log(0.4)},
                                ]
                            },
                            {"top_logprobs": [{"token": "да", "logprob": 0}]},
                        ]
                    },
                }
            return httpx.Response(200, json={"choices": [choice]})

        for variant in VARIANTS:
            transport = httpx.MockTransport(gemma_response)
            await gemma(directory, variant=variant, quick_at="https://gemma.invalid/v1", transport=transport)
            if variant == "ctx":
                assert (
                    pilot.rows(directory / "scores_gemma_ctx.jsonl")[-1]["error"] == "Gemma returned HTTP 503"
                )
            await gemma(directory, variant=variant, quick_at="https://gemma.invalid/v1", transport=transport)
            request_count = len(gemma_requests)
            await gemma(directory, variant=variant, quick_at="https://gemma.invalid/v1", transport=transport)
            assert len(gemma_requests) == request_count
            saved = pilot.rows(directory / f"scores_gemma_{variant}.jsonl")
            assert len(saved) == 15 and all(r["error"] is None for r in saved)
            assert all(abs(r["score"] - scores[r["id"]]) < 1e-12 for r in saved)
            assert saved[1]["score_source"] == saved[4]["score_source"] == "text"
            for index, row in enumerate(saved):
                row["latency_s"] = 999 if index == 0 else index / 100
            pilot.save_rows(directory / f"scores_gemma_{variant}.jsonl", saved)
        assert len(gemma_requests) == 31
        assert gemma_requests[0]["messages"][1]["content"] == "Герой: Привет (0)"
        assert gemma_requests[1]["messages"][1]["content"].startswith("NPC: Заключим сделку")
        assert all("NPC:" not in r["messages"][1]["content"] for r in gemma_requests[16:])

        embed_requests = []
        embed_failed = False

        def vector_for(turn):
            x = 1 if turn["action"] and turn["id"] != "turn-1-2" else -1
            return np.array([x, 7, int(turn["id"].split("-")[1])], dtype="<f4")

        def embed_response(request: httpx.Request) -> httpx.Response:
            nonlocal embed_failed
            body = json.loads(request.content)
            embed_requests.append(body)
            assert request.url.path == "/embed" and body["kind"] == "query"
            if not embed_failed:
                embed_failed = True
                return httpx.Response(503)
            vectors = np.array([vector_for(lookup[text.rsplit("Герой: ", 1)[1]]) for text in body["texts"]])
            return httpx.Response(
                200,
                json={
                    "model": "fake-frida",
                    "dim": 3,
                    "vectors": base64.b64encode(vectors.tobytes()).decode(),
                },
            )

        transport = httpx.MockTransport(embed_response)
        await frida(directory, embed_at="https://frida.invalid", transport=transport)
        assert not load_vectors(directory / "frida_ctx.npz")
        assert all(s["error"] for s in json.loads((directory / "frida_latency.json").read_text())["ctx"])
        await frida(directory, embed_at="https://frida.invalid", transport=transport)
        assert len(embed_requests) == 22
        partial = load_vectors(directory / "frida_ctx.npz")
        del partial[turns[-1]["id"]]
        save_vectors(directory / "frida_ctx.npz", partial)
        await frida(directory, embed_at="https://frida.invalid", transport=transport)
        assert len(embed_requests) == 23 and len(embed_requests[-1]["texts"]) == 1
        save_vectors(directory / "frida_phrase.npz", {turns[0]["id"]: vector_for(turns[0])})
        await frida(directory, variant="phrase", embed_at="https://frida.invalid", transport=transport)
        assert len(embed_requests) == 44 and len(embed_requests[23]["texts"]) == 14
        for variant in VARIANTS:
            await frida(directory, variant=variant, embed_at="https://frida.invalid", transport=transport)
            assert len(load_vectors(directory / f"frida_{variant}.npz")) == 15
        assert len(embed_requests) == 44
        timings = json.loads((directory / "frida_latency.json").read_text())
        for samples in timings.values():
            assert len(samples) == 20 and all(s["error"] is None for s in samples)
            for sample in samples:
                sample["latency_s"] = (sample["index"] + 1) / 1000
        write_json(directory / "frida_latency.json", timings)

        with patch(__name__ + ".logistic_scores", wraps=logistic_scores) as fit:
            report = evaluate(directory)
        assert fit.call_count == 18
        for call in fit.call_args_list:
            train_x, _, test_x = call.args
            assert set(train_x[:, 2]).isdisjoint(test_x[:, 2])
        assert report["n"] == 14 and len(report["traces"]) == 3
        classifiers = report["classifiers"]
        assert len(classifiers) == 6
        assert classifiers["rules"]["recall"] == 0.5
        assert classifiers["rules"]["false_left_fraction"] == 1
        assert classifiers["rules"]["precision"] == 1
        assert {m["id"] for m in classifiers["rules"]["misses"]} == {f"turn-{i}-2" for i in range(3)}
        assert classifiers["rules"]["recall_by_kind"]["deal"] == {"n": 3, "recall": 0}
        for variant in VARIANTS:
            values = classifiers[f"gemma_{variant}"]
            assert np.allclose([f["threshold"] for f in values["folds"]], [0.05, 0.6, 0.05])
            assert values["recall"] == 5 / 6 and [m["id"] for m in values["misses"]] == ["turn-1-2"]
            assert values["roc_auc"] == 41 / 48
            assert report["latency_s"][f"gemma_{variant}"] == pilot.distribution(
                i / 100 for i in range(1, 15)
            )
            assert report["latency_s"][f"frida_{variant}"] == pilot.distribution(
                i / 1000 for i in range(1, 21)
            )
        for method, values in classifiers.items():
            assert values["n"] == 14 and values["missing_n"] == 0 and len(values["folds"]) == 3
            assert {p["id"] for p in values["predictions"]} == {
                t["id"] for t in final_gold if t["label"] is not None
            }
            if method.startswith(("frida_", "gemma_")):
                assert all(
                    f["trace"] not in f["train_traces"] and f["training_recall"] >= 0.95
                    for f in values["folds"]
                )
        left = {p["id"]: p for p in classifiers["gemma_ctx"]["predictions"]}
        right = {p["id"]: p for p in classifiers["frida_ctx"]["predictions"]}
        assert all(
            p["predicted"] == (left[p["id"]]["predicted"] or right[p["id"]]["predicted"])
            for p in classifiers["or_gemma_frida"]["predictions"]
        )
        assert roc_auc(np.array([True, False]), np.array([0.5, 0.5])) == 0.5
        assert roc_auc(np.array([True]), np.array([0.5])) is None
        positive_labels = np.array([True] * 20 + [False])
        assert threshold(np.arange(21), positive_labels) == 1
        # Withholding either scoring service must remain visible in evaluation coverage.
        (directory / "scores_gemma_phrase.jsonl").unlink()
        incomplete = evaluate(directory, emit=False)["classifiers"]["gemma_phrase"]
        assert incomplete["missing_n"] == 14 and incomplete["n"] == 0 and incomplete["roc_auc"] is None

        # Unique features let the model memorize labels without generalizing to an unseen trace.
        overfit = root / "overfit"
        overfit.mkdir()
        vectors = np.eye(9)
        labels = np.tile([True, True, False], 3)
        overfit_turns = [turns[0] | {"id": f"unique-{i}", "trace": str(traces[i // 3])} for i in range(9)]
        pilot.save_rows(overfit / "turns.jsonl", overfit_turns)
        pilot.save_rows(
            overfit / "gold.jsonl",
            (
                {"id": turn["id"], "label": bool(value), "kind": "item" if value else "none"}
                for turn, value in zip(overfit_turns, labels, strict=True)
            ),
        )
        for variant in VARIANTS:
            save_vectors(
                overfit / f"frida_{variant}.npz",
                {turn["id"]: vector for turn, vector in zip(overfit_turns, vectors, strict=True)},
            )
        overfit_report = evaluate(overfit, emit=False)
        fitted, outer_scores = logistic_scores(vectors[:6], labels[:6], vectors[6:])
        _, inner_scores = logistic_scores(vectors[:3], labels[:3], vectors[3:6])
        inner_cutoff = threshold(inner_scores, labels[3:6])
        fitted_cutoff = threshold(fitted, labels[:6])
        assert inner_cutoff + 1e-3 < outer_scores.min()
        assert outer_scores.max() + 1e-3 < fitted_cutoff
        for variant in VARIANTS:
            values = overfit_report["classifiers"][f"frida_{variant}"]
            assert values["n"] == 9
            assert all(
                np.isclose(f["threshold"], inner_cutoff) and f["training_recall"] == 1
                for f in values["folds"]
            )
            assert np.allclose([p["score"] for p in values["predictions"]], np.tile(outer_scores, 3))
            assert all(
                np.isclose(p["threshold"], inner_cutoff) and p["predicted"] for p in values["predictions"]
            )

        pilot.save_rows(overfit / "turns.jsonl", overfit_turns[:6])
        impossible = evaluate(overfit, emit=False)
        for variant in VARIANTS:
            values = impossible["classifiers"][f"frida_{variant}"]
            assert values["n"] == 0 and values["missing_n"] == 6
            assert all(
                f["error"] == "FRIDA threshold needs at least two training traces" for f in values["folds"]
            )
        print(
            "Selftest passed: 15 turns, 3 traces, 6 classifiers; full pipeline, resume, errors, "
            "disputes/overrides, logprobs/text, nested fold thresholds, overfitting, AUC, misses, latency."
        )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_subparsers(dest="mode", required=True)
    command = modes.add_parser("collect")
    command.add_argument("traces", type=Path, nargs="+")
    command.add_argument("--out", type=Path, required=True)
    command = modes.add_parser("label")
    command.add_argument("directory", type=Path)
    command.add_argument("--effort", default="low")
    command = modes.add_parser("gold")
    command.add_argument("directory", type=Path)
    command.add_argument("--overrides", type=Path)
    for mode in ("gemma", "frida"):
        command = modes.add_parser(mode)
        command.add_argument("directory", type=Path)
        command.add_argument("--variant", choices=VARIANTS, default="ctx")
        if mode == "gemma":
            command.add_argument("--slot", type=int, default=1)
            command.add_argument("--quick-url")
        else:
            command.add_argument("--embed-url")
    command = modes.add_parser("eval")
    command.add_argument("directory", type=Path)
    modes.add_parser("selftest")
    args = parser.parse_args()
    if args.mode == "collect":
        collect(args.traces, args.out)
    elif args.mode == "label":
        asyncio.run(label(args.directory, effort=args.effort))
    elif args.mode == "gold":
        gold(args.directory, args.overrides)
    elif args.mode == "gemma":
        asyncio.run(gemma(args.directory, variant=args.variant, slot=args.slot, quick_at=args.quick_url))
    elif args.mode == "frida":
        asyncio.run(frida(args.directory, variant=args.variant, embed_at=args.embed_url))
    elif args.mode == "eval":
        evaluate(args.directory)
    else:
        asyncio.run(selftest())


if __name__ == "__main__":
    main()
