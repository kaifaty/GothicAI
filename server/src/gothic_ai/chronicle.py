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
import inspect
import json
import logging
from collections.abc import Awaitable, Callable
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
        self.on_fact: list[Callable[[Fact], Awaitable[None] | None]] = []
        self._hero_kills: dict[str, bool] = {}  # hero_kill precedes npc_died; its explicit monster flag
        self._background: set[asyncio.Task[Any]] = set()
        # Session loads the existing branch cache; pure consumers never wait for SQLite.
        self._generation = 0
        self._prepared_session: Session | None = None
        self._cached: str | None = None
        self._seq = 0
        self._all: list[Fact] = []
        self._by_kind: dict[str | None, list[Fact]] = {}
        link.listeners.append(self._on_message)

    async def initialize(self) -> None:
        async def prepare_current() -> None:
            session = self.link.session
            if session is not None and session is not self._prepared_session:
                await self._on_session(session)

        await self.memory.complete(prepare_current)

    async def record(self, fact: Fact) -> None:
        branch, generation, session = self.branch, self._generation, self.link.session
        if branch is None:
            log.warning("no session: fact not kept %s", fact)
            return
        content = json.dumps(fact, ensure_ascii=False)

        async def persist() -> None:
            if generation != self._generation or branch != self.branch or session is not self.link.session:
                raise GameError("session changed while recording a fact")
            seq = await self.memory.add_fact(branch, content)
            if generation != self._generation or branch != self.branch or session is not self.link.session:
                raise GameError("session changed while recording a fact")
            committed = json.loads(content)
            self._all.append(committed)
            self._by_kind.setdefault(committed.get("fact"), []).append(committed)
            self._seq = seq
            log.info("fact: %s", committed)
            # The head goes into the next save, after the fact has committed.
            try:
                await self.link.send(MemoryCmd(branch=branch, seq=seq))
            except GameError:
                pass  # the game is gone; it learns the head again on reconnect
            for listener in self.on_fact:
                if (
                    generation != self._generation or branch != self.branch
                    or session is not self.link.session
                ):
                    raise GameError("session changed while publishing a fact")
                if inspect.isawaitable(result := listener(committed)):
                    await result

        await self.memory.complete(persist)

    def facts(self, kind: str | None = None) -> list[Fact]:
        """The branch's facts (of one `kind`), oldest first. The list is the caller's own, but the dicts
        in it are shared by every reader: read them, never change them."""
        if self.branch is None:
            return []
        if self._cached != self.branch:
            raise RuntimeError("chronicle facts are not prepared for the current session")
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

    async def _on_message(self, msg: GameMessage) -> None:
        match msg:
            case Session():
                await self._on_session(msg)
            case HeroKill():
                # Only humans and tagged monsters also produce npc_died. Do not retain every wild kill.
                self._hero_kills.clear()
                self._hero_kills[msg.npc] = msg.monster
            case NpcDied():
                monster = self._hero_kills.pop(msg.npc, None)
                await self.record(
                    {
                        "fact": "died",
                        "npc": msg.npc,
                        "name": msg.name,
                        "tag": msg.tag,
                        "killer": msg.killer,
                        **({"monster": monster} if monster is not None else {}),
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

    async def _on_session(self, session: Session) -> None:
        self._prepared_session = None
        self.branch = session.branch
        self._generation += 1
        generation = self._generation
        self._cached, self._seq, self._all, self._by_kind = None, 0, [], {}
        self.tags.clear()  # the game reports them again for this world
        self._hero_kills.clear()

        async def load() -> None:
            await self.memory.ensure_branch(session.branch, session.parent, session.parent_seq)
            rows = await self.memory.facts_since(session.branch, 0)
            if generation != self._generation or session is not self.link.session:
                return
            facts = [json.loads(content) for _, content in rows]
            self._all = facts
            self._by_kind = {}
            for fact in facts:
                self._by_kind.setdefault(fact.get("fact"), []).append(fact)
            self._seq = rows[-1][0] if rows else 0
            self._cached = session.branch
            self._prepared_session = session

        await self.memory.complete(load)
