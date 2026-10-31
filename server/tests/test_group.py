import asyncio
import time

import pytest

import gothic_ai.dialogue
from gothic_ai.dialogue import DialogueManager
from gothic_ai.group import ANSWER, ANSWER_CUE, BY_NAME, QUIET_S, TESTIMONY, GroupTalk, Seat
from gothic_ai.llm import FakeBackend
from gothic_ai.persona import PersonaProvider
from gothic_ai.talk import TalkDesk
from tests.fake_game import FakeGame, connected
from tests.fakes import Clock
from tests.test_dialogue import HILDA, LOBART, _until
from tests.test_talk import person


@pytest.fixture(autouse=True)
def short_lines(monkeypatch):
    """Lines are heard for 0.3 s: the floor waits for them to be over."""
    monkeypatch.setattr(gothic_ai.dialogue, "line_duration", lambda text: 0.3)


async def _desk(fake: FakeGame, llm, clock=None):
    cm = connected(fake)
    link = await cm.__aenter__()
    dialogue = DialogueManager(link, llm, PersonaProvider(None))
    talk = TalkDesk(link, dialogue, end_silence_s=30)
    group = GroupTalk(link, dialogue, talk, **({"clock": clock} if clock else {}))
    dialogue.extensions.append(group)
    return cm, dialogue, talk, group


def notes(dialogue: DialogueManager, npc: str) -> list[str]:
    conv = dialogue.conversations.get(npc)
    return [m.content for m in conv.history if m.role == "developer"] if conv else []


def situation(llm: FakeBackend, call: int) -> str:
    return "\n".join(m.content for m in llm.calls[call][1] if m.role == "developer")


async def test_what_an_npc_says_is_heard_by_whoever_stands_by():
    fake = FakeGame()
    fake.nearby_of[LOBART] = [{"npc": HILDA, "name": "Хильда", "distance": 400, "guild": 0}]
    cm, dialogue, talk, group = await _desk(fake, FakeBackend("Чего тебе?", delay=0))
    try:
        await fake.roster(person(LOBART, "Лобарт", 3, True), person(HILDA, "Хильда", 6))
        await fake.send(type="player_text", text="Привет", focus=LOBART)
        await _until(lambda: len(notes(dialogue, HILDA)) == 2)
    finally:
        await cm.__aexit__(None, None, None)

    assert notes(dialogue, HILDA) == [
        "[Ты слышал, как герой сказал собеседнику (Лобарт): «Привет»]",
        "[Ты слышал, как Лобарт говорит герою: «Чего тебе?»]",  # not deaf to Lobart any more
    ]
    assert "говорит герою" not in " ".join(notes(dialogue, LOBART))  # not to itself


async def test_who_is_in_the_conversation_and_who_drops_out():
    fake = FakeGame()
    llm = FakeBackend("Ну?", delay=0)
    cm, dialogue, talk, group = await _desk(fake, llm)
    try:
        await fake.roster(person(LOBART, "Лобарт", 3, True), person(HILDA, "Хильда", 5, True))
        await fake.send(type="player_text", text="Лобарт, привет")
        await _until(lambda: len(llm.calls) == 1 and not dialogue.speaking(LOBART))
        await fake.send(type="player_text", text="Хильда, а ты что скажешь?")
        await _until(lambda: len(llm.calls) == 2 and not dialogue.speaking(HILDA))
        assert set(group.participants()) == {LOBART, HILDA}
        # Lobart walks off: out of it
        await fake.roster(person(LOBART, "Лобарт", 25), person(HILDA, "Хильда", 5, True))
        await fake.send(type="player_text", text="Хильда, а где он?")
        await _until(lambda: len(llm.calls) == 3)
    finally:
        await cm.__aexit__(None, None, None)

    assert "В разговоре" not in situation(llm, 0)  # alone with the hero: nothing to tell
    assert "В разговоре с героем ещё: Лобарт." in situation(llm, 1)
    assert "В разговоре" not in situation(llm, 2)
    assert set(group.participants()) == {HILDA}


