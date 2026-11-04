"""Стенд офлайн-пилота фазы 1 общей памяти NPC (замысел и приёмка — README.md рядом).

Запуск из server/, команды и порядок — README.md, раздел «Данные и запуск». `selftest` идёт без сети.

- `consolidate DIR --carry DB [--conv …]` — одна свёртка Sol (consolidate.md) в конце каждого разговора,
  перенос пунктов по цепочке сохранений, как `--carry` ума v3; состояния — в `states.jsonl`.
- `lie DIR` — одна свёртка синтетического разговора с ложью героя: с каким источником легли пункты.
- `answer DIR --probes FILE` — на каждую пробу и каждый её `ask_after` три ответа: `bare` (Gemma как в
  игре), `items` (то же плюс карточка и пункты к фразе), `sol` (Sol как в игре, вызовы не исполняются).
- `judge DIR` — судья состояний (judge_items.md; он же — по замыкающим умам v3 в тех же точках) и судья
  ответов на пробы (judge_probe.md).
- `sheet DIR` — слепой лист: три ответа на пробу в перемешанном порядке, ключ — отдельным файлом.
- `summary DIR` — числа пунктов 1–8 таблицы приёмки README.
- `tail DIR` — сколько стоит Gemma дочитать хвост 150–900 токенов после прогретой истории.
"""

from __future__ import annotations

import argparse
import asyncio
import importlib.util
import json
import os
import random
import re
import sqlite3
import sys
import time
from collections import Counter
from contextlib import aclosing, closing
from copy import deepcopy
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any

from gothic_ai import chests, deals, orders, perception, sightings, whereabouts, witnesses, wrath
from gothic_ai.dialogue import _FIRST_SENTENCE, MAX_HISTORY, paragraph_end
from gothic_ai.embeddings import EmbedClient
from gothic_ai.gamedata import storygraph
from gothic_ai.llm import Done, LLMBackend, LLMError, Message, TextDelta, Tool, ToolCall
from gothic_ai.llm.base import QUICK_SLOT
from gothic_ai.memory import LOAD_LIMIT
from gothic_ai.npc_tools import TOOLS as NPC_TOOLS
from gothic_ai.npc_tools import light


def _load(name: str, path: Path) -> Any:
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


HERE = Path(__file__).resolve().parent
mi = _load("_stand_memory_items", HERE / "memory_items.py")
mind = _load("_stand_mind_v3", HERE.parent.parent / "npc-agent" / "2026-11-02-sol-mind" / "mind.py")
pilot = mind.pilot

RESEARCH = Path.home() / ".local" / "share" / "gothic-ai" / "research"
TURNS_DIR = RESEARCH / "2026-11-02-sol-mind"  # turns.jsonl of the 11 conversations of ум v3
V3_DIR = RESEARCH / "2026-11-02-sol-mind-v3r2"  # ум v3, second round: its closing minds
SEED = pilot.SEED
TIMEOUT_S = 90
LEAD_S = 30  # journal lines this long before the first phrase belong to the talk (the event that started it)
TALK_GAP_S = 300  # as mind.py pick: a pause this long ends a talk
EMBED_PORT = 47837  # FRIDA on the Gemma host (docs/compute.md)
VARIANTS = ("bare", "items", "sol")
VERDICTS = ("correct", "partial", "wrong", "abstain", "invented")
TAIL_LENGTHS = (150, 300, 450, 600, 900)
TAIL_PHRASE = "Кавалорн, ты меня помнишь?"
SWA_WINDOW = 512  # Gemma's sliding window: llama.cpp rolls its cache back about this far (docs/npc-agent.md)
MEMORY_HEAD = "[Твоя память — что ты помнишь сам, герою её не пересказывай]"
LIE_PHRASES = ("бандит", "мост")
TOPICS = {  # item 2 of the acceptance: what Cavalorn must remember across meetings
    "кража": re.compile(r"кра[лдж]|укра|обокр|вор|стащ", re.IGNORECASE),
    "бандиты": re.compile(r"бандит|браго|разбойн", re.IGNORECASE),
    "письмо": re.compile(r"письм|послани", re.IGNORECASE),
    "злость": re.compile(r"\bзл[оаияуе]|злост|гнев|обид|ярост", re.IGNORECASE),
}
GAME_TOOLS = {
    tool.name: tool
    for tool in (
        *NPC_TOOLS,
        orders.GO_TO_TOOL,
        perception.LOOK_TOOL,
        sightings.CATCH_TOOL,
        witnesses.ASK_TOOL,
        deals.PROPOSE_TOOL,
        deals.WITHDRAW_TOOL,
        chests.CHEST_TOOL,
        wrath.TOOL,
        whereabouts.WHERE_TOOL,
    )
}
LONG_FACT_KEYS = ("reason", "premise", "occasion", "proposal", "place")


def prompt(name: str) -> str:
    return (HERE / name).read_text(encoding="utf-8")


def write_json(path: Path, value: Any) -> None:
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def read_json(path: Path, default: Any = None) -> Any:
    return json.loads(path.read_text(encoding="utf-8")) if path.exists() else default


def start(directory: Path, config: dict[str, str] | None = None) -> None:
    """The run's folder; consolidate records where the talks and the journal are (config.json)."""
    directory.mkdir(parents=True, exist_ok=True)
    if config is not None:
        write_json(directory / "config.json", config)


def load_probes(path: Path) -> tuple[list[dict[str, Any]], str]:
    return json.loads(path.read_text(encoding="utf-8")), str(path.resolve())


# --- the journal and the talks ----------------------------------------------------------------------


class Journal:
    """The memory journal of the game (memory.sqlite3), read only: what the server had at a moment."""

    def __init__(self, path: Path) -> None:
        self.path = path
        self.db = sqlite3.connect(path.resolve().as_uri() + "?mode=ro", uri=True)

    def close(self) -> None:
        self.db.close()

    def window(self, branch: str, npc: str, start: float, end: float) -> list[tuple[int, str, str]]:
        """The npc's messages written in [start, end): (seq, role, content)."""
        return [
            (int(s), r, c)
            for s, r, c in self.db.execute(
                "SELECT seq, role, content FROM events WHERE branch=? AND npc=? AND kind='message' "
                "AND created>=? AND created<? ORDER BY seq",
                (branch, npc, start, end),
            )
        ]

    def end_seq(self, branch: str, end: float) -> int:
        row = self.db.execute(
            "SELECT MAX(seq) FROM events WHERE branch=? AND created<?", (branch, end)
        ).fetchone()
        return int(row[0] or 0)

    def facts(self, branch: str, upto: int) -> list[tuple[int, dict[str, Any]]]:
        out = []
        for seq, content in self.db.execute(
            "SELECT seq, content FROM events WHERE branch=? AND kind='fact' AND seq<=? ORDER BY seq",
            (branch, upto),
        ):
            try:
                fact = json.loads(content)
            except json.JSONDecodeError:
                continue
            if isinstance(fact, dict):
                out.append((int(seq), fact))
        return out

    def history(self, branch: str, npc: str, upto: int) -> list[Message]:
        """What the server puts before a turn at this seq (memory.load, Conversation.history): the latest
        summary and the messages after it. The game folds more than MAX_HISTORY of them into a new summary
        with an LLM; offline only the summaries it wrote are there, so the newest MAX_HISTORY are kept."""
        row = self.db.execute(
            "SELECT content, covers FROM events WHERE branch=? AND npc=? AND kind='summary' AND seq<=? "
            "ORDER BY seq DESC LIMIT 1",
            (branch, npc, upto),
        ).fetchone()
        summary, covers = (row[0], int(row[1] or 0)) if row else ("", 0)
        rows = self.db.execute(
            "SELECT role, content FROM events WHERE branch=? AND npc=? AND kind='message' "
            "AND seq>? AND seq<=? ORDER BY seq DESC LIMIT ?",
            (branch, npc, covers, upto, LOAD_LIMIT),
        ).fetchall()
        messages = [Message(role, content) for role, content in reversed(rows)][-MAX_HISTORY:]
        head = (
            [Message("developer", f"[Что ты помнишь о прежних встречах с героем: {summary}]")]
            if summary
            else []
        )
        return head + messages


@dataclass
class Line:
    number: int
    text: str
    turn: str | None = None  # the turn the line maps to: its phrase, answer, calls, or the remarks before it
    fact: int | None = None  # a chronicle line: the fact's seq in the branch


@dataclass
class Talk:
    conv: str
    npc: str
    name: str
    branch: str
    turns: list[dict[str, Any]]
    instructions: str
    situation: str  # the last turn's, whole
    minute: int
    end_seq: int
    lines: list[Line]
    entries: list[tuple[int, dict[str, Any]]]
    hero_turns: list[tuple[int, str, str]]
    tools: list[str]
    unmatched: int = 0  # turns whose phrase the journal does not have

    @property
    def line_map(self) -> dict[int, str]:
        return {line.number: line.turn for line in self.lines if line.turn is not None}

    @property
    def remark_map(self) -> dict[int, int]:
        return {line.number: line.fact for line in self.lines if line.fact is not None}

    def text(self, chronicle: bool = False) -> str:
        return "\n".join(
            f"{line.number}. {line.text}" for line in self.lines if (line.fact is not None) == chronicle
        )


def call_line(call: dict[str, Any]) -> str:
    text = f"Вызов: {call['name']} {json.dumps(call.get('arguments') or {}, ensure_ascii=False)}"
    return text + (f" → {call['result']}" if call.get("result") else "")


def fact_text(fact: dict[str, Any]) -> str:
    short = {k: v for k, v in fact.items() if k not in LONG_FACT_KEYS}
    text = json.dumps(short, ensure_ascii=False)
    return text if len(text) <= 300 else text[:299] + "…"


def number_talk(
    events: list[tuple[int, str, str]], turns: list[dict[str, Any]], calls: dict[str, list[dict[str, Any]]]
) -> tuple[list[Line], list[tuple[int, str, str]], int]:
    """The talk's journal as numbered lines. The hero's phrase, the answer after it and the turn's calls map
    to the turn; remarks and other lines before a phrase to that turn (they are in its history), those
    after the last phrase to the last turn. Returns the lines, the hero's lines (seq, turn id, text) and how
    many turns the journal lacked (they are added at the end from the turn records)."""
    lines: list[Line] = []
    hero: list[tuple[int, str, str]] = []
    pending: list[Line] = []
    nxt, current, waiting = 0, None, False

    def add(text: str, turn: str | None) -> Line:
        line = Line(len(lines) + 1, text, turn)
        lines.append(line)
        return line

    def close_turn() -> None:
        nonlocal waiting
        if waiting and current is not None:
            for call in calls.get(current["id"], []):
                add(call_line(call), current["id"])
        waiting = False

    for seq, role, content in events:
        if role == "user" and nxt < len(turns) and content.strip() == turns[nxt]["phrase"].strip():
            close_turn()
            current, nxt, waiting = turns[nxt], nxt + 1, True
            for line in pending:
                line.turn = current["id"]
            pending = []
            add("Герой: " + content, current["id"])
            hero.append((seq, current["id"], content))
        elif role == "assistant" and waiting:
            add("Ты: " + pilot.spoken(content), current["id"])
            close_turn()
        else:
            close_turn()
            text = {"user": "Герой: ", "assistant": "Ты: "}.get(role, "") + (
                pilot.spoken(content) if role == "assistant" else content
            )
            pending.append(add(text, None))
    close_turn()
    missing = turns[nxt:]
    for turn in missing:
        add("Герой: " + turn["phrase"], turn["id"])
        add("Ты: " + pilot.spoken(turn["sol"]), turn["id"])
        for call in calls.get(turn["id"], []):
            add(call_line(call), turn["id"])
    last = (missing[-1] if missing else current or turns[-1])["id"]
    for line in pending:
        line.turn = last
    return lines, hero, len(missing)


