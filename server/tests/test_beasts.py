import asyncio

import pytest

from gothic_ai import beasts
from gothic_ai.beasts import BeastDesk, chances, rules_tone
from gothic_ai.dialogue import DialogueManager
from gothic_ai.llm import FakeBackend
from gothic_ai.llm.base import QUICK_SLOT
from gothic_ai.orders import Orders
from gothic_ai.persona import PersonaProvider
from gothic_ai.protocol import BeastEntry
from gothic_ai.talk import TalkDesk
from tests.fake_game import FakeGame, connected
from tests.test_dialogue import XARDAS, _until
from tests.test_orders import GORN, _follows, fights
from tests.test_talk import person


async def test_wolf_tone_never_inherits_an_npc_slot():
    quick = FakeBackend("threat", delay=0)
    async with connected(FakeGame()) as link:
        desk = BeastDesk(link, quick=quick)
        token = QUICK_SLOT.set(2)
        try:
            await desk.tone("Прочь!")
        finally:
            QUICK_SLOT.reset(token)
    assert quick.slot_ids == [None]


def wolf(n: int = 1, metres: float = 10, state: str = "threaten", **extra) -> dict:
    return {
        "id": f"WOLF#{n}",
        "name": "Волк",
        "distance": metres * 100,
        "state": state,
        "hp": 60,
        "hp_max": 60,
        **extra,
    }


def entry(**extra) -> BeastEntry:
    return BeastEntry(**wolf(**extra))


class Rolls:
    def __init__(self, *rolls: float) -> None:
        self.queue = list(rolls)

    def random(self) -> float:
        return self.queue.pop(0) if self.queue else 0.5


async def _desk(fake: FakeGame, *rolls: float, quick=None, llm=None):
    cm = connected(fake)
    link = await cm.__aenter__()
    llm = llm or FakeBackend("Что?", delay=0)
    dialogue = DialogueManager(link, llm, PersonaProvider(None))
    talk = TalkDesk(link, dialogue, end_silence_s=0.2)
    talk.orders = Orders(link, talk, dialogue)
    talk.beasts = BeastDesk(link, quick=quick, rng=Rolls(*rolls))  # type: ignore[arg-type]
    return cm, dialogue, talk, llm


def cmds(fake: FakeGame) -> list[tuple[str, str]]:
    return [(m["npc"], m["do"]) for m in fake.beast_cmds]


@pytest.mark.parametrize(
    ("text", "tone"),
    [
        ("Тихо, тихо, хороший мальчик. Я просто мимо пройду.", "calm"),
        ("Не бойся, я тебя не трону.", "calm"),
        ("Стой! Хороший, хороший...", "calm"),  # soothing over a command
        ("Стой!", "command"),
        ("Фу! Сидеть!", "command"),
        ("Пошёл вон!", "threat"),
        ("Брысь отсюда!", "threat"),
        ("Иди сюда, тварь, я тебя прикончу!", "hostile"),
        ("Пошёл вон, сволочь!", "hostile"),  # hostility over a threat
        ("Который час?", "other"),
    ],
)
def test_the_tone_of_words_by_the_rules(text, tone):
    assert rules_tone(text) == tone


