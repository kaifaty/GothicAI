"""Counterfactual mind turns: on Gemma-routed turns where recorded Sol acted (set_intent, take_offense),
the history shows Gemma's answer without Sol's calls. Does the mind act itself?"""

import asyncio
import importlib.util
import json
import sys
import time
from pathlib import Path

from gothic_ai import wrath
from gothic_ai.llm import Message
from gothic_ai.trace import item_from_json, item_json

ROOT = Path(sys.argv[1])
D = Path(sys.argv[2])
SAMPLES = 2
spec = importlib.util.spec_from_file_location("_mind", ROOT / "reports/npc-agent/2026-11-02-sol-mind/mind.py")
mind = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = mind
spec.loader.exec_module(mind)
mind.MIND_TOOLS.append(wrath.TOOL)


async def main() -> None:
    turns = mind.pilot.rows(D / "turns.jsonl")
    minds = {r["id"]: r for r in mind.pilot.rows(D / "minds.jsonl")}
    answers = {(r["id"], r["variant"]): r for r in mind.pilot.rows(D / "answers.jsonl")}
    prompt = (mind.HERE / "mind_turn.md").read_text(encoding="utf-8")
    chosen = [
        t for t in turns if t["oof_predicted"] is False and {"set_intent", "take_offense"} & set(t["calls"])
    ]
    sol = mind.pilot.make_backend("chatgpt", reasoning_effort="low")
    out = []
    try:
        for t in chosen:
            gemma = answers[(t["id"], "mind")]["text"].lstrip()
            if gemma[:5].lower() == "[sol]":
                gemma = gemma[5:].lstrip()
            history = [item for data in t["history"] if (item := item_from_json(data)) is not None]
            history += [Message("user", t["phrase"]), Message("assistant", gemma)]
            fake = {**t, "history": [item_json(item) for item in history]}
            for sample in range(SAMPLES):
                started = time.monotonic()
                result = await mind.mind_stream(
                    sol, fake, minds[t["id"]]["mind"], prompt, started + mind.TIMEOUT_S
                )
                parsed = mind.parse_mind(result["text"]) if result["error"] is None else {}
                row = {
                    "id": t["id"],
                    "sample": sample,
                    "phrase": t["phrase"],
                    "recorded_calls": t["calls"],
                    "gemma": gemma,
                    "calls": result["calls"],
                    "say": parsed.get("say"),
                    "mind": parsed.get("mind"),
                    "error": result["error"],
                }
                out.append(row)
                print(
                    t["id"],
                    sample,
                    "| recorded:",
                    t["calls"],
                    "| mind:",
                    [c["name"] for c in result["calls"]],
                    result["error"] or "",
                    "|",
                    t["phrase"][:40],
                )
    finally:
        await sol.aclose()
    (D / "initiative.json").write_text(json.dumps(out, ensure_ascii=False, indent=1), encoding="utf-8")


asyncio.run(main())