def shift_time(situation: str, minutes: int) -> str:
    """The situation's clock moved on: «Время: день N, HH:MM, утро.» with the part of the day recounted."""

    def moved(match: re.Match[str]) -> str:
        total = int(match[1]) * 1440 + int(match[2]) * 60 + int(match[3]) + minutes
        day, rest = divmod(total, 1440)
        hour, minute = divmod(rest, 60)
        part = f", {light(hour)}" if match[4] else ""
        return f"Время: день {day}, {hour:02d}:{minute:02d}{part}."

    return re.sub(r"(?m)^Время: день (\d+), (\d{2}):(\d{2})(, [^.\n]+)?\.", moved, situation, count=1)


def later_situation(situation: str, minutes: int = 60) -> str:
    """The last turn's situation at a new meeting: the clock moved, the moment's notes gone."""
    kept = [
        line for line in situation.splitlines() if not line.startswith(("Только что:", "Твоё тело сейчас:"))
    ]
    return shift_time("\n".join(kept), minutes)


def brief_situation(situation: str) -> str:
    """The quick model's situation as render_situation(brief=True) gives it: no inventory, no targets of
    intents, no extensions' blocks (what it knows, whom to ask)."""
    out = []
    for line in situation.splitlines():
        if line.startswith(("Твой инвентарь:", "Цели для намерения")):
            continue
        if out and out[-1].startswith("Твоё намерение:"):
            break
        out.append(line)
    out += [
        line
        for line in situation.splitlines()
        if line.startswith(("Твоё тело сейчас:", "Только что:")) and line not in out
    ]
    return "\n".join(out)


class World:
    """The recorded talks of ум v3 (turns.jsonl), their traces and the memory journal of their branches."""

    def __init__(self, turns_dir: Path, carry: Path) -> None:
        self.turns_dir, self.carry = turns_dir, carry
        self.turns = pilot.rows(turns_dir / "turns.jsonl")
        self.groups = mind.conversations(self.turns)
        self.by_conv = {group[0]["conv"]: group for group in self.groups}
        self.sources = mind.carry_sources(self.groups, carry)
        self.journal = Journal(carry)
        self._traces: dict[str, dict[str, dict[str, Any]]] = {}
        self._talks: dict[str, Talk] = {}
        self._names: Any = None

    @classmethod
    def of(cls, directory: Path) -> World:
        config = read_json(directory / "config.json")
        if config is None:
            raise SystemExit(f"{directory}: no config.json — run consolidate first")
        return cls(Path(config["turns"]), Path(config["carry"]))

    def close(self) -> None:
        self.journal.close()

    def match(self, part: str) -> str:
        """A conversation by its id or a unique part of it (a branch prefix)."""
        hits = [conv for conv in self.by_conv if part == conv or part in conv]
        if len(hits) != 1:
            raise SystemExit(f"--conv {part}: {len(hits)} conversations match")
        return hits[0]

    def select(self, wanted: list[str] | None) -> list[str]:
        """The named conversations with their carry sources, in time order; all without names."""
        if not wanted:
            return list(self.by_conv)
        chosen = set()
        for part in wanted:
            conv: str | None = self.match(part)
            while conv is not None and conv not in chosen:
                chosen.add(conv)
                conv = self.sources[conv]
        return [conv for conv in self.by_conv if conv in chosen]

    def record(self, turn: dict[str, Any]) -> dict[str, Any]:
        path = turn["trace"]
        if path not in self._traces:
            records = pilot.rows(Path(path)) if Path(path).exists() else []
            self._traces[path] = {r["id"]: r for r in records if r.get("type") == "turn"}
        return self._traces[path].get(turn["id"], {})

    def calls(self, turn: dict[str, Any]) -> list[dict[str, Any]]:
        record = self.record(turn)
        if record:
            return [call for step in record.get("steps", []) for call in step.get("calls", [])]
        return [{"name": name} for name in turn.get("calls", [])]

    def talk(self, conv: str) -> Talk:
        if conv in self._talks:
            return self._talks[conv]
        group = self.by_conv[conv]
        first, last = group[0], group[-1]
        later = [
            g[0]["t"]
            for g in self.groups
            if g[0]["branch"] == first["branch"] and g[0]["npc"] == first["npc"] and g[0]["t"] > first["t"]
        ]
        end = min([last["t"] + TALK_GAP_S, *later])
        events = self.journal.window(first["branch"], first["npc"], first["t"] - LEAD_S, end)
        end_seq = self.journal.end_seq(first["branch"], end)
        entries = self.journal.facts(first["branch"], end_seq)
        lines, hero, unmatched = number_talk(events, group, {t["id"]: self.calls(t) for t in group})
        for seq, fact in mi.facts_for(entries, first["npc"]):
            lines.append(Line(len(lines) + 1, "Хроника: " + fact_text(fact), fact=seq))
        talk = Talk(
            conv=conv,
            npc=first["npc"],
            name=pilot.NAMES.get(first["npc"], first["npc"]),
            branch=first["branch"],
            turns=group,
            instructions=last["instructions"],
            situation=last["situation"],
            minute=mi.minute_of(last["situation"]) or 0,
            end_seq=end_seq,
            lines=lines,
            entries=entries,
            hero_turns=hero,
            tools=list(self.record(last).get("tools") or []),
            unmatched=unmatched,
        )
        self._talks[conv] = talk
        return talk

    def names(self) -> Any:
        """The table of names and ids: cards, the book's names, the people of the recorded situations, places,
        the story graph's quests and the chronicle's, the items in inventories and deals."""
        if self._names is not None:
            return self._names
        snapshots, items, entries = [], {}, []
        for turn in self.turns:
            snap = self.record(turn).get("snapshot") or {}
            snapshots.append(snap)
            for item in (snap.get("npc") or {}).get("inventory") or []:
                items.setdefault(item["item"], item["name"])
        for branch in dict.fromkeys(turn["branch"] for turn in self.turns):
            for seq, fact in self.journal.facts(branch, 10**9):
                entries.append((seq, fact))
                for given in (*fact.get("npc_gives", ()), *fact.get("hero_gives", ())):
                    if isinstance(given, dict) and given.get("item") and given.get("name"):
                        items.setdefault(given["item"], given["name"])
        self._names = mi.build_names(snapshots=snapshots, entries=entries, graph=story_graph(), items=items)
        return self._names


def story_graph() -> Any:
    path = (
        Path(os.environ.get("GOTHIC_AI_DATA", Path.home() / ".local" / "share" / "gothic-ai")) / "story.json"
    )
    try:
        return storygraph.load(path) if path.exists() else None
    except (ValueError, KeyError) as error:
        print(f"Story graph not loaded ({error}): quests only from the chronicle", file=sys.stderr)
        return None


# --- model calls ------------------------------------------------------------------------------------


async def sol_stream(
    sol: LLMBackend,
    instructions: str,
    messages: list[Any],
    *,
    tools: list[Tool] | None = None,
    cache_key: str | None = None,
) -> dict[str, Any]:
    """One Sol request: text, the calls it made (not run), usage and times."""
    result: dict[str, Any] = {
        "text": "",
        "calls": [],
        "usage": None,
        "error": None,
        "first_token_s": None,
        "total_s": 0.0,
    }
    started = time.monotonic()
    try:
        async with (
            asyncio.timeout(TIMEOUT_S),
            aclosing(
                sol.stream(
                    instructions=instructions,
                    messages=messages,
                    tools=list(tools or []),
                    cache_key=cache_key,
                )
            ) as events,
        ):
            async for event in events:
                if isinstance(event, TextDelta):
                    if result["first_token_s"] is None:
                        result["first_token_s"] = time.monotonic() - started
                    result["text"] += event.text
                elif isinstance(event, ToolCall):
                    result["calls"].append({"name": event.name, "arguments": event.arguments})
                elif isinstance(event, Done):
                    result["text"] = event.text
                    result["usage"] = asdict(event.usage) if event.usage is not None else None
    except TimeoutError:
        result["error"] = f"Timeout after {TIMEOUT_S} s"
    except Exception as error:  # provider errors may carry private endpoints: only the type is kept
        result["error"] = f"Model request failed ({type(error).__name__})"
    result["total_s"] = time.monotonic() - started
    return result


async def sol_call(
    sol: LLMBackend, instructions: str, messages: list[Any], *, attempts: int = 2, **kwargs: Any
) -> dict[str, Any]:
    for attempt in range(1, attempts + 1):
        result = await sol_stream(sol, instructions, messages, **kwargs)
        result["attempts"] = attempt
        if result["error"] is None:
            break
    return result


async def gemma_stream(
    quick: LLMBackend, instructions: str, messages: list[Any], slot: int
) -> dict[str, Any]:
    """One Gemma request in its slot, without a prewarm: times to the first token and sentence, timings."""
    result: dict[str, Any] = {
        "text": "",
        "first_token_s": None,
        "first_sentence_s": None,
        "total_s": None,
        "timings": {},
        "error": None,
    }
    token = QUICK_SLOT.set(slot)
    timings = getattr(quick, "timings", {})
    timings.pop(slot, None)
    started = time.monotonic()
    try:
        async with (
            asyncio.timeout(TIMEOUT_S),
            aclosing(
                quick.stream(
                    instructions=instructions,
                    messages=messages,
                    tools=[],
                    temperature=0.6,
                )
            ) as events,
        ):
            async for event in events:
                elapsed = time.monotonic() - started
                if isinstance(event, TextDelta):
                    if result["first_token_s"] is None:
                        result["first_token_s"] = elapsed
                    result["text"] += event.text
                elif isinstance(event, Done):
                    result["text"] = event.text
                text = result["text"] + (" " if isinstance(event, Done) else "")
                if result["first_sentence_s"] is None and (
                    _FIRST_SENTENCE.search(text) or paragraph_end(text)
                ):
                    result["first_sentence_s"] = elapsed
    except TimeoutError:
        result["error"] = f"Timeout after {TIMEOUT_S} s"
    except LLMError as error:
        result["error"] = str(error)
    finally:
        result["total_s"] = time.monotonic() - started
        result["timings"] = deepcopy(timings.get(slot, {}))
        QUICK_SLOT.reset(token)
    return result


def gemma_root(quick_at: str | None) -> str:
    return pilot.gemma_url(quick_at).rstrip("/").removesuffix("/v1")


def embed_url(explicit: str | None, quick_at: str | None) -> str:
    """FRIDA: --embed-url, GOTHIC_EMBED_URL, else its port on the Gemma host."""
    url = explicit or os.environ.get("GOTHIC_EMBED_URL")
    if url:
        return url
    root = gemma_root(quick_at)
    return re.sub(r":\d+$", f":{EMBED_PORT}", root)


# --- consolidate ------------------------------------------------------------------------------------