def test_what_makes_a_wolf_let_him_pass():
    calm = chances(entry(), "calm", "none", pack=0, tries=0).calm
    assert calm == pytest.approx(0.8)
    # hostility never, words that are neither soothing nor a command seldom
    assert chances(entry(), "hostile", "none", 0, 0).calm == 0.0
    assert chances(entry(), "other", "none", 0, 0).calm < 0.5
    # a wolf at him already, a blade in his hand, a wound, his blows, a pack, a try that failed: less likely
    assert chances(entry(state="attack"), "calm", "none", 0, 0).calm < calm
    assert chances(entry(), "calm", "melee", 0, 0).calm == pytest.approx(calm * 0.5)
    assert chances(entry(hp=20), "calm", "none", 0, 0).calm < calm
    assert chances(entry(hit_ms=3000), "calm", "none", 0, 0).calm < calm
    assert chances(entry(), "calm", "none", 2, 0).calm < calm
    assert chances(entry(), "calm", "none", 0, 1).calm < calm
    # kinds: a young wolf is easier, a warg harder; never quite sure
    young = chances(BeastEntry(**{**wolf(), "id": "YWOLF#1"}), "calm", "none", 0, 0).calm
    warg = chances(BeastEntry(**{**wolf(), "id": "WARG#1"}), "calm", "none", 0, 0).calm
    assert warg < calm < young <= beasts.MAX_CHANCE
    # a threat scares it off, sooner if it is young or hurt; a warg hardly runs
    flee = chances(entry(), "threat", "none", 0, 0)
    assert flee.calm == 0 and flee.flee == pytest.approx(0.3)
    assert chances(entry(hp=20), "threat", "none", 0, 0).flee > flee.flee
    assert chances(BeastEntry(**{**wolf(), "id": "WARG#1"}), "threat", "none", 0, 0).flee < flee.flee


async def test_a_soothed_wolf_lets_the_hero_pass():
    fake = FakeGame()
    cm, dialogue, talk, llm = await _desk(fake, 0.0)
    try:
        await fake.beasts(wolf())
        await _until(lambda: "WOLF#1" in talk.beasts.wolves)
        await fake.send(type="player_text", text="Тихо, тихо, хороший мальчик.", focus="WOLF")
        await _until(lambda: cmds(fake) == [("WOLF#1", "calm")])
        await _until(lambda: fake.transcripts)
    finally:
        await cm.__aexit__(None, None, None)

    calm = fake.beast_cmds[0]
    assert calm["sit"] is True and calm["ms"] == beasts.CALM_MS  # soothed: it sits down
    assert talk.last[0].id == "WOLF#1" and talk.last[0].why == "beast"
    assert talk.partner is None and llm.calls == []  # no conversation, no model asked
    v = talk.beasts.verdicts[0]
    assert (v.outcome, v.state) == ("calm", "threaten") and talk.beasts.last_tone[:2] == ("calm", "rules")


async def test_a_wolf_that_does_not_believe_him_attacks_and_the_next_try_is_harder():
    fake = FakeGame()
    cm, dialogue, talk, llm = await _desk(fake, 0.99, 0.0)
    try:
        await fake.beasts(wolf())
        await _until(lambda: "WOLF#1" in talk.beasts.wolves)
        await fake.send(type="player_text", text="Тихо, хороший.", focus="WOLF")
        await _until(lambda: cmds(fake) == [("WOLF#1", "attack")])
        first = talk.beasts.verdicts[0].chance
        await fake.beasts(wolf(state="attack", metres=2))
        await _until(lambda: talk.beasts.wolves["WOLF#1"].state == "attack")
        await fake.send(type="player_text", text="Тихо, хороший.", focus="WOLF")
        await _until(lambda: cmds(fake) == [("WOLF#1", "attack"), ("WOLF#1", "calm")])
    finally:
        await cm.__aexit__(None, None, None)

    second = talk.beasts.verdicts[0]
    assert second.state == "attack" and second.chance < first * 0.6  # biting, and he failed once already


async def test_hostility_sets_it_on_him_and_a_threat_may_scare_it_off():
    fake = FakeGame()
    cm, dialogue, talk, llm = await _desk(fake, 0.0, 0.0, 0.0)  # a roll each wolf, each time
    try:
        await fake.beasts(wolf(1), wolf(2, metres=12))
        await _until(lambda: len(talk.beasts.wolves) == 2)
        await fake.send(type="player_text", text="Сдохни, тварь!", focus="WOLF")
        # both at him hear it; no dice change hostility
        await _until(lambda: sorted(cmds(fake)) == [("WOLF#1", "attack"), ("WOLF#2", "attack")])
        assert [v.chance for v in talk.beasts.verdicts] == [0.0, 0.0]
        fake.beast_cmds.clear()
        await fake.beasts(wolf(3))
        await _until(lambda: list(talk.beasts.wolves) == ["WOLF#3"])
        await fake.send(type="player_text", text="Пошёл вон! Брысь!", focus="WOLF")
        await _until(lambda: cmds(fake) == [("WOLF#3", "flee")])
    finally:
        await cm.__aexit__(None, None, None)


