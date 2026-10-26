"""What npc's remember, per save game: an append-only log in SQLite, branched like the saves themselves.

A *branch* is one line of play. The game starts a new one on "new game" and on every load; a load
branches off the saved branch at the sequence number written into the save, so whatever happened
after that save is forgotten, exactly like in the game world. Each branch holds its own copy of the
events it inherited, which keeps reads to one branch.

Events per npc: dialogue messages (`user` = the hero, `assistant` = the npc, `developer` = notes),
summaries that fold older messages into a short recollection, the npc's intents (JSON, see
executor.py): the latest one is what the npc wants now, and what it saw (JSON, sightings.py). Events of
the branch as a whole: patches of the story graph (story.py), which follow the saves the same way.
"""

from __future__ import annotations

import asyncio
import logging
import os
import re
import sqlite3
import time
from collections.abc import Awaitable, Callable
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from .llm import Message, Role

log = logging.getLogger(__name__)

DEFAULT_PATH = Path(os.environ.get("GOTHIC_AI_DATA", Path.home() / ".local" / "share" / "gothic-ai")) / (
    "memory.sqlite3"
)
LOAD_LIMIT = 100  # newest messages read back for one npc (older ones should be summarized already)
PATCHES = "*"  # the "npc" of story graph patches and facts: they belong to the branch
SEARCH_SCAN = 2000  # how far back a search looks
STEM = 4  # a crude stem for Russian: the first letters of longer words ("драконы" ~ "дракон")
_WORDS = re.compile(r"\w+")

_SCHEMA = """
CREATE TABLE IF NOT EXISTS branches (
    id         TEXT PRIMARY KEY,
    parent     TEXT,
    parent_seq INTEGER NOT NULL DEFAULT 0,
    created    REAL NOT NULL
);
CREATE TABLE IF NOT EXISTS events (
    branch    TEXT NOT NULL,
    seq       INTEGER NOT NULL,
    npc       TEXT NOT NULL,
    kind      TEXT NOT NULL,          -- message|summary|intent|seen|chest; patch, fact (npc '*': JSON)
    role      TEXT,                   -- message: user | assistant | developer
    content   TEXT NOT NULL,
    covers    INTEGER,                -- summary: messages up to this seq are folded in
    game_time TEXT,
    created   REAL NOT NULL,
    PRIMARY KEY (branch, seq)
);
CREATE INDEX IF NOT EXISTS events_by_npc ON events (branch, npc, seq);
"""


@dataclass(frozen=True)
class Entry:
    seq: int
    role: Role
    content: str

    def message(self) -> Message:
        return Message(self.role, self.content)


@dataclass(frozen=True)
class Recollection:
    seq: int
    role: Role
    content: str
    game_time: str | None


@dataclass
class NpcMemory:
    summary: str = ""
    summary_seq: int = 0  # entries up to this seq are folded into the summary
    entries: list[Entry] = field(default_factory=list)
    intent: str | None = None  # the latest intent, JSON


@dataclass(frozen=True)
class BranchInfo:
    id: str
    parent: str | None
    parent_seq: int
    created: float
    head: int
    npcs: int


async def _wait_complete[T](future: asyncio.Future[T]) -> T:
    """Finish accepted work before propagating cancellation of its awaiter."""
    cancelled = None
    while True:
        try:
            result = await asyncio.shield(future)
            break
        except asyncio.CancelledError as error:
            if future.cancelled():
                raise
            cancelled = error
    if cancelled is not None:
        raise cancelled
    return result


