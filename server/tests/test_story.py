import asyncio

import pytest

from gothic_ai.game import GameError
from gothic_ai.memory import MemoryStore
from gothic_ai.story import FREED, QUEST, StoryPatches, freed
from tests.fake_game import FakeGame, connected
from tests.test_dialogue import _until
from tests.test_memory import close_memory_stores as close_memory_stores

HIDE = {"op": "disable", "node": "DIA_LOBART_WORKNOW"}
HIDE_HILDA = {"op": "disable", "node": "DIA_HILDA_HALLO"}


async def test_changes_follow_save_and_load():
    store = await MemoryStore.open(":memory:")
    fake = FakeGame(branch="A")
    async with connected(fake) as link:
        story = StoryPatches(link, store)
        await story.initialize()
        # a session starts with the branch's changes, none yet: whatever the game had is dropped
        await _until(lambda: len(fake.graph) == 1)
        assert fake.graph[0]["replace"] is True and fake.graph[0]["ops"] == []
        version = story.version
        await story.initialize()
        await asyncio.sleep(0)
        assert story.version == version and len(fake.graph) == 1

        await story.apply(HIDE)
        assert fake.graph[-1]["ops"] == [HIDE]
        await _until(lambda: fake.heads[-1:] == [("A", 1)])  # goes into the next save
        await story.apply(HIDE_HILDA)  # made after that save
        assert await story.ops() == [HIDE, HIDE_HILDA]

        # the player loads the save made at seq 1: only the first change comes back
        await fake.send(type="session", branch="B", parent="A", parent_seq=1, reason="load")
        await _until(lambda: fake.graph[-1].get("replace") and story.branch == "B")
        assert fake.graph[-1]["ops"] == [HIDE]
        assert await story.ops() == [HIDE]

        entered, release = asyncio.Event(), asyncio.Event()
        add_patch = store.add_patch

        async def delayed(branch, content):
            seq = await add_patch(branch, content)
            entered.set()
            await release.wait()
            return seq

        store.add_patch = delayed
        pending = asyncio.create_task(story.apply(HIDE_HILDA))
        await entered.wait()
        heads = list(fake.heads)
        graphs = len(fake.graph)
        # The SQL commit can finish after the reader has announced a different Session.
        await fake.send(type="session", branch="C", parent="B", parent_seq=1, reason="load")
        await _until(lambda: link.session is not None and link.session.branch == "C")
        release.set()
        with pytest.raises(GameError, match="session changed"):
            await pending
        await _until(lambda: story.branch == "C" and len(fake.graph) == graphs + 1)
        assert await story.ops() == [HIDE]
        assert fake.heads == heads  # no stale committed head is published in the new session
    await store.close()


async def test_rejected_changes_are_not_kept():
    fake = FakeGame(branch="A")
    async with connected(fake) as link:
        story = StoryPatches(link, await MemoryStore.open(":memory:"))
        await story.initialize()
        try:
            await story.apply({"op": "disable", "node": "BAD"})
            raise AssertionError("taken")
        except GameError as err:
            assert "no dialog BAD" in str(err)
        assert await story.ops() == []
    await story.memory.close()


async def test_multiple_records_are_kept_only_after_one_complete_patch():
    fake = FakeGame(branch="A")
    async with connected(fake) as link:
        story = StoryPatches(link, await MemoryStore.open(":memory:"))
        await story.initialize()
        await _until(lambda: len(fake.graph) == 1)
        records = [{"op": QUEST, "quest": {"id": f"DIR_Q{k}"}} for k in (1, 2)]
        await story.make(records, [HIDE, HIDE_HILDA])
        assert len(fake.graph) == 2 and fake.graph[-1]["ops"] == [HIDE, HIDE_HILDA]
        assert await story.records(QUEST) == records
        assert await story.ops() == [HIDE, HIDE_HILDA]
        # Each description and each operation belongs to the save's log.
        await _until(lambda: fake.heads[-1:] == [("A", 4)])
    await story.memory.close()


async def test_refusing_a_multiple_record_patch_restores_the_previous_graph():
    fake = FakeGame(branch="A")
    async with connected(fake) as link:
        story = StoryPatches(link, await MemoryStore.open(":memory:"))
        await story.initialize()
        await _until(lambda: len(fake.graph) == 1)
        await story.apply(HIDE)
        records = [{"op": QUEST, "quest": {"id": f"DIR_Q{k}"}} for k in (1, 2)]
        with pytest.raises(GameError, match="no dialog BAD"):
            await story.make(records, [HIDE_HILDA, {"op": "disable", "node": "BAD"}])
        assert await story.records(QUEST) == [] and await story.ops() == [HIDE]
        assert fake.graph[-1]["replace"] is True and fake.graph[-1]["ops"] == [HIDE]
    await story.memory.close()


async def test_a_chosen_added_dialog_stays_told():
    fake = FakeGame(branch="A")
    async with connected(fake) as link:
        story = StoryPatches(link, await MemoryStore.open(":memory:"))
        await story.initialize()
        await fake.send(type="dialog_chosen", npc="BAU_950_LOBART", node="DIA_LOBART_WORKNOW")
        await fake.send(type="dialog_chosen", npc="BAU_950_LOBART", node="DIR_WOLVES", made=True)
        await _until(lambda: fake.heads[-1:] == [("A", 1)])
        assert await story.ops() == [{"op": "told", "node": "DIR_WOLVES"}]
    await story.memory.close()


async def test_the_freed_npcs_dialogs_are_off_on_every_branch_and_never_recorded():
    store = await MemoryStore.open(":memory:")
    fake = FakeGame(branch="A")
    static = freed(lambda node: node != "DIA_ADDON_CAVALORN_LETSKILLBANDITS")  # a game without it
    assert [op["node"] for op in static] == [
        node for node in FREED if node != "DIA_ADDON_CAVALORN_LETSKILLBANDITS"
    ]
    async with connected(fake) as link:
        story = StoryPatches(link, store, static=static)
        await story.initialize()
        await _until(lambda: len(fake.graph) == 1)
        assert fake.graph[0]["ops"] == static
        await story.apply(HIDE)
        await fake.send(type="session", branch="B", parent="A", parent_seq=1, reason="load")
        await _until(lambda: len(fake.graph) == 3)
        assert fake.graph[-1]["replace"] is True and fake.graph[-1]["ops"] == [*static, HIDE]
        assert await story.ops() == [HIDE]
    await store.close()