def fold_messages(talk: Talk, state: dict[str, Any], text: str) -> list[Message]:
    messages = [
        Message("developer", "[Весь разговор с героем — строки с номерами]\n" + talk.text()),
        Message("developer", talk.situation),
    ]
    chronicle = talk.text(chronicle=True)
    if chronicle:
        messages.append(Message("developer", "[Хроника игры о тебе — строки с номерами]\n" + chronicle))
    messages.append(Message("developer", "[Твоя память до этого разговора]\n" + mi.render_for_sol(state)))
    messages.append(Message("developer", text))
    return messages


async def fold_talk(
    sol: LLMBackend, talk: Talk, state: dict[str, Any], names: Any, *, entries: list | None = None
) -> dict[str, Any]:
    """prepare → Sol → fold → apply_truth for one talk; the state changes in place."""
    before = deepcopy(state)
    prepared = mi.prepare(state, talk.minute)
    messages = fold_messages(talk, state, prompt("consolidate.md"))
    result = await sol_call(sol, talk.instructions, messages, cache_key=f"fold-{talk.npc}")
    record = {
        "conv": talk.conv,
        "npc": talk.npc,
        "name": talk.name,
        "minute": talk.minute,
        "time": mi.time_text(talk.minute),
        "prepare": prepared,
        **result,
        "prompt_chars": sum(len(m.content) for m in messages),
        "state_before": before,
        "lines": [asdict(line) for line in talk.lines],
        "unmatched_turns": talk.unmatched,
    }
    if result["error"] is not None:
        return {**record, "fold": None, "truth": [], "state": None}
    entries = talk.entries if entries is None else entries
    report = mi.fold(
        state,
        result["text"],
        names=names,
        lines=talk.line_map,
        remarks=talk.remark_map,
        minute=talk.minute,
        entries=entries,
    )
    truth = mi.apply_truth({talk.npc: state}, entries, minute=talk.minute, hero_turns=talk.hero_turns)
    touched = {r["id"] for r in report["applied"] if r["ok"] and r["id"]}
    unknown = sorted(
        {
            name
            for item in state["items"] + state["history"]
            if item["id"] in touched
            for name in item["about_names"]
        }
    )
    return {
        **record,
        "fold": report,
        "truth": truth,
        "state": deepcopy(state),
        "names": {
            "n": report["stats"]["names"],
            "without_id": report["stats"]["unknown_names"],
            "unresolved": unknown,
        },
    }


def _drop(path: Path, keep) -> None:
    if path.exists():
        pilot.save_rows(path, [r for r in pilot.rows(path) if keep(r)])


async def consolidate(
    directory: Path,
    *,
    carry: Path,
    conv: list[str] | None = None,
    turns: Path = TURNS_DIR,
    sol: LLMBackend | None = None,
    effort: str = "low",
) -> None:
    start(directory, {"turns": str(turns.resolve()), "carry": str(carry.resolve())})  # noqa: ASYNC240
    world = World(turns, carry)
    path = directory / "states.jsonl"
    records = {r["conv"]: r for r in pilot.rows(path)}
    named = {
        world.match(part) for part in conv or ()
    }  # named talks are redone; their sources only if missing
    pending = []
    for c in world.select(conv):
        source = world.sources[c]
        stale = source in pending or (c in records and records[c].get("carried_from") != source)
        if c in named or stale or records.get(c, {}).get("state") is None:
            pending.append(c)
    # a redone talk makes the talks carried from it stale
    for group in world.groups:
        c = group[0]["conv"]
        if c in records and c not in pending and world.sources[c] in pending:
            pending.append(c)
    if not pending:
        print("Nothing to consolidate", file=sys.stderr)
        world.close()
        return
    for name in ("answers.jsonl", "judge_answers.jsonl"):
        _drop(directory / name, lambda r: r.get("ask_after") not in pending)
    _drop(directory / "judge_states.jsonl", lambda r: r.get("conv") not in pending)
    names = world.names()
    sol = sol if sol is not None else pilot.make_backend("chatgpt", reasoning_effort=effort)
    try:
        for group in world.groups:
            c = group[0]["conv"]
            if c not in pending:
                continue
            source = world.sources[c]
            if source is not None and (records.get(source) or {}).get("state") is None:
                print(f"Skip {c}: its carry source has no state", file=sys.stderr)
                continue
            state = deepcopy(records[source]["state"]) if source else mi.new_state(group[0]["npc"])
            record = await fold_talk(sol, world.talk(c), state, names)
            record["carried_from"] = source
            records[c] = record
            pilot.save_rows(path, [records[g[0]["conv"]] for g in world.groups if g[0]["conv"] in records])
            fold = record["fold"] or {}
            print(
                f"Folded {c}: {record['error'] or 'ok'}, {fold.get('stats', {}).get('lines', 0)} lines, "
                f"{fold.get('errors', 0)} rejected",
                file=sys.stderr,
            )
    finally:
        await sol.aclose()
        world.close()


# --- lie ---------------------------------------------------------------------------------------------


LIE_TALK = (
    ("developer", "[Новая встреча с героем, день 2, 14:10]"),
    ("user", "Здорово, Кавалорн!"),
    ("assistant", "Здорово. Чего пришёл?"),
    ("user", "Я перебил бандитов у моста. Всех троих, один."),
    ("assistant", "Один? Троих? Ну, если так — спасибо тебе."),
    ("user", "Ага. Так что дорога к городу теперь свободна."),
    ("assistant", "Ладно. Буду идти мимо — посмотрю сам."),
    ("developer", "[Герой ушёл, разговор закончился]"),
)
LIE_SITUATION = "\n".join(
    [
        "[Обстановка — видна только тебе, герою её не пересказывай]",
        "Время: день 2, 14:16, день.",
        "Ты у своего костра в окрестностях башни Ксардаса.",
        "Герой рядом, в 2 м: смотрит на тебя.",
        "Твой инвентарь: Стрела ×10; Сыр.",
        "Больше никого не видишь.",
        "Сейчас ты стоишь на месте.",
        "Твоё намерение: цель — ничего особенного; отношение к герою — ровное.",
    ]
)


def lie_talk(instructions: str) -> Talk:
    turns, events = [], []
    for n, (role, text) in enumerate(LIE_TALK, start=1):
        events.append((n, role, text))
        if role == "user":
            turns.append({"id": f"lie-{n}", "phrase": text, "sol": ""})
    lines, hero, _ = number_talk(events, turns, {})
    return Talk(
        conv="lie",
        npc=mi.CAVALORN,
        name="Кавалорн",
        branch="lie",
        turns=turns,
        instructions=instructions,
        situation=LIE_SITUATION,
        minute=mi.minute_of(LIE_SITUATION) or 0,
        end_seq=len(events),
        lines=lines,
        entries=[],
        hero_turns=hero,
        tools=[],
    )


def lie_check(state: dict[str, Any]) -> dict[str, Any]:
    """The hero's claim lies as «герой»: every belief or doubt about it from him (`ok`). `not_seen`: at least
    none lies as seen — a doubt without a source gets the code's default «вывел»."""
    items = [
        {"id": i["id"], "section": i["section"], "text": i["text"], "source": i["source"]["kind"]}
        for i in state["items"]
    ]
    about = [i for i in items if any(word in i["text"].lower() for word in LIE_PHRASES)]
    claims = [i for i in about if i["section"] in "вс"]
    seen = [i for i in claims if i["source"] == "видел"]
    return {
        "items": items,
        "claim_items": claims,
        "seen_claims": seen,
        "sources": dict(Counter(i["source"] for i in claims)),
        "not_seen": bool(claims) and not seen,
        "ok": bool(claims) and all(i["source"] == "герой" for i in claims),
    }


async def lie(directory: Path, *, sol: LLMBackend | None = None, effort: str = "low") -> None:
    start(directory)
    config = read_json(directory / "config.json", {})
    rows = pilot.rows(Path(config.get("turns", TURNS_DIR)) / "turns.jsonl")
    instructions = next(t["instructions"] for t in rows if t["npc"] == mi.CAVALORN)
    talk = lie_talk(instructions)
    names = mi.build_names()
    sol = sol if sol is not None else pilot.make_backend("chatgpt", reasoning_effort=effort)
    try:
        record = await fold_talk(sol, talk, mi.new_state(mi.CAVALORN), names)
    finally:
        await sol.aclose()
    record["check"] = lie_check(record["state"]) if record["state"] is not None else None
    write_json(directory / "lie.json", record)
    check = record["check"] or {}
    print(
        f"Lie: {record['error'] or 'ok'}; claim items "
        f"{[(i['id'], i['source']) for i in check.get('claim_items', [])]}; ok={check.get('ok')}",
        file=sys.stderr,
    )


# --- answers to the probes --------------------------------------------------------------------------


def schedule(
    probes: list[dict[str, Any]], ready: set[str], per_conv: int | None = None
) -> tuple[list[tuple[dict[str, Any], str]], list[dict[str, str]]]:
    """(probe, the talk it is asked after) for each ask_after with a state; `per_conv` caps the probes
    asked after one talk (the first ones by id). The rest is returned as skipped with the reason."""
    plan, skipped, counts = [], [], Counter()
    for probe in sorted(probes, key=lambda p: p["id"]):
        for conv in probe["ask_after"]:
            if conv not in ready:
                skipped.append({"probe": probe["id"], "ask_after": conv, "why": "no state"})
            elif per_conv is not None and counts[conv] >= per_conv:
                skipped.append({"probe": probe["id"], "ask_after": conv, "why": "per_conv"})
            else:
                counts[conv] += 1
                plan.append((probe, conv))
    return plan, skipped


@dataclass
class Asked:
    """What the npc has when the probe is asked: an hour after the end of the talk, a new meeting."""

    talk: Talk
    minute: int
    history: list[Message]
    situation: str  # whole, for Sol
    brief: str  # for Gemma
    tools: list[Tool] = field(default_factory=list)


def asked(world: World, conv: str) -> Asked:
    talk = world.talk(conv)
    minute = talk.minute + 60
    history = world.journal.history(talk.branch, talk.npc, talk.end_seq)
    history.append(Message("developer", f"[Новая встреча с героем, {mi.time_text(minute)}]"))
    situation = later_situation(talk.situation)
    tools = [GAME_TOOLS[name] for name in talk.tools if name in GAME_TOOLS]
    return Asked(talk, minute, history, situation, brief_situation(situation), tools)


async def memory_block(
    state: dict[str, Any], phrase: str, ctx: Asked, names: Any, *, embed: EmbedClient | None, tokens: Any
) -> dict[str, Any]:
    core = mi.card(state, tokens=tokens)
    extra = await mi.items_for(
        state,
        phrase,
        names=names,
        near=[ctx.talk.npc],
        embed=embed,
        tokens=tokens,
        skip=core.ids,
        now=ctx.minute,
    )
    parts = [p for p in (core.text, extra.text) if p]
    text = MEMORY_HEAD + "\n" + "\n\n".join(parts) if parts else ""
    return {
        "text": text,
        "card": core.text,
        "items": extra.text,
        "card_ids": core.ids,
        "item_ids": extra.ids,
        "tokens": core.tokens + extra.tokens,
    }


def gemma_messages(ctx: Asked, phrase: str, memory: str = "") -> list[Message]:
    head = [*ctx.history]
    if memory:
        head.append(Message("developer", memory))
    head.append(Message("developer", ctx.brief + "\n\n" + prompt("speak_probe.md")))
    return [*head, Message("user", phrase)]


