"""Offline Sol gist / Gemma voice stand. Run from server/; selftest needs no network."""

from __future__ import annotations

import argparse
import asyncio
import json
import re
import statistics
import subprocess
import sys
import time
from collections import Counter
from copy import deepcopy
from dataclasses import replace
from pathlib import Path
from tempfile import TemporaryDirectory
from typing import Any
from unittest.mock import patch

from gothic_ai.dialogue import (
    REACTION_S,
    first_words,
    first_words_outcome,
    is_pure_sound,
)
from gothic_ai.llm import Message

HERE = Path(__file__).resolve().parent
# Reuse phase 2's imports, including the unmodified phase 1 stand and pilot.
sys.path.insert(0, str(HERE.parent.parent / "npc-memory/2026-11-05-shared-memory-phase2"))
import mind2  # noqa: E402

stand, pilot = mind2.stand, mind2.pilot
RESEARCH = stand.RESEARCH
P1 = RESEARCH / "2026-11-04-shared-memory/full-2026-11-05"
STALL = RESEARCH / "2026-11-05-shared-memory-phase2/gist_stall-2026-11-05"
WORDS = re.compile(r"[а-яёa-z]+", re.IGNORECASE)


def prompt(name: str) -> str:
    return (HERE / name).read_text(encoding="utf-8")


def thinking(ctx: Any) -> Any:
    return replace(ctx, situation=ctx.situation + "\n\n" + prompt("gist.md"))


def first_messages(ctx: Any, phrase: str, previous: list[dict]) -> list[Message]:
    recent = [r["said"] for r in previous if r["said"]][-4:]
    hint = prompt("first_words.md").format(
        recent=prompt("first_words_recent.md").format(said=", ".join(f"«{s}»" for s in recent))
        if recent
        else ""
    )
    return [*ctx.history, Message("developer", ctx.brief + "\n" + hint), Message("user", phrase)]


def voice_messages(ctx: Any, phrase: str, gist: str, said: str) -> tuple[list, list]:
    head = [*ctx.history, Message("developer", ctx.brief)]
    tail = prompt("voice.md").format(gist=gist)
    if said:
        tail += prompt("voice_said.md").format(said=said)
    return [*head, Message("user", phrase), Message("developer", tail)], head


def without_said(text: str, said: str) -> str:
    """The voice as it would sound: a repeat of the first words at its start is cut, as the game would
    cut it before TTS. The mood tag stays."""
    match = re.match(r"\s*(\[[^\]]*\])?\s*", text)
    tag, body = (match.group(1) or ""), text[match.end() :]
    said_words = WORDS.findall(said.lower())
    body_words = list(WORDS.finditer(body))
    if not said_words or [m.group().lower() for m in body_words[: len(said_words)]] != said_words:
        return text
    rest = body[body_words[len(said_words) - 1].end() :].lstrip(' .,!?…—-–:;»"')
    return f"{tag} {rest}".strip() if tag else rest


def filter_first(result: dict, previous: list[dict]) -> dict:
    if result.get("first_sentence_s") is None and result["text"].strip() and not result.get("error"):
        result = {**result, "first_sentence_s": result["total_s"]}
    words, _ = first_words(result["text"])
    last = next((r["said"] for r in reversed(previous) if r["said"]), "")
    outcome = first_words_outcome(words, last)
    elapsed = result.get("first_sentence_s")
    if result.get("error"):
        outcome = "error"
    elif words and (elapsed is None or elapsed > REACTION_S):
        outcome = "late"
    elif (
        outcome == "said"
        and is_pure_sound(words)
        and any(r["said"] and is_pure_sound(r["said"]) for r in previous[-4:])
    ):
        outcome = "sound"
    return {**result, "words": words, "outcome": outcome, "said": words if outcome == "said" else ""}


def turn_context(world: Any, turn: dict) -> Any:
    talk = replace(world.talk(turn["conv"]), instructions=turn["instructions"])
    upto = world.journal.end_seq(talk.branch, turn["t"])
    _, before = world.journal.history(talk.branch, talk.npc, upto)
    # Keep the recorded input, rather than reconstructing a history from the journal.
    history = [pilot.item_from_json(m) for m in turn["history"]]
    return stand.Asked(
        talk,
        stand.mi.minute_of(turn["situation"]) or 0,
        history,
        turn["situation"],
        stand.brief_situation(turn["situation"]),
        before,
        [stand.GAME_TOOLS[name] for name in talk.tools],
    )