async def test_the_conversation_is_over_when_nobody_speaks_in_it():
    fake = FakeGame()
    clock = Clock()
    cm, dialogue, talk, group = await _desk(fake, FakeBackend("Ну?", delay=0), clock)
    try:
        await fake.roster(person(LOBART, "Лобарт", 3, True), person(HILDA, "Хильда", 5, True))
        await fake.send(type="player_text", text="Лобарт, привет")
        await _until(lambda: LOBART in group.participants() and not dialogue.speaking(LOBART))
        group.on_said(HILDA, "Хильда", "Опять он.", "incident")
        assert set(group.participants()) == {LOBART, HILDA}  # she spoke in it near the hero
        assert group.situation(HILDA) == f"В разговоре с героем ещё: Лобарт. {BY_NAME}"
        dialogue.speaking = lambda npc: npc == LOBART  # he answers the hero
        assert (
            group.situation(HILDA) == f"В разговоре с героем ещё: Лобарт. Сейчас говорит: Лобарт. {BY_NAME}"
        )
        clock.t += QUIET_S + 1
        assert group.participants() == {}
    finally:
        await cm.__aexit__(None, None, None)


MALETH, VINO = "BAU_954_MALETH", "BAU_952_VINO"


def calls_of(llm: FakeBackend, name: str) -> list[int]:
    """Which of the model's calls were for the npc of that name."""
    return [i for i, (instructions, _, _) in enumerate(llm.calls) if name in instructions]


def says_of(fake: FakeGame, npc: str) -> list[float]:
    return [t for m, t in fake.says if m["npc"] == npc]


async def test_one_voice_at_a_time(monkeypatch):
    # 2026-10-13: Brago shouted «Держи его!» over his mate's answer to the hero, not knowing what it was
    monkeypatch.setattr(gothic_ai.dialogue, "line_duration", lambda text: 0.4)
    fake = FakeGame()
    llm = FakeBackend("Ну, здравствуй.", delay=0, first=0.3)  # each thinks a while before it writes
    cm, dialogue, talk, group = await _desk(fake, llm)
    try:
        await fake.roster(person(LOBART, "Лобарт", 3, True), person(HILDA, "Хильда", 5, True))
        await fake.send(type="player_text", text="Лобарт, привет")
        await _until(lambda: len(llm.calls) == 1)
        dialogue.open(HILDA, "Хильда")  # the fast loop's urgent shout of hers, while he thinks
        dialogue.event_turn(HILDA, "[Событие: герой сказал рядом: «Лобарт, привет»]", overlay=True)
        await _until(lambda: len(says_of(fake, HILDA)) == 1 and not dialogue.speaking(HILDA), 5)
    finally:
        await cm.__aexit__(None, None, None)

    assert calls_of(llm, "Лобарт") == [0] and calls_of(llm, "Хильда") == [1]
    # she thought once his text was ready: his words are in what she knows
    assert "[Ты слышал, как Лобарт говорит герою: «Ну, здравствуй.»]" in situation(llm, 1)
    # ...and was heard after him
    assert says_of(fake, HILDA)[0] >= says_of(fake, LOBART)[-1] + 0.35


