"""Compare repeated three-word phrases across NPCs; selftest uses no network. Run from server/."""

from __future__ import annotations

import argparse
import asyncio
import importlib.util
import json
import re
import sys
from collections import defaultdict
from itertools import combinations
from pathlib import Path
from tempfile import TemporaryDirectory
from typing import Any
from unittest.mock import patch

from gothic_ai.llm import FakeBackend, LLMBackend, LLMError, Message, TextDelta
from gothic_ai.replay import _quick_context
from gothic_ai.trace import item_json, read

HERE = Path(__file__).resolve().parent
HYBRID = HERE.parent / "2026-11-02-gemma-sol-hybrid"
SPEC = importlib.util.spec_from_file_location(
    "_sol_thinks_pilot", HERE.parent / "2026-11-02-sol-thinks/pilot.py"
)
assert SPEC is not None and SPEC.loader is not None
pilot = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = pilot
SPEC.loader.exec_module(pilot)

NPCS = (
    "BAU_4300_ADDON_CAVALORN",
    "NONE_100_XARDAS",
    "BDT_1013_BANDIT_L",
    "BDT_1015_BANDIT_L",
    "VLK_468_CANTHAR",
    "PIR_1300_ADDON_GREG_NW",
    "MIL_310_STADTWACHE",
    "MIL_305_TORWACHE",
    "PAL_203_LOTHAR",
    "VLK_451_BUERGER",
)
KIN = ((NPCS[2], NPCS[3]), (NPCS[6], NPCS[7]))
VARIANTS = ("sol", "gemma", "hybrid")
STOP = set(
    "в во на с со к ко у о об от до из за по под над для без при через "
    "и а но или либо что чтобы если то же ли бы не ни "
    "я ты он она оно мы вы они меня тебя его ее нас вас их мне тебе ему ей нам вам им "
    "мой твой свой наш ваш этот тот кто все быть есть это так как да нет ну вот уже еще".split()
)


def pick(traces: list[Path], directory: Path) -> list[dict[str, Any]]:
    selected = {}
    for trace in dict.fromkeys(path.resolve() for path in traces):
        records = read(trace)
        instructions = {r["hash"]: r["text"] for r in records if r.get("type") == "instructions"}
        for record in records:
            if (
                record.get("type") != "turn"
                or record.get("npc") not in NPCS
                or record.get("trigger", {}).get("kind") != "player"
                or not record.get("steps")
                or record.get("instructions") not in instructions
            ):
                continue
            history, situation = _quick_context(record)
            npc = record["npc"]
            text = instructions[record["instructions"]]
            name = re.match(r"Ты играешь персонажа по имени (.+?) в мире\b", text.splitlines()[0])
            if name is None:
                raise SystemExit(f"Cannot read character name from instructions: {npc}")
            candidate = {
                "npc": npc,
                "name": name[1],
                "trace": str(trace),
                "t": record["t"],
                "turn_id": record["id"],
                "instructions": text,
                "situation": situation,
                "history_empty": not history,
            }
            previous = selected.get(npc)
            if previous is None or (candidate["history_empty"], candidate["t"]) > (
                previous["history_empty"],
                previous["t"],
            ):
                selected[npc] = candidate
    missing = [npc for npc in NPCS if npc not in selected]
    if missing:
        raise SystemExit("No eligible player turn for: " + ", ".join(missing))
    characters = [selected[npc] for npc in NPCS]
    directory.mkdir(parents=True, exist_ok=True)
    pilot.save_rows(directory / "characters.jsonl", characters)
    print(f"Picked {len(characters)} characters")
    return characters


async def think(directory: Path, *, effort: str = "low", sol: LLMBackend | None = None) -> None:
    prompt = (HYBRID / "think_short.md").read_text(encoding="utf-8")
    results = {r["npc"]: r for r in pilot.rows(directory / "thoughts.jsonl")}
    pending = [
        character
        for character in read(directory / "characters.jsonl")
        if character["npc"] not in results or results[character["npc"]]["error"] is not None
    ]
    if not pending:
        return
    sol = sol if sol is not None else pilot.make_backend("chatgpt", reasoning_effort=effort)
    try:
        for character in pending:
            npc = character["npc"]
            measured = await pilot.measure(
                sol,
                character["instructions"],
                [Message("developer", character["situation"]), Message("developer", prompt)],
                cache_key=f"repeats-{npc}",
            )
            results[npc] = {"npc": npc, **measured}
            pilot.save_rows(directory / "thoughts.jsonl", results.values())
            print(f"Thought {npc}: {'error' if measured['error'] is not None else 'ok'}")
    finally:
        await sol.aclose()