class MemoryStore:
    """One worker owns SQLite; logical completions apply and publish on the event loop."""

    def __init__(self, path: str | Path = DEFAULT_PATH) -> None:
        self.path = path
        self._loop = asyncio.get_running_loop()
        self._executor = ThreadPoolExecutor(max_workers=1, thread_name_prefix="npc-memory")
        self._sql: _SqlMemory
        self._completion_lock = asyncio.Lock()
        self._completions: set[asyncio.Task[Any]] = set()
        self._active_completion: asyncio.Task[Any] | None = None
        self._closing = False
        self._close_task: asyncio.Task[None] | None = None

    @classmethod
    async def open(cls, path: str | Path = DEFAULT_PATH) -> MemoryStore:
        store = cls(path)

        async def initialize() -> None:
            store._sql = await store._run(_SqlMemory, path)

        try:
            await _wait_complete(asyncio.create_task(initialize()))
        except BaseException:
            await store.close()
            raise
        return store

    async def _run[T](self, operation: Callable[..., T], *args: Any) -> T:
        if self._closing and asyncio.current_task() is not self._active_completion:
            raise RuntimeError("Memory store is closing")
        return await _wait_complete(self._loop.run_in_executor(self._executor, operation, *args))

    @property
    def in_completion(self) -> bool:
        """Whether this task owns the current logical completion."""
        return asyncio.current_task() is self._active_completion

    async def complete[T](self, operation_factory: Callable[[], Awaitable[T]]) -> T:
        """Serialize persistence, local bookkeeping and publication; drain accepted work on cancel."""
        if asyncio.current_task() is self._active_completion:
            return await operation_factory()
        if self._closing:
            raise RuntimeError("Memory store is closing")

        async def run() -> T:
            async with self._completion_lock:
                self._active_completion = asyncio.current_task()
                try:
                    return await operation_factory()
                finally:
                    self._active_completion = None

        task = asyncio.create_task(run())
        self._completions.add(task)
        task.add_done_callback(self._completions.discard)
        return await _wait_complete(task)

    async def flush(self) -> None:
        """Complete previously submitted SQL work; caller owns the publication/save barrier."""
        await self._run(lambda: None)

    async def close(self) -> None:
        if asyncio.current_task() is self._active_completion:
            raise RuntimeError("Cannot close memory inside a logical completion")
        if self._close_task is None:
            self._closing = True

            async def finish() -> None:
                try:
                    await asyncio.gather(*tuple(self._completions), return_exceptions=True)
                    if hasattr(self, "_sql"):
                        await self._loop.run_in_executor(self._executor, self._sql.close)
                finally:
                    self._executor.shutdown(wait=False, cancel_futures=False)

            self._close_task = asyncio.create_task(finish())
        await _wait_complete(self._close_task)

    async def ensure_branch(self, branch: str, parent: str | None = None, parent_seq: int = 0) -> int:
        return await self._run(self._sql.ensure_branch, branch, parent, parent_seq)

    async def head(self, branch: str) -> int:
        return await self._run(self._sql.head, branch)

    async def append(
        self, branch: str, npc: str, role: Role, content: str, game_time: str | None = None
    ) -> int:
        return await self._run(self._sql.append, branch, npc, role, content, game_time)

    async def append_messages(
        self, branch: str, npc: str, messages: tuple[Message, ...], game_time: str | None = None
    ) -> tuple[Entry, ...]:
        return await self._run(self._sql.append_messages, branch, npc, messages, game_time)

    async def add_summary(self, branch: str, npc: str, summary: str, covers: int) -> int:
        return await self._run(self._sql.add_summary, branch, npc, summary, covers)

    async def set_intent(self, branch: str, npc: str, intent: str, game_time: str | None = None) -> int:
        return await self._run(self._sql.set_intent, branch, npc, intent, game_time)

    async def load(self, branch: str, npc: str) -> NpcMemory:
        return await self._run(self._sql.load, branch, npc)

    async def intent(self, branch: str, npc: str) -> str | None:
        return await self._run(self._sql.intent, branch, npc)

    async def search(
        self, branch: str, npc: str, query: str, before_seq: int, limit: int = 6
    ) -> list[Recollection]:
        return await self._run(self._sql.search, branch, npc, query, before_seq, limit)

    async def add_seen(self, branch: str, npc: str, sighting: str) -> int:
        return await self._run(self._sql.add_seen, branch, npc, sighting)

    async def seen(self, branch: str, npc: str) -> list[str]:
        return await self._run(self._sql.seen, branch, npc)

    async def add_chest(self, branch: str, npc: str, record: str) -> int:
        return await self._run(self._sql.add_chest, branch, npc, record)

    async def chests(self, branch: str, npc: str) -> list[str]:
        return await self._run(self._sql.chests, branch, npc)

    async def add_fact(self, branch: str, fact: str) -> int:
        return await self._run(self._sql.add_fact, branch, fact)

    async def facts(self, branch: str) -> list[str]:
        return await self._run(self._sql.facts, branch)

    async def facts_since(self, branch: str, seq: int) -> list[tuple[int, str]]:
        return await self._run(self._sql.facts_since, branch, seq)

    async def add_patch(self, branch: str, op: str) -> int:
        return await self._run(self._sql.add_patch, branch, op)

    async def patches(self, branch: str) -> list[str]:
        return await self._run(self._sql.patches, branch)

    async def branches(self) -> list[BranchInfo]:
        return await self._run(self._sql.branches)