async def test_three_voices_to_a_phrase_of_the_hero():
    fake = FakeGame()
    llm = FakeBackend("Ага.", delay=0, first=0.1)
    cm, dialogue, talk, group = await _desk(fake, llm)
    try:
        far = person("BAU_960_FAR", "Дальний", 30)
        near = [
            person(n, nm, d, True)
            for n, nm, d in ((HILDA, "Хильда", 4), (MALETH, "Малет", 5), (VINO, "Вино", 6))
        ]
        await fake.roster(person(LOBART, "Лобарт", 3, True), *near, far)
        await fake.send(type="player_text", text="Лобарт, привет")
        await _until(lambda: len(llm.calls) == 1)
        for npc, name in ((HILDA, "Хильда"), (MALETH, "Малет"), (VINO, "Вино"), ("BAU_960_FAR", "Дальний")):
            dialogue.open(npc, name)
            dialogue.event_turn(npc, "[Событие: герой сказал рядом: «Лобарт, привет»]", overlay=True)
        # one away from the conversation is not on its floor: it does not wait
        await _until(lambda: calls_of(llm, "Дальний") != [], 2)
        hilda = calls_of(llm, "Хильда")
        assert not hilda or calls_of(llm, "Дальний")[0] < hilda[0]
        await _until(lambda: calls_of(llm, "Малет") != [] and not dialogue.speaking(MALETH), 5)
        await asyncio.sleep(0.3)
    finally:
        await cm.__aexit__(None, None, None)

    # the addressee and two more, in the order they asked; the fourth says nothing
    order = sorted((calls_of(llm, n)[0], n) for n in ("Лобарт", "Хильда", "Малет"))
    assert [n for _, n in order] == ["Лобарт", "Хильда", "Малет"]
    assert calls_of(llm, "Вино") == [] and says_of(fake, VINO) == []


async def test_the_heros_voice_cuts_the_floor():
    fake = FakeGame()
    llm = FakeBackend("Сейчас скажу.", delay=0, first=0.5)
    cm, dialogue, talk, group = await _desk(fake, llm)
    try:
        await fake.roster(person(LOBART, "Лобарт", 3, True), person(HILDA, "Хильда", 5, True))
        await fake.send(type="player_text", text="Лобарт, привет")
        await _until(lambda: len(llm.calls) == 1)
        dialogue.open(HILDA, "Хильда")
        dialogue.event_turn(HILDA, "[Событие: герой сказал рядом: «Лобарт, привет»]", overlay=True)
        await asyncio.sleep(0.1)
        assert group.waiting(HILDA)
        await fake.send(type="ptt_start")  # he speaks again: Lobart stops, Hilda says nothing
        await _until(lambda: not dialogue.speaking(LOBART) and not dialogue.speaking(HILDA))
        await fake.send(type="player_text", text="Хильда, а ты что скажешь?")
        await _until(lambda: len(calls_of(llm, "Хильда")) == 1 and not dialogue.speaking(HILDA), 5)
    finally:
        await cm.__aexit__(None, None, None)

    assert len(calls_of(llm, "Лобарт")) == 1  # cut while it thought
    hilda = dialogue.conversations[HILDA].history
    assert "Событие" not in " ".join(m.content for m in hilda)  # her queued shout never came
    assert "Хильда, а ты что скажешь?" in [m.content for m in hilda if m.role == "user"]


async def test_a_phrase_to_several_goes_to_the_conversation():
    fake = FakeGame()
    llm = FakeBackend("Чего надо?", delay=0)
    cm, dialogue, talk, group = await _desk(fake, llm)
    talk.lead = {"BDT_1014_BANDIT_L": 8, "BDT_1015_BANDIT_L": 6}.get  # Brago leads
    try:
        await fake.roster(
            person("BDT_1015_BANDIT_L", "Бандит", 3, True), person("BDT_1014_BANDIT_L", "Браго", 5, True)
        )
        await fake.send(type="player_text", text="Пацаны, здорово!")
        await _until(lambda: len(llm.calls) == 1 and not dialogue.speaking("BDT_1014_BANDIT_L"))
    finally:
        await cm.__aexit__(None, None, None)

    assert calls_of(llm, "Браго") == [0]  # the leader answers first, the nearer one is not asked
    assert "Последнюю фразу герой сказал всем вам, ты отвечаешь первым." in situation(llm, 0)
    assert "В разговоре с героем ещё: Бандит." in situation(llm, 0)
    assert notes(dialogue, "BDT_1015_BANDIT_L")[0] == (
        "[Герой сказал всем вам, и тебе тоже: «Пацаны, здорово!» (первым отвечает Браго)]"
    )


