import json

from gothic_ai.dialogue import DialogueManager
from gothic_ai.journal import PREFIX, Journal, print_sink
from gothic_ai.llm.fake import FakeBackend
from gothic_ai.memory import MemoryStore
from gothic_ai.persona import PersonaProvider
from gothic_ai.talk import TalkDesk
from tests.fake_game import FakeGame, connected
from tests.test_dialogue import XARDAS, FailingBackend, _until
from tests.test_memory import close_memory_stores as close_memory_stores


async def test_player_and_npc_lines_reach_the_journal():
    fake = FakeGame()
    entries: list[dict] = []
    llm = FakeBackend("Придержи язык.", delay=0)
    async with connected(fake) as link:
        TalkDesk(
            link,
            DialogueManager(
                link,
                llm,
                PersonaProvider(None),
                journal=Journal(entries.append),
                memory=await MemoryStore.open(":memory:"),
            ),
        )
        await fake.roster({"id": XARDAS, "name": "Ксардас", "distance": 300.0, "in_view": True})
        await fake.send(type="player_text", text="Привет, Ксардас.")
        await _until(lambda: any(e["kind"] == "npc" for e in entries))

    player, npc = entries
    assert player["kind"] == "player" and player["text"] == "Привет, Ксардас." and player["via"] == "text"
    assert player["npc"] == XARDAS and player["name"] == "Ксардас" and player["by"] == "name"
    assert npc["kind"] == "npc" and npc["text"] == "Придержи язык." and npc["llm"] == "fake"
    assert npc["total_s"] >= npc["first_token_s"] >= 0 and npc["t"] >= player["t"]


async def test_llm_errors_reach_the_journal():
    fake = FakeGame()
    entries: list[dict] = []
    async with connected(fake) as link:
        dialogue = DialogueManager(
            link,
            FailingBackend(),
            PersonaProvider(None),
            journal=Journal(entries.append),
            memory=await MemoryStore.open(":memory:"),
        )
        await dialogue.initialize()
        await dialogue.open(XARDAS, "Ксардас")
        await dialogue.say_as_player(XARDAS, "Привет")

    assert [(e["kind"], e["detail"]) for e in entries] == [("llm_error", "plan limit reached")]


def test_print_sink_writes_one_prefixed_json_line(capsys):
    print_sink({"kind": "npc", "text": "Ступай."})
    out = capsys.readouterr().out
    assert out.startswith(PREFIX) and out.count("\n") == 1
    assert json.loads(out.removeprefix(PREFIX)) == {"kind": "npc", "text": "Ступай."}
