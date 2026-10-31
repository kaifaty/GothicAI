"""Run a recorded turn again: the same request to another model, prompt or tool set, without the game.

The first step's request comes from the trace. When the model calls a tool, the recorded result of the
same call (name and arguments) is given back; a call the original turn did not make goes to `resolve`
(e.g. the checks against the recorded snapshot), or the turn is marked as diverged there.
"""

from __future__ import annotations

import asyncio
import time
from collections.abc import Callable, Collection, Mapping, Sequence
from contextlib import aclosing
from typing import Any

from . import npc_tools
from .dialogue import (
    _FIRST_SENTENCE,
    _LETTERS,
    QUICK_HINT,
    REACT_HINT,
    REACTION_S,
    SOUND_SAID,
    SVM_HINT,
    SVM_REPLY_S,
    SVM_WORDS_MAX,
    SentenceSplitter,
    TurnState,
    first_words,
    first_words_outcome,
    is_pure_sound,
    paragraph_end,
    svm_hint,
    svm_text,
    to_game_text,
)
from .llm import Done, Item, LLMBackend, LLMError, Message, TextDelta, ToolCall, ToolResult
from .trace import CallTrace, item_from_json
from .turn import MAX_STEPS, ToolOutcome, go_on, tool_choice, tool_output
from .vanilla import VanillaQuests

Resolver = Callable[[ToolCall, dict[str, Any]], ToolOutcome | None]


def _quick_context(record: dict[str, Any]) -> tuple[list[Item], str]:
    added = record["steps"][0]["added"]
    situation_at = next(i for i in range(len(added) - 1, -1, -1) if added[i].get("role") == "developer")
    situation = next(
        (e["situation"] for e in record.get("events", []) if e["kind"] == "first_words" and "situation" in e),
        None,
    )
    if situation is None:
        situation = added[situation_at]["content"].split(SOUND_SAID.split("{}", 1)[0], 1)[0]
        situation = situation.removesuffix("\n" + QUICK_HINT).rstrip()
    history = [item for data in added[:situation_at] if (item := item_from_json(data)) is not None]
    return history, situation


async def replay_first_words(
    llm: LLMBackend,
    record: dict[str, Any],
    instructions: str,
    *,
    hint: str = REACT_HINT,
    runs: int = 1,
    temperature: float = 0.6,
    previous: Sequence[str] = (),
) -> dict[str, Any]:
    """Replay only the quick first reaction, with independent repetition history for each run."""
    history, situation = _quick_context(record)
    phrase = record["trigger"]["text"]
    request = [*history, Message("developer", situation + "\n" + hint), Message("user", phrase)]
    results = []
    for run in range(runs):
        started = time.monotonic()
        text = ""
        outcome = "said"
        error = ""
        try:
            async with (
                asyncio.timeout(REACTION_S),
                aclosing(
                    llm.stream(instructions=instructions, messages=request, tools=[], temperature=temperature)
                ) as events,
            ):
                async for event in events:
                    if isinstance(event, TextDelta):
                        text += event.text
                        if _FIRST_SENTENCE.search(text) or paragraph_end(text) is not None:
                            break
        except TimeoutError:
            outcome = "late"
        except LLMError as err:
            outcome, error = "error", str(err)
        words, _ = first_words(text)
        if outcome == "said":
            outcome = first_words_outcome(words, previous[run] if run < len(previous) else "")
        result = {
            "text": text,
            "words": words,
            "outcome": outcome,
            "pure_sound": is_pure_sound(words),
            "elapsed_ms": round((time.monotonic() - started) * 1000),
        }
        if error:
            result["error"] = error
        results.append(result)
    return {
        "turn": record.get("id"),
        "npc": record["npc"],
        "phrase": phrase,
        "situation": situation,
        "hint": hint,
        "temperature": temperature,
        "runs": results,
    }


async def replay_svm_lines(
    llm: LLMBackend,
    record: dict[str, Any],
    instructions: str,
    lines: Sequence[dict[str, Any]],
    *,
    names: Mapping[str, str],
    hint: str = SVM_HINT,
    history: bool = False,
    runs: int = 1,
) -> list[dict[str, Any]]:
    """Compare SVM replacements against one NPC's last recorded situation."""
    recorded_history, situation = _quick_context(record)
    results = []
    for line in lines:
        target = (line.get("target") or "").upper()
        target_name = names.get(target, target) if target else None
        prompt = svm_hint(
            line["svm"], line["text"], target_name=target_name,
            self_target=target == record["npc"].upper(), hint=hint,
        )
        request = [*(recorded_history if history else []), Message("developer", situation + "\n" + prompt)]
        attempts = []
        for _ in range(runs):
            started = time.monotonic()
            text = ""
            outcome = "said"
            error = ""
            try:
                async with (
                    asyncio.timeout(SVM_REPLY_S),
                    aclosing(llm.stream(
                        instructions=instructions, messages=request, tools=[], temperature=0.7,
                    )) as events,
                ):
                    async for event in events:
                        if isinstance(event, TextDelta):
                            text += event.text
                            if _FIRST_SENTENCE.search(text) or paragraph_end(text) is not None:
                                break
            except TimeoutError:
                outcome = "late"
            except LLMError as err:
                outcome, error = "error", str(err)
            words, _ = first_words(svm_text(text))
            words = to_game_text(words)
            elapsed = time.monotonic() - started
            if outcome == "said":
                if elapsed >= SVM_REPLY_S:
                    outcome = "late"
                elif len(_LETTERS.findall(words)) > SVM_WORDS_MAX:
                    outcome = "too_long"
                elif not _LETTERS.findall(words):
                    outcome = "nothing"
            attempt = {
                "text": text, "words": words, "outcome": outcome,
                "first_phrase_ms": round(elapsed * 1000),
            }
            if error:
                attempt["error"] = error
            attempts.append(attempt)
        results.append({
            "turn": record.get("id"), "npc": record["npc"], "svm": line["svm"],
            "text": line["text"], "target": target, "situation": situation, "hint": prompt,
            "history": history, "temperature": 0.7, "runs": attempts,
        })
    return results


