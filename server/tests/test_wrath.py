from gothic_ai.dialogue import DialogueManager
from gothic_ai.llm import ToolCall
from gothic_ai.llm.fake import FakeBackend, FakeStep
from gothic_ai.memory import MemoryStore
from gothic_ai.persona import PersonaProvider
from gothic_ai.wrath import TAKE_OFFENSE, TEMPERS, Wrath
from tests.fake_game import FakeGame, connected
from tests.test_dialogue import XARDAS, _until
from tests.test_memory import close_memory_stores as close_memory_stores


def offense(severity: int, what: str, unforgivable: bool = False, cid: str = "c1") -> ToolCall:
    return ToolCall(TAKE_OFFENSE, {"severity": severity, "unforgivable": unforgivable, "what": what}, cid)


async def _setup(fake: FakeGame, llm: FakeBackend):
    link_cm = connected(fake)
    link = await link_cm.__aenter__()
    dialogue = DialogueManager(link, llm, PersonaProvider(None), memory=await MemoryStore.open(":memory:"))
    await dialogue.initialize()
    wrath = Wrath(link, dialogue)
    dialogue.extensions.append(wrath)
    await link.attach(XARDAS)
    await link.snapshot(XARDAS)
    return link_cm, dialogue, wrath


def _raised(fake: FakeGame) -> list[dict]:
    return [m for m in fake.story if m["type"] == "raise_dead"]


async def test_insults_fill_the_scales_until_they_tip():
    fake = FakeGame(branch="A")
    llm = FakeBackend(
        script=[
            FakeStep("Следи за языком.", [offense(2, "назвал стариком")]),
            FakeStep("Ты испытываешь моё терпение.", [offense(2, "назвал шарлатаном", cid="c2")]),
            FakeStep("", [offense(1, "нахамил", cid="c3")]),
            FakeStep("Довольно."),
        ],
        delay=0,
    )
    link_cm, dialogue, wrath = await _setup(fake, llm)
    try:
        await dialogue.open(XARDAS, "Ксардас")
        assert TAKE_OFFENSE in [t.name for t in await dialogue._offered(XARDAS)]
        assert "осталось 5 из 5" in wrath.situation(XARDAS)
        await dialogue.say_as_player(XARDAS, "Эй, старик!")
        await _until(lambda: wrath.of(XARDAS).took == 2)
        await dialogue.say_as_player(XARDAS, "Да ты шарлатан.")
        await _until(lambda: wrath.of(XARDAS).took == 4)
        assert "ты на пределе" in wrath.situation(XARDAS)
        assert _raised(fake) == []
        # the third: the scales tip, he says his last word knowing it, then the game raises the hero
        await dialogue.say_as_player(XARDAS, "Заткнись.")
        await _until(lambda: _raised(fake) != [])
        msg = _raised(fake)[0]
        assert (msg["npc"], msg["spell"]) == (XARDAS, "SPL_SKULL")
        assert msg["card"]["image"].endswith("xardas-end.jpg")
        assert "Чаша твоего терпения переполнена" in llm.calls[3][1][-1].output  # the last word knows
        assert wrath.of(XARDAS).tipped and wrath.situation(XARDAS) is None
        assert TAKE_OFFENSE not in [t.name for t in await dialogue._offered(XARDAS)]
        assert dialogue.conversations[XARDAS].intent.attitude == "hostile"
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_the_unforgivable_tips_them_at_once():
    fake = FakeGame(branch="A")
    llm = FakeBackend(script=[FakeStep("", [offense(3, "оскорбил мать", True)]), FakeStep("Зря.")], delay=0)
    link_cm, dialogue, wrath = await _setup(fake, llm)
    try:
        assert "матери" in wrath.situation(XARDAS)
        await dialogue.open(XARDAS, "Ксардас")
        await dialogue.say_as_player(XARDAS, "Твоя мама...")
        await _until(lambda: _raised(fake) != [])
        assert wrath.of(XARDAS).took == TEMPERS[XARDAS].patience
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_only_npcs_with_a_temper_take_offense():
    fake = FakeGame(branch="A")
    link_cm, dialogue, wrath = await _setup(fake, FakeBackend(script=[], delay=0))
    try:
        assert wrath.tools("PC_THIEF") == [] and wrath.situation("PC_THIEF") is None
    finally:
        await link_cm.__aexit__(None, None, None)
