"""Counterfactual mind turns on Gemma-routed turns: the history shows Gemma's answer without Sol's calls.
Does the mind act itself where recorded Sol acted (set_intent, take_offense), and stay quiet elsewhere?

Run from server/:
    uv run python ../reports/npc-agent/2026-11-02-sol-mind/initiative.py \
        REPO DATA [PROMPT [OUT]] [--flag] [--all]

PROMPT is a prompt file next to mind.py (mind_turn.md by default), OUT the result file in DATA.
--flag tells the mind that the voice said the last line; --all takes every Gemma-routed turn,
not only those where recorded Sol acted.
"""

import asyncio
import importlib.util
import json
import sys
import time
from pathlib import Path

from gothic_ai.llm import Message
from gothic_ai.trace import item_from_json, item_json

ARGS = [arg for arg in sys.argv[1:] if not arg.startswith("--")]
ROOT, D = Path(ARGS[0]), Path(ARGS[1])
PROMPT = ARGS[2] if len(ARGS) > 2 else "mind_turn.md"
OUT = ARGS[3] if len(ARGS) > 3 else "initiative.json"
FLAG = "--flag" in sys.argv
ALL = "--all" in sys.argv
SAMPLES = 2
VOICE = "[Прошлую реплику сказал твой голос — сразу, по мыслям, без раздумий и без действий]"
ACTS = {"set_intent", "take_offense"}

spec = importlib.util.spec_from_file_location("_mind", ROOT / "reports/npc-agent/2026-11-02-sol-mind/mind.py")
mind = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = mind
spec.loader.exec_module(mind)


async def main() -> None:
    turns = mind.pilot.rows(D / "turns.jsonl")
    minds = {r["id"]: r for r in mind.pilot.rows(D / "minds.jsonl")}
    answers = {(r["id"], r["variant"]): r for r in mind.pilot.rows(D / "answers.jsonl")}
    prompt = (mind.HERE / PROMPT).read_text(encoding="utf-8")
    chosen = [t for t in turns if t["oof_predicted"] is False and (ALL or ACTS & set(t["calls"]))]
    sol = mind.pilot.make_backend("chatgpt", reasoning_effort="low")
    out = []
    try:
        for t in chosen:
            gemma = answers[(t["id"], "mind")]["text"].lstrip()
            if gemma[:5].lower() == "[sol]":
                gemma = gemma[5:].lstrip()
            history = [item for data in t["history"] if (item := item_from_json(data)) is not None]
            history += [Message("user", t["phrase"]), Message("assistant", gemma)]
            if FLAG:
                history.append(Message("developer", VOICE))
            fake = {**t, "history": [item_json(item) for item in history]}
            for sample in range(SAMPLES):
                started = time.monotonic()
                result = await mind.mind_stream(
                    sol, fake, minds[t["id"]]["mind"], prompt, started + mind.TIMEOUT_S
                )
                parsed = mind.parse_mind(result["text"]) if result["error"] is None else {}
                out.append(
                    {
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
                )
                print(
                    t["id"],
                    sample,
                    "| recorded:",
                    t["calls"],
                    "| mind:",
                    [call["name"] for call in result["calls"]],
                    result["error"] or "",
                    "|",
                    t["phrase"][:40],
                )
    finally:
        await sol.aclose()
    (D / OUT).write_text(json.dumps(out, ensure_ascii=False, indent=1), encoding="utf-8")


asyncio.run(main())