def setup(directory: Path) -> dict:
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / "config.json"
    config = stand.read_json(path)
    if config is None:
        config = {
            "turns": str(stand.TURNS_DIR),
            "carry": str(stand.V3_DIR / "memory.sqlite3"),
            "sol_requests": 0,
        }
        stand.write_json(path, config)
    if not (directory / "schedule.json").exists():
        stand.write_json(directory / "schedule.json", stand.read_json(P1 / "schedule.json"))
    return config


class CountedSol:
    """Persist the actual number of Sol requests, including failed and resumed attempts."""

    def __init__(self, backend: Any, directory: Path) -> None:
        self.backend, self.directory = backend, directory

    async def stream(self, **kwargs):
        config = stand.read_json(self.directory / "config.json")
        limit = config.get("sol_request_limit")
        if limit is not None and config["sol_requests"] >= limit:
            raise RuntimeError("Sol request budget exhausted")
        config["sol_requests"] += 1
        stand.write_json(self.directory / "config.json", config)
        async for event in self.backend.stream(**kwargs):
            yield event

    async def aclose(self) -> None:
        await self.backend.aclose()


async def wait_gemma() -> None:
    """Only observe the shared service; never start it."""
    deadline = time.monotonic() + 30 * 60
    while True:
        process = await asyncio.create_subprocess_exec(
            "ssh",
            "-o",
            "ConnectTimeout=10",
            "dev-ubuntu",
            "systemctl",
            "--user",
            "is-active",
            "gothic-gemma",
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )
        stdout, _ = await process.communicate()
        if process.returncode == 0 and stdout.strip() == b"active":
            return
        if time.monotonic() >= deadline:
            raise RuntimeError("Gemma unavailable after 30 minutes")
        print("Gemma unavailable; checking again in 30 s", file=sys.stderr, flush=True)
        await asyncio.sleep(30)


async def pipeline(
    sol: Any, quick: Any, ctx: Any, phrase: str, store: Any, previous: list, *, saved: dict | None = None
) -> dict:
    if saved is None:
        started = time.monotonic()
        gist = await stand.sol_answer(sol, thinking(ctx), phrase, store)
        gist["total_s"] = time.monotonic() - started  # includes recall, not just the model requests
        for call in gist["calls"]:
            if call["name"] != "recall":
                call["result"] = "Принято."
    else:
        gist = deepcopy(saved["gist"])  # keep the source's text and measured Sol times unchanged
    record = {
        "gist": gist,
        "first_words": None,
        "voice": None,
        "voice_after_phrase_s": None,
        "text": "",
        "calls": deepcopy(saved["calls"]) if saved is not None else gist["calls"],
        "error": gist["error"],
    }
    if not gist["text"].strip() and not record["error"]:
        record["error"] = "Empty gist"
    if record["error"]:
        return record
    await wait_gemma()
    messages = first_messages(ctx, phrase, previous)
    first = await pilot.measure(quick, ctx.talk.instructions, messages, prefix=messages[:-1], slot=mind2.SLOT)
    first = filter_first(first, previous)
    record["first_words"] = first
    if first["error"]:
        record["error"] = first["error"]
        return record
    await wait_gemma()
    messages, head = voice_messages(ctx, phrase, gist["text"], first["said"])
    voice = await pilot.measure(quick, ctx.talk.instructions, messages, prefix=head, slot=mind2.SLOT)
    voice["first_sentence"] = first_words(voice["text"])[0]
    if voice["first_sentence_s"] is None and voice["text"].strip() and not voice["error"]:
        voice["first_sentence_s"] = voice["total_s"]
    voice["said_cut"] = without_said(voice["text"], first["said"])
    if not pilot.spoken(voice["said_cut"]).strip() and not voice["error"]:  # nothing left: the gist itself
        voice["said_cut"], voice["fallback"] = gist["text"].strip(), "gist"
    record["voice"] = voice
    record["error"] = voice["error"] or ("Empty voice" if not voice["text"].strip() else None)
    if voice["first_sentence_s"] is not None:
        record["voice_after_phrase_s"] = gist["total_s"] + voice["first_sentence_s"]
    record["text"] = " ".join(s for s in (first["said"], pilot.spoken(voice["said_cut"])) if s)
    return record