class ByName:
    """A scripted model for each npc: the one whose «Ты — {name}» its instructions have."""

    name = "fake"

    def __init__(self, by: dict[str, FakeBackend]) -> None:
        self.by = by
        self.order: list[str] = []  # whose turns, in the order they began to think

    async def aclose(self) -> None:
        pass

    async def prewarm(self, **kw) -> None:
        pass

    def stream(self, *, instructions: str, **kw):
        name = next(n for n in self.by if f"Ты — {n}" in instructions)
        self.order.append(name)
        return self.by[name].stream(instructions=instructions, **kw)


def says(fake: FakeGame, npc: str) -> list[dict]:
    return [m for m, _ in fake.says if m["npc"] == npc]


async def test_an_npc_speaks_to_another_by_name_and_it_answers_him():
    fake = FakeGame()
    lobart = FakeBackend("Она самая, двадцать лет уже вместе живём. Хильда, поздоровайся с гостем!", delay=0)
    hilda = FakeBackend("Ну здравствуй, раз пришёл.", delay=0)
    llm = ByName({"Лобарт": lobart, "Хильда": hilda})
    cm, dialogue, talk, group = await _desk(fake, llm)
    try:
        await fake.roster(person(LOBART, "Лобарт", 3, True), person(HILDA, "Хильда", 4, True))
        await fake.send(type="player_text", text="Лобарт, это твоя жена?")
        await _until(lambda: len(says(fake, HILDA)) == 1 and not dialogue.speaking(HILDA), 5)
    finally:
        await cm.__aexit__(None, None, None)

    # «Хильда, …» is said to her (the two look at each other); what came before it, to the hero
    assert says(fake, LOBART)[-1]["to"] == HILDA and says(fake, LOBART)[0].get("to") in (None, HILDA)
    # she answers him next, not the hero: her line goes to him, the one who spoke to her
    assert llm.order == ["Лобарт", "Хильда"] and says(fake, HILDA)[0]["to"] == LOBART
    assert hilda.calls[0][1][-1].content == ANSWER_CUE.format(name="Лобарт")
    assert (
        "[Ты слышал, как Лобарт говорит тебе: «Она самая, двадцать лет уже вместе живём. Хильда, "
        "поздоровайся с гостем!»]"
    ) in notes(dialogue, HILDA)
    # ...heard after him
    t = {id(m): at for m, at in fake.says}
    assert t[id(says(fake, HILDA)[0])] >= t[id(says(fake, LOBART)[-1])] + 0.25


@pytest.mark.parametrize("kind", [TESTIMONY, "incident"])
async def test_a_witness_names_the_one_who_asked_it_and_he_does_not_get_an_answer_turn(kind):
    # 2026-10-29: Hilda's «Лобарт, видела я его утром» gave Lobart an answer turn with set_intent only; his
    # «соврал» went there, and the witness turn of his with catch_lie never came (witnesses.py reacts itself)
    fake = FakeGame()
    lobart = FakeBackend("Ну?", delay=0)
    hilda = FakeBackend("Лобарт, видела я его утром. У нас во дворе стоял, у репы.", delay=0.05)
    llm = ByName({"Лобарт": lobart, "Хильда": hilda})
    cm, dialogue, talk, group = await _desk(fake, llm)
    try:
        await fake.roster(person(LOBART, "Лобарт", 3, True), person(HILDA, "Хильда", 4, True))
        await _until(lambda: HILDA in talk.people and LOBART in talk.people, 5)
        dialogue.open(LOBART, "Лобарт")
        dialogue.open(HILDA, "Хильда")
        # her testimony; or a turn of hers before it while he waits for the answer (he called her aloud)
        group.asks = lambda npc: kind != TESTIMONY and npc == LOBART
        dialogue.event_turn(HILDA, "[Лобарт спрашивает тебя о герое.]", tools=(), kind=kind, to=LOBART)
        await _until(lambda: len(says(fake, HILDA)) == 2 and not dialogue.speaking(HILDA), 5)
        await asyncio.sleep(0.2)
    finally:
        await cm.__aexit__(None, None, None)

    assert llm.order == ["Хильда"]  # Lobart has no turn of the floor's
    assert all(m["to"] == LOBART for m in says(fake, HILDA))
    heard = (
        "[Ты слышал, как Хильда говорит тебе: «Лобарт, видела я его утром. У нас во дворе стоял, "
        "у репы.»]"
    )
    assert heard in notes(dialogue, LOBART)