async def replay(
    llm: LLMBackend,
    record: dict[str, Any],
    instructions: str,
    resolve: Resolver | None = None,
    *,
    after_step: int = 0,
) -> dict[str, Any]:
    tools = [t for t in [*npc_tools.TOOLS, *VanillaQuests.all_tools] if t.name in record.get("tools", [])]
    steps = record.get("steps") or []
    if not steps:
        raise ValueError(f"turn {record.get('id')} has no steps")
    if not 0 <= after_step < len(steps):
        raise ValueError(f"after_step must be between 0 and {len(steps) - 1}")
    request: list[Item] = [i for d in steps[0]["added"] if (i := item_from_json(d)) is not None]
    recorded = [CallTrace(**c) for s in steps for c in s.get("calls", [])]
    out: list[dict[str, Any]] = []
    first_words = " ".join(
        e.get("words", e["text"])
        for e in record.get("events", [])
        if e.get("kind") == "first_words" and e.get("outcome", "said") == "said"
    )
    state = TurnState(reply="replay", splitter=SentenceSplitter(), started=0.0, filler=first_words)
    names = {t.name for t in [*npc_tools.TOOLS, *VanillaQuests.all_tools]} | set(record.get("tools", []))
    said = to_game_text(first_words)
    for n in range(after_step):
        previous, following = steps[n], steps[n + 1]
        if not previous.get("silent"):
            if state.text and not state.text.endswith((" ", "\n")):
                state.text += " "
            said = _said(said, previous.get("text", ""), state, names, record["npc"])
            said = _said(said, "", state, names, record["npc"], flush=True)
        added = [i for d in following["added"] if (i := item_from_json(d)) is not None]
        calls = {c["call_id"]: CallTrace(**c) for c in previous.get("calls", [])}
        for i, item in enumerate(added):
            if isinstance(item, ToolResult) and (call := calls.get(item.call_id)) is not None:
                outcome = ToolOutcome(call.result, call.outcome)  # type: ignore[arg-type]
                output = tool_output(outcome, said) if not previous.get("silent") else outcome.output
                added[i] = ToolResult(item.call_id, output)
        # The conversational model starts a whole new request after the quick model's fight line.
        request = added if previous.get("llm") else [*request, *added]
    said_before = said
    initial_tool_results = [i.output for i in request if isinstance(i, ToolResult)]
    silent = bool(steps[after_step].get("silent"))
    first_step = after_step - sum(bool(s.get("llm")) for s in steps[:after_step])
    diverged = False
    for step in range(first_step, MAX_STEPS):
        started = time.monotonic()
        first: float | None = None
        done: Done | None = None
        if not silent and state.text and not state.text.endswith((" ", "\n")):
            state.text += " "
        async for event in llm.stream(
            instructions=instructions, messages=request, tools=tools, tool_choice=tool_choice(step)
        ):
            if isinstance(event, TextDelta):
                if first is None:
                    first = time.monotonic() - started
                if not silent:
                    said = _said(said, event.text, state, names, record["npc"])
            elif isinstance(event, Done):
                done = event
        assert done is not None
        if not silent:
            said = _said(said, "", state, names, record["npc"], flush=True)
        calls = [i for i in done.items if isinstance(i, ToolCall)]
        outcomes: list[ToolOutcome] = []
        for call in calls:
            match = next((rc for rc in recorded if rc.matches(call)), None)
            outcome = ToolOutcome(match.result, match.outcome) if match else None  # type: ignore[arg-type]
            if outcome is None and resolve is not None:
                outcome = resolve(call, record)
            if outcome is None:
                diverged = True
                outcome = ToolOutcome("(этот вызов при воспроизведении не выполняется)", "ignored")
            outcomes.append(outcome)
        outputs = [tool_output(o, said) if not silent else o.output for o in outcomes]
        out.append(
            {
                "text": done.text.strip(),
                "silent": silent,
                "calls": [
                    {"name": c.name, "arguments": c.arguments, "outcome": o.status, "output": output}
                    for c, o, output in zip(calls, outcomes, outputs, strict=True)
                ],
                "first_token_s": round(first or 0.0, 2),
                "total_s": round(time.monotonic() - started, 2),
            }
        )
        if not go_on(outcomes, bool(state.text.strip())):
            break
        request += [
            *done.items,
            *(ToolResult(c.call_id, output) for c, output in zip(calls, outputs, strict=True)),
        ]
    return {
        "turn": record.get("id"),
        "npc": record.get("npc"),
        "trigger": record.get("trigger"),
        "after_step": after_step,
        "said_before": said_before,
        "initial_tool_results": initial_tool_results,
        "original": [
            {"text": s.get("text", "").strip(), "calls": [c["name"] for c in s.get("calls", [])]}
            for s in steps
        ],
        "replay": out,
        "diverged": diverged,
    }


def _said(
    previous: str,
    text: str,
    state: TurnState,
    names: Collection[str],
    npc: str,
    *,
    flush: bool = False,
) -> str:
    pieces = state.spoken(
        state.moods.flush() if flush else state.moods.feed(text), names, npc, flush=flush
    )
    lines = (to_game_text(state.strip_line(line)) for line in state.lines(pieces, flush=flush))
    return " ".join(p for p in (previous, *lines) if p)
