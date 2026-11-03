"""Offline hero-name benchmark. Run from server/; selftest needs no live data."""

from __future__ import annotations

import argparse
import json
import re
from collections import Counter
from collections.abc import Iterator
from pathlib import Path
from tempfile import TemporaryDirectory
from typing import Any

from gothic_ai import characters, lore
from gothic_ai.talk import fix_names, words


def names() -> tuple[dict[str, str], list[str]]:
    cards = {npc: card.name for npc, card in characters.load().items()}
    book = json.loads(lore.LORE.read_text(encoding="utf-8"))
    known = {**{npc.upper(): name for npc, name in book["names"].items()}, **cards}
    return cards, list(known.values())


def live(turns_path: Path, labels_path: Path) -> dict[str, int]:
    cards, known = names()
    turns = [json.loads(line) for line in turns_path.read_text(encoding="utf-8").splitlines() if line]
    labels = json.loads(labels_path.read_text(encoding="utf-8"))
    expected = {**labels["called"], **labels["mentioned"]}
    by_id = {turn["id"]: turn for turn in turns}
    if len(by_id) != len(turns):
        raise ValueError("Turn ids must be unique")
    if missing := expected.keys() - by_id.keys():
        raise ValueError(f"Labelled turns missing: {sorted(missing)}")
    fixed = errors = 0
    for turn in turns:
        nearby = {
            other["npc"].upper() for other in turns
            if other["trace"] == turn["trace"] and abs(other["t"] - turn["t"]) <= 600
        }
        corrected, replacements = fix_names(turn["phrase"], [cards[npc] for npc in sorted(nearby)], known)
        label = expected.get(turn["id"])
        allowed: Counter[tuple[str, str]] = Counter()
        if label is not None:
            before, after = label
            ok = (before, after) in replacements if after is not None else not replacements
            if after is not None:
                allowed[(before, after)] = 1
                fixed += int(ok)
            print(f"{'OK' if ok else 'MISS'} {turn['id']}: {before} -> {after}; {corrected}")
        for before, after in replacements:
            if allowed[(before, after)]:
                allowed[(before, after)] -= 1
            else:
                errors += 1
                print(f"ERROR {turn['id']}: {before} -> {after}; {turn['phrase']}")
    summary = {
        "fixed": fixed,
        "expected": sum(after is not None for _, after in expected.values()),
        "erroneous": errors,
    }
    print(json.dumps(summary))
    return summary


def strings(value: Any) -> Iterator[str]:
    if isinstance(value, str):
        if re.search("[а-яё]", value, re.IGNORECASE):
            yield value
    elif isinstance(value, dict):
        for item in value.values():
            yield from strings(item)
    elif isinstance(value, list):
        for item in value:
            yield from strings(item)


def corpus(story: Path) -> dict[str, Any]:
    _, known = names()
    lines = list(strings(json.loads(story.read_text(encoding="utf-8"))))
    # One call reuses each word's decision across the corpus; no nearby people means no wide matches.
    text = "\n".join(lines)
    _, replacements = fix_names(text, [], known)
    total = len(words(text))
    summary = {
        "lines": len(lines),
        "words": total,
        "replaced": len(replacements),
        "per_1000": 1000 * len(replacements) / total if total else 0,
        "frequent": [
            {"before": before, "after": after, "count": count}
            for (before, after), count in Counter(replacements).most_common(30)
        ],
    }
    print(json.dumps(summary, ensure_ascii=False))
    return summary


def selftest() -> None:
    near = ["Кавалорн", "Ксардас", "Браго", "Вольф", "Бандит"]
    known = [*near, "Лорд Хаген", "Лобарт", "Вульф", "Брайан", "Гарад", "Бастер", "Гром"]
    cases = {
        "Ковалорн, бей его.": "Кавалорн, бей его.",
        "Сардас.": "Ксардас.",
        "Брага, чё твои шавки тут вякают?": "Браго, чё твои шавки тут вякают?",
        "я пойду скажу Брага": "я пойду скажу Браго",
        "я к лорду Хагину": "я к лорду Хагену",
        "Кавалором, бей их": "Кавалорном, бей их",
        "Привет, братан": "Привет, братан",
        "Здарова, бандиты!": "Здарова, бандиты!",
        "пойдём в город": "пойдём в город",
        "Да, мастер.": "Да, мастер.",
        "Вы кроме как ругаться ничего не можете": "Вы кроме как ругаться ничего не можете",
        "Вольф, привет": "Вольф, привет",
        "скажи Лобарту": "скажи Лобарту",
        "с Ксардасом": "с Ксардасом",
    }
    for phrase, expected in cases.items():
        assert fix_names(phrase, near, known)[0] == expected, phrase
    assert fix_names("Сардас, привет", [], known) == ("Сардас, привет", [])
    assert fix_names("Лабарт!", ["Лобарт"], ["Лобарт", "Лобарт"])[1] == []
    with TemporaryDirectory() as directory:
        turns = Path(directory) / "turns.jsonl"
        labels = Path(directory) / "labels.json"
        turns.write_text("\n".join(json.dumps(turn) for turn in [
            {"id": "a", "trace": "one", "t": 1000, "npc": "BAU_4300_ADDON_CAVALORN", "phrase": "Сардас."},
            {"id": "b", "trace": "one", "t": 1600, "npc": "NONE_100_XARDAS", "phrase": "Привет"},
            {"id": "c", "trace": "two", "t": 1000, "npc": "BAU_4300_ADDON_CAVALORN", "phrase": "Сардас."},
            {"id": "d", "trace": "one", "t": 2201, "npc": "BAU_4300_ADDON_CAVALORN", "phrase": "Сардас."},
        ]), encoding="utf-8")
        labels.write_text(json.dumps({"called": {
            "a": ["Сардас", "Ксардас"], "c": ["Сардас", None], "d": ["Сардас", None],
        }, "mentioned": {}}), encoding="utf-8")
        assert live(turns, labels) == {"fixed": 1, "expected": 1, "erroneous": 0}
        story = Path(directory) / "story.json"
        story.write_text(json.dumps({"lines": ["лорду Хагину", "с Ксардасом", "ASCII"]}), encoding="utf-8")
        result = corpus(story)
        assert (result["lines"], result["words"], result["replaced"]) == (2, 4, 1)
    print(f"selftest: {len(cases) + 4} checks passed")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    live_parser = sub.add_parser("live")
    live_parser.add_argument("turns", type=Path)
    live_parser.add_argument("labels", type=Path)
    corpus_parser = sub.add_parser("corpus")
    corpus_parser.add_argument("story", type=Path)
    sub.add_parser("selftest")
    args = parser.parse_args()
    if args.command == "live":
        live(args.turns, args.labels)
    elif args.command == "corpus":
        corpus(args.story)
    else:
        selftest()


if __name__ == "__main__":
    main()