async def test_the_one_called_takes_a_voice_of_those_waiting():
    fake = FakeGame()
    lobart = FakeBackend("Привет, привет, проходи. Хильда, принеси гостю воды!", delay=0, first=0.3)
    llm = ByName(
        {
            "Лобарт": lobart,
            "Хильда": FakeBackend("Сейчас принесу.", delay=0),
            "Малет": FakeBackend("Ага.", delay=0),
            "Вино": FakeBackend("Ну-ну.", delay=0),
        }
    )
    cm, dialogue, talk, group = await _desk(fake, llm)
    try:
        await fake.roster(
            person(LOBART, "Лобарт", 3, True),
            person(HILDA, "Хильда", 4, True),
            person(MALETH, "Малет", 5, True),
            person(VINO, "Вино", 6, True),
        )
        await fake.send(type="player_text", text="Лобарт, привет")
        await _until(lambda: llm.order == ["Лобарт"])
        for npc, name in ((MALETH, "Малет"), (VINO, "Вино")):  # the fast loop's shouts: two voices more
            dialogue.open(npc, name)
            dialogue.event_turn(npc, "[Событие: герой сказал рядом: «Лобарт, привет»]", overlay=True)
        await _until(lambda: len(says(fake, MALETH)) == 1 and not dialogue.speaking(MALETH), 5)
        await asyncio.sleep(0.3)
    finally:
        await cm.__aexit__(None, None, None)

    # Hilda answers right after him; Vino, the last of those waiting, gives her his voice
    assert llm.order == ["Лобарт", "Хильда", "Малет"] and says(fake, VINO) == []
    assert says(fake, HILDA)[0]["to"] == LOBART and "to" not in says(fake, MALETH)[0]


async def test_whom_a_line_calls():
    fake = FakeGame()
    cm, dialogue, talk, group = await _desk(fake, FakeBackend("Ну?", delay=0))
    brago, mate, steps = "BDT_1014_BANDIT_L", "BDT_1015_BANDIT_L", "BDT_1013_BANDIT_L"
    try:
        await fake.roster(
            person(mate, "Бандит", 3, True),
            person(brago, "Браго", 4, True),
            person(steps, "Бандит", 5, True),
            person("BAU_960_FAR", "Дальний", 30),
        )
        await _until(lambda: len(talk.people) == 4)
        assert group.called(brago, "Ну что, бандит, скажешь?") == mate  # of two, the nearer to the hero
        group.on_said(steps, "Бандит", "А я его знаю!", "incident")
        assert group.called(brago, "Ну что, бандит, скажешь?") == steps  # ...but the one who just spoke
        assert group.called(mate, "Браго, глянь на него.") == brago
        assert group.called(mate, "Только троньте его, Браго.") == brago
        assert group.called(brago, "Скажи бандиту, пусть идёт сюда.") is None  # spoken of, not called
        assert group.called(brago, "Браго, Браго…") is None  # not itself
        assert group.called(brago, "Дальний, иди сюда!") is None  # away from the hero
        assert group.called("BAU_960_FAR", "Браго, иди сюда!") is None  # nor one away calling
    finally:
        await cm.__aexit__(None, None, None)


