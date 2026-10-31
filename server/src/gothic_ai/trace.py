"""Turn trace: one JSON line per npc turn, to see what happened, to replay it and to collect training data.

A file per day under `<data dir>/traces/`. Records:
- `instructions` — `{hash, text}`: an npc's instructions, written once per file (they are large and
  rarely change); turns refer to them by hash;
- `turn` — what started it (`trigger`: a player phrase, a greeting, events), the npc's snapshot, the
  offered tools and every step of the turn: what was added to the request, what came back (text, calls
  and their results), timings and token usage; and how the turn ended (`status`).

The first step's input is the whole request; later steps only list what was added to it (the previous
response and the tool results). Reasoning is not kept, only marked: it is opaque and only valid for the
original request. A line in a fight (dialogue.py) starts with the quick model's step (`llm`); the turn's
model then acts on a request of its own, whole again, with its text unsaid (`silent`).
"""

from __future__ import annotations

import hashlib
import json
import logging
import time
import uuid
from collections.abc import Sequence
from dataclasses import asdict, dataclass, field
from datetime import date
from pathlib import Path
from typing import Any

from .llm import Done, Item, Message, Reasoning, ToolCall, ToolResult

log = logging.getLogger(__name__)


def item_json(item: Item) -> dict[str, Any]:
    match item:
        case Message():
            return {"role": item.role, "content": item.content}
        case ToolCall():
            return {
                "type": "function_call",
                "call_id": item.call_id,
                "name": item.name,
                "arguments": item.raw or json.dumps(item.arguments, ensure_ascii=False),
            }
        case ToolResult():
            return {"type": "function_call_output", "call_id": item.call_id, "output": item.output}
        case Reasoning():
            return {"type": "reasoning"}  # marker only


def item_from_json(data: dict[str, Any]) -> Item | None:
    """The inverse of `item_json`; None for reasoning markers (they cannot be sent again)."""
    match data.get("type"):
        case "function_call":
            raw = data.get("arguments") or "{}"
            try:
                args = json.loads(raw)
            except json.JSONDecodeError:
                args = {}
            return ToolCall(data["name"], args if isinstance(args, dict) else {}, data["call_id"], raw)
        case "function_call_output":
            return ToolResult(data["call_id"], data["output"])
        case "reasoning":
            return None
    return Message(data["role"], data["content"])


@dataclass
class CallTrace:
    name: str
    arguments: dict[str, Any]
    call_id: str
    result: str  # what the model was told
    outcome: str = ""  # accepted / rejected / read / ...

    def matches(self, call: ToolCall) -> bool:
        return call.name == self.name and call.arguments == self.arguments


@dataclass
class StepTrace:
    added: list[dict[str, Any]]
    text: str = ""
    calls: list[CallTrace] = field(default_factory=list)
    tool_choice: str | None = None
    first_token_s: float | None = None
    total_s: float = 0.0
    started_s: float = 0.0  # from the turn's trigger, including work before the model call
    ended_s: float = 0.0
    usage: dict[str, int] | None = None
    timing: dict[str, Any] | None = None
    llm: str | None = None  # another model than the turn's (the quick model of a fight, dialogue.py)
    silent: bool = False  # its text was not said: the quick model spoke, this step only acts