def answer_key(record: dict[str, Any]) -> tuple[str, str, str, int]:
    return record["npc"], record["phrase"], record["variant"], record["sample"]


async def run(
    directory: Path,
    *,
    variants: tuple[str, ...] = VARIANTS,
    samples: int = 3,
    slot: int = 0,
    quick_at: str | None = None,
    effort: str = "low",
    sol: LLMBackend | None = None,
    quick: LLMBackend | None = None,
) -> None:
    if samples < 1:
        raise SystemExit("--samples must be at least 1")
    phrases = [line.strip() for line in (HERE / "phrases.txt").read_text(encoding="utf-8").splitlines()]
    phrases = list(dict.fromkeys(phrase for phrase in phrases if phrase))
    speak = (HYBRID / "speak_hybrid.md").read_text(encoding="utf-8")
    characters = read(directory / "characters.jsonl")
    thoughts = {r["npc"]: r for r in pilot.rows(directory / "thoughts.jsonl")}
    results = {answer_key(r): r for r in pilot.rows(directory / "answers.jsonl")}
    try:
        for character in characters:
            npc = character["npc"]
            instructions, situation = character["instructions"], character["situation"]
            for phrase in phrases:
                for variant in variants:
                    for sample in range(1 if variant == "sol" else samples):
                        key = (npc, phrase, variant, sample)
                        if key in results and results[key]["error"] is None:
                            continue
                        prefix = [Message("developer", situation)]
                        thought = thoughts.get(npc)
                        if variant == "hybrid" and (
                            thought is None or thought["error"] is not None or not thought["text"].strip()
                        ):
                            measured = {
                                "text": "",
                                "error": "no thought",
                                "first_token_s": None,
                                "total_s": None,
                            }
                        elif variant == "sol":
                            if sol is None:
                                sol = pilot.make_backend("chatgpt", reasoning_effort=effort)
                            measured = await pilot.measure(
                                sol,
                                instructions,
                                [*prefix, Message("user", phrase)],
                                cache_key=f"repeats-{npc}",
                            )
                        else:
                            if variant == "hybrid":
                                prefix = [
                                    Message(
                                        "developer",
                                        situation + "\n\n" + speak.format(thought=thought["text"]),
                                    )
                                ]
                            if quick is None:
                                quick = pilot.make_quick(pilot.gemma_url(quick_at), max_tokens=300)
                            measured = await pilot.measure(
                                quick,
                                instructions,
                                [*prefix, Message("user", phrase)],
                                prefix=prefix,
                                slot=slot,
                            )
                        results[key] = {
                            "npc": npc,
                            "phrase": phrase,
                            "variant": variant,
                            "sample": sample,
                            **{
                                field: measured[field]
                                for field in ("text", "error", "first_token_s", "total_s")
                            },
                        }
                        pilot.save_rows(directory / "answers.jsonl", results.values())
                        state = "error" if measured["error"] is not None else "ok"
                        print(f"Answer {npc} {variant} #{sample}: {state}")
    finally:
        if sol is not None:
            await sol.aclose()
        if quick is not None:
            await quick.aclose()


def words(text: str) -> list[str]:
    return re.findall(r"[а-яa-z]+", re.sub(r"\[[^\]]*\]", "", text).lower().replace("ё", "е"))


def trigrams(text: str) -> set[str]:
    tokens = words(text)
    return {
        " ".join(triple)
        for triple in zip(tokens, tokens[1:], tokens[2:], strict=False)
        if any(word not in STOP for word in triple)
    }


def family(npc: str) -> str:
    return next((pair[0] for pair in KIN if npc in pair), npc)


def common(records: list[dict[str, Any]]) -> dict[str, Any]:
    speakers = defaultdict(set)
    for record in records:
        for trigram in trigrams(record["text"]):
            speakers[record["phrase"], trigram].add(record["npc"])
    shared, unrelated = set(), set()
    items = []
    for (phrase, trigram), npcs in sorted(speakers.items()):
        if len(npcs) < 2:
            continue
        shared.update((npc, phrase) for npc in npcs)
        if len({family(npc) for npc in npcs}) > 1:
            unrelated.update((npc, phrase) for npc in npcs)
        items.append({"phrase": phrase, "trigram": trigram, "npcs": sorted(npcs)})
    return {
        "n": len(records),
        "shared_n": len(shared),
        "fraction": pilot.fraction(len(shared), len(records)),
        "unrelated_n": len(unrelated),
        "unrelated_fraction": pilot.fraction(len(unrelated), len(records)),
        "items": items,
    }