async def run(directory: Path, dataset: str, convs: list[str] | None, gists_from: Path | None = None) -> int:
    setup(directory)
    world = stand.World.of(directory)
    selected = {world.match(c) for c in convs} if convs else set(world.by_conv)
    path = directory / ("turns.jsonl" if dataset == "turns" else "answers.jsonl")
    records = {r["id"]: r for r in pilot.rows(path)}
    if dataset == "turns":
        plan = [(t["id"], t["conv"], t) for t in world.turns if t["conv"] in selected]
    else:
        schedule = stand.read_json(directory / "schedule.json")
        probes = {p["id"]: p for p in stand.load_probes(Path(schedule["probes"]))[0]}
        plan = [(f"{pid}@{c}", c, probes[pid]) for pid, c in schedule["asked"] if c in selected]
    recorded_probes = (
        {(r["probe"], r["ask_after"]): r for r in pilot.rows(P1 / "answers.jsonl") if r["variant"] == "sol"}
        if dataset == "probes"
        else {}
    )
    pending = [i for i, _, _ in plan if i not in records or records[i]["error"] is not None]
    if not pending:
        world.close()
        print("Nothing to run", file=sys.stderr)
        return 0
    source = (
        {
            r["id"] if dataset == "turns" else (r["probe"], r["ask_after"]): r
            for r in pilot.rows(gists_from / path.name)
        }
        if gists_from is not None
        else {}
    )
    await wait_gemma()
    sol = embed = store = None
    if gists_from is None:
        sol = CountedSol(pilot.make_backend("chatgpt", reasoning_effort=stand.EFFORT), directory)
        embed = stand.EmbedClient(stand.embed_url(None, None))
        store = stand.MemoryStore(stand.journal_copy(directory, world.carry), embeddings=embed)
    quick = pilot.make_quick(pilot.gemma_url(None), max_tokens=300)
    previous: dict[str, list[dict]] = {}
    try:
        for identifier, conv, item in plan:
            recent = previous.setdefault(conv, [])
            if identifier in pending:
                ctx = turn_context(world, item) if dataset == "turns" else stand.asked(world, conv)
                meta = {k: item[k] for k in ("npc", "kind", "phrase")}
                if dataset == "turns":
                    meta.update(index=item["index"], label=item["label"], recorded_calls=world.calls(item))
                else:
                    meta.update(
                        probe=item["id"],
                        ask_after=conv,
                        variant="note",
                        recorded_calls=recorded_probes[item["id"], conv]["calls"],
                    )
                saved = None
                if gists_from is not None:
                    key = identifier if dataset == "turns" else (item["id"], conv)
                    saved = source[key]
                    meta["gists_from"] = str(gists_from)
                records[identifier] = {
                    "id": identifier,
                    "conv": conv,
                    **meta,
                    **await pipeline(sol, quick, ctx, item["phrase"], store, recent, saved=saved),
                }
                pilot.save_rows(path, records.values())
                # A regenerated answer must be judged again, even if its old judgment was successful.
                judged_path = directory / (
                    "judge_turns.jsonl" if dataset == "turns" else "judge_answers.jsonl"
                )
                judged = [
                    r
                    for r in pilot.rows(judged_path)
                    if (
                        r["id"] != identifier
                        if dataset == "turns"
                        else (r["probe"], r["ask_after"]) != (item["id"], conv)
                    )
                ]
                if judged_path.exists():
                    pilot.save_rows(judged_path, judged)
                print(
                    f"{dataset} {identifier}: {records[identifier]['error'] or 'ok'}",
                    file=sys.stderr,
                    flush=True,
                )
            first = records[identifier].get("first_words")
            recent.append(first or {"said": ""})
        return int(any(records[i]["error"] is not None for i, _, _ in plan))
    finally:
        if sol is not None:
            await sol.aclose()
        await quick.aclose()
        if embed is not None:
            await embed.close()
        if store is not None:
            store.close()
        world.close()


