"""What happened in the game, for people: player and npc lines, npc actions, trouble with voices.

The launcher shows it as its journal. With `serve --journal` every entry goes to stdout as one line
`JOURNAL {json}`; the launcher reads the server's output and picks these lines out.

Entries (`t` is the unix time, `npc` the npc's symbol, `name` its display name):

    {"kind": "player", "npc", "name", "text", "via": "text"|"voice", "asr"?, "latency_s"?}
    {"kind": "npc", "npc", "name", "text", "llm", "first_token_s", "total_s"}
    {"kind": "action", "npc", "name", "action", "arguments"}
    {"kind": "defend", "npc", "name", "target"}   a companion fights for the hero without an order (orders.py)
    {"kind": "attack", "npc", "name", "target", "reason"}   an npc turns on the hero (dialogue.py)
    {"kind": "beast", "npc", "name", "text", "tone", "via", "chance", "roll", "outcome"}   a wolf heard the
        hero (beasts.py): outcome calm, flee, attack or keep
    {"kind": "beast_event", "npc", "name", "event", "detail"}   a wolf's calm is over or broken (beasts.py)
    {"kind": "tts_off", "npc", "name", "seconds", "detail"}   lines go out silent for a while
    {"kind": "llm_error", "npc", "name", "detail"}
    {"kind": "voice_error", "npc", "detail"}
    {"kind": "feat", "id", "title", "text", "category"}   a deed recorded by the chronicler
"""

from __future__ import annotations

import json
import sys
import time
from collections.abc import Callable
from typing import Any

PREFIX = "JOURNAL "

Sink = Callable[[dict[str, Any]], None]


class Journal:
    def __init__(self, sink: Sink | None = None) -> None:
        self.sink = sink

    def add(self, kind: str, **data: Any) -> None:
        if self.sink is not None:
            self.sink({"t": round(time.time(), 3), "kind": kind, **data})


def print_sink(entry: dict[str, Any]) -> None:
    print(PREFIX + json.dumps(entry, ensure_ascii=False), file=sys.stdout, flush=True)
