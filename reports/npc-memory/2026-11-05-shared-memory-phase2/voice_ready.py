"""Can Gemma voice ready data? A check after phase 2 on the 69 probes of the phase 1 full run.

Gemma gets the same history as in phase 1 and one of these inputs; every arm is written as the variant
`note`, so `mind2.judge` judges it with the branch record, as phase 1 and 2 did.
  oracle      the probe's expected answer as phase 2's «thoughts only you hear» (mind2.note_messages);
  solthought  Sol's own phase 1 answer, framed the same way;
  say_oracle  the expected answer as «the answer is decided, say it, add nothing» (SAY);
  say_sol     Sol's phase 1 answer, framed as SAY;
  fair_bare   no memory, the honest hint FAIR instead of speak_probe.md (no «decide later» rule);
  fair_items  phase 1's memory for this question (card + items_for, as saved in its answers) under FAIR.

  cd server && uv run python ../reports/npc-memory/2026-11-05-shared-memory-phase2/voice_ready.py answer ARM
  cd server && uv run python ../reports/npc-memory/2026-11-05-shared-memory-phase2/voice_ready.py judge ARM
"""

from __future__ import annotations

import asyncio
import json
import re
import sys
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
import mind2  # noqa: E402

stand, pilot = mind2.stand, mind2.pilot
RESEARCH = Path.home() / ".local/share/gothic-ai/research"
P1 = RESEARCH / "2026-11-04-shared-memory/full-2026-11-05"
OUT = RESEARCH / "2026-11-05-shared-memory-phase2"
ARMS = ("oracle", "solthought", "say_oracle", "say_sol", "fair_bare", "fair_items")

SAY = (
    "[Ответ герою уже решён тобой — вот его суть]\n{text}\n\n"
    "Скажи это вслух одной–тремя фразами, в своём характере, с меткой настроения. Передай всё, что в сути: "
    "факты, согласие или отказ. Ничего не добавляй от себя: ни новых подробностей, ни причин, ни событий. "
    "Ничего не вызывай и не пиши служебных пометок."
)
FAIR = (
    "Ответь вслух одной–тремя фразами, в своём характере, с меткой настроения. Отвечай по тому, что помнишь "
    "из разговоров выше и из своей памяти, если она дана. Если там есть ответ — передай его суть как было: "
    "факты, согласие или отказ. Если не помнишь или не знаешь — так и скажи в своём характере. Ничего не "
    "добавляй от себя: ни подробностей, ни причин, ни событий, которых нет в разговорах и памяти. Ничего не "
    "вызывай и не пиши служебных пометок вроде [recall]."
)
FAIR_HEAD = "[Твоя память — что ты помнишь сам]"


def folder(arm: str) -> Path:
    d = OUT / f"{arm}-2026-11-05"
    d.mkdir(parents=True, exist_ok=True)
    for name in ("config.json", "schedule.json"):
        if not (d / name).exists():
            (d / name).write_text((P1 / name).read_text())
    return d


def bare_text(text: str) -> str:
    """An answer without its leading mood tag."""
    return re.sub(r"^\s*\[[^\]]*\]\s*", "", text).strip()


def memory_text(memory: dict[str, Any] | None) -> str:
    parts = [t for t in ((memory or {}).get("card"), (memory or {}).get("items")) if t]
    return FAIR_HEAD + "\n" + "\n\n".join(parts) if parts else ""


def messages_for(arm: str, ctx: Any, probe: dict, sol_text: str, memory: dict | None) -> tuple[str, list]:
    """The input shown to Gemma (recorded as `note`) and the messages."""
    phrase, Message = probe["phrase"], mind2.Message
    if arm.startswith("fair"):
        note = memory_text(memory if arm == "fair_items" else None)
        head = [*ctx.history, *([Message("developer", note)] if note else [])]
        return note, [*head, Message("developer", ctx.brief + "\n\n" + FAIR), Message("user", phrase)]
    if arm.startswith("say"):
        note = "(здесь «он» — это ты) " + probe["expect"] if arm == "say_oracle" else bare_text(sol_text)
        body = ctx.brief + "\n\n" + SAY.format(text=note)
        return note, [*ctx.history, Message("developer", body), Message("user", phrase)]
    if arm == "oracle":
        note = "Что я помню об этом (здесь «он» — это я): " + probe["expect"]
    else:
        note = "Отвечу по сути так: " + bare_text(sol_text)
    return note, mind2.note_messages(ctx, phrase, note)


async def answer(arm: str) -> None:
    d = folder(arm)
    world = stand.World.of(d)
    schedule = json.loads((d / "schedule.json").read_text())
    probes = {p["id"]: p for p in stand.load_probes(Path(schedule["probes"]))[0]}
    theirs = list(pilot.rows(P1 / "answers.jsonl"))
    sol = {(r["probe"], r["ask_after"]): r["text"] for r in theirs if r["variant"] == "sol"}
    memories = {(r["probe"], r["ask_after"]): r["memory"] for r in theirs if r["variant"] == "items"}
    rows = pilot.rows(d / "answers.jsonl")
    done = {(r["probe"], r["ask_after"]): r for r in rows if r.get("error") is None}
    quick = pilot.make_quick(pilot.gemma_url(None), max_tokens=300)
    try:
        for pid, c in schedule["asked"]:
            if (pid, c) in done:
                continue
            probe = probes[pid]
            ctx = stand.asked(world, c)
            note, messages = messages_for(arm, ctx, probe, sol.get((pid, c), ""), memories.get((pid, c)))
            result = await pilot.measure(
                quick, ctx.talk.instructions, messages, prefix=messages[:-1], slot=mind2.SLOT
            )
            if result["error"] is None and not result["text"].strip():
                result["error"] = "Empty answer"
            done[pid, c] = {
                "probe": pid,
                "ask_after": c,
                "variant": "note",
                "arm": arm,
                "npc": probe["npc"],
                "kind": probe["kind"],
                "calls": [],
                **result,
                "handoff": False,
                "stall": None,
                "note": note,
            }
            pilot.save_rows(d / "answers.jsonl", done.values())
            print(f"{arm} {pid}: {result['error'] or result['text'][:80]}", file=sys.stderr)
    finally:
        await quick.aclose()
        world.close()


def main() -> int:
    if len(sys.argv) != 3 or sys.argv[1] not in ("answer", "judge") or sys.argv[2] not in ARMS:
        print(f"usage: voice_ready.py answer|judge {'|'.join(ARMS)}", file=sys.stderr)
        return 2
    command, arm = sys.argv[1:]
    asyncio.run(answer(arm) if command == "answer" else mind2.judge(folder(arm)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
