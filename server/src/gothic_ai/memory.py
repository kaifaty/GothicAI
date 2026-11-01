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

import logging
import os
import re
import sqlite3
import time
from dataclasses import dataclass, field
from pathlib import Path

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
CREATE INDEX IF NOT EXISTS events_by_feat ON events (branch, seq)
WHERE kind='fact'
AND json_extract(content, '$.fact') IN ('feat', 'feat_epilogue', 'feat_epilogue_shown', 'fate_end');
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


class MemoryStore:
    def __init__(self, path: str | Path = DEFAULT_PATH) -> None:
        if str(path) != ":memory:":
            Path(path).parent.mkdir(parents=True, exist_ok=True)
        self.path = path
        self._db = sqlite3.connect(str(path), isolation_level=None)  # autocommit; explicit transactions
        self._db.execute("PRAGMA journal_mode=WAL")
        index = self._db.execute(
            "SELECT sql FROM sqlite_master WHERE type='index' AND name='events_by_feat'",
        ).fetchone()
        if index and "'fate_end'" not in index[0]:
            self._db.execute("DROP INDEX events_by_feat")
        self._db.executescript(_SCHEMA)

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

    def feat_facts(self, branch: str) -> list[tuple[int, float, str]]:
        """Achievement and ending facts with original recording times, including inherited copies."""
        rows = self._db.execute(
            "SELECT seq, created, content FROM events WHERE branch=? AND kind='fact' "
            "AND json_extract(content, '$.fact') "
            "IN ('feat', 'feat_epilogue', 'feat_epilogue_shown', 'fate_end') "
            "ORDER BY seq",
            (branch,),
        ).fetchall()
        return [(int(seq), created, content) for seq, created, content in rows]

    def fact_clock(self, branch: str, seq: int) -> tuple[int, int]:
        """Chapter and day last recorded in the shared prefix of a save fork."""
        chapter, day = self._db.execute(
            "SELECT (SELECT CASE WHEN json_extract(content, '$.fact')='chapter' "
            "THEN json_extract(content, '$.value') ELSE json_extract(content, '$.chapter') END "
            "FROM events WHERE branch=? AND kind='fact' AND seq<=? "
            "AND (json_extract(content, '$.fact')='chapter' "
            "OR json_extract(content, '$.chapter') IS NOT NULL) ORDER BY seq DESC LIMIT 1), "
            "(SELECT json_extract(content, '$.day') FROM events "
            "WHERE branch=? AND kind='fact' AND seq<=? "
            "AND json_extract(content, '$.day') IS NOT NULL ORDER BY seq DESC LIMIT 1)",
            (branch, seq, branch, seq),
        ).fetchone()
        return int(chapter or 1), int(day or 0)

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
        # Separate NPC ranges let the index skip copied '*' facts and patches.
        rows = self._db.execute(
            "SELECT b.id, b.parent, b.parent_seq, b.created, "
            "COALESCE((SELECT MAX(seq) FROM events WHERE branch=b.id), 0), "
            "(SELECT COUNT(DISTINCT npc) FROM events WHERE branch=b.id AND npc<'*') + "
            "(SELECT COUNT(DISTINCT npc) FROM events WHERE branch=b.id AND npc>'*') "
            "FROM branches b ORDER BY b.created"
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
