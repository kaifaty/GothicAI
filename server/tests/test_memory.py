import asyncio
import hashlib
import sqlite3

import numpy as np
import pytest

from gothic_ai.embeddings import EmbedError
from gothic_ai.memory import SEARCH_NOTES, MemoryStore


def test_branches_follow_saves() -> None:
    db = MemoryStore(":memory:")
    assert db.ensure_branch("A") == 0
    db.append("A", "XARDAS", "user", "Меня зовут Тестер", "день 1, 08:10")
    db.append("A", "XARDAS", "assistant", "Запомню.")
    saved_at = db.head("A")  # the game writes (A, 2) into the save
    db.append("A", "XARDAS", "user", "Это было после сохранения")
    db.append("A", "LESTER", "user", "Привет, Лестер")

    # loading the save starts branch B from (A, 2): later events are forgotten
    assert db.ensure_branch("B", "A", saved_at) == 2
    assert [e.content for e in db.load("B", "XARDAS").entries] == ["Меня зовут Тестер", "Запомню."]
    assert db.load("B", "LESTER").entries == []
    # the old branch is untouched, the new one continues after the inherited head
    assert len(db.load("A", "XARDAS").entries) == 3
    assert db.append("B", "XARDAS", "user", "Снова здесь") == 3
    # ensure_branch is idempotent (reconnects resend the session)
    assert db.ensure_branch("B", "A", saved_at) == 3

    # a branch of a branch inherits through the copy
    assert db.ensure_branch("C", "B", 3) == 3
    assert [e.seq for e in db.load("C", "XARDAS").entries] == [1, 2, 3]
    assert {b.id: (b.head, b.npcs) for b in db.branches()} == {"A": (4, 2), "B": (3, 1), "C": (3, 1)}


def test_summary_folds_older_messages() -> None:
    db = MemoryStore(":memory:")
    db.ensure_branch("A")
    for i in range(5):
        db.append("A", "XARDAS", "user", f"сообщение {i}")
    db.add_summary("A", "XARDAS", "Герой пять раз что-то говорил.", covers=3)
    mem = db.load("A", "XARDAS")
    assert mem.summary == "Герой пять раз что-то говорил." and mem.summary_seq == 3
    assert [e.content for e in mem.entries] == ["сообщение 3", "сообщение 4"]


def test_unknown_parent_starts_empty(tmp_path) -> None:
    db = MemoryStore(tmp_path / "m.sqlite3")
    assert db.ensure_branch("B", "LOST", 7) == 0
    db.close()
    # persisted across reopen
    db = MemoryStore(tmp_path / "m.sqlite3")
    assert [b.id for b in db.branches()] == ["B"]


def test_search_finds_older_messages_by_word_stems() -> None:
    store = MemoryStore(":memory:")
    store.ensure_branch("A")
    store.append("A", "X", "user", "Я видел ДРАКОНА у прохода!", "день 1, 10:00")
    store.append("A", "X", "assistant", "Драконы? Ты уверен?", "день 1, 10:00")
    store.append("A", "X", "user", "Принеси мне грибов", "день 1, 11:00")
    store.append("A", "X", "user", "Дракон снова прилетел", "день 2, 09:00")  # newer than asked for

    found = store.search("A", "X", "драконы", before_seq=3, limit=2)
    assert [(r.seq, r.game_time) for r in found] == [(1, "день 1, 10:00"), (2, "день 1, 10:00")]
    assert store.search("A", "X", "драконы", before_seq=0) == []
    # nothing matches: the latest older messages come back anyway (the model reads them)
    assert [r.seq for r in store.search("A", "X", "как меня зовут", before_seq=3, limit=2)] == [2, 3]


def test_facts_since_reads_only_the_newer_ones() -> None:
    store = MemoryStore(":memory:")
    store.ensure_branch("A")
    first = store.add_fact("A", '{"fact": "died"}')
    store.append("A", "X", "user", "не факт")  # other events share the seqs
    second = store.add_fact("A", '{"fact": "quest"}')
    third = store.add_fact("A", '{"fact": "paid"}')
    assert (first, second, third) == (1, 3, 4)
    assert store.facts_since("A", 0) == [
        (1, '{"fact": "died"}'),
        (3, '{"fact": "quest"}'),
        (4, '{"fact": "paid"}'),
    ]
    assert store.facts_since("A", first) == [(second, '{"fact": "quest"}'), (third, '{"fact": "paid"}')]
    assert store.facts_since("A", third) == []
    # a load inherits them with their seqs
    store.ensure_branch("B", "A", second)
    assert [seq for seq, _ in store.facts_since("B", 0)] == [first, second]
    assert store.facts_since("B", second) == []


class _Embeddings:
    def __init__(self):
        self.calls = []
        self.failure = None
        self.gate = None

    async def health(self):
        if self.failure is not None:
            raise self.failure
        return {"model": "test", "dim": 2}

    async def embed(self, texts, kind):
        self.calls.append((list(texts), kind))
        if self.gate is not None:
            await self.gate.wait()
        return np.array([[1, 0] if "башн" in text else [0, 1] for text in texts], dtype=np.float32)