async def test_a_calm_wolf_shrugs_off_a_shout_but_not_hostility():
    fake = FakeGame()
    cm, dialogue, talk, llm = await _desk(fake, 0.99, 0.99)
    try:
        await fake.beasts(wolf(state="calm", sitting=True))
        await _until(lambda: "WOLF#1" in talk.beasts.wolves)
        await fake.send(type="player_text", text="Пошёл вон!", focus="WOLF")
        await _until(lambda: talk.beasts.verdicts and talk.beasts.verdicts[0].outcome == "keep")
        assert cmds(fake) == []
        await fake.send(type="player_text", text="Убью!", focus="WOLF")
        await _until(lambda: cmds(fake) == [("WOLF#1", "attack")])
    finally:
        await cm.__aexit__(None, None, None)


async def test_stoi_is_the_wolfs_and_other_orders_the_companions():
    fake = FakeGame()
    cm, dialogue, talk, llm = await _desk(fake, 0.0)
    try:
        await _follows(fake, dialogue)  # Gorn goes with the hero
        await fake.beasts(wolf())
        await _until(lambda: "WOLF#1" in talk.beasts.wolves)
        await fake.send(type="player_text", text="Стой!")  # looking at no one
        await _until(lambda: cmds(fake) == [("WOLF#1", "calm")])
        assert talk.orders.standing[GORN].kind == "follow"  # he was not told to halt
        assert fake.beast_cmds[0].get("sit") is None  # a command: it stands
        await fake.send(type="player_text", text="За мной!")
        await _until(lambda: talk.last is not None and talk.last[0].why == "order")
    finally:
        await cm.__aexit__(None, None, None)

    assert cmds(fake) == [("WOLF#1", "calm")]


@pytest.mark.parametrize(
    "text",
    [
        "Он, смотри, волк, волк, бей его.",
        "¿Я валорн? Бей волка.",
        "О, волки, волки, бей волков.",
        "Бей его!",
        "В атаку!",
        "Взять его!",
    ],
)
async def test_live_attack_orders_never_reach_the_wolfs_tone_model(text):
    fake, quick = FakeGame(), FakeBackend("hostile", delay=0)
    fake.views[GORN] = {
        "npc": GORN,
        "name": "Горн",
        "sees": [
            {"id": "WOLF", "name": "Волк", "kind": "monster", "distance": 1000, "hero_cm": 900},
        ],
    }
    cm, dialogue, talk, llm = await _desk(fake, quick=quick)
    try:
        await _follows(fake, dialogue)
        await fake.beasts(wolf(1, 9, "threaten"), wolf(2, 12, "threaten"))
        await _until(lambda: len(talk.beasts.wolves) == 2)
        await fake.send(type="player_text", text=text)
        await _until(lambda: fights(fake) == ["WOLF"])
        assert (talk.last[0].id, talk.last[0].why) == (GORN, "order")
        assert talk.beasts.verdicts == [] and quick.calls == [] and cmds(fake) == []
        assert llm.calls == []
    finally:
        await cm.__aexit__(None, None, None)