class TurnTrace:
    """Collects one turn; `close` writes it."""

    def __init__(
        self,
        writer: TraceWriter | None,
        *,
        npc: str,
        branch: str | None,
        trigger: dict[str, Any],
        instructions: str,
        tools: Sequence[str],
        snapshot: dict[str, Any] | None,
        llm: str,
        model: str | None,
    ) -> None:
        self.writer = writer
        self.id = uuid.uuid4().hex[:12]
        self.started = time.time()
        self.record: dict[str, Any] = {
            "type": "turn",
            "id": self.id,
            "t": round(self.started, 3),
            "npc": npc,
            "branch": branch,
            "trigger": trigger,
            "llm": llm,
            "model": model,
            "instructions": writer.instructions(instructions) if writer else _hash(instructions),
            "tools": list(tools),
            "snapshot": snapshot,
            "steps": [],
            "events": [],
        }
        self.steps: list[StepTrace] = []
        self.closed = False

    def step(
        self, added: Sequence[Item], tool_choice: str | None, *, llm: str | None = None, silent: bool = False
    ) -> StepTrace:
        st = StepTrace([item_json(i) for i in added], tool_choice=tool_choice, llm=llm, silent=silent)
        st.started_s = round(time.time() - self.started, 3)
        self.steps.append(st)
        return st

    @staticmethod
    def finish_step(st: StepTrace, done: Done | None, first_token_s: float | None, total_s: float) -> None:
        st.first_token_s = None if first_token_s is None else round(first_token_s, 3)
        st.total_s = round(total_s, 3)
        st.ended_s = round(st.started_s + total_s, 3)
        if done is not None:
            st.text = done.text
            st.usage = asdict(done.usage) if done.usage else None
            st.timing = done.timing

    def event(self, kind: str, **data: Any) -> None:
        """Something else that belongs to the turn (a reflex, an action outcome...)."""
        self.record["events"].append({"kind": kind, "at_s": round(time.time() - self.started, 3), **data})

    def close(self, status: str) -> None:
        if self.closed:
            return
        self.closed = True
        self.record["status"] = status
        self.record["total_s"] = round(time.time() - self.started, 3)
        self.record["steps"] = [asdict(s) for s in self.steps]
        if self.writer is not None:
            self.writer.write(self.record)


class TraceWriter:
    def __init__(self, directory: Path) -> None:
        self.directory = directory
        self._written: dict[Path, set[str]] = {}  # file -> instruction hashes already in it
        self._pending: dict[str, str] = {}  # hash -> text, until a turn lands in today's file

    def path(self, day: date | None = None) -> Path:
        return self.directory / f"{(day or date.today()).isoformat()}.jsonl"

    def instructions(self, text: str) -> str:
        h = _hash(text)
        self._pending[h] = text
        return h

    def write(self, record: dict[str, Any]) -> None:
        path = self.path()
        try:
            path.parent.mkdir(parents=True, exist_ok=True)
            written = self._written.setdefault(path, _hashes_in(path))
            lines = []
            h = record.get("instructions")
            if h and h not in written and h in self._pending:
                lines.append({"type": "instructions", "hash": h, "text": self._pending[h]})
                written.add(h)
            lines.append(record)
            with path.open("a", encoding="utf-8") as f:
                for line in lines:
                    f.write(json.dumps(line, ensure_ascii=False) + "\n")
        except OSError as err:
            log.warning("trace not written: %s", err)


def _hash(text: str) -> str:
    return hashlib.sha1(text.encode()).hexdigest()[:16]


def _hashes_in(path: Path) -> set[str]:
    out: set[str] = set()
    if path.exists():
        for rec in read(path):
            if rec.get("type") == "instructions":
                out.add(rec["hash"])
    return out


def read(path: Path) -> list[dict[str, Any]]:
    out = []
    with path.open(encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line:
                try:
                    out.append(json.loads(line))
                except json.JSONDecodeError:
                    continue
    return out


def find_turn(path: Path, turn: str | None) -> tuple[dict[str, Any], str | None]:
    """A turn (the last one when `turn` is None) and its instructions' text, if the file has it."""
    records = read(path)
    turns = [r for r in records if r.get("type") == "turn"]
    if not turns:
        raise LookupError(f"no turns in {path}")
    rec = turns[-1] if turn is None else next((t for t in turns if t["id"].startswith(turn)), None)
    if rec is None:
        raise LookupError(f"turn {turn} not in {path}")
    text = next(
        (r["text"] for r in records if r.get("type") == "instructions" and r["hash"] == rec["instructions"]),
        None,
    )
    return rec, text


def latest(directory: Path) -> Path | None:
    files = sorted(directory.glob("*.jsonl"))
    return files[-1] if files else None
