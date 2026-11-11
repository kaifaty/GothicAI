"""Can Gemma voice ready data? A check after phase 2 on the 69 probes of the phase 1 full run.

Gemma gets the same history as in phase 1 and one of these inputs; every arm is written as the variant
`note`, so `mind2.judge` judges it with the branch record, as phase 1 and 2 did.
  oracle      the probe's expected answer as phase 2's «thoughts only you hear» (mind2.note_messages);
  solthought  Sol's own phase 1 answer, framed the same way;
  say_oracle  the expected answer as «the answer is decided, say it, add nothing» (SAY);
  say_sol     Sol's phase 1 answer, framed as SAY;
  fair_bare   no memory, the honest hint FAIR instead of speak_probe.md (no «decide later» rule);
  fair_items  phase 1's memory for this question (card + items_for, as saved in its answers) under FAIR;
  gist        the pipeline: Sol gets phase 1's `sol` input plus GIST and writes the gist after the phrase
              (recall runs, other calls are recorded), Gemma voices it under SAY. The gist comes as the
              tail of Gemma's prompt, after the warmed history and brief, so its prefill is measured;
  gist_stall  the gists of `gist` (no new Sol calls) with Gemma's thinking words first: STALL right after
              the phrase, then the gist voiced under SAY with «Ты уже сказал вслух». The judged answer is
              both said in a row.

  cd server && uv run python ../reports/npc-memory/2026-11-05-shared-memory-phase2/voice_ready.py answer ARM
  cd server && uv run python ../reports/npc-memory/2026-11-05-shared-memory-phase2/voice_ready.py judge ARM
"""

from __future__ import annotations

import asyncio
import dataclasses
import json
import re
import sys
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
import mind2  # noqa: E402

from gothic_ai.dialogue import _SENTENCE_END  # noqa: E402

stand, pilot = mind2.stand, mind2.pilot
RESEARCH = Path.home() / ".local/share/gothic-ai/research"
P1 = RESEARCH / "2026-11-04-shared-memory/full-2026-11-05"
OUT = RESEARCH / "2026-11-05-shared-memory-phase2"
ARMS = ("oracle", "solthought", "say_oracle", "say_sol", "fair_bare", "fair_items", "gist", "gist_stall")

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
GIST = (
    "[Голос] Вслух за тебя говорит голос: он уже начал отвечать и скажет твою суть своими словами. Сам ты "
    "вслух не говоришь. Напиши суть ответа герою — то, что голос должен передать: факты, согласие или "
    "отказ, что ты помнишь или чего не знаешь. Одна–две короткие фразы от первого лица, без ремарок, метки "
    "настроения и приветствий. Если нужно действие — вызови инструмент как обычно."
)


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


STALL = (
    "Герой только что это сказал. Ответа ты ещё не знаешь: сначала надо вспомнить или прикинуть, это займёт "
    "пару мгновений. Скажи вслух только это — что вспоминаешь или задумался, — одной короткой фразой до "
    "восьми слов, своими словами, как сказал бы ты сам; можно зацепиться за его слова. Ответа не начинай: "
    "ни да, ни нет, ни фактов, ни оценок."
)
SAID = "\nТы уже сказал вслух: «{said}» — ответ продолжает это, не повторяй его."


def spoken(text: str, words: int) -> str:
    """What sounds of a short line: whole sentences until it has three words, at most `words` words."""
    text, out = bare_text(text), []
    for part in _SENTENCE_END.split(text):
        out += part.split()
        if len(out) >= 3:
            break
    return " ".join(out[:words]).strip()


async def stall_turn(quick: Any, ctx: Any, probe: dict, gist: str) -> tuple[str, dict]:
    """Gemma's thinking words from the brief alone (no history: nothing to answer with yet), then the gist
    voiced after them with the history."""
    Message, phrase = mind2.Message, probe["phrase"]
    head = [*ctx.history, Message("developer", ctx.brief)]
    brief = [Message("developer", ctx.brief)]
    first = await pilot.measure(
        quick,
        ctx.talk.instructions,
        [*brief, Message("user", phrase), Message("developer", STALL)],
        prefix=brief,
        slot=mind2.SLOT,
    )
    stall = spoken(first["text"], 8)
    voice_messages = [
        *head,
        Message("developer", SAY.format(text=gist) + SAID.format(said=stall)),
        Message("user", phrase),
    ]
    result = await pilot.measure(quick, ctx.talk.instructions, voice_messages, prefix=head, slot=mind2.SLOT)
    error = first["error"] or result["error"]
    words = " ".join(t for t in (stall, bare_text(result["text"])) if t)
    result.update(
        text=words,
        error=error,
        voice=result["text"],
        stall=stall,
        stall_first_sentence_s=first.get("first_sentence_s"),
    )
    return gist, result


async def gist_turn(sol: Any, quick: Any, ctx: Any, probe: dict, store: Any) -> tuple[str, dict]:
    """Sol's gist, then Gemma voicing it. Timings: Sol's first token and whole turn, then Gemma's."""
    thinking = dataclasses.replace(ctx, situation=ctx.situation + "\n\n" + GIST)
    sol_result = await stand.sol_answer(sol, thinking, probe["phrase"], store)
    gist = sol_result["text"].strip()
    if sol_result["error"] is not None or not gist:
        return gist, {"text": "", "error": sol_result["error"] or "Empty gist", "sol": sol_result}
    Message = mind2.Message
    messages = [
        *ctx.history,
        Message("developer", ctx.brief),
        Message("developer", SAY.format(text=gist)),
        Message("user", probe["phrase"]),
    ]
    result = await pilot.measure(
        quick, ctx.talk.instructions, messages, prefix=messages[:-2], slot=mind2.SLOT
    )
    voice = result.get("first_sentence_s")
    result["sol"] = sol_result
    result["voice_after_phrase_s"] = sol_result["total_s"] + voice if voice is not None else None
    return gist, result


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
    sol_backend = embed = store = None
    gists = {}
    if arm == "gist_stall":
        gists = {
            (r["probe"], r["ask_after"]): r["note"] for r in pilot.rows(folder("gist") / "answers.jsonl")
        }
    if arm == "gist":
        sol_backend = pilot.make_backend("chatgpt", reasoning_effort=stand.EFFORT)
        embed = stand.EmbedClient(stand.embed_url(None, None))
        store = stand.MemoryStore(stand.journal_copy(d, world.carry), embeddings=embed)
    try:
        for pid, c in schedule["asked"]:
            if (pid, c) in done:
                continue
            probe = probes[pid]
            ctx = stand.asked(world, c)
            if arm == "gist":
                note, result = await gist_turn(sol_backend, quick, ctx, probe, store)
            elif arm == "gist_stall":
                note, result = await stall_turn(quick, ctx, probe, gists[pid, c])
            else:
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
                "stall": result.get("stall"),
                "note": note,
            }
            pilot.save_rows(d / "answers.jsonl", done.values())
            print(f"{arm} {pid}: {result['error'] or result['text'][:80]}", file=sys.stderr)
    finally:
        await quick.aclose()
        if sol_backend is not None:
            await sol_backend.aclose()
        if embed is not None:
            await embed.close()
        if store is not None:
            store.close()
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