async def test_a_line_waits_by_the_seat_of_the_turn_that_said_it():
    # 2026-10-14, e2e `group`: Cavalorn's «Браго, верни…» still waited for its voice when Brago called him
    # back; by Cavalorn's newest seat, behind Brago's, it waited for Brago's answer to it
    fake = FakeGame()
    cm, dialogue, talk, group = await _desk(fake, FakeBackend("Ну?", delay=0))
    try:
        await fake.roster(person(LOBART, "Лобарт", 3, True), person(HILDA, "Хильда", 4, True))
        await _until(lambda: len(talk.people) == 2)
        first = Seat(LOBART, 0.0, id=1, ready=True)
        group.seats = [first]
        said_then = group.seat(LOBART)
        group.seats += [Seat(HILDA, 0.0, id=2, kind=ANSWER), Seat(LOBART, 0.0, id=3, kind=ANSWER)]
        await asyncio.wait_for(group.hear(LOBART, said_then), 0.5)  # his line of the first turn: at once
        with pytest.raises(TimeoutError):
            await asyncio.wait_for(group.hear(LOBART, group.seat(LOBART)), 0.3)  # of the next: after her
    finally:
        await cm.__aexit__(None, None, None)


async def test_lines_of_a_reply_still_to_be_heard_when_the_next_turn_begins():
    # ...and once his next turn began, his lines of the one before were no longer waited for at all
    fake = FakeGame()
    cm, dialogue, talk, group = await _desk(fake, FakeBackend("Посмотрим ещё.", delay=0, first=0.3))
    try:
        conv = dialogue.open(LOBART, "Лобарт")
        dialogue._lines["AIB_X"] = ("Браго, верни то, что взял!", None)  # its voice is being made
        dialogue._sent["AIB_X"] = time.monotonic()
        conv.last_lines = ["AIB_X"]
        dialogue.event_turn(LOBART, "[Браго обращается к тебе.]")
        await _until(lambda: conv.last_lines == [] and conv.reply is not None)
        assert dialogue.audible(LOBART)  # the line of the turn before is still to come
        dialogue._lines["AIB_X"] = ("Браго, верни то, что взял!", time.monotonic() - 10)  # said long ago
        assert not dialogue.audible(LOBART)
        await _until(lambda: not dialogue.speaking(LOBART), 5)
    finally:
        await cm.__aexit__(None, None, None)


async def test_a_seat_waits_for_the_lines_of_those_before_it_not_for_their_next_turns():
    # e2e `group`, 2026-10-14: Brago's answer waited for Cavalorn's next turn, which waited for Brago's answer
    fake = FakeGame()
    cm, dialogue, talk, group = await _desk(fake, FakeBackend("Ну?", delay=0))
    try:
        await fake.roster(person(LOBART, "Лобарт", 3, True), person(HILDA, "Хильда", 4, True))
        await _until(lambda: len(talk.people) == 2)
        group.seats = [
            Seat(LOBART, 0.0, id=1, ready=True),  # he called her: all of it heard by now
            Seat(HILDA, 0.0, id=2, kind=ANSWER, ready=True),  # her answer, its voice being made
            Seat(LOBART, 0.0, id=3, kind=ANSWER, ready=True),  # she called him back: his line waits for hers
        ]
        now = time.monotonic()
        for npc, line, text, seat in (
            (HILDA, "H2", "Ты бы помолчал.", 2),
            (LOBART, "L3", "Посмотрим ещё.", 3),
        ):
            dialogue.open(npc, "x").last_lines = [line]
            dialogue._lines[line], dialogue._sent[line], dialogue._line_seats[line] = (text, None), now, seat
        await asyncio.wait_for(group.hear(HILDA, 2), 0.5)  # hers: not after his line of the next turn
        with pytest.raises(TimeoutError):
            await asyncio.wait_for(group.hear(LOBART, 3), 0.3)  # his: after hers
    finally:
        await cm.__aexit__(None, None, None)