async def test_vectors_are_float16_persisted_and_shared_by_branches(tmp_path):
    path = tmp_path / "memory.sqlite3"
    store = MemoryStore(path)
    store.ensure_branch("A")
    text = "Маг живёт в башне."
    store.append("A", "X", "assistant", text)
    store.append("A", "X", "assistant", text)
    store.ensure_branch("B", "A", 2)
    client = _Embeddings()
    store.embeddings = client
    await store.backfill("A", "X")
    await store.backfill("B", "X")
    rows = store._db.execute("SELECT hash, model, vec FROM vectors").fetchall()
    assert len(rows) == 1
    key, model, blob = rows[0]
    assert key == hashlib.sha256(text.encode()).hexdigest() and model == "test"
    assert len(blob) == 4 and list(np.frombuffer(blob, dtype="<f2")) == [1, 0]
    assert client.calls == [([text], "document")]
    store.close()
    store = MemoryStore(path)
    assert store._db.execute("SELECT COUNT(*) FROM vectors").fetchone() == (1,)
    store.close()
    with sqlite3.connect(path) as db:
        db.execute("DROP TABLE vectors")  # the schema of a database from before embeddings
    store = MemoryStore(path)
    assert store._db.execute("SELECT COUNT(*) FROM vectors").fetchone() == (0,)
    store.close()


async def test_searches_exclude_only_service_notes_and_keep_them_in_memory():
    store = MemoryStore(":memory:")
    store.ensure_branch("A")
    first = store.append("A", "X", "assistant", "Маг живёт в башне.")
    for note in SEARCH_NOTES:
        store.append("A", "X", "developer", note + ", башня]")
    ordinary = store.append("A", "X", "developer", "Ты видел башню.")
    player = store.append("A", "X", "user", SEARCH_NOTES[0] + ", башня]")
    assert len(store.load("A", "X").entries) == 7
    before = store.head("A")
    assert [r.seq for r in store.search("A", "X", "башня", before)] == [first, ordinary, player]
    client = _Embeddings()
    store.embeddings = client
    found = await store.semantic_search("A", "X", "башня", before)
    assert [r.seq for r in found] == [first, ordinary, player]
    assert len(client.calls[0][0]) == 3 and client.calls[0][1] == "document"
    store.close()


async def test_semantic_search_obeys_window_branch_npc_and_sequence(monkeypatch):
    from gothic_ai import memory

    monkeypatch.setattr(memory, "SEARCH_SCAN", 3)
    store = MemoryStore(":memory:")
    store.ensure_branch("A")
    store.append("A", "X", "assistant", "Старая башня.")
    store.append("A", "Y", "assistant", "Башня другого NPC.")
    second = store.append("A", "X", "assistant", "Река.")
    third = store.append("A", "X", "assistant", "Новая башня.")
    store.append("A", "X", "assistant", "Лес.")
    store.ensure_branch("B", "A", 5)
    store.append("B", "X", "assistant", "Башня другой ветки.")
    store.append("A", "X", "assistant", "Башня после границы поиска.")
    store.embeddings = _Embeddings()
    found = await store.semantic_search("A", "X", "башня", before_seq=5, limit=2)
    assert [r.seq for r in found] == [third, 5]
    found = await store.semantic_search("A", "X", "река", before_seq=third, limit=1)
    assert [r.seq for r in found] == [second]
    assert await store.semantic_search("A", "X", "башня", before_seq=0) == []
    store.close()


async def test_append_batches_in_background_and_open_can_backfill_old_lines():
    store = MemoryStore(":memory:")
    store.ensure_branch("A")
    store.append("A", "OLD", "user", "Старая реплика.")
    client = _Embeddings()
    client.gate = asyncio.Event()
    store.embeddings = client
    for text in ["Раз.", "Два.", "Три."]:
        store.append("A", "X", "user", text)
    task = store._embedding_task
    await asyncio.sleep(0)
    assert client.calls == [(["Раз.", "Два.", "Три."], "document")]
    assert store.head("A") == 4 and not task.done()  # appends did not wait for the service
    client.gate.set()
    await task
    store.queue_embeddings("A", "OLD")
    await store._embedding_task
    assert client.calls[-1] == (["Старая реплика."], "document")
    assert store._db.execute("SELECT COUNT(*) FROM vectors").fetchone() == (4,)
    store.close()


async def test_background_embedding_errors_are_logged_at_most_once_a_minute(monkeypatch, caplog):
    from gothic_ai import memory

    now = 100.0
    monkeypatch.setattr(memory.time, "monotonic", lambda: now)
    store = MemoryStore(":memory:", embeddings=_Embeddings())
    store.ensure_branch("A")
    store.embeddings.failure = EmbedError("offline\nretry later")
    for now in (100.0, 110.0, 160.0):
        store.append("A", "X", "user", f"Сообщение {now}")
        await store._embedding_task
    warnings = [record for record in caplog.records if "memory background embeddings:" in record.message]
    assert len(warnings) == 2 and all("\n" not in record.message for record in warnings)
    store.close()


async def test_recall_waits_for_backfill_instead_of_dropping_uncached_candidates():
    store = MemoryStore(":memory:")
    store.ensure_branch("A")
    for i in range(257):
        store.append("A", "X", "user", f"Башня {i}")
    store.embeddings = _Embeddings()
    with pytest.raises(EmbedError, match="backfill pending: 257"):
        await store.semantic_search("A", "X", "башня", before_seq=257)
    await store._embedding_task
    documents = [texts for texts, kind in store.embeddings.calls if kind == "document"]
    assert [len(texts) for texts in documents] == [256, 1]
    assert len(await store.semantic_search("A", "X", "башня", before_seq=257)) == 6
    store.close()