def parse_voice(raw: str) -> dict:
    value = json.loads(raw)
    keys = {"lost", "changed", "added", "first_words_claim", "first_words_conflict", "repeat", "why"}
    if not isinstance(value, dict) or set(value) != keys:
        raise ValueError("Expected exactly the voice judge fields")
    for key in ("lost", "changed", "added"):
        if not isinstance(value[key], list) or any(not isinstance(s, str) for s in value[key]):
            raise ValueError(f"{key} must be a list of strings")
    for key in ("first_words_claim", "first_words_conflict", "repeat"):
        if type(value[key]) is not bool:
            raise ValueError(f"{key} must be a boolean")
    if not isinstance(value["why"], str):
        raise ValueError("why must be a string")
    return value


def voice_sections(record: dict) -> str:
    sections = {
        "Фраза героя": record["phrase"],
        "Суть": record["gist"]["text"],
        "Вызовы инструментов": json.dumps(record["calls"], ensure_ascii=False),
        "Первые слова": record["first_words"]["said"] or "(ничего)",
        "Ответ голоса": record["voice"].get("said_cut", record["voice"]["text"]),
    }
    return "\n\n".join(f"{title}\n{body}" for title, body in sections.items())


async def judge(directory: Path) -> int:
    path = directory / "judge_turns.jsonl"
    judged = {r["id"]: r for r in pilot.rows(path)}
    sol = CountedSol(pilot.make_backend("chatgpt", reasoning_effort=stand.EFFORT), directory)
    try:
        for record in pilot.rows(directory / "turns.jsonl"):
            if record["error"] is not None or (judged.get(record["id"], {}).get("error", "missing") is None):
                continue
            result = await stand.sol_stream(
                sol, prompt("judge_voice.md"), [Message("user", voice_sections(record))]
            )
            result.update(raw=result.pop("text"), result=None)
            if result["error"] is None:
                try:
                    result["result"] = parse_voice(result["raw"])
                except (ValueError, json.JSONDecodeError) as error:
                    result["error"] = f"Invalid judge JSON: {error}"
            judged[record["id"]] = {"id": record["id"], **result}
            pilot.save_rows(path, judged.values())
            print(f"Judged turn {record['id']}: {result['error'] or 'ok'}", file=sys.stderr, flush=True)
    finally:
        await sol.aclose()
    # Phase 2's unmodified probe judge sees the entire spoken answer and branch record.
    sol = CountedSol(pilot.make_backend("chatgpt", reasoning_effort=stand.EFFORT), directory)
    await mind2.judge(directory, sol=sol)
    return int(any(r["error"] for p in (path, directory / "judge_answers.jsonl") for r in pilot.rows(p)))


def stats(values: list[float]) -> dict:
    if not values:
        return {"n": 0, "median": None, "p75": None, "p90": None, "max": None}
    values = sorted(values)

    def percentile(p: float) -> float:
        at = (len(values) - 1) * p
        lo = int(at)
        return values[lo] + (values[min(lo + 1, len(values) - 1)] - values[lo]) * (at - lo)

    return {
        "n": len(values),
        "median": statistics.median(values),
        "p75": percentile(0.75),
        "p90": percentile(0.9),
        "max": values[-1],
    }


def names(calls: list) -> list[str]:
    return sorted({c["name"] for c in calls})


def baseline(path: Path, variant: str, selected: set[tuple]) -> dict:
    rows = [r for r in pilot.rows(path) if r["variant"] == variant]

    def tally(rs: list) -> dict:
        return {
            "n": len(rs),
            "errors": sum(r.get("error") is not None for r in rs),
            "verdicts": dict(Counter(r["result"]["verdict"] for r in rs if r.get("result"))),
        }

    return {
        "available": path.exists(),
        "full": tally(rows),
        "matched": tally([r for r in rows if (r["probe"], r["ask_after"]) in selected]),
    }