@pytest.mark.parametrize(
    ("text", "focus", "target"),
    [
        ("Бей волка!", "YBLOODFLY", "YBLOODFLY"),  # focus before species
        ("Убей муху!", None, "YBLOODFLY"),
        ("Мочи мух!", None, "YBLOODFLY"),
        ("Руби волков!", None, "YWOLF"),
    ],
)
async def test_attack_target_is_pointed_or_nearest_visible_species(text, focus, target):
    fake = FakeGame()
    fake.views[GORN] = {
        "npc": GORN,
        "name": "Горн",
        "sees": [
            {"id": "YBLOODFLY", "name": "Кровавая муха", "kind": "monster", "distance": 1200, "hero_cm": 900},
            {"id": "YWOLF", "name": "Молодой волк", "kind": "monster", "distance": 1400, "hero_cm": 1100},
        ],
    }
    cm, dialogue, talk, _ = await _desk(fake)
    try:
        await _follows(fake, dialogue)
        # A closer wolf behind a wall must not win the named-species lookup.
        await fake.beasts(wolf(1, 6, "routine"))
        await _until(lambda: "WOLF#1" in talk.beasts.wolves)
        await fake.send(type="player_text", text=text, focus=focus)
        await _until(lambda: fights(fake) == [target])
        assert cmds(fake) == [] and talk.beasts.verdicts == []
    finally:
        await cm.__aexit__(None, None, None)


async def test_an_unseen_named_species_is_a_question_to_the_companion():
    fake, quick = FakeGame(), FakeBackend("hostile", delay=0)
    cm, dialogue, talk, _ = await _desk(fake, quick=quick)
    try:
        await _follows(fake, dialogue)
        await fake.beasts(wolf())
        await _until(lambda: "WOLF#1" in talk.beasts.wolves)
        await fake.send(type="player_text", text="Бей муху!")
        await _until(lambda: fake.said == ["Что?"])
        assert talk.last[0].id == GORN and fights(fake) == []
        assert quick.calls == [] and cmds(fake) == []
    finally:
        await cm.__aexit__(None, None, None)


async def test_a_request_for_help_does_not_start_a_fight_with_a_threatening_wolf():
    fake = FakeGame()
    cm, dialogue, talk, _ = await _desk(fake)
    try:
        await _follows(fake, dialogue)
        await fake.beasts(wolf())
        await _until(lambda: "WOLF#1" in talk.beasts.wolves)
        await fake.send(type="player_text", text="Помоги!")
        await _until(lambda: fake.said == ["Что?"])
        assert fights(fake) == [] and cmds(fake) == []
    finally:
        await cm.__aexit__(None, None, None)


async def test_a_plural_greeting_to_people_is_not_heard_by_a_wolf():
    fake, quick = FakeGame(), FakeBackend("hostile", delay=0)
    cm, dialogue, talk, _ = await _desk(fake, quick=quick)
    try:
        await _follows(fake, dialogue, person(XARDAS, "Ксардас", 6, True))
        talk.begin(GORN, "Горн")
        await fake.beasts(wolf())
        await _until(lambda: "WOLF#1" in talk.beasts.wolves)
        await fake.send(type="player_text", text="Пацаны, здорово!")
        await _until(lambda: fake.said == ["Что?"])
        assert talk.last[0].id == XARDAS and talk.last[0].why == "group"
        assert quick.calls == [] and cmds(fake) == []
    finally:
        await cm.__aexit__(None, None, None)


async def test_a_name_or_a_person_looked_at_keeps_the_words_for_people():
    fake = FakeGame()
    llm = FakeBackend("Держись!", delay=0)
    cm, dialogue, talk, _ = await _desk(fake, 0.0, llm=llm)
    try:
        await fake.roster(person(XARDAS, "Ксардас", 6, True))
        await fake.beasts(wolf())
        await _until(lambda: "WOLF#1" in talk.beasts.wolves and XARDAS in talk.people)
        await fake.send(type="player_text", text="Ксардас, помоги!")
        await _until(lambda: fake.said == ["Держись!"])
        assert talk.last[0].id == XARDAS
        await fake.send(type="player_text", text="Что мне делать?", focus=XARDAS)
        await _until(lambda: len(fake.said) == 2)
        assert talk.last[0].id == XARDAS
    finally:
        await cm.__aexit__(None, None, None)

    assert cmds(fake) == []