def summary(directory: Path, *, emit: bool = True) -> dict[str, Any]:
    answers = pilot.rows(directory / "answers.jsonl")
    report = {"variants": {}}
    for variant in VARIANTS:
        all_rows = [r for r in answers if r["variant"] == variant]
        first = [r for r in all_rows if r["sample"] == 0]
        valid = [r for r in first if r["error"] is None]
        occurrences = defaultdict(list)
        own = defaultdict(set)
        for record in valid:
            for trigram in trigrams(record["text"]):
                occurrences[trigram].append(record)
                own[record["npc"], trigram].add(record["phrase"])
        cliches = []
        for trigram, records in occurrences.items():
            npcs = sorted({r["npc"] for r in records})
            families = {family(npc) for npc in npcs}
            if len(families) >= 3:
                cliches.append(
                    {
                        "trigram": trigram,
                        "family_n": len(families),
                        "npcs": npcs,
                        "answer_n": len(records),
                        "phrases": sorted({r["phrase"] for r in records}),
                    }
                )
        result = {
            "n": len(first),
            "errors": sum(r["error"] is not None for r in first),
            "empty": sum(not words(r["text"]) for r in valid),
            "words": pilot.distribution(len(words(r["text"])) for r in valid),
            "common": common(valid),
            "cliches": sorted(cliches, key=lambda r: (-r["family_n"], -r["answer_n"], r["trigram"]))[:30],
            "own_repeats": [
                {"npc": npc, "trigram": trigram, "phrase_n": len(phrases)}
                for (npc, trigram), phrases in sorted(own.items())
                if len(phrases) >= 3
            ],
        }
        if variant != "sol":
            pairs = defaultdict(list)
            for record in all_rows:
                if record["error"] is None:
                    pairs[record["npc"], record["phrase"]].append(trigrams(record["text"]))
            stability = []
            empty_pairs_n = 0
            for (npc, phrase), sets in sorted(pairs.items()):
                scores = []
                for a, b in combinations(sets, 2):
                    if not a | b:
                        empty_pairs_n += 1
                        continue
                    scores.append(len(a & b) / len(a | b))
                if scores:
                    stability.append(
                        {
                            "npc": npc,
                            "phrase": phrase,
                            "samples": len(sets),
                            "jaccard": sum(scores) / len(scores),
                        }
                    )
            result["stability"] = {
                **pilot.distribution(pair["jaccard"] for pair in stability),
                "empty_pairs_n": empty_pairs_n,
                "pairs": stability,
            }
            result["common_unrelated_by_sample"] = []
            for sample in sorted({r["sample"] for r in all_rows}):
                sampled = common([r for r in all_rows if r["sample"] == sample and r["error"] is None])
                result["common_unrelated_by_sample"].append(
                    {
                        "sample": sample,
                        "n": sampled["n"],
                        "shared_n": sampled["unrelated_n"],
                        "fraction": sampled["unrelated_fraction"],
                    }
                )
        report["variants"][variant] = result
        if emit:
            shared = result["common"]
            print(
                f"{variant}: sample 0, {len(valid)}/{len(first)} valid, "
                f"common={shared['fraction']}, unrelated={shared['unrelated_fraction']}, "
                f"cliches={len(result['cliches'])}, errors={result['errors']}, empty={result['empty']}"
            )
            if variant != "sol":
                print(
                    f"  stability: {result['stability']['median']} median, {result['stability']['p90']} p90, "
                    f"empty_pairs_n={result['stability']['empty_pairs_n']}"
                )
    (directory / "summary.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    return report


async def selftest() -> None:
    npcs = (NPCS[2], NPCS[3], NPCS[1])
    with (
        TemporaryDirectory(prefix=".selftest-", dir=HERE) as temporary,
        patch(__name__ + ".NPCS", npcs),
        patch.object(pilot, "make_backend", side_effect=AssertionError("Unexpected Sol creation")),
        patch.object(pilot, "make_quick", side_effect=AssertionError("Unexpected Gemma creation")),
    ):
        root = Path(temporary)
        traces = [root / "one.jsonl", root / "two.jsonl"]
        instructions = [
            {
                "type": "instructions",
                "hash": npc,
                "text": f"Ты играешь персонажа по имени {i} в мире Gothic 2.",
            }
            for i, npc in enumerate(npcs)
        ]

        def turn(npc: str, t: int, history: bool = False, **extra) -> dict[str, Any]:
            messages = [Message("user", "Earlier"), Message("assistant", "Earlier reply")] if history else []
            return {
                "type": "turn",
                "npc": npc,
                "id": f"{npc}-{t}",
                "t": t,
                "instructions": npc,
                "trigger": {"kind": "player", "text": "Hello"},
                "steps": [
                    {
                        "added": [
                            item_json(m)
                            for m in [
                                *messages,
                                Message("developer", f"Situation {t}"),
                                Message("user", "Hello"),
                            ]
                        ]
                    }
                ],
                **extra,
            }

        pilot.save_rows(
            traces[0],
            instructions
            + [
                turn(npcs[0], 10),
                turn(npcs[0], 30, True),
                turn(npcs[1], 40, True),
                turn(npcs[2], 15),
                turn(npcs[2], 60, instructions="missing"),
                turn(npcs[2], 70, trigger={"kind": "event"}),
                turn(npcs[2], 80, steps=[]),
            ],
        )
        pilot.save_rows(traces[1], instructions + [turn(npcs[0], 20), turn(npcs[1], 10, True)])
        directory = root / "data"
        characters = pick([traces[1], traces[0], traces[1]], directory)
        assert [r["npc"] for r in characters] == list(npcs)
        assert [r["t"] for r in characters] == [20, 40, 15]
        assert [r["history_empty"] for r in characters] == [True, False, True]
        assert [r["name"] for r in characters] == ["0", "1", "2"]
        assert characters[0]["situation"] == "Situation 20"
        assert characters[0]["instructions"] == instructions[0]["text"]
        assert characters[0]["turn_id"] == f"{npcs[0]}-20"
        assert all(Path(r["trace"]).is_absolute() for r in characters)
        before = (directory / "characters.jsonl").read_bytes()
        with patch(__name__ + ".NPCS", (*npcs, "MISSING_NPC")):
            for out in (directory, root / "missing"):
                try:
                    pick(traces, out)
                except SystemExit as error:
                    assert "MISSING_NPC" in str(error)
                else:
                    raise AssertionError("Missing character accepted")
        assert (directory / "characters.jsonl").read_bytes() == before
        assert not (root / "missing/characters.jsonl").exists()

        sol = FakeBackend("A short thought.", delay=0)
        await think(directory, sol=sol)
        await think(directory, sol=sol)
        assert len(sol.calls) == 3
        assert sol.cache_keys == [f"repeats-{npc}" for npc in npcs]
        assert sol.calls[0] == (
            characters[0]["instructions"],
            [
                Message("developer", characters[0]["situation"]),
                Message("developer", (HYBRID / "think_short.md").read_text(encoding="utf-8")),
            ],
            [],
        )
        thoughts = pilot.rows(directory / "thoughts.jsonl")
        thoughts[0]["error"] = "Synthetic error"
        pilot.save_rows(directory / "thoughts.jsonl", thoughts)

        async def failed_stream(**kwargs):
            yield TextDelta("Partial")
            raise LLMError("Synthetic model error")

        broken = FakeBackend(delay=0)
        broken.stream = failed_stream
        await think(directory, sol=broken)
        failed = pilot.rows(directory / "thoughts.jsonl")
        assert failed[0]["error"] == "Synthetic model error" and failed[0]["text"] == "Partial"
        assert failed[1:] == thoughts[1:]
        await think(directory, sol=sol)
        await think(directory, sol=sol)
        assert len(sol.calls) == 4

        # Patch only the phrase location; real hybrid prompts still come from HYBRID.
        (root / "phrases.txt").write_text("Hello\nThanks\n", encoding="utf-8")
        with patch(__name__ + ".HERE", root):
            sol = FakeBackend("[neutral] A Sol answer.", delay=0)
            quick = FakeBackend("[neutral] A Gemma answer.", delay=0)
            await run(directory, sol=sol, quick=quick, slot=2)
            await run(directory, sol=sol, quick=quick, slot=2)
            answers = pilot.rows(directory / "answers.jsonl")
            assert len(answers) == len({answer_key(r) for r in answers}) == 42
            assert len(sol.calls) == 6 and len(quick.calls) == len(quick.prewarms) == 36
            assert all(r["sample"] == 0 for r in answers if r["variant"] == "sol")
            assert quick.slot_ids == quick.prewarm_slot_ids == [2] * 36
            assert all(not tools for _, _, tools in sol.calls + quick.calls)
            assert sol.cache_keys == [f"repeats-{npc}" for npc in npcs for _ in range(2)]
            assert sol.calls[0][1] == [Message("developer", "Situation 20"), Message("user", "Hello")]
            assert quick.prewarms[0][1] == [Message("developer", "Situation 20")]
            assert quick.prewarms[3][1] == [
                Message(
                    "developer",
                    "Situation 20\n\n"
                    + (HYBRID / "speak_hybrid.md")
                    .read_text(encoding="utf-8")
                    .format(thought="A short thought."),
                )
            ]
            for (_, prefix), (_, request, _) in zip(quick.prewarms, quick.calls, strict=True):
                assert request[:-1] == prefix and request[-1].role == "user"
            assert all(r["error"] is None and 0 <= r["first_token_s"] <= r["total_s"] for r in answers)
            answers[0]["error"] = answers[1]["error"] = "Retry this answer"
            pilot.save_rows(directory / "answers.jsonl", answers)
            await run(directory, sol=broken, quick=broken, slot=2)
            retried = pilot.rows(directory / "answers.jsonl")
            assert len(retried) == 42 and retried[2:] == answers[2:]
            assert all(r["text"] == "Partial" and r["error"] == "Synthetic model error" for r in retried[:2])
            await run(directory, sol=sol, quick=quick, slot=2)
            await run(directory, sol=sol, quick=quick, slot=2)
            assert len(sol.calls) == 7 and len(quick.calls) == 37
            (root / "phrases.txt").write_text("Hello\nThanks\nNew phrase\n", encoding="utf-8")
            await run(directory, variants=("sol",), sol=sol)
            assert len(sol.calls) == 10 and len(pilot.rows(directory / "answers.jsonl")) == 45

            no_thought = root / "no-thought"
            no_thought.mkdir()
            pilot.save_rows(no_thought / "characters.jsonl", characters)
            pilot.save_rows(
                no_thought / "thoughts.jsonl",
                [
                    {"npc": npcs[1], "text": "Partial", "error": "Failed thought"},
                    {"npc": npcs[2], "text": " \n", "error": None},
                ],
            )
            await run(no_thought, variants=("hybrid",))
            missing = pilot.rows(no_thought / "answers.jsonl")
            assert len(missing) == 27 and all(r["error"] == "no thought" for r in missing)
            assert all(r["text"] == "" and r["first_token_s"] is r["total_s"] is None for r in missing)
            pilot.save_rows(no_thought / "thoughts.jsonl", pilot.rows(directory / "thoughts.jsonl"))
            await run(no_thought, variants=("hybrid",), quick=quick, slot=2)
            await run(no_thought, variants=("hybrid",), quick=quick, slot=2)
            assert len(quick.calls) == 64
            assert all(r["error"] is None for r in pilot.rows(no_thought / "answers.jsonl"))

        # Three trace characters, plus an unrelated speaker for the three-family threshold.
        fourth = "VLK_468_CANTHAR"
        fixtures = [
            (npcs[0], "shared", "[neutral] Ёжик любит яблоки. В и на."),
            (npcs[1], "shared", "[sol] ежик любит яблоки."),
            (npcs[2], "shared", "ежик любит яблоки."),
            (npcs[0], "kin", "Грубый голос бандита"),
            (npcs[1], "kin", "Грубый голос бандита"),
            *((npc, f"stamp-{i}", "Темная башня зовет") for i, npc in enumerate((*npcs, fourth))),
            *((npcs[0], f"own-{i}", "Твой жалкий меч") for i in range(3)),
            (npcs[2], "stop", "И на в"),
            (fourth, "stop", "И на в"),
            (npcs[0], "empty", "[neutral]"),
        ]
        manual = []
        for variant in VARIANTS:
            for npc, phrase, text in fixtures:
                manual.append(
                    {
                        "npc": npc,
                        "phrase": phrase,
                        "text": text,
                        "variant": variant,
                        "sample": 0,
                        "error": None,
                    }
                )
            manual.append(
                {
                    "npc": npcs[1],
                    "phrase": "error",
                    "text": "ежик любит яблоки",
                    "variant": variant,
                    "sample": 0,
                    "error": "Synthetic error",
                }
            )
            if variant != "sol":
                for npc, phrase, sample, text in (
                    (npcs[0], "shared", 1, "ежик любит яблоки в и на"),
                    (npcs[0], "shared", 2, "Белая птица летит"),
                    (npcs[1], "shared", 1, ""),
                    (npcs[0], "empty", 1, ""),
                    (npcs[0], "polite", 1, "Не за что."),
                    (npcs[0], "polite", 2, "Пожалуйста!"),
                ):
                    manual.append(
                        {
                            "npc": npc,
                            "phrase": phrase,
                            "sample": sample,
                            "text": text,
                            "variant": variant,
                            "error": None,
                        }
                    )
        pilot.save_rows(directory / "answers.jsonl", manual)
        report = summary(directory, emit=False)
        assert json.loads((directory / "summary.json").read_text(encoding="utf-8")) == report
        assert words("[SoL][neutral] ЁЖИК 123 likes apples!") == ["ежик", "likes", "apples"]
        assert not trigrams("И на в")
        for variant, result in report["variants"].items():
            assert result["n"] == 16 and result["errors"] == result["empty"] == 1
            assert result["words"]["n"] == 15
            shared = result["common"]
            assert shared["shared_n"] == 5 and shared["fraction"] == 5 / 15
            assert shared["unrelated_n"] == 3 and shared["unrelated_fraction"] == 3 / 15
            assert any(r["trigram"] == "ежик любит яблоки" and len(r["npcs"]) == 3 for r in shared["items"])
            assert result["cliches"] == [
                {
                    "trigram": "темная башня зовет",
                    "family_n": 3,
                    "npcs": sorted((*npcs, fourth)),
                    "answer_n": 4,
                    "phrases": [f"stamp-{i}" for i in range(4)],
                }
            ]
            assert result["own_repeats"] == [{"npc": npcs[0], "trigram": "твой жалкий меч", "phrase_n": 3}]
            if variant != "sol":
                # The shared pair has one equal and two disjoint comparisons: (1 + 0 + 0) / 3.
                stability = result["stability"]
                expected = pilot.distribution([1 / 3, 0])
                assert {k: stability[k] for k in expected} == expected
                assert stability["empty_pairs_n"] == 2
                assert not any(p["phrase"] in ("empty", "polite") for p in stability["pairs"])
                assert (
                    next(p for p in stability["pairs"] if p["npc"] == npcs[0] and p["phrase"] == "shared")[
                        "jaccard"
                    ]
                    == 1 / 3
                )
                assert [r["fraction"] for r in result["common_unrelated_by_sample"]] == [3 / 15, 0, 0]
        print(
            "Selftest passed: 5 modes; pick, resume, retries, slots, no thought, repeat metrics, stability."
        )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_subparsers(dest="mode", required=True)
    command = modes.add_parser("pick")
    command.add_argument("traces", type=Path, nargs="+")
    command.add_argument("--out", type=Path, required=True)
    command = modes.add_parser("think")
    command.add_argument("directory", type=Path)
    command.add_argument("--effort", default="low")
    command = modes.add_parser("run")
    command.add_argument("directory", type=Path)
    command.add_argument("--variant", choices=VARIANTS, nargs="+", action="extend")
    command.add_argument("--samples", type=int, default=3)
    command.add_argument("--slot", type=int, default=0)
    command.add_argument("--quick-at")
    command.add_argument("--effort", default="low")
    command = modes.add_parser("summary")
    command.add_argument("directory", type=Path)
    modes.add_parser("selftest")
    args = parser.parse_args()
    if args.mode == "pick":
        pick(args.traces, args.out)
    elif args.mode == "think":
        asyncio.run(think(args.directory, effort=args.effort))
    elif args.mode == "run":
        asyncio.run(
            run(
                args.directory,
                variants=tuple(dict.fromkeys(args.variant or VARIANTS)),
                samples=args.samples,
                slot=args.slot,
                quick_at=args.quick_at,
                effort=args.effort,
            )
        )
    elif args.mode == "summary":
        summary(args.directory)
    else:
        asyncio.run(selftest())


if __name__ == "__main__":
    main()