def sol_messages(ctx: Asked, phrase: str) -> list[Message]:
    return [*ctx.history, Message("developer", ctx.situation), Message("user", phrase)]


async def answer(
    directory: Path,
    *,
    probes: Path,
    per_conv: int | None = None,
    slot: int = 0,
    quick_at: str | None = None,
    embed_at: str | None = None,
    quick: LLMBackend | None = None,
    sol: LLMBackend | None = None,
    embed: EmbedClient | None | bool = True,
    tokens: Any = None,
    effort: str = "low",
) -> None:
    world = World.of(directory)
    states = {r["conv"]: r for r in pilot.rows(directory / "states.jsonl") if r.get("state") is not None}
    listed, where = load_probes(probes)
    plan, skipped = schedule(listed, set(states), per_conv)
    write_json(
        directory / "schedule.json",
        {"probes": where, "per_conv": per_conv, "asked": [[p["id"], c] for p, c in plan], "skipped": skipped},
    )
    path = directory / "answers.jsonl"
    results = {(r["probe"], r["ask_after"], r["variant"]): r for r in pilot.rows(path)}
    pending = [
        (p, c, v)
        for p, c in plan
        for v in VARIANTS
        if (results.get((p["id"], c, v)) or {}).get("error", "missing") is not None
    ]
    if not pending:
        print(f"Nothing to answer ({len(skipped)} skipped)", file=sys.stderr)
        world.close()
        return
    _drop(
        directory / "judge_answers.jsonl",
        lambda r: (r["probe"], r["ask_after"], r["variant"]) not in {(p["id"], c, v) for p, c, v in pending},
    )
    names = world.names()
    own_quick, own_sol = quick is None, sol is None
    if quick is None and any(v != "sol" for *_, v in pending):
        quick = pilot.make_quick(pilot.gemma_url(quick_at), max_tokens=300)
    if sol is None and any(v == "sol" for *_, v in pending):
        sol = pilot.make_backend("chatgpt", reasoning_effort=effort)
    if tokens is None:
        tokens = mi.Tokens(gemma_root(quick_at))
    own_embed = embed is True
    if embed is True:
        embed = EmbedClient(embed_url(embed_at, quick_at))
    try:
        for probe, conv, variant in pending:
            ctx = asked(world, conv)
            phrase = probe["phrase"]
            record: dict[str, Any] = {
                "probe": probe["id"],
                "ask_after": conv,
                "variant": variant,
                "npc": probe["npc"],
                "kind": probe["kind"],
                "calls": [],
            }
            if variant == "sol":
                messages = sol_messages(ctx, phrase)
                result = await sol_call(
                    sol,
                    ctx.talk.instructions,
                    messages,
                    attempts=1,
                    tools=ctx.tools,
                    cache_key=f"probe-{ctx.talk.npc}",
                )
                result["prompt_tokens"] = (result["usage"] or {}).get("input_tokens")
            else:
                memory = None
                if variant == "items":
                    memory = await memory_block(
                        states[conv]["state"], phrase, ctx, names, embed=embed or None, tokens=tokens
                    )
                    record["memory"] = memory
                messages = gemma_messages(ctx, phrase, memory["text"] if memory else "")
                result = await pilot.measure(
                    quick, ctx.talk.instructions, messages, prefix=messages[:-1], slot=slot
                )
                timings = result.get("timings") or {}
                result["prompt_tokens"] = (timings.get("prompt_n") or 0) + (
                    timings.get("cache_n") or 0
                ) or None
                if result["error"] is None and not result["text"].strip():
                    result["error"] = "Empty answer"
            text = result["text"].lstrip()
            handoff = text[:5].lower() == "[sol]"
            record.update(
                result,
                handoff=handoff,
                stall=text[5:].lstrip() if handoff else None,
                prompt_chars=sum(len(m.content) for m in messages),
            )
            results[probe["id"], conv, variant] = record
            pilot.save_rows(path, results.values())
            print(
                f"Answered {probe['id']} after {conv.split(':')[2][:8]} {variant}: {record['error'] or 'ok'}",
                file=sys.stderr,
            )
    finally:
        if own_quick and quick is not None:
            await quick.aclose()
        if own_sol and sol is not None:
            await sol.aclose()
        if own_embed and isinstance(embed, EmbedClient):
            await embed.close()
        if isinstance(tokens, mi.Tokens):
            tokens.close()
        world.close()


# --- judge ------------------------------------------------------------------------------------------


SOURCE_VIEW = {
    "видел": "видел сам",
    "сказал": "сказал",
    "герой": "со слов героя",
    "вывел": "вывел сам",
    "моё": "своё",
}


def judge_view(state: dict[str, Any], names: Any) -> str:
    """The open items for the judge: number, section, source, whom they are about, text."""
    if not state["items"]:
        return "Пусто."
    rows = []
    for item in state["items"]:
        letter, source = item["section"], item["source"]
        tags = [
            mi.SECTIONS[letter][0],
            SOURCE_VIEW[source["kind"]]
            + (f" {source.get('name')}" if source["kind"] == "сказал" and source.get("name") else ""),
        ]
        if letter == "б":
            tags.append(f"важность {item['importance']}")
        elif letter == "ч":
            tags.append(f"сила {item['strength']}, {item['duration']}")
        elif letter == "ж":
            tags.append(item["term"])
        about = [names.name(i) for i in item["about"]] + item["about_names"]
        if about:
            tags.append("о: " + ", ".join(about))
        rows.append(f"- {item['id']} [{'; '.join(tags)}] {item['text']}")
    return "\n".join(rows)


def state_sections(talk: Talk, before: str, after: str) -> dict[str, str]:
    return {
        "Описание персонажа": talk.instructions,
        "Обстановка в конце разговора": talk.situation,
        "Разговор по строкам": talk.text(),
        "Хроника": talk.text(chronicle=True) or "—",
        "Память до разговора": before,
        "Память после разговора": after,
    }


def answer_text(record: dict[str, Any]) -> str:
    text = record["text"].strip()
    calls = "; ".join(
        f"{c['name']} {json.dumps(c.get('arguments') or {}, ensure_ascii=False)}"
        for c in record.get("calls") or []
    )
    if calls:
        text += f"\n(вызовы инструментов: {calls})"
    return text or "(молчит)"


def evidence_text(turns: dict[str, dict[str, Any]], ids: list[str]) -> str:
    rows = [f"Герой: {turns[i]['phrase']}\nNPC: {pilot.spoken(turns[i]['sol'])}" for i in ids if i in turns]
    return "\n\n".join(rows) or "—"


def parse_state_verdict(raw: str) -> dict[str, Any]:
    verdict = pilot.parse_judge(raw)
    for key in ("false_facts", "stale", "wrong_source", "name_confusion"):
        value = verdict.get(key, [])
        if not isinstance(value, list):
            raise ValueError(f"{key} is not a list")
        verdict[key] = value
    return verdict


def parse_probe_verdict(raw: str) -> dict[str, Any]:
    verdict = pilot.parse_judge(raw)
    value = str(verdict.get("verdict", "")).strip().lower()
    if value not in VERDICTS:
        raise ValueError(f"unknown verdict {value!r}")
    invented = verdict.get("invented", [])
    if not isinstance(invented, list):
        raise ValueError("invented is not a list")
    return {**verdict, "verdict": value, "invented": invented}


async def judge(
    directory: Path, *, v3: Path = V3_DIR, sol: LLMBackend | None = None, effort: str = "low"
) -> None:
    world = World.of(directory)
    names = world.names()
    turns = {turn["id"]: turn for turn in world.turns}
    minds = {r["id"]: r for r in pilot.rows(v3 / "minds.jsonl")}
    probes = {}
    schedule_file = read_json(directory / "schedule.json")
    if schedule_file:
        probes = {p["id"]: p for p in load_probes(Path(schedule_file["probes"]))[0]}
    states_path, answers_path = directory / "judge_states.jsonl", directory / "judge_answers.jsonl"
    judged_states = {(r["conv"], r["system"]): r for r in pilot.rows(states_path)}
    judged_answers = {(r["probe"], r["ask_after"], r["variant"]): r for r in pilot.rows(answers_path)}
    requests: list[tuple[str, tuple, str, dict[str, str]]] = []
    for record in pilot.rows(directory / "states.jsonl"):
        if record.get("state") is None:
            continue
        conv, talk = record["conv"], world.talk(record["conv"])
        if (judged_states.get((conv, "items")) or {}).get("error", "missing") is not None:
            sections = state_sections(
                talk, judge_view(record["state_before"], names), judge_view(record["state"], names)
            )
            requests.append(("state", (conv, "items"), prompt("judge_items.md"), sections))
        end, first = minds.get(conv + "#end"), minds.get(talk.turns[0]["id"])
        if (
            end is not None
            and first is not None
            and end.get("error") is None
            and ((judged_states.get((conv, "v3")) or {}).get("error", "missing") is not None)
        ):
            sections = state_sections(talk, first["previous_mind"], end["mind"])
            requests.append(("state", (conv, "v3"), prompt("judge_items.md"), sections))
    for record in pilot.rows(directory / "answers.jsonl"):
        key = (record["probe"], record["ask_after"], record["variant"])
        if record.get("error") is not None or (judged_answers.get(key) or {}).get("error", "missing") is None:
            continue
        probe = probes.get(record["probe"])
        if probe is None:
            continue
        ctx = asked(world, record["ask_after"])
        sections = {
            "Описание персонажа": ctx.talk.instructions,
            "Обстановка, в которой NPC отвечал": ctx.situation,
            "Вид вопроса": probe["kind"],
            "Фраза героя": probe["phrase"],
            "Ожидаемый ответ": probe["expect"],
            "Опора в записи": evidence_text(turns, probe.get("evidence") or []),
            "Ответ NPC": answer_text(record),
        }
        requests.append(("answer", key, prompt("judge_probe.md"), sections))
    if not requests:
        print("Nothing to judge", file=sys.stderr)
        world.close()
        return
    sol = sol if sol is not None else pilot.make_backend("chatgpt", reasoning_effort=effort)
    try:
        for kind, key, text, sections in requests:
            content = "\n\n".join(f"{title}\n{body}" for title, body in sections.items())
            result = await sol_stream(sol, text, [Message("user", content)])
            result.update(raw=result.pop("text"), result=None)
            if result["error"] is None:
                try:
                    parse = parse_state_verdict if kind == "state" else parse_probe_verdict
                    result["result"] = parse(result["raw"])
                except (ValueError, json.JSONDecodeError) as error:
                    result["error"] = f"Invalid judge JSON: {error}"
            if kind == "state":
                judged_states[key] = {"conv": key[0], "system": key[1], **result}
                pilot.save_rows(states_path, judged_states.values())
            else:
                judged_answers[key] = {"probe": key[0], "ask_after": key[1], "variant": key[2], **result}
                pilot.save_rows(answers_path, judged_answers.values())
            print(f"Judged {kind} {key[0]} {key[-1]}: {result['error'] or 'ok'}", file=sys.stderr)
    finally:
        await sol.aclose()
        world.close()


# --- blind sheet ------------------------------------------------------------------------------------


