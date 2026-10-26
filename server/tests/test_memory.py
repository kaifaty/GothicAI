import asyncio
import sqlite3
import threading

import pytest

from gothic_ai import memory
from gothic_ai.llm import Message
from gothic_ai.memory import MemoryStore


@pytest.fixture(autouse=True)
async def close_memory_stores(monkeypatch):
    stores = []
    original_open = MemoryStore.open.__func__

    async def tracked_open(cls, *args, **kwargs):
        store = await original_open(cls, *args, **kwargs)
        stores.append(store)
        return store

    monkeypatch.setattr(MemoryStore, "open", classmethod(tracked_open))
    try:
        yield
    finally:
        for store in reversed(stores):
            await store.close()


async def test_branches_follow_saves() -> None:
    db = await MemoryStore.open(":memory:")
    assert await db.ensure_branch("A") == 0
    await db.append("A", "XARDAS", "user", "Меня зовут Тестер", "день 1, 08:10")
    await db.append("A", "XARDAS", "assistant", "Запомню.")
    saved_at = await db.head("A")  # the game writes (A, 2) into the save
    await db.append("A", "XARDAS", "user", "Это было после сохранения")
    await db.append("A", "LESTER", "user", "Привет, Лестер")

    # loading the save starts branch B from (A, 2): later events are forgotten
    assert await db.ensure_branch("B", "A", saved_at) == 2
    assert [e.content for e in (await db.load("B", "XARDAS")).entries] == ["Меня зовут Тестер", "Запомню."]
    assert (await db.load("B", "LESTER")).entries == []
    # the old branch is untouched, the new one continues after the inherited head
    assert len((await db.load("A", "XARDAS")).entries) == 3
    assert await db.append("B", "XARDAS", "user", "Снова здесь") == 3
    # ensure_branch is idempotent (reconnects resend the session)
    assert await db.ensure_branch("B", "A", saved_at) == 3

    # a branch of a branch inherits through the copy
    assert await db.ensure_branch("C", "B", 3) == 3
    assert [e.seq for e in (await db.load("C", "XARDAS")).entries] == [1, 2, 3]
    assert {b.id: b.head for b in await db.branches()} == {"A": 4, "B": 3, "C": 3}
    await db.close()


async def test_summary_folds_older_messages() -> None:
    db = await MemoryStore.open(":memory:")
    await db.ensure_branch("A")
    for i in range(5):
        await db.append("A", "XARDAS", "user", f"сообщение {i}")
    await db.add_summary("A", "XARDAS", "Герой пять раз что-то говорил.", covers=3)
    mem = await db.load("A", "XARDAS")
    assert mem.summary == "Герой пять раз что-то говорил." and mem.summary_seq == 3
    assert [e.content for e in mem.entries] == ["сообщение 3", "сообщение 4"]
    await db.close()


async def test_unknown_parent_starts_empty(tmp_path) -> None:
    db = await MemoryStore.open(tmp_path / "m.sqlite3")
    assert await db.ensure_branch("B", "LOST", 7) == 0
    await db.close()
    # persisted across reopen
    db = await MemoryStore.open(tmp_path / "m.sqlite3")
    assert [b.id for b in await db.branches()] == ["B"]
    await db.close()


async def test_search_finds_older_messages_by_word_stems() -> None:
    store = await MemoryStore.open(":memory:")
    await store.ensure_branch("A")
    await store.append("A", "X", "user", "Я видел ДРАКОНА у прохода!", "день 1, 10:00")
    await store.append("A", "X", "assistant", "Драконы? Ты уверен?", "день 1, 10:00")
    await store.append("A", "X", "user", "Принеси мне грибов", "день 1, 11:00")
    await store.append("A", "X", "user", "Дракон снова прилетел", "день 2, 09:00")  # newer than asked for

    found = await store.search("A", "X", "драконы", before_seq=3, limit=2)
    assert [(r.seq, r.game_time) for r in found] == [(1, "день 1, 10:00"), (2, "день 1, 10:00")]
    assert await store.search("A", "X", "драконы", before_seq=0) == []
    # nothing matches: the latest older messages come back anyway (the model reads them)
    assert [r.seq for r in await store.search("A", "X", "как меня зовут", before_seq=3, limit=2)] == [2, 3]
    await store.close()


async def test_facts_since_reads_only_the_newer_ones() -> None:
    store = await MemoryStore.open(":memory:")
    await store.ensure_branch("A")
    first = await store.add_fact("A", '{"fact": "died"}')
    await store.append("A", "X", "user", "не факт")  # other events share the seqs
    second = await store.add_fact("A", '{"fact": "quest"}')
    third = await store.add_fact("A", '{"fact": "paid"}')
    assert (first, second, third) == (1, 3, 4)
    assert await store.facts_since("A", 0) == [
        (1, '{"fact": "died"}'),
        (3, '{"fact": "quest"}'),
        (4, '{"fact": "paid"}'),
    ]
    assert await store.facts_since("A", first) == [(second, '{"fact": "quest"}'), (third, '{"fact": "paid"}')]
    assert await store.facts_since("A", third) == []
    # a load inherits them with their seqs
    await store.ensure_branch("B", "A", second)
    assert [seq for seq, _ in await store.facts_since("B", 0)] == [first, second]
    assert await store.facts_since("B", second) == []
    await store.close()