async def test_the_talk_key_makes_the_growling_wolves_wait_for_his_words():
    fake = FakeGame()
    cm, dialogue, talk, llm = await _desk(fake)
    try:
        # one growls at him, one bites him already, one growls too far off to hear him
        await fake.beasts(wolf(1), wolf(2, metres=2, state="attack"), wolf(3, metres=20))
        await _until(lambda: len(talk.beasts.wolves) == 3)
        await fake.send(type="ptt_start")
        await _until(lambda: cmds(fake) == [("WOLF#1", "listen")])
        assert fake.beast_cmds[0]["ms"] == beasts.LISTEN_MS
        await fake.send(type="ptt_stop")
        await _until(lambda: len(cmds(fake)) == 2)
        assert fake.beast_cmds[1]["ms"] == beasts.AFTER_STOP_MS  # a while yet: his words are on their way
        await fake.send(type="ptt_start")
        await _until(lambda: len(cmds(fake)) == 3)
        await fake.send(type="ptt_cancel")  # a tap: no words come
        await _until(lambda: cmds(fake)[-1] == ("WOLF#1", "resume"))
    finally:
        await cm.__aexit__(None, None, None)


async def test_the_quick_model_tells_the_tone_and_the_rules_stand_in(monkeypatch):
    monkeypatch.setattr(beasts, "TONE_S", 0.2)
    desk = BeastDesk(_NoLink(), quick=FakeBackend("calm", delay=0))  # type: ignore[arg-type]
    # the rules hear nothing soothing here, the model does
    assert rules_tone("Я тебе ничего не сделаю, дружище.") == "other"
    assert await desk.tone("Я тебе ничего не сделаю, дружище.") == ("calm", "quick")
    instructions, messages, _ = desk.quick.calls[0]  # type: ignore[union-attr]
    assert "волку" in instructions and messages[0].content == "Я тебе ничего не сделаю, дружище."
    # a word not on the list, or no word in time: the rules decide
    desk.quick = FakeBackend("Не знаю.", delay=0)
    assert await desk.tone("Пошёл вон!") == ("threat", "rules")
    desk.quick = FakeBackend("calm", first=1.0)
    assert await desk.tone("Пошёл вон!") == ("threat", "rules")


async def test_companions_leave_a_calm_wolf_be():
    fake = FakeGame()
    fake.nearby = [{"id": "WOLF", "name": "Волк", "distance": 700.0, "guild": 27, "dead": False}]
    cm, dialogue, talk, llm = await _desk(fake)
    try:
        await _follows(fake, dialogue)
        await fake.beasts(wolf(state="calm"))
        await _until(lambda: "WOLF#1" in talk.beasts.wolves)
        await fake.perception(GORN, "ASSESSENEMY", "WOLF", distance=700)
        await asyncio.sleep(0.1)
        assert fights(fake) == []
        await fake.beasts(wolf(state="threaten"))  # another one, or the same that he let down
        await _until(lambda: talk.beasts.wolves["WOLF#1"].state == "threaten")
        await fake.perception(GORN, "ASSESSENEMY", "WOLF", distance=700)
        await _until(lambda: fights(fake) == ["WOLF"])
    finally:
        await cm.__aexit__(None, None, None)


class _NoLink:
    def __init__(self) -> None:
        self.listeners: list = []


async def test_a_person_fighting_him_takes_the_words_unless_he_looks_at_the_wolf():
    fake = FakeGame()
    llm = FakeBackend("Сам напросился!", delay=0)
    cm, dialogue, talk, _ = await _desk(fake, 0.0, llm=llm)
    try:
        await fake.roster(person("BDT_1013_BANDIT_L", "Бандит", 3, True, state="ZS_ATTACK"))
        await fake.beasts(wolf())
        await _until(lambda: "WOLF#1" in talk.beasts.wolves and talk.people)
        await fake.send(type="player_text", text="Стой, хватит!")
        await _until(lambda: talk.last is not None)
        assert talk.last[0].why == "attacker" and cmds(fake) == []
        await fake.send(type="player_text", text="Тихо, хороший.", focus="WOLF")
        await _until(lambda: cmds(fake) == [("WOLF#1", "calm")])
        assert talk.last[0].why == "beast"
    finally:
        await cm.__aexit__(None, None, None)