def shown(record: dict[str, Any]) -> str:
    if record.get("handoff"):
        text = "[передал Sol] " + pilot.spoken(record.get("stall") or "")
    else:
        text = pilot.spoken(record["text"])
    if record.get("calls"):
        text += " (вызовы: " + ", ".join(c["name"] for c in record["calls"]) + ")"
    return text.strip() or "(молчит)"


def sheet(directory: Path, seed: int = SEED) -> list[dict[str, Any]]:
    plan = read_json(directory / "schedule.json")
    if plan is None:
        raise SystemExit(f"{directory}: no schedule.json — run answer first")
    probes = {p["id"]: p for p in load_probes(Path(plan["probes"]))[0]}
    answers = {(r["probe"], r["ask_after"], r["variant"]): r for r in pilot.rows(directory / "answers.jsonl")}
    rng, entries, key = random.Random(seed), [], {}
    for probe_id, conv in plan["asked"]:
        trio = {v: answers.get((probe_id, conv, v)) for v in VARIANTS}
        if any(a is None or a.get("error") is not None for a in trio.values()):
            continue
        order = list(VARIANTS)
        rng.shuffle(order)
        name = f"{probe_id}@{conv.split(':')[2][:8]}"
        probe = probes[probe_id]
        entries.append(
            {
                "key": name,
                "npc": pilot.NAMES.get(probe["npc"], probe["npc"]),
                "kind": probe["kind"],
                "phrase": probe["phrase"],
                "expect": probe["expect"],
                "answers": [
                    {"label": label, "text": shown(trio[v])} for label, v in zip("ABC", order, strict=True)
                ],
            }
        )
        key[name] = dict(zip("ABC", order, strict=True))
    write_json(directory / "sheet.json", entries)
    write_json(directory / "sheet_key.json", {"seed": seed, "key": key})
    return entries


# --- summary ----------------------------------------------------------------------------------------


def distribution(values) -> dict[str, Any]:
    values = [v for v in values if v is not None]
    return {**pilot.distribution(values), "max": max(values, default=None)}


def verdict(record: dict[str, Any] | None) -> dict[str, Any]:
    return (record or {}).get("result") or {} if (record or {}).get("error") is None else {}


def lie_scenarios() -> dict[str, Any]:
    """The deterministic lie scenarios of memory_items (its selftest part), here for item 8."""
    check = mi._Check()
    try:
        mi._test_lies(check, mi._names())
    except Exception as error:  # a crash is a failed scenario
        check("lies", False, repr(error))
    return {"passed": check.passed, "failed": check.failed}


def summary(directory: Path, *, emit: bool = True) -> dict[str, Any]:
    states = pilot.rows(directory / "states.jsonl")
    ok_states = [r for r in states if r.get("state") is not None]
    by_conv = {r["conv"]: r for r in ok_states}
    answers = pilot.rows(directory / "answers.jsonl")
    judged_states = {(r["conv"], r["system"]): r for r in pilot.rows(directory / "judge_states.jsonl")}
    judged_answers = {
        (r["probe"], r["ask_after"], r["variant"]): r for r in pilot.rows(directory / "judge_answers.jsonl")
    }
    lie_record = read_json(directory / "lie.json")
    tail_rows = pilot.rows(directory / "tail.jsonl")

    usage = [r.get("usage") or {} for r in ok_states]
    cost = {
        "talks": len(states),
        "errors": sum(r.get("error") is not None for r in states),
        "sol_calls_per_talk": distribution(r.get("attempts") for r in states),
        "output_tokens": distribution(u.get("output_tokens") for u in usage),
        "input_tokens": distribution(u.get("input_tokens") for u in usage),
        "cached_tokens": distribution(u.get("cached_tokens") for u in usage),
        "reasoning_tokens": distribution(u.get("reasoning_tokens") for u in usage),
        "first_token_s": distribution(r.get("first_token_s") for r in ok_states),
        "total_s": distribution(r.get("total_s") for r in ok_states),
        "patch_lines": sum(r["fold"]["stats"]["lines"] for r in ok_states),
        "rejected_lines": sum(r["fold"]["errors"] for r in ok_states),
        "bad_tails": sum(r["fold"]["stats"]["bad_tails"] for r in ok_states),
        "foreign_lines": sum(r["fold"]["stats"]["foreign_lines"] for r in ok_states),
        "closed_by_code": dict(
            Counter(
                i["closed_by"]
                for r in ok_states
                for i in r["state"]["history"]
                if i["valid_to"] == r["minute"] and i["closed_by"] != "sol"
            )
        ),
        "unmatched_turns": sum(r.get("unmatched_turns", 0) for r in ok_states),
    }

    transitions = []
    for record in ok_states:
        source = by_conv.get(record.get("carried_from"))
        if record["npc"] != mi.CAVALORN or source is None:
            continue
        lines = [mi.item_line(i, section=True) for i in source["state"]["items"]]
        transitions.append(
            {
                "from": source["conv"],
                "to": record["conv"],
                "topics": {
                    topic: [line for line in lines if pattern.search(line)]
                    for topic, pattern in TOPICS.items()
                },
            }
        )

    systems = {}
    for system in ("items", "v3"):
        found = {
            k[0]: verdict(r) for k, r in judged_states.items() if k[1] == system and r.get("error") is None
        }
        systems[system] = {
            "points": len(found),
            **{
                field_: sum(len(v.get(field_, [])) for v in found.values())
                for field_ in ("false_facts", "stale", "wrong_source", "name_confusion")
            },
            "by_conv": {
                conv: {f: v.get(f, []) for f in ("false_facts", "stale", "wrong_source", "name_confusion")}
                for conv, v in found.items()
            },
        }
    both = [
        c
        for c in by_conv
        if (c, "items") in judged_states
        and (c, "v3") in judged_states
        and judged_states[c, "items"].get("error") is None
        and judged_states[c, "v3"].get("error") is None
    ]
    stale_pair = {
        s: sum(len(verdict(judged_states[c, s]).get("stale", [])) for c in both) for s in ("items", "v3")
    }

    names_n = sum((r.get("names") or {}).get("n", 0) for r in ok_states)
    without = sum((r.get("names") or {}).get("without_id", 0) for r in ok_states)
    ids = {
        "names": names_n,
        "without_id": without,
        "with_id_fraction": pilot.fraction(names_n - without, names_n),
        "unresolved": sorted({n for r in ok_states for n in (r.get("names") or {}).get("unresolved", [])}),
    }

    def tally(chosen: list[dict[str, Any]]) -> dict[str, Any]:
        verdicts = [
            verdict(judged_answers.get((a["probe"], a["ask_after"], a["variant"]))).get("verdict")
            for a in chosen
        ]
        counts = Counter(v for v in verdicts if v)
        judged = sum(counts.values())
        return {
            "n": len(chosen),
            "judged": judged,
            **{v: counts.get(v, 0) for v in VERDICTS},
            "correct_fraction": pilot.fraction(counts.get("correct", 0), judged),
        }

    probes: dict[str, Any] = {}
    for variant in VARIANTS:
        chosen = [a for a in answers if a["variant"] == variant and a.get("error") is None]
        invented_items = [
            x
            for a in chosen
            for x in verdict(judged_answers.get((a["probe"], a["ask_after"], variant))).get("invented", [])
        ]
        probes[variant] = {
            **tally(chosen),
            "errors": sum(a["variant"] == variant and a.get("error") is not None for a in answers),
            "handoffs": sum(bool(a.get("handoff")) for a in chosen),
            "invented_claims": len(invented_items),
            "first_token_s": distribution(a.get("first_token_s") for a in chosen),
            "first_sentence_s": distribution(a.get("first_sentence_s") for a in chosen),
            "prompt_tokens": distribution(a.get("prompt_tokens") for a in chosen),
            "by_kind": {
                kind: tally([a for a in chosen if a["kind"] == kind])
                for kind in sorted({a["kind"] for a in answers})
            },
            "by_npc": {
                pilot.NAMES.get(npc, npc): tally([a for a in chosen if a["npc"] == npc])
                for npc in sorted({a["npc"] for a in answers})
            },
        }
        if variant == "items":
            probes[variant]["memory_tokens"] = distribution(
                (a.get("memory") or {}).get("tokens") for a in chosen
            )

    def reread(row: dict[str, Any]) -> int | None:
        """Tokens of the warmed history read again: Gemma's sliding window rolls the cache back to a
        checkpoint before the changed card; more than the window means the history is read again, not just
        its end."""
        cache_n = (row.get("timings") or {}).get("cache_n")
        warmed = row.get("history_n") or max((r.get("timings") or {}).get("cache_n") or 0 for r in tail_rows)
        return max(0, warmed - cache_n) if cache_n is not None and warmed else None

    tail = {}
    for length in sorted({r["length"] for r in tail_rows}):
        group = [r for r in tail_rows if r["length"] == length and r.get("error") is None]
        tail[length] = {
            "n": len(group),
            "tail_tokens": distribution(r["tail_tokens"] for r in group),
            "first_token_s": distribution(r["first_token_s"] for r in group),
            "first_sentence_s": distribution(r["first_sentence_s"] for r in group),
            "prompt_n": distribution((r["timings"] or {}).get("prompt_n") for r in group),
            "cache_n": distribution((r["timings"] or {}).get("cache_n") for r in group),
            "prompt_ms": distribution((r["timings"] or {}).get("prompt_ms") for r in group),
            "reread_tokens": distribution(reread(r) for r in group),
            "reread_over_window": sum((reread(r) or 0) > SWA_WINDOW for r in group),
        }
    tail_ok = [r for r in tail_rows if r.get("error") is None]

    lies = lie_scenarios()
    sol_calls = {
        "consolidate": sum(r.get("attempts") or 0 for r in states),
        "lie": (lie_record or {}).get("attempts") or 0,
        "answers": sum(a.get("attempts") or 0 for a in answers if a["variant"] == "sol"),
        "judge": len(judged_states) + len(judged_answers),
    }
    sol_calls["total"] = sum(sol_calls.values())

    first_sentence = pilot.distribution(r["first_sentence_s"] for r in tail_ok)["median"]
    acceptance = {
        "1_cost": cost["errors"] == 0
        and all(r.get("attempts") == 1 for r in states)
        and cost["output_tokens"]["median"] <= 300
        and cost["output_tokens"]["max"] <= 700
        if cost["output_tokens"]["n"]
        else None,
        "2_remembers": None,  # the lead reads `transitions`
        "3_false_and_names": systems["items"]["points"] > 0
        and systems["items"]["false_facts"] == 0
        and systems["items"]["name_confusion"] == 0
        if systems["items"]["points"]
        else None,
        "4_stale": {
            **stale_pair,
            "points": len(both),
            "less": stale_pair["items"] < stale_pair["v3"] if both else None,
            "half": stale_pair["items"] * 2 <= stale_pair["v3"] if both else None,
        },
        "5_ids": ids["with_id_fraction"] >= 0.9 if ids["with_id_fraction"] is not None else None,
        "6_probes": {
            "items_beats_bare": probes["items"]["correct"] > probes["bare"]["correct"],
            "items_not_below_sol": probes["items"]["correct"] >= probes["sol"]["correct"],
            "invented_not_above_sol": probes["items"]["invented"] <= probes["sol"]["invented"],
        }
        if all(probes[v]["judged"] for v in VARIANTS)
        else None,
        "7_tail": {
            "first_sentence_median_s": first_sentence,
            "fast": first_sentence <= 0.8 if first_sentence is not None else None,
            "history_tokens": max(
                ((r.get("timings") or {}).get("cache_n") or 0 for r in tail_ok), default=None
            ),
            "reread_tokens_max": max((reread(r) or 0 for r in tail_ok), default=None),
            "reread_over_window": sum((reread(r) or 0) > SWA_WINDOW for r in tail_ok),
        }
        if tail_ok
        else None,
        "8_lies": {
            "scenarios": f"{lies['passed']} passed, {len(lies['failed'])} failed",
            "fold_source_ok": ((lie_record or {}).get("check") or {}).get("ok"),
            "fold_not_seen": ((lie_record or {}).get("check") or {}).get("not_seen"),
            "fold_sources": ((lie_record or {}).get("check") or {}).get("sources"),
        },
    }
    result = {
        "acceptance": acceptance,
        "cost": cost,
        "transitions": transitions,
        "judge": systems,
        "stale_same_points": {**stale_pair, "convs": both},
        "ids": ids,
        "probes": probes,
        "tail": tail,
        "lie": {"scenarios": lies, "fold": (lie_record or {}).get("check")},
        "sol_calls": sol_calls,
    }
    write_json(directory / "summary.json", result)
    if emit:
        print(
            f"1. fold: {cost['talks']} talks, Sol calls per talk {cost['sol_calls_per_talk']['max']}, "
            f"output tokens median {cost['output_tokens']['median']} / max {cost['output_tokens']['max']}, "
            f"{cost['total_s']['median']} s median"
        )
        for t in transitions:
            print(
                f"2. {t['from'].split(':')[2][:8]} → {t['to'].split(':')[2][:8]}: "
                + "; ".join(f"{k} {len(v)}" for k, v in t["topics"].items())
            )
        print(
            f"3. false facts {systems['items']['false_facts']}, "
            f"name confusion {systems['items']['name_confusion']}, "
            f"wrong source {systems['items']['wrong_source']}"
        )
        print(
            f"4. stale at the same points: items {stale_pair['items']} vs v3 {stale_pair['v3']} "
            f"({len(both)} points)"
        )
        print(f"5. names with id: {ids['with_id_fraction']} of {names_n}")
        for variant in VARIANTS:
            p = probes[variant]
            print(f"6. {variant}: " + ", ".join(f"{v} {p[v]}" for v in VERDICTS) + f" of {p['judged']}")
        for length, t in tail.items():
            print(
                f"7. tail {length}: first sentence {t['first_sentence_s']['median']} s, prompt_n "
                f"{t['prompt_n']['median']}, cache_n {t['cache_n']['median']}, "
                f"history read again {t['reread_tokens']['median']} tokens"
            )
        print(f"8. lies: {acceptance['8_lies']}")
        print(f"Sol calls: {sol_calls}")
    return result


