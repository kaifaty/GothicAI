"""What really happened (docs/story-graph.md, stage 4): code owns the truth, npc's own their beliefs.

Facts are JSON events in the memory log of the save branch (`kind='fact'`), so a load brings back
exactly what had happened before that save. Each has a `fact` kind:

- `died`: an npc died (`npc`, `tag` for the director's creatures, `killer`: PLAYER or an npc);
- `spawned`: the director put `count` creatures of `instance` into the world under `tag`;
- the quests (quests.py) add `quest`, `paid`, `belief`, `lie`, `found_out`, `rumor`.

The game also reports, once a second, how many of each tag still live, how far the nearest one is from
the npc's being watched and how far the nearest one it sees (`story_world`, the engine's own sight since
v12): the rules that find lies out look at that.
"""

from __future__ import annotations

import asyncio
import json
import logging
from collections.abc import Callable, Coroutine
from dataclasses import dataclass, field
from typing import Any

from .game import GameError, GameLink
from .memory import MemoryStore
from .protocol import GameMessage, HeroKill, MemoryCmd, NpcDied, Session, StoryWorld

log = logging.getLogger(__name__)

Fact = dict[str, Any]


@dataclass
class Tag:
    """The director's creatures under one tag, as the game last reported them."""

    alive: int = 0
    dead: int = 0
    near: dict[str, float] = field(default_factory=dict)  # watched npc -> cm to the nearest living one
    seen: dict[str, float] = field(default_factory=dict)  # watched npc -> cm to the nearest one it sees


class Chronicle:
    def __init__(self, link: GameLink, memory: MemoryStore) -> None:
        self.link = link
        self.memory = memory
        self.branch: str | None = None
        self.tags: dict[str, Tag] = {}
        self.on_fact: list[Callable[[Fact], None]] = []  # every new fact, after it is written
        self.on_entry: list[Callable[[int, Fact], None]] = []
        self._hero_kills: dict[str, HeroKill] = {}  # hero_kill precedes npc_died
        self._background: set[asyncio.Task[Any]] = set()
        # the branch's facts as read so far: each read takes only the newer rows (by seq), since trust,
        # deals and the rumors read them on every turn
        self._cached: str | None = None
        self._seq = 0
        self._all: list[tuple[int, Fact]] = []
        self._by_kind: dict[str | None, list[tuple[int, Fact]]] = {}
        link.listeners.append(self._on_message)
        if link.session is not None:
            self._on_session(link.session)

    def record(self, fact: Fact) -> int | None:
        if self.branch is None:
            log.warning("no session: fact not kept %s", fact)
            return
        seq = self.memory.add_fact(self.branch, json.dumps(fact, ensure_ascii=False))
        log.info("fact: %s", fact)
        # the head goes into the next save, so a load brings this fact back
        self._spawn(self.link.send(MemoryCmd(branch=self.branch, seq=seq)))
        for listener in self.on_fact:
            listener(fact)
        for listener in self.on_entry:
            listener(seq, fact)
        return seq

    def facts(self, kind: str | None = None) -> list[Fact]:
        """The branch's facts (of one `kind`), oldest first. The list is the caller's own, but the dicts
        in it are shared by every reader: read them, never change them."""
        return [fact for _, fact in self.entries(kind)]

    def entries(self, kind: str | None = None) -> list[tuple[int, Fact]]:
        """The branch's facts with their save sequence numbers, oldest first, optionally by kind."""
        if self.branch is None:
            return []
        if self._cached != self.branch:
            self._cached, self._seq, self._all, self._by_kind = self.branch, 0, [], {}
        # facts written straight into the store (tests, another chronicle) are seen too
        for seq, content in self.memory.facts_since(self.branch, self._seq):
            f = json.loads(content)
            self._all.append((seq, f))
            self._by_kind.setdefault(f.get("fact"), []).append((seq, f))
            self._seq = seq
        return list(self._all) if kind is None else list(self._by_kind.get(kind, ()))

    def killed(self, tag: str, by: str | None = None) -> int:
        """How many of the tag died (by `by`, e.g. PLAYER), as written down."""
        return sum(
            1 for f in self.facts("died") if f.get("tag") == tag and (by is None or f.get("killer") == by)
        )

    def near(self, tag: str, npc: str) -> float | None:
        """cm from the npc to the nearest living creature of the tag; None: none lives, or not watched."""
        state = self.tags.get(tag)
        return state.near.get(npc.upper()) if state else None

    def seen(self, tag: str, npc: str) -> float | None:
        """cm from the npc to the nearest living creature of the tag it sees now (not behind a wall); None:
        it sees none."""
        state = self.tags.get(tag)
        return state.seen.get(npc.upper()) if state else None

    def _on_message(self, msg: GameMessage) -> None:
        match msg:
            case Session():
                self._on_session(msg)
            case HeroKill():
                # Only humans and tagged monsters also produce npc_died. Do not retain every wild kill.
                self._hero_kills.clear()
                self._hero_kills[msg.npc] = msg
            case NpcDied():
                kill = self._hero_kills.pop(msg.npc, None)
                self.record(
                    {
                        "fact": "died",
                        "npc": msg.npc,
                        "name": msg.name,
                        "tag": msg.tag,
                        "killer": msg.killer,
                        **({"monster": kill.monster, "guild": kill.guild} if kill is not None else {}),
                    }
                )
            case StoryWorld():
                self.tags = {
                    t.tag: Tag(
                        t.alive,
                        t.dead,
                        {n.npc.upper(): n.distance for n in t.near},
                        {n.npc.upper(): n.seen for n in t.near if n.seen is not None},
                    )
                    for t in msg.tags
                }

    def _on_session(self, session: Session) -> None:
        self.branch = session.branch
        self.memory.ensure_branch(session.branch, session.parent, session.parent_seq)
        self.tags.clear()  # the game reports them again for this world
        self._hero_kills.clear()

    def _spawn(self, coro: Coroutine[Any, Any, Any]) -> None:
        async def run() -> None:
            try:
                await coro
            except GameError:
                pass  # the game is gone; it learns the head again on reconnect

        task = asyncio.create_task(run())
        self._background.add(task)
        task.add_done_callback(self._background.discard)
