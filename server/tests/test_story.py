import pytest

from gothic_ai.game import GameError
from gothic_ai.memory import MemoryStore
from gothic_ai.story import FREED, QUEST, StoryPatches, freed
from tests.fake_game import FakeGame, connected
from tests.test_dialogue import _until

HIDE = {"op": "disable", "node": "DIA_LOBART_WORKNOW"}
HIDE_HILDA = {"op": "disable", "node": "DIA_HILDA_HALLO"}


async def test_changes_follow_save_and_load():
    store = MemoryStore(":memory:")
    fake = FakeGame(branch="A")
    async with connected(fake) as link:
        story = StoryPatches(link, store)
        # a session starts with the branch's changes, none yet: whatever the game had is dropped
        await _until(lambda: len(fake.graph) == 1)
        assert fake.graph[0]["replace"] is True and fake.graph[0]["ops"] == []

        await story.apply(HIDE)
        assert fake.graph[-1]["ops"] == [HIDE]
        await _until(lambda: fake.heads[-1:] == [("A", 1)])  # goes into the next save
        await story.apply(HIDE_HILDA)  # made after that save
        assert story.ops() == [HIDE, HIDE_HILDA]

        # the player loads the save made at seq 1: only the first change comes back
        await fake.send(type="session", branch="B", parent="A", parent_seq=1, reason="load")
        await _until(lambda: fake.graph[-1].get("replace") and story.branch == "B")
        assert fake.graph[-1]["ops"] == [HIDE]
        assert story.ops() == [HIDE]


async def test_rejected_changes_are_not_kept():
    fake = FakeGame(branch="A")
    async with connected(fake) as link:
        story = StoryPatches(link, MemoryStore(":memory:"))
        try:
            await story.apply({"op": "disable", "node": "BAD"})
            raise AssertionError("taken")
        except GameError as err:
            assert "no dialog BAD" in str(err)
        assert story.ops() == []


async def test_multiple_records_are_kept_only_after_one_complete_patch():
    fake = FakeGame(branch="A")
    async with connected(fake) as link:
        story = StoryPatches(link, MemoryStore(":memory:"))
        await _until(lambda: len(fake.graph) == 1)
        records = [{"op": QUEST, "quest": {"id": f"DIR_Q{k}"}} for k in (1, 2)]
        await story.make(records, [HIDE, HIDE_HILDA])
        assert len(fake.graph) == 2 and fake.graph[-1]["ops"] == [HIDE, HIDE_HILDA]
        assert story.records(QUEST) == records
        assert story.ops() == [HIDE, HIDE_HILDA]
        # Each description and each operation belongs to the save's log.
        await _until(lambda: fake.heads[-1:] == [("A", 4)])


async def test_refusing_a_multiple_record_patch_restores_the_previous_graph():
    fake = FakeGame(branch="A")
    async with connected(fake) as link:
        story = StoryPatches(link, MemoryStore(":memory:"))
        await _until(lambda: len(fake.graph) == 1)
        await story.apply(HIDE)
        records = [{"op": QUEST, "quest": {"id": f"DIR_Q{k}"}} for k in (1, 2)]
        with pytest.raises(GameError, match="no dialog BAD"):
            await story.make(records, [HIDE_HILDA, {"op": "disable", "node": "BAD"}])
        assert story.records(QUEST) == [] and story.ops() == [HIDE]
        assert fake.graph[-1]["replace"] is True and fake.graph[-1]["ops"] == [HIDE]


async def test_a_chosen_added_dialog_stays_told():
    fake = FakeGame(branch="A")
    async with connected(fake) as link:
        story = StoryPatches(link, MemoryStore(":memory:"))
        await fake.send(type="dialog_chosen", npc="BAU_950_LOBART", node="DIA_LOBART_WORKNOW")
        await fake.send(type="dialog_chosen", npc="BAU_950_LOBART", node="DIR_WOLVES", made=True)
        await _until(lambda: story.ops() == [{"op": "told", "node": "DIR_WOLVES"}])


async def test_the_freed_npcs_dialogs_are_off_on_every_branch_and_never_recorded():
    store = MemoryStore(":memory:")
    fake = FakeGame(branch="A")
    static = freed(lambda node: node != "DIA_ADDON_CAVALORN_LETSKILLBANDITS")  # a game without it
    assert [op["node"] for op in static] == [
        node for node in FREED if node != "DIA_ADDON_CAVALORN_LETSKILLBANDITS"
    ]
    async with connected(fake) as link:
        story = StoryPatches(link, store, static=static)
        await _until(lambda: len(fake.graph) == 1)
        assert fake.graph[0]["ops"] == static
        await story.apply(HIDE)
        await fake.send(type="session", branch="B", parent="A", parent_seq=1, reason="load")
        await _until(lambda: len(fake.graph) == 3)
        assert fake.graph[-1]["replace"] is True and fake.graph[-1]["ops"] == [*static, HIDE]
        assert story.ops() == [HIDE]