class _SqlMemory:
    def __init__(self, path: str | Path = DEFAULT_PATH) -> None:
        if str(path) != ":memory:":
            Path(path).parent.mkdir(parents=True, exist_ok=True)
        self.path = path
        self._db = sqlite3.connect(str(path), isolation_level=None)  # autocommit; explicit transactions
        try:
            self._db.execute("PRAGMA journal_mode=WAL")
            self._db.executescript(_SCHEMA)
        except BaseException:
            self._db.close()
            raise

    def close(self) -> None:
        self._db.close()

    def ensure_branch(self, branch: str, parent: str | None = None, parent_seq: int = 0) -> int:
        """Create the branch on first sight (copying what it inherits from `parent`). Returns its head."""
        if self._db.execute("SELECT 1 FROM branches WHERE id=?", (branch,)).fetchone():
            return self.head(branch)
        with self._tx():
            self._db.execute(
                "INSERT INTO branches (id, parent, parent_seq, created) VALUES (?,?,?,?)",
                (branch, parent, parent_seq, time.time()),
            )
            if parent:
                copied = self._db.execute(
                    "INSERT INTO events SELECT ?, seq, npc, kind, role, content, covers, game_time, created "
                    "FROM events WHERE branch=? AND seq<=?",
                    (branch, parent, parent_seq),
                ).rowcount
                if not copied and parent_seq and not self._known(parent):
                    log.warning("branch %s: parent %s is unknown, starting without memories", branch, parent)
        return self.head(branch)

    def head(self, branch: str) -> int:
        row = self._db.execute("SELECT MAX(seq) FROM events WHERE branch=?", (branch,)).fetchone()
        return int(row[0] or 0)

    def append(self, branch: str, npc: str, role: Role, content: str, game_time: str | None = None) -> int:
        return self._insert(branch, npc, "message", role, content, None, game_time)

    def append_messages(
        self, branch: str, npc: str, messages: tuple[Message, ...], game_time: str | None = None
    ) -> tuple[Entry, ...]:
        # One worker job prevents interleaving; each row keeps its original COMMIT/crash prefix.
        return tuple(
            Entry(self.append(branch, npc, m.role, m.content, game_time), m.role, m.content) for m in messages
        )

    def add_summary(self, branch: str, npc: str, summary: str, covers: int) -> int:
        return self._insert(branch, npc, "summary", None, summary, covers, None)

    def set_intent(self, branch: str, npc: str, intent: str, game_time: str | None = None) -> int:
        return self._insert(branch, npc, "intent", None, intent, None, game_time)

    def load(self, branch: str, npc: str) -> NpcMemory:
        mem = NpcMemory()
        row = self._db.execute(
            "SELECT content, covers FROM events WHERE branch=? AND npc=? AND kind='summary' "
            "ORDER BY seq DESC LIMIT 1",
            (branch, npc),
        ).fetchone()
        if row:
            mem.summary, mem.summary_seq = row[0], int(row[1] or 0)
        rows = self._db.execute(
            "SELECT seq, role, content FROM events WHERE branch=? AND npc=? AND kind='message' AND seq>? "
            "ORDER BY seq DESC LIMIT ?",
            (branch, npc, mem.summary_seq, LOAD_LIMIT),
        ).fetchall()
        mem.entries = [Entry(int(seq), role, content) for seq, role, content in reversed(rows)]
        mem.intent = self.intent(branch, npc)
        return mem

    def intent(self, branch: str, npc: str) -> str | None:
        """The npc's latest intent (JSON), if it ever had one in this branch."""
        row = self._db.execute(
            "SELECT content FROM events WHERE branch=? AND npc=? AND kind='intent' ORDER BY seq DESC LIMIT 1",
            (branch, npc),
        ).fetchone()
        return row[0] if row else None

    def search(
        self, branch: str, npc: str, query: str, before_seq: int, limit: int = 6
    ) -> list[Recollection]:
        """Messages up to `before_seq` (older than what the npc has in mind): those sharing the most word
        stems with `query`, topped up with the latest of them (a word search misses "как меня зовут" vs
        "моё имя"; the model reads what comes back). Matching is done here, not in SQL: SQLite's LIKE
        ignores case for ASCII only."""
        if before_seq <= 0:
            return []
        rows = self._db.execute(
            "SELECT seq, role, content, game_time FROM events "
            "WHERE branch=? AND npc=? AND kind='message' AND seq<=? ORDER BY seq DESC LIMIT ?",
            (branch, npc, before_seq, SEARCH_SCAN),
        ).fetchall()
        stems = _stems(query)
        scored = []
        for seq, role, content, game_time in rows:
            text = content.lower()
            score = sum(1 for stem in stems if stem in text)
            if score:
                scored.append((score, int(seq), Recollection(int(seq), role, content, game_time)))
        scored.sort(key=lambda x: (-x[0], -x[1]))
        found = {r.seq: r for _, _, r in scored[:limit]}
        for seq, role, content, game_time in rows:  # newest first
            if len(found) >= limit:
                break
            found.setdefault(int(seq), Recollection(int(seq), role, content, game_time))
        return sorted(found.values(), key=lambda r: r.seq)

    def add_seen(self, branch: str, npc: str, sighting: str) -> int:
        """Something the npc saw itself (JSON, sightings.py); a later record of the same sighting wins."""
        return self._insert(branch, npc, "seen", None, sighting, None, None)

    def seen(self, branch: str, npc: str) -> list[str]:
        """What the npc saw, oldest first (inherited records included)."""
        rows = self._db.execute(
            "SELECT content FROM events WHERE branch=? AND npc=? AND kind='seen' ORDER BY seq", (branch, npc)
        ).fetchall()
        return [row[0] for row in rows]

    def add_chest(self, branch: str, npc: str, record: str) -> int:
        """What the npc knows lies in its chest (JSON, chests.py): a later record of the same chest wins."""
        return self._insert(branch, npc, "chest", None, record, None, None)

    def chests(self, branch: str, npc: str) -> list[str]:
        """What the npc knows of its chests, oldest first."""
        rows = self._db.execute(
            "SELECT content FROM events WHERE branch=? AND npc=? AND kind='chest' ORDER BY seq", (branch, npc)
        ).fetchall()
        return [row[0] for row in rows]

    def add_fact(self, branch: str, fact: str) -> int:
        """Something that really happened on this branch (JSON, chronicle.py): what code knows as true."""
        return self._insert(branch, PATCHES, "fact", None, fact, None, None)

    def facts(self, branch: str) -> list[str]:
        """The branch's facts, oldest first (inherited ones included)."""
        rows = self._db.execute(
            "SELECT content FROM events WHERE branch=? AND kind='fact' ORDER BY seq", (branch,)
        ).fetchall()
        return [row[0] for row in rows]

    def facts_since(self, branch: str, seq: int) -> list[tuple[int, str]]:
        """The branch's facts after `seq`, oldest first, with their seq: what a reader that has the facts up
        to `seq` lacks (seqs only grow on a branch, and a branch copies what it inherits when it is made)."""
        rows = self._db.execute(
            "SELECT seq, content FROM events WHERE branch=? AND kind='fact' AND seq>? ORDER BY seq",
            (branch, seq),
        ).fetchall()
        return [(int(s), content) for s, content in rows]

    def add_patch(self, branch: str, op: str) -> int:
        """A change to the story graph (JSON of one operation) made on this branch."""
        return self._insert(branch, PATCHES, "patch", None, op, None, None)

    def patches(self, branch: str) -> list[str]:
        """The story graph changes of the branch, oldest first (inherited ones included)."""
        rows = self._db.execute(
            "SELECT content FROM events WHERE branch=? AND kind='patch' ORDER BY seq", (branch,)
        ).fetchall()
        return [row[0] for row in rows]

    def branches(self) -> list[BranchInfo]:
        rows = self._db.execute(
            "SELECT b.id, b.parent, b.parent_seq, b.created, COALESCE(MAX(e.seq), 0), "
            "COUNT(DISTINCT CASE WHEN e.npc != '*' THEN e.npc END) "
            "FROM branches b LEFT JOIN events e ON e.branch=b.id GROUP BY b.id ORDER BY b.created"
        ).fetchall()
        return [BranchInfo(*row) for row in rows]

    def _insert(
        self,
        branch: str,
        npc: str,
        kind: str,
        role: str | None,
        content: str,
        covers: int | None,
        game_time: str | None,
    ) -> int:
        with self._tx():
            seq = self.head(branch) + 1
            self._db.execute(
                "INSERT INTO events (branch, seq, npc, kind, role, content, covers, game_time, created) "
                "VALUES (?,?,?,?,?,?,?,?,?)",
                (branch, seq, npc, kind, role, content, covers, game_time, time.time()),
            )
        return seq

    def _known(self, branch: str) -> bool:
        return (
            self._db.execute("SELECT 1 FROM events WHERE branch=? LIMIT 1", (branch,)).fetchone() is not None
        )

    def _tx(self) -> _Transaction:
        return _Transaction(self._db)


class _Transaction:
    def __init__(self, db: sqlite3.Connection) -> None:
        self._db = db

    def __enter__(self) -> None:
        self._db.execute("BEGIN IMMEDIATE")

    def __exit__(self, exc_type, *_exc) -> None:
        self._db.execute("ROLLBACK" if exc_type else "COMMIT")


def _stems(text: str) -> set[str]:
    return {w[:STEM] for w in _WORDS.findall(text.lower()) if len(w) >= 3}