def summary(directory: Path, *, emit: bool = True) -> dict:
    turns = pilot.rows(directory / "turns.jsonl")
    probes = pilot.rows(directory / "answers.jsonl")
    judged = {r["id"]: r for r in pilot.rows(directory / "judge_turns.jsonl")}
    probe_judged = {(r["probe"], r["ask_after"]): r for r in pilot.rows(directory / "judge_answers.jsonl")}
    all_rows = turns + probes
    ok = [r for r in all_rows if r["error"] is None]

    def loss_counts(rows: list) -> dict:
        results = [judged[r["id"]]["result"] for r in rows if judged.get(r["id"], {}).get("result")]
        return {
            "n": len(rows),
            "judged": len(results),
            "clean": sum(not any(r[key] for key in ("lost", "changed", "added")) for r in results),
            **{key: sum(bool(r[key]) for r in results) for key in ("lost", "changed", "added")},
        }

    decisions = []
    for r in turns:
        j = judged.get(r["id"], {}).get("result")
        if r["label"] and j and (j["lost"] or j["changed"]):
            decisions.append(
                {
                    "id": r["id"],
                    "phrase": r["phrase"],
                    "gist": r["gist"]["text"],
                    "voice": r["voice"]["text"],
                    "lost": j["lost"],
                    "changed": j["changed"],
                }
            )
    first = [r["first_words"] for r in all_rows if r.get("first_words")]
    said = [WORDS.findall(f["said"].casefold()) for f in first if f["said"]]

    def most_common(length: int) -> dict:
        counts = Counter(" ".join(words[:length]) for words in said if len(words) >= length)
        text, count = counts.most_common(1)[0] if counts else (None, 0)
        return {"text": text, "n": count, "of_said": count / len(said) if said else None}

    results = [r["result"] for r in judged.values() if r.get("result")]
    tool_rows = ok
    mismatches = [
        {"id": r["id"], "gist": names(r["calls"]), "recorded_sol": names(r["recorded_calls"])}
        for r in tool_rows
        if names(r["calls"]) != names(r["recorded_calls"])
    ]
    timings: dict[str, list] = {
        name: []
        for name in (
            "sol_first_token_s",
            "sol_total_s",
            "first_words_first_sentence_s",
            "voice_first_sentence_s",
            "voice_after_phrase_s",
            "silence_after_first_words_s",
        )
    }
    for r in ok:
        f, v = r["first_words"], r["voice"]
        values = {
            "sol_first_token_s": r["gist"]["first_token_s"],
            "sol_total_s": r["gist"]["total_s"],
            "first_words_first_sentence_s": f["first_sentence_s"],
            "voice_first_sentence_s": v["first_sentence_s"],
            "voice_after_phrase_s": r["voice_after_phrase_s"],
        }
        after = r["voice_after_phrase_s"]
        values["silence_after_first_words_s"] = (
            (after - (f["first_sentence_s"] + 0.3 + len(f["said"]) / 13) if f["said"] else after)
            if after is not None
            else None
        )
        for name, value in values.items():
            if value is not None:
                timings[name].append(value)
    selected = {(r["probe"], r["ask_after"]) for r in probes}
    verdicts = [
        probe_judged[k]["result"]["verdict"] for k in selected if probe_judged.get(k, {}).get("result")
    ]
    out = {
        "sol_requests": stand.read_json(directory / "config.json", {}).get("sol_requests", 0),
        "records": {
            "turns": len(turns),
            "probes": len(probes),
            "complete": len(ok),
            "errors": len(all_rows) - len(ok),
            "judge_errors": sum(
                r.get("error") is not None for r in [*judged.values(), *probe_judged.values()]
            ),
        },
        "turns": {
            "by_label": {
                str(label).lower(): loss_counts([r for r in turns if r["label"] == label])
                for label in (False, True)
            },
            "by_kind": {
                kind: loss_counts([r for r in turns if r["kind"] == kind])
                for kind in sorted({r["kind"] for r in turns})
            },
            "decision_losses": decisions,
        },
        "first_words": {
            "outcomes": {
                key: sum(f["outcome"] == key for f in first)
                for key in ("said", "nothing", "too_long", "same", "sound", "late", "error")
            },
            "most_common_first_word": most_common(1),
            "most_common_first_two_words": most_common(2),
            "judged_turns": len(results),
            **{
                key: sum(r[key] for r in results)
                for key in ("first_words_claim", "first_words_conflict", "repeat")
            },
        },
        "tools": {
            "n": len(tool_rows),
            **{
                name: sum(bool(r[field]) for r in tool_rows) / len(tool_rows) if tool_rows else None
                for name, field in (("gist_fraction", "calls"), ("recorded_sol_fraction", "recorded_calls"))
            },
            "name_mismatches": mismatches,
        },
        "probes": {
            "n": len(probes),
            "judged": len(verdicts),
            "verdicts": dict(Counter(verdicts)),
            "sol": baseline(P1 / "judge_answers.jsonl", "sol", selected),
            "gist_stall": baseline(STALL / "judge_answers.jsonl", "note", selected),
        },
        "timing": {name: stats(values) for name, values in timings.items()},
        "silence_over_5_s": sum(v > 5 for v in timings["silence_after_first_words_s"]),
    }
    if emit:
        stand.write_json(directory / "summary.json", out)
        print(json.dumps(out, ensure_ascii=False, indent=2))
    return out