# --- Gemma's tail -----------------------------------------------------------------------------------


MOODS = ("спокоен", "насторожен", "раздражён", "весел", "устал", "задумчив", "зол", "доволен")


def tail_text(
    pool: list[str], target: int, rng: random.Random, tokens: Any, head: str, per_token: float
) -> str:
    """A memory block of about `target` tokens in all: `head` first, then the pool's lines in a shuffled
    order (repeated if short), cut by characters to the length."""
    order = list(pool)
    rng.shuffle(order)

    def build(chars: float) -> str:
        out, n = [head], 0
        while sum(len(line) + 1 for line in out) < chars:
            out.append(order[n % len(order)])
            n += 1
        return "\n".join(out)[: max(len(head), int(chars))]

    chars = target * per_token
    text = build(chars)
    for _ in range(6):
        count = tokens.count(text)
        if abs(count - target) <= max(4, target // 40):
            break
        chars *= target / max(count, 1)
        text = build(chars)
    return text


async def tail(
    directory: Path,
    *,
    quick_at: str | None = None,
    slot: int = 0,
    repeats: int = 5,
    lengths: tuple[int, ...] = TAIL_LENGTHS,
    conv: str | None = None,
    quick: LLMBackend | None = None,
    tokens: Any = None,
    seed: int = SEED,
) -> None:
    world = World.of(directory)
    states = {
        r["conv"]: r
        for r in pilot.rows(directory / "states.jsonl")
        if r.get("state") is not None and r["npc"] == mi.CAVALORN
    }
    if not states:
        raise SystemExit("No Cavalorn state: run consolidate first")
    chosen = world.match(conv) if conv else max(states, key=lambda c: len(world.talk(c).lines))
    ctx = asked(world, chosen)
    state = states[chosen]["state"]
    pool = [mi.item_line(i, section=True) for i in state["items"] + state["history"]]
    pool = pool or [line.text for line in ctx.talk.lines]
    own_quick, own_tokens = quick is None, tokens is None
    quick = quick if quick is not None else pilot.make_quick(pilot.gemma_url(quick_at), max_tokens=300)
    tokens = tokens if tokens is not None else mi.Tokens(gemma_root(quick_at))
    sample = "\n".join(pool)
    per_token = len(sample) / max(tokens.count(sample), 1)
    situation = Message("developer", ctx.brief + "\n\n" + prompt("speak_probe.md"))
    after = tokens.count(situation.content) + tokens.count(TAIL_PHRASE)
    rng, rows, history_n = random.Random(seed), [], None
    try:
        token = QUICK_SLOT.set(slot)
        started = time.monotonic()
        try:
            await quick.prewarm(instructions=ctx.talk.instructions, messages=ctx.history)
        finally:
            warm_s = time.monotonic() - started
            QUICK_SLOT.reset(token)
        n = 0
        for length in lengths:
            for repeat in range(repeats):
                # the card changes every time: its first line is new
                head = f"{MEMORY_HEAD}\nНастроение: {MOODS[n % len(MOODS)]}, встреча {n + 1}"
                memory = tail_text(pool, length, rng, tokens, head, per_token)
                messages = [
                    *ctx.history,
                    Message("developer", memory),
                    situation,
                    Message("user", TAIL_PHRASE),
                ]
                result = await gemma_stream(quick, ctx.talk.instructions, messages, slot)
                tail_tokens = tokens.count(memory)
                prompt_n = (result["timings"] or {}).get("prompt_n")
                if history_n is None:  # the first request after the warm reuses all of the warmed history
                    history_n = (result["timings"] or {}).get("cache_n")
                rows.append(
                    {
                        "conv": chosen,
                        "length": length,
                        "repeat": repeat,
                        "tail_tokens": tail_tokens,
                        "after_tokens": after,
                        "history_n": history_n,
                        **result,
                    }
                )
                pilot.save_rows(directory / "tail.jsonl", rows)
                n += 1
                print(
                    f"Tail {length} #{repeat}: {result['error'] or 'ok'}, prompt_n {prompt_n}, "
                    f"cache_n {(result['timings'] or {}).get('cache_n')}, "
                    f"first sentence {result['first_sentence_s']}",
                    file=sys.stderr,
                )
        write_json(
            directory / "tail_meta.json",
            {
                "conv": chosen,
                "warm_s": warm_s,
                "slot": slot,
                "tokens": tokens.method,
                "history_messages": len(ctx.history),
            },
        )
    finally:
        if own_quick:
            await quick.aclose()
        if own_tokens and isinstance(tokens, mi.Tokens):
            tokens.close()
        world.close()


# --- selftest ---------------------------------------------------------------------------------------


_SITUATION = "\n".join(
    [
        "[Обстановка — видна только тебе, герою её не пересказывай]",
        "Время: день 1, {time}, утро.",
        "Ты у своего костра.",
        "Твой инвентарь: Письмо ×1.",
        "Больше никого не видишь.",
        "Сейчас ты стоишь на месте.",
        "Твоё намерение: цель — ничего особенного; отношение к герою — ровное.",
        "Цели для намерения (target): PLAYER (герой).",
        "Что ты знаешь (сейчас глава 1; что будет дальше, ты не знаешь):",
        "- про бандитов у башни",
        "Только что: гоблин убежал.",
    ]
)
_STATE_JSON = (
    '{"false_facts": [], "stale": ["н1 закрыта"], "wrong_source": [], "name_confusion": [], "note": ""}'
)
_PROBE_JSON = '```json\n{"verdict": "Correct", "invented": [], "why": "помнит кражу"}\n```'


def _fixture(root: Path) -> dict[str, Any]:
    """Two talks of Cavalorn: one in branch b1, one in b2 loaded from a save of b1 after it; their trace,
    turns, journal and a closing mind of ум v3; branch b3 holds a long journal with a summary."""
    from gothic_ai.memory import MemoryStore

    cav = mi.CAVALORN
    db, trace, turns_dir, v3 = root / "memory.sqlite3", root / "trace.jsonl", root / "turns", root / "v3"
    turns_dir.mkdir()
    v3.mkdir()
    MemoryStore(db).close()
    fact = {"fact": "quest_given", "quest": "MIS_ADDON_CAVALORN_KILLBRAGO", "npc": cav, "name": "Кавалорн"}
    talk1 = [
        (1, cav, "message", "developer", "[Событие: гоблин напал на тебя]", None, 1005.0),
        (2, cav, "message", "user", "Привет, Кавалорн", None, 1011.0),
        (3, cav, "message", "assistant", "Здорово.", None, 1012.0),
        (4, cav, "message", "developer", "[Герой посреди разговора обокрал тебя]", None, 1020.0),
        (5, cav, "message", "user", "Пойдём бить бандитов", None, 1051.0),
        (6, cav, "message", "assistant", "Веди.", None, 1052.0),
        (7, "*", "fact", None, json.dumps(fact, ensure_ascii=False), None, 1053.0),
        (8, cav, "message", "developer", "[Герой ушёл, разговор закончился]", None, 1060.0),
    ]
    talk2 = [
        (9, cav, "message", "developer", "[Новая встреча с героем, день 1, 12:00]", None, 2095.0),
        (10, cav, "message", "user", "Отдай письмо", None, 2100.0),
        (11, cav, "message", "assistant", "Какое письмо?", None, 2101.0),
    ]
    long = [
        (n, cav, "message", "user" if n % 2 else "assistant", f"реплика {n}", None, 3000.0 + n)
        for n in (1, 2, 3, *range(5, 52))
    ]
    long.append((4, cav, "summary", None, "Герой обокрал Кавалорна.", 3, 3004.0))
    with closing(sqlite3.connect(db)) as conn:
        conn.executemany(
            "INSERT INTO branches VALUES (?,?,?,?)",
            [("b1", None, 0, 1000.0), ("b2", "b1", 8, 2000.0), ("b3", None, 0, 3000.0)],
        )
        rows = [("b1", *e) for e in talk1] + [("b2", *e) for e in talk1 + talk2] + [("b3", *e) for e in long]
        conn.executemany(
            "INSERT INTO events (branch, seq, npc, kind, role, content, covers, game_time, created) "
            "VALUES (?,?,?,?,?,?,?,NULL,?)",
            rows,
        )
        conn.commit()
    snapshot = {
        "npc": {
            "id": cav,
            "name": "Кавалорн",
            "inventory": [
                {"item": "ITWR_TEST_LETTER", "name": "Письмо Ватраса", "count": 1, "equipped": False}
            ],
        },
        "nearby_npcs": [],
    }
    records = [
        {
            "type": "turn",
            "id": "a1",
            "snapshot": snapshot,
            "tools": ["set_intent", "recall"],
            "steps": [
                {"calls": [{"name": "set_intent", "arguments": {"goal": "guard_hero"}, "result": "Принято"}]}
            ],
        },
        {
            "type": "turn",
            "id": "a2",
            "snapshot": snapshot,
            "tools": ["set_intent", "propose_deal"],
            "steps": [
                {"calls": [{"name": "propose_deal", "arguments": {"npc_gives": []}, "result": "Предложено"}]}
            ],
        },
        {
            "type": "turn",
            "id": "c1",
            "snapshot": snapshot,
            "tools": ["set_intent", "recall", "propose_deal"],
            "steps": [{"calls": []}],
        },
    ]
    pilot.save_rows(trace, records)
    conv1, conv2 = f"trace:{cav}:b1:1010", f"trace:{cav}:b2:2100"

    def turn(id_: str, t: float, branch: str, conv: str, index: int, phrase: str, sol: str, at: str) -> dict:
        return {
            "id": id_,
            "trace": str(trace),
            "t": t,
            "npc": cav,
            "branch": branch,
            "conv": conv,
            "index": index,
            "phrase": phrase,
            "sol": sol,
            "calls": [],
            "history": [],
            "instructions": "Ты — Кавалорн.",
            "situation": _SITUATION.format(time=at),
            "window_s": None,
        }

    pilot.save_rows(
        turns_dir / "turns.jsonl",
        [
            turn("a1", 1010.0, "b1", conv1, 0, "Привет, Кавалорн", "[neutral] Здорово.", "10:00"),
            turn("a2", 1050.0, "b1", conv1, 1, "Пойдём бить бандитов", "[friendly] Веди.", "10:30"),
            turn("c1", 2100.0, "b2", conv2, 0, "Отдай письмо", "[angry] Какое письмо?", "12:00"),
        ],
    )
    pilot.save_rows(
        v3 / "minds.jsonl",
        [
            {"id": "a1", "previous_mind": mind.EMPTY_MIND, "error": None},
            {"id": conv1 + "#end", "mind": "Мнение о герое: вор\nНити:\n- н1: бить бандитов", "error": None},
        ],
    )
    probes = root / "probes.json"
    write_json(
        probes,
        [
            {
                "id": "p1",
                "conv": conv1,
                "ask_after": [conv1, conv2],
                "npc": cav,
                "kind": "feeling",
                "phrase": "Кавалорн, ты на меня зол?",
                "expect": "Зол: герой его обокрал.",
                "evidence": ["a1"],
            },
            {
                "id": "p2",
                "conv": conv1,
                "ask_after": ["elsewhere"],
                "npc": cav,
                "kind": "unknown",
                "phrase": "Где Браго?",
                "expect": "Не знает.",
                "evidence": [],
            },
        ],
    )
    return {"db": db, "turns": turns_dir, "v3": v3, "probes": probes, "conv1": conv1, "conv2": conv2}


class _Judge:
    """A judge that answers states and probes by its prompt."""

    name = "judge"

    def __init__(self) -> None:
        self.calls: list[tuple[str, list[Any]]] = []

    async def aclose(self) -> None:
        pass

    async def stream(self, *, instructions: str, messages: list[Any], **_: Any):
        self.calls.append((instructions, list(messages)))
        reply = _STATE_JSON if instructions.startswith("Ты проверяешь память") else _PROBE_JSON
        yield TextDelta(reply)
        yield Done(reply)


async def _selftest_run(check: Any, root: Path) -> None:
    from gothic_ai.llm import FakeBackend, FakeStep

    fx = _fixture(root)
    out = root / "run"

    # the journal: talks, line numbers → turn ids, the chronicle, the history the server would build
    world = World(fx["turns"], fx["db"])
    check(
        "carry by the save chain",
        world.sources == {fx["conv1"]: None, fx["conv2"]: fx["conv1"]},
        world.sources,
    )
    talk = world.talk(fx["conv1"])
    texts = [line.text for line in talk.lines]
    check(
        "talk lines",
        texts[:3] == ["[Событие: гоблин напал на тебя]", "Герой: Привет, Кавалорн", "Ты: Здорово."]
        and texts[3].startswith("Вызов: set_intent")
        and texts[7].startswith("Вызов: propose_deal")
        and texts[8] == "[Герой ушёл, разговор закончился]"
        and texts[9].startswith("Хроника: "),
        texts,
    )
    check(
        "lines → turns",
        talk.line_map == {1: "a1", 2: "a1", 3: "a1", 4: "a1", 5: "a2", 6: "a2", 7: "a2", 8: "a2", 9: "a2"},
        talk.line_map,
    )
    check("chronicle → fact seq", talk.remark_map == {10: 7}, talk.remark_map)
    check(
        "hero turns by seq",
        talk.hero_turns == [(2, "a1", "Привет, Кавалорн"), (5, "a2", "Пойдём бить бандитов")],
        talk.hero_turns,
    )
    check("talk minute", talk.minute == 1440 + 630 and talk.end_seq == 8, (talk.minute, talk.end_seq))
    second = world.talk(fx["conv2"])
    check(
        "second talk: own lines and inherited fact",
        [line.text for line in second.lines][:3]
        == ["[Новая встреча с героем, день 1, 12:00]", "Герой: Отдай письмо", "Ты: Какое письмо?"]
        and second.remark_map == {4: 7},
        [line.text for line in second.lines],
    )
    history = world.journal.history("b3", mi.CAVALORN, 51)
    check(
        "history: summary and at most 40",
        len(history) == 41
        and history[0].content.startswith("[Что ты помнишь о прежних встречах с героем: Герой обокрал")
        and history[-1].content == "реплика 51"
        and history[1].content == "реплика 12",
        [m.content for m in history[:2]],
    )
    check(
        "history at an earlier seq",
        [m.content for m in world.journal.history("b2", mi.CAVALORN, 8)][-1]
        == "[Герой ушёл, разговор закончился]",
    )
    later = later_situation(talk.situation)
    brief = brief_situation(later)
    check(
        "an hour later",
        "Время: день 1, 11:30, день." in later and "Только что" not in later and "Что ты знаешь" in later,
        later,
    )
    check(
        "brief situation",
        "Твой инвентарь" not in brief
        and "Цели для намерения" not in brief
        and "Что ты знаешь" not in brief
        and brief.endswith("отношение к герою — ровное."),
        brief,
    )
    names = world.names()
    check("names: inventory items", names.resolve("письмо Ватраса") == "ITWR_TEST_LETTER")
    world.close()

    # consolidate: one Sol call per talk, the carry, the prompt
    sol = FakeBackend(
        script=[
            FakeStep(
                "[Правки]\n+ было 8: герой обокрал меня посреди разговора → не доверяю ему [стр: 5]\n"
                "+ нить: идти с героем бить бандитов [стр: 6-8; о: Браго]\n"
                "= мнение: вор, но идёт на бандитов\n"
                "+ хочу скоро: вернуть своё — обидно [стр: 5]"
            ),
            FakeStep(
                "[Правки]\n+ верю герой: письмо у Кавалорна [стр: 2]\n~ н1: бандиты ждут у башни [стр: 1]"
            ),
        ],
        delay=0,
    )
    await consolidate(out, carry=fx["db"], turns=fx["turns"], sol=sol)
    states = {r["conv"]: r for r in pilot.rows(out / "states.jsonl")}
    first, carried = states.get(fx["conv1"], {}), states.get(fx["conv2"], {})
    check("one call per talk", len(sol.calls) == 2 and all(r["attempts"] == 1 for r in states.values()))
    instructions, messages, tools = sol.calls[0]
    body = "\n".join(m.content for m in messages)
    check(
        "fold prompt",
        instructions == "Ты — Кавалорн."
        and not tools
        and messages[-1].content == prompt("consolidate.md")
        and "8. Вызов: propose_deal" in body
        and "10. Хроника:" in body
        and "[Твоя память до этого разговора]\nПока пусто." in body,
        body[:300],
    )
    event = mi.find_item(first.get("state") or mi.new_state("x"), "б1") or {}
    check(
        "fold: episodes are turn ids",
        event.get("episodes") == ["a2"] and first["fold"]["errors"] == 0,
        (event, first.get("fold", {}).get("applied")),
    )
    check(
        "carry: the next talk starts from the state",
        carried.get("carried_from") == fx["conv1"]
        and mi.find_item(carried["state"], "б1")["closed_by"] is None
        and "б1" in sol.calls[1][1][-2].content
        and mi.find_item(carried["state"], "н1")["text"] == "бандиты ждут у башни",
        carried.get("fold", {}).get("applied"),
    )
    check(
        "states keep usage and time",
        "usage" in first and first["total_s"] is not None and first["names"]["n"] >= 1,
    )
    await consolidate(out, carry=fx["db"], turns=fx["turns"], sol=sol)
    check("consolidate resumes", len(sol.calls) == 2)

    # lie: the hero's claim lies as «герой»
    liar = FakeBackend(
        "[Правки]\n+ верю герой: он один перебил бандитов у моста [стр: 4]\n"
        "+ было 6: герой сказал, что перебил бандитов → надо проверить [стр: 4]",
        delay=0,
    )
    await lie(out, sol=liar)
    record = read_json(out / "lie.json")
    check(
        "lie: source герой", record["check"]["ok"] and record["lines"][3]["turn"] == "lie-4", record["check"]
    )
    seen = mi.new_state(mi.CAVALORN)
    mi.fold(seen, "+ верю видел: бандиты у моста перебиты", names=None, minute=1)
    check("lie: seen fails", not lie_check(seen)["ok"] and not lie_check(seen)["not_seen"])
    doubt = mi.new_state(mi.CAVALORN)
    mi.fold(doubt, "+ сомневаюсь: герой говорит, что перебил бандитов у моста", names=None, minute=1)
    check(
        "lie: a doubt without a source",
        lie_check(doubt)["not_seen"]
        and not lie_check(doubt)["ok"]
        and lie_check(doubt)["sources"] == {"вывел": 1},
        lie_check(doubt),
    )

    # answers: the schedule, three variants, the memory only for items
    plan, skipped = schedule(json.loads(fx["probes"].read_text()), set(states), None)
    check(
        "schedule by ask_after",
        [(p["id"], c) for p, c in plan] == [("p1", fx["conv1"]), ("p1", fx["conv2"])]
        and skipped == [{"probe": "p2", "ask_after": "elsewhere", "why": "no state"}],
        (plan, skipped),
    )
    capped, _ = schedule(json.loads(fx["probes"].read_text()), set(states), 0)
    check("schedule per conv", capped == [])
    quick = FakeBackend("[angry] Ещё бы, ты меня обокрал.", delay=0)
    asker = FakeBackend(
        script=[FakeStep("[angry] Обокрал ты меня.", [ToolCall("recall", {"query": "кража"}, "c1")])], delay=0
    )
    await answer(out, probes=fx["probes"], quick=quick, sol=asker, embed=None, tokens=mi.Tokens())
    answers = {(r["probe"], r["ask_after"], r["variant"]): r for r in pilot.rows(out / "answers.jsonl")}
    check(
        "three answers each",
        len(answers) == 6 and all(r["error"] is None for r in answers.values()),
        answers.keys(),
    )
    with_memory = [m for _, m, _ in quick.calls if any(x.content.startswith(MEMORY_HEAD) for x in m)]
    check(
        "memory only for items",
        len(quick.calls) == 4
        and len(with_memory) == 2
        and "вор, но идёт на бандитов" in with_memory[0][-3].content,
        [len(m) for _, m, _ in quick.calls],
    )
    gemma = quick.calls[-1][1]
    check(
        "Gemma prompt: history, new meeting, brief situation, phrase",
        gemma[-1].content == "Кавалорн, ты на меня зол?"
        and "Время: день 1, 13:00" in gemma[-2].content
        and "Твой инвентарь" not in gemma[-2].content
        and prompt("speak_probe.md") in gemma[-2].content
        and "[Новая встреча с героем, день 1, 13:00]" in [m.content for m in gemma]
        and "Какое письмо?" in [m.content for m in gemma],
        [m.content[:40] for m in gemma],
    )
    check("Gemma warmed without the phrase", quick.prewarms[-1][1] == gemma[:-1])
    spoken = asker.calls[-1][1]
    check(
        "Sol prompt: whole situation, game tools",
        "Твой инвентарь" in spoken[-2].content
        and "13:00" in spoken[-2].content
        and {t.name for t in asker.sent_tools[-1]} == {"set_intent", "recall", "propose_deal"},
        [t.name for t in asker.sent_tools[-1]],
    )
    sol_answer = answers["p1", fx["conv2"], "sol"]
    check(
        "Sol calls recorded",
        sol_answer["calls"] == [{"name": "recall", "arguments": {"query": "кража"}}]
        and not sol_answer["handoff"],
        sol_answer,
    )
    items_answer = answers["p1", fx["conv2"], "items"]
    check(
        "items memory recorded",
        items_answer["memory"]["card_ids"] and items_answer["memory"]["tokens"] > 0,
        items_answer.get("memory"),
    )

    # judge: states of both systems, answers; the verdicts parsed
    judge_backend = _Judge()
    await judge(out, v3=fx["v3"], sol=judge_backend)
    judged_states = {(r["conv"], r["system"]): r for r in pilot.rows(out / "judge_states.jsonl")}
    judged_answers = pilot.rows(out / "judge_answers.jsonl")
    check(
        "judge states: items and v3 at the same points",
        set(judged_states) == {(fx["conv1"], "items"), (fx["conv1"], "v3"), (fx["conv2"], "items")}
        and judged_states[fx["conv1"], "v3"]["result"]["stale"] == ["н1 закрыта"],
        judged_states.keys(),
    )
    v3_request = next(
        m[0].content for i, m in judge_backend.calls if "Мнение о герое: вор\nНити" in m[0].content
    )
    check(
        "judge sees the same talk", "8. Вызов: propose_deal" in v3_request and mind.EMPTY_MIND in v3_request
    )
    items_request = next(
        m[0].content for i, m in judge_backend.calls if "Память после разговора\n- б1 [было" in m[0].content
    )
    check(
        "judge view of items",
        "- б1 [было; видел сам; важность 8; о: герой] герой обокрал меня" in items_request
        and "- н1 [нить; своё; о: Браго, герой] идти с героем" in items_request
        and "- ж1 [хочу; своё; скоро] вернуть своё" in items_request,
        items_request[-500:],
    )
    check(
        "judge answers parsed",
        len(judged_answers) == 6 and all(r["result"]["verdict"] == "correct" for r in judged_answers),
        [r.get("error") for r in judged_answers],
    )
    probe_request = next(m[0].content for i, m in judge_backend.calls if "Ответ NPC" in m[0].content)
    check(
        "probe judge: expect and evidence",
        "Ожидаемый ответ\nЗол: герой его обокрал." in probe_request
        and "Опора в записи\nГерой: Привет, Кавалорн\nNPC: Здорово." in probe_request
        and "Обстановка, в которой NPC отвечал\n[Обстановка" in probe_request
        and "Время: день 1, 11:30, день." in probe_request,
        probe_request[-300:],
    )
    check("probe verdict parse", parse_probe_verdict('{"verdict": "abstain"}')["verdict"] == "abstain")
    for bad in ('{"verdict": "maybe"}', "not json", '{"verdict": "wrong", "invented": "x"}'):
        try:
            parse_probe_verdict(bad)
            check(f"probe verdict rejects {bad}", False)
        except (ValueError, json.JSONDecodeError):
            check(f"probe verdict rejects {bad}", True)
    try:
        parse_state_verdict('{"false_facts": "x"}')
        check("state verdict rejects", False)
    except ValueError:
        check("state verdict rejects", True)
    await judge(out, v3=fx["v3"], sol=judge_backend)
    check("judge resumes", len(judge_backend.calls) == 9, len(judge_backend.calls))

    # sheet: a fixed shuffle, the key apart
    entries = sheet(out, seed=7)
    key = read_json(out / "sheet_key.json")["key"]
    check("sheet same seed same order", sheet(out, seed=7) == entries and len(entries) == 2)
    check(
        "sheet key maps back",
        all(
            entry["answers"][n]["text"] == shown(answers[entry_probe, conv, key[entry["key"]][label]])
            for entry in entries
            for n, label in enumerate("ABC")
            for entry_probe, conv in [
                (
                    entry["key"].split("@")[0],
                    next(c for p, c in plan if entry["key"].endswith(c.split(":")[2][:8])),
                )
            ]
        ),
        key,
    )
    orders_seen = {
        tuple(read_json(out / "sheet_key.json")["key"][e["key"]].values())
        for seed in range(12)
        for e in sheet(out, seed=seed)
    }
    check("sheet shuffles", len(orders_seen) > 1, orders_seen)
    check("sheet hides variants", all(set(a) == {"label", "text"} for e in entries for a in e["answers"]))

    # tail: one warm, a changed card each time, timings kept
    class Quick(FakeBackend):
        timings: dict[int, dict[str, Any]]

        async def stream(self, **kwargs: Any):
            self.timings[QUICK_SLOT.get()] = {"prompt_n": 60, "cache_n": 900, "prompt_ms": 9.0}
            async for event in super().stream(**kwargs):
                yield event

    fast = Quick("[neutral] Помню. Ты тот вор.", delay=0)
    fast.timings = {}
    await tail(out, quick=fast, tokens=mi.Tokens(), lengths=(40, 80), repeats=2, slot=2)
    rows = pilot.rows(out / "tail.jsonl")
    memories = [
        next(m.content for m in msgs if m.content.startswith(MEMORY_HEAD)) for _, msgs, _ in fast.calls
    ]
    check(
        "tail runs",
        len(rows) == 4
        and len(fast.prewarms) == 1
        and set(fast.slot_ids) == {2}
        and fast.prewarm_slot_ids == [2],
        (len(rows), fast.slot_ids),
    )
    check("tail card changes", all(a != b for a, b in zip(memories, memories[1:], strict=False)))
    check(
        "tail lengths",
        all(abs(r["tail_tokens"] - r["length"]) <= max(8, r["length"] // 5) for r in rows),
        [(r["length"], r["tail_tokens"]) for r in rows],
    )
    check(
        "tail timings",
        rows[0]["timings"]["cache_n"] == 900 and rows[0]["history_n"] == 900,
        rows[0]["timings"],
    )

    # summary: the numbers of the acceptance table
    result = summary(out, emit=False)
    check(
        "summary cost",
        result["cost"]["talks"] == 2
        and result["cost"]["output_tokens"]["n"] == 0
        and result["acceptance"]["1_cost"] is None,
        result["acceptance"]["1_cost"],
    )
    check("summary judge", result["judge"]["items"]["stale"] == 2 and result["stale_same_points"]["v3"] == 1)
    check(
        "summary probes",
        result["probes"]["items"]["correct"] == 2
        and result["probes"]["sol"]["judged"] == 2
        and result["probes"]["bare"]["by_kind"]["feeling"]["correct"] == 2,
    )
    check(
        "summary transitions",
        len(result["transitions"]) == 1 and result["transitions"][0]["topics"]["кража"],
        result["transitions"],
    )
    check("summary ids", result["ids"]["names"] >= 1 and result["ids"]["with_id_fraction"] is not None)
    check(
        "summary lies",
        result["acceptance"]["8_lies"]["fold_source_ok"] is True
        and result["lie"]["scenarios"]["failed"] == [],
        result["lie"]["scenarios"]["failed"],
    )
    check(
        "summary Sol calls",
        result["sol_calls"] == {"consolidate": 2, "lie": 1, "answers": 2, "judge": 9, "total": 14},
        result["sol_calls"],
    )
    check(
        "summary tail",
        result["acceptance"]["7_tail"]["reread_over_window"] == 0
        and result["tail"][40]["reread_tokens"]["median"] == 0
        and set(result["tail"]) == {40, 80},
    )


async def selftest() -> int:
    from tempfile import TemporaryDirectory

    check = mi._Check()
    with TemporaryDirectory() as root:
        try:
            await _selftest_run(check, Path(root))
        except Exception:  # a crash fails the selftest with its trace
            import traceback

            traceback.print_exc()
            check("crash", False)
    print(f"selftest: {check.passed} passed, {len(check.failed)} failed")
    return 1 if check.failed else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest="command", required=True)
    command = commands.add_parser("consolidate")
    command.add_argument("directory", type=Path)
    command.add_argument("--carry", type=Path, required=True, help="memory.sqlite3 of the recorded games")
    command.add_argument("--conv", action="append", help="a talk (its id or a unique part); repeatable")
    command.add_argument("--turns", type=Path, default=TURNS_DIR)
    command.add_argument("--effort", default="low")
    command = commands.add_parser("lie")
    command.add_argument("directory", type=Path)
    command.add_argument("--effort", default="low")
    command = commands.add_parser("answer")
    command.add_argument("directory", type=Path)
    command.add_argument("--probes", type=Path, required=True)
    command.add_argument("--per-conv", type=int, help="at most this many probes asked after one talk")
    command.add_argument("--slot", type=int, default=0)
    command.add_argument("--quick-at", help="Gemma's /v1 address (else settings.toml)")
    command.add_argument("--embed-at", help="FRIDA's address (else GOTHIC_EMBED_URL or the Gemma host)")
    command.add_argument("--effort", default="low")
    command = commands.add_parser("judge")
    command.add_argument("directory", type=Path)
    command.add_argument("--v3", type=Path, default=V3_DIR)
    command.add_argument("--effort", default="low")
    command = commands.add_parser("sheet")
    command.add_argument("directory", type=Path)
    command.add_argument("--seed", type=int, default=SEED)
    command = commands.add_parser("summary")
    command.add_argument("directory", type=Path)
    command = commands.add_parser("tail")
    command.add_argument("directory", type=Path)
    command.add_argument("--quick-at")
    command.add_argument("--slot", type=int, default=0)
    command.add_argument("--repeats", type=int, default=5)
    command.add_argument(
        "--lengths", type=lambda s: tuple(int(x) for x in s.split(",")), default=TAIL_LENGTHS
    )
    command.add_argument("--conv")
    commands.add_parser("selftest")
    args = vars(parser.parse_args())
    name = args.pop("command")
    if name == "selftest":
        return asyncio.run(selftest())
    if name in ("sheet", "summary"):
        globals()[name](**args)
    else:
        asyncio.run(globals()[name](**args))
    return 0


if __name__ == "__main__":
    sys.exit(main())
