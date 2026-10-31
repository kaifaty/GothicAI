import asyncio

import pytest

import gothic_ai.dialogue
from gothic_ai.brain.decider import Outcome
from gothic_ai.brain.reflex import Reflexes
from gothic_ai.budget import Budget
from gothic_ai.dialogue import DialogueManager
from gothic_ai.group import GroupTalk
from gothic_ai.interject import STEP_IN_CUE, WORD_CUE, Interjections
from gothic_ai.llm import FakeBackend, FakeStep
from gothic_ai.persona import PersonaProvider
from gothic_ai.talk import Addressee, TalkDesk
from tests.fake_game import FakeGame, connected
from tests.test_dialogue import HILDA, LOBART, _until
from tests.test_talk import person

MALETH, VINO = "BAU_954_MALETH", "BAU_952_VINO"


class ByCharacter:
    """A behavioral model that answers `interject` by who asks: name -> (P(word), P(step in))."""

    def __init__(self, urge: dict[str, tuple[float, float]]) -> None:
        self.urge = urge
        self.asked: list[str] = []

    async def decide(self, rid, lane, priority, deadline_ms, state, questions) -> Outcome:
        answers = {}
        for name in questions:
            who = next((n for n in self.urge if f"You are {n}." in state), None)
            if name != "interject" or who is None:
                continue
            self.asked.append(who)
            word, step_in = self.urge[who]
            probs = {"silent": 1 - word - step_in, "word": word, "step_in": step_in}
            pick = max(probs, key=lambda k: probs[k])
            answers[name] = {
                "type": "choice",
                "choice": pick,
                "probabilities": probs,
                "confidence": probs[pick],
            }
        return Outcome("answers", answers, 1.0, 20.0)

    async def cancel(self, rid: str) -> None:
        pass


@pytest.fixture(autouse=True)
def short_lines(monkeypatch):
    monkeypatch.setattr(gothic_ai.dialogue, "line_duration", lambda text: 0.2)


async def _desk(fake: FakeGame, llm, decider, budget: Budget | None = None):
    cm = connected(fake)
    link = await cm.__aenter__()
    dialogue = DialogueManager(link, llm, PersonaProvider(None))
    talk = TalkDesk(link, dialogue, end_silence_s=30)
    group = GroupTalk(link, dialogue, talk)
    dialogue.extensions.append(group)
    interject = Interjections(dialogue, talk, group, Reflexes(decider), budget or Budget())
    return cm, dialogue, talk, interject


def cues(dialogue: DialogueManager, npc: str) -> list[str]:
    conv = dialogue.conversations.get(npc)
    return [m.content for m in conv.history if m.content in (WORD_CUE, STEP_IN_CUE)] if conv else []


NEAR = (
    person(LOBART, "Лобарт", 3, True),
    person(HILDA, "Хильда", 4, True),
    person(MALETH, "Малет", 5, True),
    person(VINO, "Вино", 6, True),
)


async def test_the_likeliest_to_speak_up_do_two_at_most():
    fake = FakeGame()
    llm = FakeBackend("Ну?", delay=0)
    decider = ByCharacter({"Хильда": (0.2, 0.7), "Малет": (0.7, 0.15), "Вино": (0.6, 0.22)})
    cm, dialogue, talk, interject = await _desk(fake, llm, decider)
    try:
        await fake.roster(*NEAR)
        await fake.send(type="player_text", text="Лобарт, привет")
        await _until(
            lambda: len(llm.calls) == 3 and not any(dialogue.speaking(n) for n in (HILDA, MALETH)), 5
        )
        await asyncio.sleep(0.3)
    finally:
        await cm.__aexit__(None, None, None)

    assert sorted(decider.asked) == ["Вино", "Малет", "Хильда"]  # not Lobart: he answered
    # Hilda (0.9) steps in, Maleth (0.85) puts in a word; Vino (0.82) would too, but two at most
    assert cues(dialogue, HILDA) == [STEP_IN_CUE] and cues(dialogue, MALETH) == [WORD_CUE]
    assert cues(dialogue, VINO) == [] and len(llm.calls) == 3
    assert interject.spoke == {HILDA, MALETH}


async def test_a_witness_being_asked_is_not_asked_to_speak_up():
    # it speaks from its testimony only (witnesses.py): Lobart's question to her is no cue to chime in on
    fake = FakeGame()
    llm = FakeBackend("Ну?", delay=0)
    decider = ByCharacter({"Хильда": (0.2, 0.7), "Малет": (0.7, 0.15), "Вино": (0.05, 0.05)})
    cm, dialogue, talk, interject = await _desk(fake, llm, decider)
    interject.group.witness = lambda npc: npc == HILDA
    try:
        await fake.roster(*NEAR)
        await fake.send(type="player_text", text="Лобарт, привет")
        await _until(lambda: len(llm.calls) == 2 and not dialogue.speaking(MALETH), 5)
        await asyncio.sleep(0.3)
    finally:
        await cm.__aexit__(None, None, None)

    assert sorted(decider.asked) == ["Вино", "Малет"] and cues(dialogue, HILDA) == []
    assert cues(dialogue, MALETH) == [WORD_CUE]