async def selftest() -> int:
    from types import SimpleNamespace

    from gothic_ai.llm import FakeBackend, FakeStep, ToolCall

    ctx = stand.Asked(
        SimpleNamespace(instructions="persona", npc="npc", branch="branch"),
        1,
        [Message("assistant", "history")],
        "whole",
        "brief",
        tools=[stand.GAME_TOOLS["recall"]],
    )
    phrase = "hero phrase"
    assert stand.sol_messages(thinking(ctx), phrase) == [
        *ctx.history,
        Message("developer", "whole\n\n" + prompt("gist.md")),
        Message("user", phrase),
    ]
    first = first_messages(ctx, phrase, [])
    assert first == [
        *ctx.history,
        Message("developer", "brief\n" + prompt("first_words.md").format(recent="")),
        Message("user", phrase),
    ]
    previous = [{"said": str(i)} for i in range(6)]
    first = first_messages(ctx, phrase, previous)
    recent = prompt("first_words_recent.md").format(said=", ".join(f"«{i}»" for i in range(2, 6)))
    assert first[-2].content == "brief\n" + prompt("first_words.md").format(recent=recent)
    messages, head = voice_messages(ctx, phrase, "gist {1}", "Wait.")
    assert head == [*ctx.history, Message("developer", "brief")]
    assert messages == [
        *head,
        Message("user", phrase),
        Message(
            "developer",
            prompt("voice.md").format(gist="gist {1}") + prompt("voice_said.md").format(said="Wait."),
        ),
    ]
    assert "Ты уже сказал" not in voice_messages(ctx, phrase, "gist", "")[0][-1].content
    assert (
        without_said("[angry] Да я уже в деле! Сейчас прижму.", "Да я уже в деле!")
        == "[angry] Сейчас прижму."
    )
    assert without_said("[angry] Сейчас прижму.", "Да я уже в деле!") == "[angry] Сейчас прижму."
    assert without_said("Хм... Дай-ка вспомнить. Они в пещере.", "Хм... Дай-ка вспомнить.") == "Они в пещере."
    valid = dict(
        lost=[],
        changed=[],
        added=[],
        first_words_claim=False,
        first_words_conflict=False,
        repeat=False,
        why="ok",
    )
    assert parse_voice(json.dumps(valid)) == valid
    for raw in (
        "```json\n{}\n```",
        "[]",
        json.dumps({**valid, "repeat": "false"}),
        json.dumps({**valid, "lost": "a fact"}),
    ):
        try:
            parse_voice(raw)
        except ValueError:
            pass
        else:
            raise AssertionError("Invalid judge JSON accepted")
    stand.parse_probe_verdict('{"verdict":"correct","invented":[],"why":"ok"}')

    def filtered(text: str, previous: list | None = None, elapsed: float = 0.4) -> dict:
        return filter_first({"text": text, "error": None, "first_sentence_s": elapsed}, previous or [])

    assert filtered("—")["outcome"] == "nothing"
    assert filtered("[neutral] Дай подумать.")["said"] == "Дай подумать."
    assert filtered("один два три четыре пять шесть семь восемь девять.")["outcome"] == "too_long"
    assert filtered("Дай подумать!", [{"said": "Дай подумать."}])["outcome"] == "same"
    assert filtered("Хм...", [{"said": "Ох!"}, *[{"said": ""}] * 3])["outcome"] == "sound"
    assert filtered("Хм...", [{"said": "Ох!"}, *[{"said": ""}] * 4])["outcome"] == "said"
    assert filtered("Дай подумать.", elapsed=1.3)["outcome"] == "late"
    assert (
        filter_first({"text": "Wait", "error": None, "first_sentence_s": None, "total_s": 0.4}, [])["said"]
        == "Wait"
    )
    # Exercise phase 1's actual tool loop and the measured Gemma requests without network access.
    fake_sol = FakeBackend(
        script=[
            FakeStep(tool_calls=[ToolCall("recall", {"query": "past"}, "recall-1")]),
            FakeStep("I offer 25 gold."),
        ],
        delay=0,
    )
    fake_quick = FakeBackend(
        script=[FakeStep("Дай подумать. "), FakeStep("[neutral] I offer 25 gold. ")], delay=0
    )
    with (
        patch.object(stand, "recall", return_value=("journal result", "lexical")) as recalled,
        patch(__name__ + ".wait_gemma"),
    ):
        row = await pipeline(fake_sol, fake_quick, ctx, phrase, None, [])
    assert row["error"] is None and row["calls"][0]["run"]
    assert recalled.call_count == 1 and row["gist"]["attempts"] == 2
    assert row["voice_after_phrase_s"] >= row["gist"]["total_s"]
    row.update(id="t", conv="c", phrase=phrase, npc="npc", label=True, kind="deal", recorded_calls=[])
    with TemporaryDirectory() as temp:
        directory = Path(temp)
        stand.write_json(directory / "config.json", {"sol_requests": 4})
        pilot.save_rows(directory / "turns.jsonl", [row])
        pilot.save_rows(
            directory / "judge_turns.jsonl",
            [
                {
                    "id": "t",
                    "error": None,
                    "result": {**valid, "lost": ["condition"], "first_words_claim": True},
                }
            ],
        )
        probe = {**row, "id": "p@c", "probe": "p", "ask_after": "c", "variant": "note"}
        pilot.save_rows(directory / "answers.jsonl", [probe])
        pilot.save_rows(
            directory / "judge_answers.jsonl",
            [{"probe": "p", "ask_after": "c", "error": None, "result": {"verdict": "correct"}}],
        )
        out = summary(directory, emit=False)
        assert out["records"]["complete"] == 2 and out["sol_requests"] == 4
        assert out["turns"]["by_label"]["true"]["lost"] == 1
        assert out["turns"]["by_label"]["true"]["clean"] == 0
        assert len(out["turns"]["decision_losses"]) == 1
        assert out["first_words"]["first_words_claim"] == 1
        assert out["first_words"]["most_common_first_word"]["of_said"] == 1
        assert out["tools"]["gist_fraction"] == 1 and len(out["tools"]["name_mismatches"]) == 2
        assert out["probes"]["verdicts"] == {"correct": 1}
        assert out["timing"]["sol_total_s"]["n"] == 2
        expected = row["voice_after_phrase_s"] - (
            row["first_words"]["first_sentence_s"] + 0.3 + len(row["first_words"]["said"]) / 13
        )
        assert out["timing"]["silence_after_first_words_s"]["median"] == expected
        # A one-word sound is not a pair of first words, even when it is more frequent.
        sound = {**probe, "first_words": {**probe["first_words"], "said": "Хм..."}}
        pilot.save_rows(directory / "answers.jsonl", [sound, {**sound, "probe": "p2", "id": "p2@c"}])
        pair = summary(directory, emit=False)["first_words"]["most_common_first_two_words"]
        assert pair == {"text": "дай подумать", "n": 1, "of_said": 1 / 3}
        # The persisted counter must prevent request 2 from reaching the provider.
        stand.write_json(directory / "config.json", {"sol_requests": 0, "sol_request_limit": 1})
        backend = FakeBackend("answer", delay=0)
        counted = CountedSol(backend, directory)
        _ = [event async for event in counted.stream(instructions="", messages=[], tools=[])]
        try:
            _ = [event async for event in counted.stream(instructions="", messages=[], tools=[])]
        except RuntimeError:
            pass
        else:
            raise AssertionError("Sol request budget exceeded")
        assert len(backend.calls) == 1 and stand.read_json(directory / "config.json")["sol_requests"] == 1
        # Exercise --gists-from through run for both key shapes, with every Sol entry point forbidden.
        source = directory / "source"
        source.mkdir()
        # Source calls can carry recorded results absent from the nested gist.calls.
        saved_row = {**row, "calls": [{**call, "result": "Source result"} for call in row["calls"]]}
        pilot.save_rows(source / "turns.jsonl", [saved_row])
        pilot.save_rows(
            source / "answers.jsonl", [{**probe, "calls": saved_row["calls"], "id": "legacy-probe-id"}]
        )
        base = directory / "phase1"
        base.mkdir()
        pilot.save_rows(
            base / "answers.jsonl", [{"variant": "sol", "probe": "p", "ask_after": "c", "calls": []}]
        )
        probes_path = directory / "probes.json"
        stand.write_json(probes_path, [{"id": "p", "npc": "npc", "kind": "detail", "phrase": phrase}])
        world = SimpleNamespace(
            by_conv={"c": [row]},
            turns=[{**row, "index": 0}],
            match=lambda _: "c",
            calls=lambda _: [],
            close=lambda: None,
        )
        for dataset, filename in (("turns", "turns.jsonl"), ("probes", "answers.jsonl")):
            destination = directory / dataset
            destination.mkdir()
            stand.write_json(destination / "config.json", {"sol_requests": 0})
            stand.write_json(
                destination / "schedule.json", {"probes": str(probes_path), "asked": [["p", "c"]]}
            )
            quick = FakeBackend(script=[FakeStep("Дай подумать. "), FakeStep("I offer 25 gold. ")], delay=0)
            with (
                patch.object(stand.World, "of", return_value=world),
                patch(__name__ + ".turn_context", return_value=ctx),
                patch.object(stand, "asked", return_value=ctx),
                patch.object(
                    pilot, "make_backend", side_effect=AssertionError("Sol must not be constructed")
                ),
                patch.object(stand, "sol_answer", side_effect=AssertionError("Sol must not be called")),
                patch.object(pilot, "make_quick", return_value=quick),
                patch.object(pilot, "gemma_url", return_value="fake"),
                patch(__name__ + ".wait_gemma"),
                patch(__name__ + ".P1", base),
            ):
                assert await run(destination, dataset, ["c"], source) == 0
            copied = pilot.rows(destination / filename)[0]
            assert copied["gist"] == row["gist"] and copied["calls"] == saved_row["calls"]
            assert copied["gists_from"] == str(source)
            assert stand.read_json(destination / "config.json")["sol_requests"] == 0
            assert len(quick.calls) == 2 and quick.calls[-1][1][-1].role == "developer"
    print("selftest: requests, tool loop, judge JSON, first words and summary passed (offline)")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    p = commands.add_parser("run")
    p.add_argument("directory", type=Path)
    p.add_argument("--set", choices=("turns", "probes"), required=True, dest="dataset")
    p.add_argument("--conv", action="append")
    p.add_argument("--gists-from", type=Path)
    for command in ("judge", "summary"):
        commands.add_parser(command).add_argument("directory", type=Path)
    commands.add_parser("selftest")
    args = parser.parse_args()
    if args.command == "summary":
        summary(args.directory)
        return 0
    if args.command == "selftest":
        return asyncio.run(selftest())
    if args.command == "run":
        source = args.gists_from.resolve() if args.gists_from is not None else None
        return asyncio.run(run(args.directory, args.dataset, args.conv, source))
    return asyncio.run(judge(args.directory))


if __name__ == "__main__":
    raise SystemExit(main())
