from gothic_ai import dialogue as dialogue_mod
from gothic_ai.dialogue import DialogueManager
from gothic_ai.llm.fake import FakeBackend
from gothic_ai.memory import MemoryStore
from gothic_ai.persona import PersonaProvider
from gothic_ai.talk import TalkDesk
from tests.fake_game import FakeGame, connected
from tests.test_dialogue import XARDAS, _until


async def test_memories_follow_save_and_load():
    store = MemoryStore(":memory:")
    fake = FakeGame(branch="A")
    async with connected(fake) as link:
        dialogue = DialogueManager(
            link, FakeBackend("Запомню тебя.", delay=0), PersonaProvider(None), memory=store
        )
        assert dialogue.branch == "A"
        dialogue.open(XARDAS, "Ксардас")
        await dialogue.say_as_player(XARDAS, "Меня зовут Тестер")
        await _until(lambda: fake.heads[-1:] == [("A", 2)])  # the game would write (A, 2) into a save
        await dialogue.say_as_player(XARDAS, "Это уже после сохранения")
        await _until(lambda: fake.heads[-1:] == [("A", 4)])

        # the player loads that save: the game starts branch B off (A, 2)
        await fake.send(type="session", branch="B", parent="A", parent_seq=2, reason="load")
        await _until(lambda: dialogue.branch == "B")
        conv = dialogue.open(XARDAS, "Ксардас")
        assert [m.content for m in conv.history] == ["Меня зовут Тестер", "Запомню тебя."]
        await _until(lambda: fake.heads[-1:] == [("B", 2)])

    # a reconnect (new link, same branch) reads the same memories back
    fake2 = FakeGame(branch="B")
    async with connected(fake2) as link:
        again = DialogueManager(link, FakeBackend(delay=0), PersonaProvider(None), memory=store)
        assert len(again.open(XARDAS, "Ксардас").history) == 2


async def test_a_new_conversation_is_noted_as_a_new_meeting():
    fake = FakeGame()
    llm = FakeBackend("Опять ты.", delay=0)
    xardas = {"id": XARDAS, "name": "Ксардас", "distance": 300.0, "in_view": True}
    async with connected(fake) as link:
        dialogue = DialogueManager(link, llm, PersonaProvider(None))
        talk = TalkDesk(link, dialogue, end_silence_s=0)
        await link.snapshot(XARDAS)  # game time for the note
        await fake.roster(xardas)
        await fake.send(type="player_text", text="Привет", focus=XARDAS)
        await _until(lambda: fake.statuses[-2:] == ["thinking", "idle"])
        await fake.roster({**xardas, "distance": 2000.0})  # the hero walks off mid-line: it is over
        await _until(lambda: talk.partner is None and XARDAS not in talk.attached)
        await fake.roster(xardas)
        await fake.send(type="player_text", text="Это снова я", focus=XARDAS)
        await _until(lambda: len(llm.calls) == 2 and fake.statuses.count("idle") == 2)

    contents = [m.content for m in dialogue.conversations[XARDAS].history]
    assert contents[:2] == ["Привет", "Опять ты."]
    assert contents[2] == "[Герой ушёл, не дослушав тебя]"
    assert contents[3] == "[Новая встреча с героем, день 1, 08:00]"
    assert contents[4:] == ["Это снова я", "Опять ты."]


async def test_old_messages_are_folded_into_a_summary(monkeypatch):
    monkeypatch.setattr(dialogue_mod, "MAX_HISTORY", 6)
    monkeypatch.setattr(dialogue_mod, "KEEP_RECENT", 2)
    store = MemoryStore(":memory:")
    llm = FakeBackend("Герой много спрашивал о драконах.", delay=0)
    async with connected(FakeGame()) as link:
        dialogue = DialogueManager(link, llm, PersonaProvider(None), memory=store)
        conv = dialogue.open(XARDAS, "Ксардас")
        for i in range(4):
            await dialogue.say_as_player(XARDAS, f"Вопрос {i}")
        await _until(lambda: conv.memory.summary != "")

    instructions, messages, tools = llm.calls[-1]
    assert "память персонажа" in instructions and not tools
    assert "Вопрос 0" in messages[0].content and "Вопрос 2" in messages[0].content
    assert [e.content for e in conv.memory.entries] == ["Вопрос 3", "Герой много спрашивал о драконах."]
    assert conv.history[0].role == "developer" and "драконах" in conv.history[0].content
    # stored: a later session reads the summary and only the recent messages
    mem = store.load("test-branch", XARDAS)
    assert mem.summary == conv.memory.summary and len(mem.entries) == 2


async def test_game_without_sessions_still_works():
    async with connected(FakeGame(branch=None)) as link:
        dialogue = DialogueManager(link, FakeBackend("Да.", delay=0), PersonaProvider(None))
        dialogue.open(XARDAS, "Ксардас")
        await dialogue.say_as_player(XARDAS, "Привет")
        assert dialogue.branch is not None and dialogue.branch.startswith("unsaved-")