async def test_nobody_speaks_up_twice_in_a_row_and_the_quiet_keep_quiet():
    fake = FakeGame()
    llm = FakeBackend("Ну?", delay=0)
    decider = ByCharacter({"Хильда": (0.3, 0.6), "Малет": (0.1, 0.1), "Вино": (0.2, 0.1)})
    cm, dialogue, talk, interject = await _desk(fake, llm, decider)
    try:
        await fake.roster(*NEAR)
        await fake.send(type="player_text", text="Лобарт, привет")
        await _until(lambda: len(llm.calls) == 2 and not dialogue.speaking(HILDA), 5)
        await asyncio.sleep(0.3)
        await fake.send(type="player_text", text="Лобарт, как дела?")
        await _until(lambda: len(llm.calls) >= 3 and not dialogue.speaking(LOBART), 5)
        await asyncio.sleep(0.5)
    finally:
        await cm.__aexit__(None, None, None)

    # Maleth (0.2) and Vino (0.3) keep quiet; Hilda spoke up after the first phrase, so not after the second
    assert cues(dialogue, HILDA) == [STEP_IN_CUE] and cues(dialogue, MALETH) == cues(dialogue, VINO) == []
    assert decider.asked.count("Хильда") == 1 and decider.asked.count("Малет") == 2
    assert len(llm.calls) == 3


async def test_no_budget_no_words():
    fake = FakeGame()
    llm = FakeBackend("Ну?", delay=0)
    budget = Budget(per_npc_per_min=1, total_per_min=1)  # Lobart's answer takes the one call
    cm, dialogue, talk, interject = await _desk(fake, llm, ByCharacter({"Хильда": (0.1, 0.8)}), budget)
    try:
        assert budget.allow(LOBART)
        await fake.roster(*NEAR[:2])
        await fake.send(type="player_text", text="Лобарт, привет")
        await _until(lambda: len(llm.calls) == 1 and not dialogue.speaking(LOBART), 5)
        await asyncio.sleep(0.3)
    finally:
        await cm.__aexit__(None, None, None)
    assert cues(dialogue, HILDA) == [] and len(llm.calls) == 1


@pytest.mark.parametrize("brago_p, speaks", [(0.79, True), (0.74, False)])
async def test_bandit_replies_after_cavalorns_threat_from_live_game(brago_p, speaks):
    # 2026-10-18 06:32:40: 0.79 appeared in the log, but the 0.8 gate never started a turn.
    # Reproduce the already routed exchange independently of the plural-address fix in talk.py.
    cavalorn, brago, mate = "BAU_4300_ADDON_CAVALORN", "BDT_1014_BANDIT_L", "BDT_1015_BANDIT_L"
    fake = FakeGame()
    llm = FakeBackend(script=[FakeStep("Прикрой мне спину!"), FakeStep("Сам ты свинья!")], delay=0)
    decider = ByCharacter({"Браго": (brago_p, 0.0), "Бандит": (0.17, 0.0)})
    cm, dialogue, talk, interject = await _desk(fake, llm, decider)
    try:
        await fake.roster(
            person(cavalorn, "Кавалорн", 2),
            person(brago, "Браго", 4, True),
            person(mate, "Бандит", 5, True),
        )
        await _until(lambda: len(talk.people) == 3)
        talk.last = (Addressee(cavalorn, "Кавалорн", "partner"), "Здарова, псы.")
        interject.group.on_phrase(*talk.last)
        dialogue.open(cavalorn, "Кавалорн")
        await dialogue.say_as_player(cavalorn, talk.last[1])
        await _until(lambda: bool(interject.asked))
        if speaks:
            await _until(lambda: any(m["npc"] == brago for m, _ in fake.says), 5)
            await _until(lambda: not dialogue.speaking(brago))
        else:
            await _until(lambda: not dialogue.speaking(cavalorn))
    finally:
        await cm.__aexit__(None, None, None)

    assert interject.spoke == ({brago} if speaks else set())
    assert cues(dialogue, brago) == ([WORD_CUE] if speaks else [])
    assert not any(m["npc"] == mate for m, _ in fake.says)  # 0.17 still keeps quiet
    assert len(llm.calls) == (2 if speaks else 1)
    if speaks:
        heard = [(m["npc"], at) for m, at in fake.says]
        assert [npc for npc, _ in heard] == [cavalorn, brago]
        assert heard[1][1] >= heard[0][1] + 0.15  # the bandit's actual say follows Cavalorn's line
        assert fake.says[1][0]["text"] == "Сам ты свинья!"