async def test_worker_keeps_commits_off_loop_and_message_rows_contiguous(tmp_path, monkeypatch) -> None:
    loop = asyncio.get_running_loop()
    loop_thread = threading.get_ident()
    threads = []
    entered = asyncio.Event()
    release = threading.Event()
    block = False
    original_connect = sqlite3.connect
    original_exit = memory._Transaction.__exit__
    original_close = memory._SqlMemory.close

    def connect(*args, **kwargs):
        threads.append(threading.get_ident())
        return original_connect(*args, **kwargs)

    def commit(self, exc_type, *exc):
        threads.append(threading.get_ident())
        if block and not release.is_set():
            loop.call_soon_threadsafe(entered.set)
            assert release.wait(5), "The test did not release the worker commit"
        return original_exit(self, exc_type, *exc)

    def close(self):
        threads.append(threading.get_ident())
        original_close(self)

    monkeypatch.setattr(sqlite3, "connect", connect)
    monkeypatch.setattr(memory._Transaction, "__exit__", commit)
    monkeypatch.setattr(memory._SqlMemory, "close", close)
    db = await MemoryStore.open(tmp_path / "worker.sqlite3")
    await db.ensure_branch("A")
    block = True
    batch = asyncio.create_task(
        db.append_messages("A", "X", (Message("user", "first"), Message("assistant", "reply")))
    )
    try:
        await asyncio.wait_for(entered.wait(), 3)
        following = asyncio.create_task(db.append("A", "Y", "user", "next"))
        await asyncio.sleep(0)  # another loop task progresses while the worker is blocked at COMMIT
        assert not batch.done() and not following.done()
    finally:
        release.set()
    entries, after = await asyncio.gather(batch, following)
    assert [entry.seq for entry in entries] == [1, 2] and after == 3
    await db.flush()

    original_append = memory._SqlMemory.append

    def fail_second(self, branch, npc, role, content, game_time=None):
        if content == "fail":
            raise sqlite3.OperationalError("Injected second-row failure")
        return original_append(self, branch, npc, role, content, game_time)

    monkeypatch.setattr(memory._SqlMemory, "append", fail_second)
    with pytest.raises(sqlite3.OperationalError, match="second-row failure"):
        await db.append_messages("A", "X", (Message("user", "durable prefix"), Message("assistant", "fail")))
    assert await db.head("A") == 4  # the first row retains its old per-row COMMIT
    await db.close()
    assert len(set(threads)) == 1 and threads[0] != loop_thread
    reopened = await MemoryStore.open(tmp_path / "worker.sqlite3")
    assert (await reopened.load("A", "X")).entries[-1].content == "durable prefix"
    await reopened.close()


async def test_complete_drains_repeated_cancellation_and_close() -> None:
    db = await MemoryStore.open(":memory:")
    await db.ensure_branch("A")
    entered, release = asyncio.Event(), asyncio.Event()
    published = []
    loop_thread = threading.get_ident()

    async def first_operation():
        assert db.in_completion
        first = await db.append("A", "X", "user", "first")
        entered.set()
        await release.wait()

        async def nested():
            assert db.in_completion
            return await db.add_fact("A", '{"fact": "nested"}')

        second = await db.complete(nested)
        assert threading.get_ident() == loop_thread
        published.extend((first, second))  # the real caller sends MemoryCmd after these receipts
        return second

    async def following_operation():
        seq = await db.append("A", "Y", "user", "following")
        published.append(seq)
        return seq

    first = asyncio.create_task(db.complete(first_operation))
    await entered.wait()
    assert not db.in_completion
    following = asyncio.create_task(db.complete(following_operation))
    await asyncio.sleep(0)
    first.cancel()
    await asyncio.sleep(0)
    first.cancel()
    closing = asyncio.create_task(db.close())
    await asyncio.sleep(0)
    closing.cancel()
    await asyncio.sleep(0)
    closing.cancel()
    shared_close = asyncio.create_task(db.close())
    with pytest.raises(RuntimeError, match="closing"):
        await db.complete(following_operation)
    assert not first.done() and not closing.done() and published == []
    release.set()
    assert await following == 3
    await shared_close
    with pytest.raises(asyncio.CancelledError):
        await first
    with pytest.raises(asyncio.CancelledError):
        await closing
    assert published == [1, 2, 3]
    with pytest.raises(RuntimeError, match="closing"):
        await db.head("A")
