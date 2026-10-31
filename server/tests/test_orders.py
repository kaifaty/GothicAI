import asyncio

import pytest

from gothic_ai.dialogue import DialogueManager
from gothic_ai.executor import Intent
from gothic_ai.llm import FakeBackend, FakeStep, ToolCall
from gothic_ai.orders import Orders
from gothic_ai.persona import PersonaProvider
from gothic_ai.places import PlaceBook
from gothic_ai.protocol import NpcView
from gothic_ai.talk import TalkDesk
from tests.fake_game import FakeGame, connected
from tests.test_dialogue import LOBART, XARDAS, _until
from tests.test_talk import person

GORN = "PC_FIGHTER"
BANDIT = "BDT_1013_BANDIT"


@pytest.mark.parametrize(
    "text",
    [
        "Не бей волка.",
        "Он сказал: бей волка.",
        "Если увидишь волка, бей его.",
        "Он крикнул «убей волка».",
        "Как убить волка?",
    ],
)
def test_long_attack_parser_ignores_negation_and_reported_speech(text):
    assert Orders.parse_attack(text) is None


async def _desk(fake: FakeGame, llm, **orders):
    cm = connected(fake)
    link = await cm.__aenter__()
    dialogue = DialogueManager(link, llm, PersonaProvider(None))
    talk = TalkDesk(link, dialogue, end_silence_s=0.2)
    talk.orders = Orders(link, talk, dialogue, **orders)
    return cm, dialogue, talk


def companion(dialogue: DialogueManager, npc: str, name: str) -> None:
    dialogue.open(npc, name).intent = Intent("follow_hero", "PLAYER", "friendly")


def acts(fake: FakeGame) -> list[tuple[str, str]]:
    return [(m["npc"], m["action"]) for m in fake.received if m["type"] == "action"]


async def test_a_companion_stops_at_once_and_follows_again_later():
    fake = FakeGame()
    llm = FakeBackend("Стою.", delay=0)
    cm, dialogue, talk = await _desk(fake, llm, halt_s=0.1)
    try:
        await fake.roster(person(GORN, "Горн", 4), person(XARDAS, "Ксардас", 3, True))
        companion(dialogue, GORN, "Горн")
        await fake.send(type="player_text", text="Стой!", focus=XARDAS)
        await _until(lambda: (GORN, "stop") in acts(fake))
        assert talk.orders.standing[GORN].kind == "halt"
        assert talk.last[0].id == GORN and talk.last[0].why == "order"  # not Xardas he looks at
        await _until(lambda: (GORN, "follow") in acts(fake))  # HALT_S later, it goes on as before
        await _until(lambda: talk.held.get(GORN) == {"follow"})  # free of the order once it follows
    finally:
        await cm.__aexit__(None, None, None)

    assert acts(fake) == [(GORN, "stop"), (GORN, "stop"), (GORN, "follow")]
    assert llm.calls == []  # no model was asked
    assert GORN not in talk.orders.standing and talk.held[GORN] == {"follow"}
    notes = [m.content for m in dialogue.conversations[GORN].history]
    assert any("Герой приказал тебе: «Стой!» — остановиться. Ты подчинился." in n for n in notes)


async def test_an_order_to_someone_who_is_no_companion_is_words_to_him():
    fake = FakeGame()
    llm = FakeBackend("С чего бы мне идти за тобой?", delay=0)
    cm, dialogue, talk = await _desk(fake, llm)
    try:
        await fake.roster(person(LOBART, "Лобарт", 3, True, state="ZS_STAND"))
        await fake.send(type="player_text", text="За мной!", focus=LOBART)
        await _until(lambda: fake.said == ["С чего бы мне идти за тобой?"])
    finally:
        await cm.__aexit__(None, None, None)

    assert talk.orders.standing == {}
    assert (LOBART, "follow") not in acts(fake)  # he decides in words


@pytest.mark.parametrize(
    "text",
    [
        "Ну ты идешь, нет?",
        "Давай за мной к пещере.",
        "Пошли бандитов бить.",
        "Ну, ты, если что, не туда идешь, ты в стену куда-то идешь. Иди за мной.",
    ],
)
async def test_long_follow_requests_move_an_existing_companion_without_a_model(text):
    fake, llm = FakeGame(), FakeBackend("Иду!", delay=0)
    cm, dialogue, talk = await _desk(fake, llm)
    try:
        await fake.roster(person(GORN, "Горн", 4))
        companion(dialogue, GORN, "Горн")
        await fake.send(type="player_text", text=text)
        await _until(lambda: (GORN, "follow") in acts(fake))
        assert llm.calls == [] and talk.last[0].why == "order"
    finally:
        await cm.__aexit__(None, None, None)


async def test_reported_follow_words_do_not_order_an_existing_companion():
    fake = FakeGame()
    cm, dialogue, talk = await _desk(fake, FakeBackend("Что он сказал?", delay=0))
    try:
        await fake.roster(person(GORN, "Горн", 4))
        companion(dialogue, GORN, "Горн")
        await fake.send(type="player_text", text="Он мне сказал. Иди за мной.", focus=GORN)
        await _until(lambda: fake.said == ["Что он сказал?"])
        assert talk.last[0].why != "order"
    finally:
        await cm.__aexit__(None, None, None)

    assert (GORN, "follow") not in acts(fake)


async def test_attack_whom_the_hero_looks_at_then_back_to_him():
    fake = FakeGame()
    cm, dialogue, talk = await _desk(fake, FakeBackend("На кого?", delay=0))
    try:
        await fake.roster(person(GORN, "Горн", 4), person(BANDIT, "Бандит", 9, True))
        companion(dialogue, GORN, "Горн")
        await fake.send(type="player_text", text="В атаку!", focus=BANDIT)
        await _until(lambda: (GORN, "follow") in acts(fake))
    finally:
        await cm.__aexit__(None, None, None)

    fight = next(m for m in fake.received if m["type"] == "action" and m["action"] == "fight")
    assert fight["args"] == {"npc": BANDIT}
    assert acts(fake) == [(GORN, "fight"), (GORN, "stop"), (GORN, "follow")]  # the fight over: follows


async def test_attack_with_nobody_to_attack_is_a_question_for_the_model():
    fake = FakeGame()
    llm = FakeBackend("На кого?", delay=0)
    cm, dialogue, talk = await _desk(fake, llm)
    try:
        await fake.roster(person(GORN, "Горн", 4, True))
        companion(dialogue, GORN, "Горн")
        await fake.send(type="player_text", text="В атаку!", focus=GORN)
        await _until(lambda: fake.said == ["На кого?"])
    finally:
        await cm.__aexit__(None, None, None)

    assert (GORN, "fight") not in acts(fake)


async def test_a_name_picks_one_companion_out_of_two():
    fake = FakeGame()
    cm, dialogue, talk = await _desk(fake, FakeBackend("-", delay=0), wait_s=60)
    try:
        await fake.roster(person(GORN, "Горн", 4), person(LOBART, "Лобарт", 6))
        companion(dialogue, GORN, "Горн")
        companion(dialogue, LOBART, "Лобарт")
        await fake.send(type="player_text", text="Лобарт, жди здесь!")
        await _until(lambda: (LOBART, "stop") in acts(fake))
        assert set(talk.orders.standing) == {LOBART}
        # the weapon changes no standing order: it waits with the weapon out
        await fake.send(type="player_text", text="Все, к оружию!")
        await _until(
            lambda: (
                {a for a in acts(fake) if a[1] == "draw_weapon"}
                == {
                    (GORN, "draw_weapon"),
                    (LOBART, "draw_weapon"),
                }
            )
        )
        assert talk.orders.standing[LOBART].kind == "wait"
        await _until(lambda: GORN not in talk.held)  # held only to draw: free again once it is out
    finally:
        await cm.__aexit__(None, None, None)


async def test_dismissed_it_goes_back_to_its_business():
    fake = FakeGame()
    cm, dialogue, talk = await _desk(fake, FakeBackend("-", delay=0))
    try:
        await fake.roster(person(GORN, "Горн", 4))
        companion(dialogue, GORN, "Горн")
        await fake.send(type="player_text", text="За мной!")
        await _until(lambda: (GORN, "follow") in acts(fake))
        assert GORN in fake.attached
        await fake.send(type="player_text", text="Свободен.")
        await _until(lambda: GORN not in fake.attached)
        await asyncio.sleep(0.05)
    finally:
        await cm.__aexit__(None, None, None)

    assert dialogue.conversations[GORN].intent.goal == "keep_routine"
    assert GORN not in talk.held and talk.orders.standing == {}
    assert acts(fake) == [(GORN, "follow"), (GORN, "stop")]  # no second stop from the intent


def fights(fake: FakeGame) -> list[str]:
    return [m["args"]["npc"] for m in fake.received if m["type"] == "action" and m["action"] == "fight"]


async def _follows(fake: FakeGame, dialogue: DialogueManager, *people: dict) -> None:
    """Gorn goes with the hero (attached, following), the people around."""
    await fake.roster(person(GORN, "Горн", 4), *people)
    companion(dialogue, GORN, "Горн")
    await fake.send(type="player_text", text="За мной!")
    await _until(lambda: (GORN, "follow") in acts(fake))


async def test_a_companion_stands_up_for_the_hero_without_an_order():
    fake = FakeGame()
    llm = FakeBackend("-", delay=0)
    cm, dialogue, talk = await _desk(fake, llm)
    try:
        await _follows(fake, dialogue, person(BANDIT, "Бандит", 6, True))
        await fake.perception(GORN, "ASSESSOTHERSDAMAGE", BANDIT, victim="PLAYER")
        await _until(lambda: acts(fake)[-2:] == [(GORN, "stop"), (GORN, "follow")])
        await _until(lambda: talk.held.get(GORN) == {"follow"})
    finally:
        await cm.__aexit__(None, None, None)

    assert fights(fake) == [BANDIT]
    assert acts(fake) == [(GORN, "follow"), (GORN, "fight"), (GORN, "stop"), (GORN, "follow")]  # back to him
    assert llm.calls == []  # the body did not wait for the conversational model
    notes = [m.content for m in dialogue.conversations[GORN].history]
    assert any("Бандит напал на героя — ты бросился в бой." in n for n in notes)


async def test_a_monster_close_by_is_fought_one_far_off_is_not():
    fake = FakeGame()
    fake.nearby = [{"id": "YWOLF", "name": "Волк", "distance": 700.0, "guild": 38, "dead": False}]
    cm, dialogue, talk = await _desk(fake, FakeBackend("-", delay=0))
    try:
        await _follows(fake, dialogue)
        await fake.perception(GORN, "ASSESSENEMY", "YWOLF", distance=2500)
        await asyncio.sleep(0.1)
        assert fights(fake) == []
        await fake.perception(GORN, "ASSESSENEMY", "YWOLF", distance=700)
        await _until(lambda: fights(fake) == ["YWOLF"])
    finally:
        await cm.__aexit__(None, None, None)


@pytest.mark.parametrize(
    "changes",
    [
        {"combat_target": "OTHER_NPC"},
        {"distance": 2600},
        {"fighting": False, "moving": False},
        {"kind": "human"},
        {"body": "down"},
    ],
)
async def test_a_companion_does_not_mistake_other_monster_activity_for_a_hunt(changes):
    fake = FakeGame()
    cm, dialogue, talk = await _desk(fake, FakeBackend("-", delay=0))
    try:
        await _follows(fake, dialogue)
        wolf = {
            "id": "YWOLF",
            "name": "Волк",
            "kind": "monster",
            "distance": 2300,
            "combat_target": "PLAYER",
            "fighting": True,
            **changes,
        }
        await fake.push_views({"npc": GORN, "name": "Горн", "sees": [wolf]})
        await asyncio.sleep(0.05)
        assert fights(fake) == []
    finally:
        await cm.__aexit__(None, None, None)


async def test_a_companion_intercepts_a_monster_before_any_damage_perception():
    fake = FakeGame()
    cm, dialogue, talk = await _desk(fake, FakeBackend("-", delay=0))
    try:
        await _follows(fake, dialogue)
        await fake.push_views(
            {
                "npc": GORN,
                "name": "Горн",
                "sees": [
                    {
                        "id": "YWOLF",
                        "name": "Волк",
                        "kind": "monster",
                        "distance": 2300,
                        "combat_target": "PLAYER",
                        "fighting": True,
                    },
                ],
            }
        )
        await _until(lambda: fights(fake) == ["YWOLF"])
    finally:
        await cm.__aexit__(None, None, None)


@pytest.mark.parametrize("flee_state", ["ZS_FLEE", "ZS_MM_FLEE"])
async def test_a_hostile_monster_closing_on_the_hero_is_intercepted_before_attack_state(flee_state):
    fake = FakeGame()
    cm, dialogue, talk = await _desk(fake, FakeBackend("-", delay=0))
    try:
        await _follows(fake, dialogue)
        wolf = {
            "id": "YWOLF",
            "name": "Волк",
            "kind": "monster",
            "distance": 2400,
            "combat_target": "PLAYER",
            "moving": True,
            "att_hero": "hostile",
            "hero_cm": 2200,
            "state": flee_state,
        }
        await fake.push_views({"npc": GORN, "name": "Горн", "sees": [wolf]})
        await asyncio.sleep(0.05)
        assert fights(fake) == []
        await fake.push_views({"npc": GORN, "name": "Горн", "sees": [{**wolf, "hero_cm": 2000}]})
        await asyncio.sleep(0.05)
        assert fights(fake) == []  # a fleeing monster can retain the hero as its native target
        await fake.push_views(
            {"npc": GORN, "name": "Горн", "sees": [{**wolf, "hero_cm": 1800, "state": "ZS_MM_THREATENENEMY"}]}
        )
        await _until(lambda: fights(fake) == ["YWOLF"])
    finally:
        await cm.__aexit__(None, None, None)


async def test_a_retreating_companion_does_not_intercept_a_monster_hunt():
    fake = FakeGame()
    cm, dialogue, talk = await _desk(fake, FakeBackend("-", delay=0))
    try:
        await _follows(fake, dialogue)
        await fake.send(type="player_text", text="Отступаем!")
        await _until(lambda: acts(fake).count((GORN, "follow")) == 2)
        await fake.push_views(
            {
                "npc": GORN,
                "name": "Горн",
                "sees": [
                    {
                        "id": "YWOLF",
                        "name": "Волк",
                        "kind": "monster",
                        "distance": 1300,
                        "combat_target": "PLAYER",
                        "fighting": True,
                    },
                ],
            }
        )
        await asyncio.sleep(0.05)
        assert fights(fake) == []
    finally:
        await cm.__aexit__(None, None, None)


async def test_after_retreat_it_stands_up_only_for_itself():
    fake = FakeGame()
    cm, dialogue, talk = await _desk(fake, FakeBackend("-", delay=0))
    try:
        await _follows(fake, dialogue, person(BANDIT, "Бандит", 6))
        await fake.send(type="player_text", text="Отступаем!")
        await _until(lambda: acts(fake).count((GORN, "follow")) == 2)  # broke off, follows him
        await fake.perception(GORN, "ASSESSOTHERSDAMAGE", BANDIT, victim="PLAYER")
        await asyncio.sleep(0.1)
        assert fights(fake) == []  # it gets away with the hero
        await fake.perception(GORN, "ASSESSDAMAGE", BANDIT)
        await _until(lambda: fights(fake) == [BANDIT])  # struck itself, it fights back
    finally:
        await cm.__aexit__(None, None, None)


async def test_halt_said_looking_at_whoever_strikes_the_hero_is_words_to_him():
    fake = FakeGame()
    llm = FakeBackend("Стоять? Ещё чего!", delay=0)
    cm, dialogue, talk = await _desk(fake, llm)
    try:
        await fake.roster(person(GORN, "Горн", 4), person(BANDIT, "Бандит", 3, True))
        companion(dialogue, GORN, "Горн")
        await fake.perception(GORN, "ASSESSOTHERSDAMAGE", BANDIT, victim="PLAYER")
        await fake.send(type="player_text", text="Стой, стой!", focus=BANDIT)
        await _until(lambda: fake.said == ["Стоять? Ещё чего!"])
    finally:
        await cm.__aexit__(None, None, None)

    assert talk.last[0].id == BANDIT and talk.last[0].why == "focus"
    assert GORN not in talk.orders.standing and (GORN, "stop") not in acts(fake)


async def test_attack_with_no_one_in_sight_goes_at_whoever_struck_the_hero():
    fake = FakeGame()
    cm, dialogue, talk = await _desk(fake, FakeBackend("-", delay=0))
    try:
        await _follows(fake, dialogue, person(BANDIT, "Бандит", 9))
        talk.orders.foes[BANDIT] = talk.orders.clock()  # struck him a moment ago
        await fake.send(type="player_text", text="Бей его, бей!")
        await _until(lambda: fights(fake) == [BANDIT])
    finally:
        await cm.__aexit__(None, None, None)


async def test_a_named_stranger_is_whom_to_attack():
    fake = FakeGame()
    llm = FakeBackend("-", delay=0)
    cm, dialogue, talk = await _desk(fake, llm)
    try:
        await _follows(fake, dialogue, person(BANDIT, "Бандит", 12))
        await fake.send(type="player_text", text="Убей бандита!")
        await _until(lambda: fights(fake) == [BANDIT])
    finally:
        await cm.__aexit__(None, None, None)
    assert llm.calls == [] and talk.last[0].id == GORN  # an order to Gorn, not words to the bandit


async def test_the_conversational_models_fight_ends_with_the_companion_back_at_the_heros_side():
    fake = FakeGame()
    fake.nearby = [{"id": BANDIT, "name": "Бандит", "distance": 600.0, "guild": 9, "dead": False}]
    fight = {"goal": "fight", "target": BANDIT, "attitude": "friendly", "agenda": None}
    llm = FakeBackend("Сейчас я его!", delay=0, tool_calls=[ToolCall("set_intent", fight)])
    cm, dialogue, talk = await _desk(fake, llm)
    try:
        await _follows(fake, dialogue, person(BANDIT, "Бандит", 6))
        await fake.send(type="player_text", text="Горн, разберись с ним.")
        await _until(lambda: fights(fake) == [BANDIT])
        await _until(lambda: dialogue.conversations[GORN].intent.goal == "follow_hero")
        await _until(lambda: acts(fake)[-1] == (GORN, "follow") and talk.held.get(GORN) == {"follow"})
    finally:
        await cm.__aexit__(None, None, None)

    assert acts(fake) == [(GORN, "follow"), (GORN, "fight"), (GORN, "stop"), (GORN, "follow")]
    assert GORN in fake.attached  # never let go of in between


async def test_guarding_the_hero_keeps_the_companion_with_him():
    fake = FakeGame()
    guard = {"goal": "guard_hero", "target": "PLAYER", "attitude": "friendly", "agenda": None}
    llm = FakeBackend("Я прикрою.", delay=0, tool_calls=[ToolCall("set_intent", guard)])
    cm, dialogue, talk = await _desk(fake, llm)
    try:
        await _follows(fake, dialogue)
        await fake.send(type="player_text", text="Горн, прикрой меня.")
        await _until(lambda: dialogue.conversations[GORN].intent.goal == "guard_hero")
        await asyncio.sleep(0.1)
    finally:
        await cm.__aexit__(None, None, None)
    assert GORN in fake.attached and "follow" in talk.held[GORN]
    assert (GORN, "stop") not in acts(fake)


BOOK = {
    "version": 2,
    "world": "NEWWORLD.ZEN",
    "places": [
        {"id": "TOWER", "ru": "башня", "where_ru": "в башне", "en": "tower", "where_en": "in the tower",
         "center": [1, 2, 3], "kind": "house", "inside": True},
        {"id": "CAVE", "ru": "пещера бандитов", "where_ru": "в пещере бандитов", "en": "cave",
         "where_en": "in the cave", "center": [500, 2, 3], "kind": "cave", "inside": True,
         "entrances": [{"in": "NW_XARDAS_STAIRS_01", "out": "NW_XARDAS_TOWER_IN1_31", "pos": [450, 2, 3],
                        "side": "TOWER"}]},
        {"id": "FARM", "ru": "ферма", "where_ru": "на ферме", "en": "farm", "where_en": "at the farm",
         "center": [90000, 2, 3], "kind": "yard"},
    ],
    "points": {"NW_XARDAS_TOWER_IN1_31": 0, "NW_XARDAS_STAIRS_01": 1, "NW_XARDAS_TOWER_04": 1, "NW_FARM": 2},
}  # fmt: skip


async def test_a_companion_goes_ahead_to_a_place_near_and_says_what_is_there():
    fake = FakeGame()
    go = ToolCall("go_to", {"place": "cave"})
    llm = FakeBackend(script=[FakeStep("Пойду первым.", [go]), FakeStep("Тут двое.", [])], delay=0)
    cm, dialogue, talk = await _desk(fake, llm, places=PlaceBook(BOOK))
    dialogue.extensions.append(talk.orders)
    try:
        await _follows(fake, dialogue)
        await fake.send(type="player_text", text="Горн, иди в пещеру первым.")
        await _until(lambda: len(llm.calls) == 2)
        await _until(lambda: fake.said[-1:] == ["Тут двое."])
    finally:
        await cm.__aexit__(None, None, None)

    situation = next(m.content for m in llm.calls[0][1] if "Места рядом" in m.content)
    places = next(line for line in situation.splitlines() if line.startswith("Места рядом"))
    # not the tower it is in, not the far farm; its kind, which way from the hero, its way in
    assert places.endswith(": CAVE — пещера бандитов: пещера, 4 м, перед героем; вход — в башне.")
    assert any(t.name == "go_to" for t in llm.calls[0][2])
    walk = next(m for m in fake.received if m["type"] == "action" and m["action"] == "go_to_waypoint")
    assert walk["args"]["pos"] == [500, 2, 3]
    assert walk["args"]["points"] == ["NW_XARDAS_STAIRS_01", "NW_XARDAS_TOWER_04"]
    assert talk.orders.standing[GORN].kind == "go"  # it waits there for the hero
    cue = llm.calls[1][1][-1].content
    assert "ты пришёл первым — в пещере бандитов" in cue


async def test_set_on_one_it_saw_a_while_ago_it_goes_there_first():
    # step M2: Cavalorn saw the bandits in their cave and came out; «там бандиты, разберись»
    fake = FakeGame()
    fake.nearby = [{"id": BANDIT, "name": "Бандит", "distance": 900.0, "guild": 9, "dead": False}]
    fight = {"goal": "fight", "target": BANDIT, "attitude": "friendly", "agenda": None}
    llm = FakeBackend("Сейчас я им!", delay=0, tool_calls=[ToolCall("set_intent", fight)])
    cm, dialogue, talk = await _desk(fake, llm, places=PlaceBook(BOOK))
    looked: list[int] = []

    async def view_of(npc: str) -> NpcView:
        looked.append(len(acts(fake)))
        there = [{"id": BANDIT, "name": "Бандит", "distance": 500}] if len(looked) >= 3 else []
        return NpcView.model_validate({"npc": npc, "name": "Горн", "sees": there})

    talk.orders.view_of = view_of
    talk.orders.last_seen = lambda npc, who: "CAVE" if who == BANDIT else None
    try:
        await _follows(fake, dialogue)
        await fake.send(type="player_text", text="Горн, там бандиты, разберись.")
        await _until(lambda: fights(fake) == [BANDIT])
    finally:
        await cm.__aexit__(None, None, None)

    # to the cave, on through its point (it does not see him from the middle), then at him
    assert acts(fake)[:4] == [
        (GORN, "follow"),
        (GORN, "go_to_waypoint"),
        (GORN, "go_to_waypoint"),
        (GORN, "fight"),
    ]
    walk = next(m for m in fake.received if m["type"] == "action" and m["action"] == "go_to_waypoint")
    assert walk["args"]["pos"] == [500, 2, 3]


async def test_go_to_a_place_that_is_not_near_is_rejected():
    fake = FakeGame()
    go = ToolCall("go_to", {"place": "FARM"})
    llm = FakeBackend(script=[FakeStep("Пойду.", [go]), FakeStep("Далековато.", [])], delay=0)
    cm, dialogue, talk = await _desk(fake, llm, places=PlaceBook(BOOK))
    dialogue.extensions.append(talk.orders)
    try:
        await _follows(fake, dialogue)
        await fake.send(type="player_text", text="Горн, иди на ферму.")
        await _until(lambda: len(llm.calls) == 2)
    finally:
        await cm.__aexit__(None, None, None)
    assert not any(m["type"] == "action" and m["action"] == "go_to_waypoint" for m in fake.received)


async def test_go_to_any_place_near_though_not_among_the_nearest_shown():
    # 2026-10-14: by the steps the bandits' cave was the seventh place near, and «иди в пещеру» was refused
    fake = FakeGame()
    many = {
        **BOOK,
        "places": [
            *[
                {"id": f"SPOT{i}", "ru": f"место {i}", "where_ru": f"у места {i}", "en": "spot",
                 "where_en": "at a spot", "center": [100 + 50 * i, 2, 3]}
                for i in range(7)
            ],
            {"id": "CAVE", "ru": "пещера бандитов", "where_ru": "в пещере бандитов", "en": "cave",
             "where_en": "in the cave", "center": [2000, 2, 3]},
        ],
        "points": {f"WP{i}": i for i in range(8)},
    }  # fmt: skip
    go = ToolCall("go_to", {"place": "CAVE"})
    llm = FakeBackend(script=[FakeStep("Пойду первым.", [go]), FakeStep("Тут пусто.", [])], delay=0)
    cm, dialogue, talk = await _desk(fake, llm, places=PlaceBook(many))
    dialogue.extensions.append(talk.orders)
    try:
        await _follows(fake, dialogue)
        assert "CAVE" not in [p.id for p, _ in talk.orders.nearby(GORN)]  # not shown: a spot like the rest
        await fake.send(type="player_text", text="Горн, иди в пещеру первым.")
        await _until(
            lambda: any(m["type"] == "action" and m["action"] == "go_to_waypoint" for m in fake.received)
        )
    finally:
        await cm.__aexit__(None, None, None)
    walk = next(m for m in fake.received if m["type"] == "action" and m["action"] == "go_to_waypoint")
    assert walk["args"]["pos"] == [2000, 2, 3]


async def test_the_nearest_cave_is_shown_though_six_places_are_nearer():
    # М3: «зайди в пещеру» without its name; by Cavalorn's fire six places were nearer than the bandits' cave
    fake = FakeGame()
    many = {
        **BOOK,
        "places": [
            *[
                {"id": f"SPOT{i}", "ru": f"место {i}", "where_ru": f"у места {i}", "en": "spot",
                 "where_en": "at a spot", "center": [100 + 50 * i, 2, 3]}
                for i in range(7)
            ],
            {"id": "CAVE", "ru": "пещера бандитов", "where_ru": "в пещере бандитов", "en": "cave",
             "where_en": "in the cave", "center": [2000, 2, 3], "kind": "cave"},
        ],
        "points": {f"WP{i}": i for i in range(8)},
    }  # fmt: skip
    cm, dialogue, talk = await _desk(fake, FakeBackend("-", delay=0), places=PlaceBook(many))
    try:
        await _follows(fake, dialogue)
        shown = [p.id for p, _ in talk.orders.nearby(GORN)]
    finally:
        await cm.__aexit__(None, None, None)
    assert shown == [f"SPOT{i}" for i in range(6)] + ["CAVE"]


async def test_a_companion_sent_ahead_hears_its_name_from_afar():
    fake = FakeGame()
    cm, dialogue, talk = await _desk(fake, FakeBackend("-", delay=0))
    try:
        await fake.roster(person(GORN, "Горн", 35), person(LOBART, "Лобарт", 6))
        companion(dialogue, GORN, "Горн")
        companion(dialogue, LOBART, "Лобарт")
        await fake.send(type="player_text", text="Горн, за мной!")  # too far for talk, not for a shout
        await _until(lambda: (GORN, "follow") in acts(fake))
    finally:
        await cm.__aexit__(None, None, None)
    assert (LOBART, "follow") not in acts(fake)  # only the one called


async def test_sent_into_a_place_it_looks_around_until_it_sees_someone():
    fake = FakeGame()
    go = ToolCall("go_to", {"place": "CAVE"})
    llm = FakeBackend(script=[FakeStep("Пойду первым.", [go]), FakeStep("Тут бандит.", [])], delay=0)
    cm, dialogue, talk = await _desk(fake, llm, places=PlaceBook(BOOK))
    dialogue.extensions.append(talk.orders)
    looked: list[int] = []

    async def view_of(npc: str) -> NpcView:
        looked.append(len(walks(fake)))  # how many walks it had made when it looked
        sees = [{"id": BANDIT, "name": "Бандит", "distance": 600.0}] if len(walks(fake)) >= 2 else []
        return NpcView.model_validate({"npc": npc, "name": "Горн", "sees": sees})

    talk.orders.view_of = view_of
    try:
        await _follows(fake, dialogue)
        await fake.send(type="player_text", text="Горн, иди в пещеру первым.")
        await _until(lambda: len(llm.calls) == 2)
    finally:
        await cm.__aexit__(None, None, None)

    # to the middle (nobody seen there), on to the next point (the bandit seen): no third walk
    assert [w["args"]["waypoint"] for w in walks(fake)] == ["NW_XARDAS_STAIRS_01", "NW_XARDAS_STAIRS_01"]
    assert walks(fake)[1]["args"].get("pos") is None and looked == [1, 2]


def walks(fake: FakeGame) -> list[dict]:
    return [m for m in fake.received if m["type"] == "action" and m["action"] == "go_to_waypoint"]


async def test_a_foe_that_called_off_its_fight_is_let_be():
    fake = FakeGame()
    cm, dialogue, talk = await _desk(fake, FakeBackend("-", delay=0))
    try:
        await _follows(fake, dialogue, person(BANDIT, "Бандит", 6))
        fake.slow_actions = {"fight"}  # a fight that goes on until something ends it
        await fake.perception(GORN, "ASSESSOTHERSDAMAGE", BANDIT, victim="PLAYER")
        await _until(lambda: GORN in talk.orders.fights)
        talk.orders.truce(BANDIT)  # talked down by the hero (dialogue.py)
        await _until(lambda: acts(fake)[-1] == (GORN, "follow"))
    finally:
        await cm.__aexit__(None, None, None)
    assert GORN not in talk.orders.fights and not talk.orders.foe(BANDIT)


async def test_a_blow_at_the_hero_heard_from_afar_brings_it_into_the_fight():
    # 2026-10-14 the bandits beat the hero 20 m from Cavalorn: the blow was out of sight, not out of earshot
    fake = FakeGame()
    cm, dialogue, talk = await _desk(fake, FakeBackend("-", delay=0))
    try:
        await _follows(fake, dialogue, person(BANDIT, "Бандит", 20))
        await fake.perception(GORN, "ASSESSFIGHTSOUND", BANDIT, victim="PLAYER", distance=1800)
        await _until(lambda: fights(fake) == [BANDIT])
    finally:
        await cm.__aexit__(None, None, None)


async def test_one_who_turns_on_the_hero_is_fought_before_its_first_blow():
    fake = FakeGame()
    cm, dialogue, talk = await _desk(fake, FakeBackend("-", delay=0))
    try:
        await _follows(fake, dialogue, person(BANDIT, "Бандит", 8))
        await fake.roster(person(GORN, "Горн", 40), person(BANDIT, "Бандит", 8))
        await _until(lambda: talk.people[GORN].distance == 4000)
        dialogue.on_attack[0](BANDIT)  # its model's own attack (dialogue._attack_hero)
        await asyncio.sleep(0.1)
        assert fights(fake) == [] and talk.orders.foe(BANDIT)  # too far off to hear the fight
        await fake.roster(person(GORN, "Горн", 25), person(BANDIT, "Бандит", 8))
        await _until(lambda: talk.people[GORN].distance == 2500)
        dialogue.on_attack[0](BANDIT)
        await _until(lambda: fights(fake) == [BANDIT])
    finally:
        await cm.__aexit__(None, None, None)


async def test_sent_ahead_it_goes_on_with_the_hero_once_he_comes_up():
    fake = FakeGame()
    go = ToolCall("go_to", {"place": "CAVE"})
    llm = FakeBackend(script=[FakeStep("Пойду первым.", [go]), FakeStep("Тут пусто.", [])], delay=0)
    cm, dialogue, talk = await _desk(fake, llm, places=PlaceBook(BOOK))
    dialogue.extensions.append(talk.orders)
    talk.orders.view_of = None
    try:
        await _follows(fake, dialogue)
        await fake.send(type="player_text", text="Горн, иди в пещеру первым.")
        await _until(lambda: len(llm.calls) == 2)
        assert talk.orders.standing[GORN].kind == "go"
        await fake.roster(person(GORN, "Горн", 30))  # it waits in the cave
        await asyncio.sleep(1.2)
        await fake.roster(person(GORN, "Горн", 3))  # the hero comes in
        await _until(lambda: GORN not in talk.orders.standing and acts(fake)[-1] == (GORN, "follow"), 5)
    finally:
        await cm.__aexit__(None, None, None)


async def test_a_companion_is_the_scripts_party_member_while_it_goes_with_the_hero():
    # 2026-10-14 Cavalorn killed three bandits and the hero got no experience: ZS_Dead gives it for a party
    # member's kills only
    fake = FakeGame()
    cm, dialogue, talk = await _desk(fake, FakeBackend("-", delay=0))
    try:
        await _follows(fake, dialogue)
        await fake.roster(person(GORN, "Горн", 4))
        await _until(lambda: party(fake) == [(GORN, 1)])
        await fake.send(type="player_text", text="Свободен.")
        await _until(lambda: GORN not in fake.attached)
        await fake.roster(person(GORN, "Горн", 4))
        await _until(lambda: party(fake) == [(GORN, 1), (GORN, 0)])
        await fake.roster(person(LOBART, "Лобарт", 4))
        await asyncio.sleep(0.05)
    finally:
        await cm.__aexit__(None, None, None)
    assert party(fake) == [(GORN, 1), (GORN, 0)]  # nobody else is touched


async def test_a_companion_dialogs_party_mark_is_the_scripts_to_take_off():
    # Cavalorn's HELFEN marks him (vanilla.py), the end of his quest unmarks him: let go in between, he stays
    assert await _let_go(dialog_first=True) == [(GORN, 1)]
    assert await _let_go(dialog_first=False) == [(GORN, 1), (GORN, 1)]


async def _let_go(*, dialog_first: bool) -> list[tuple[str, int]]:
    """The `party` effects sent when a companion dialog marks Gorn before or after he goes with the hero,
    and the hero then lets him go."""
    fake = FakeGame()
    cm, dialogue, talk = await _desk(fake, FakeBackend("-", delay=0))
    try:
        if dialog_first:
            await talk.link.story_effect(GORN, [{"do": "party", "value": 1}])
        await _follows(fake, dialogue)
        await fake.roster(person(GORN, "Горн", 4))
        if not dialog_first:
            await _until(lambda: party(fake) == [(GORN, 1)])
            await talk.link.story_effect(GORN, [{"do": "party", "value": 1}])
        await fake.send(type="player_text", text="Свободен.")
        await _until(lambda: GORN not in fake.attached)
        await fake.roster(person(GORN, "Горн", 4))
        await asyncio.sleep(0.1)
    finally:
        await cm.__aexit__(None, None, None)
    return party(fake)


def party(fake: FakeGame) -> list[tuple[str, int]]:
    return [
        (m["npc"], e["value"])
        for m in fake.received
        if m["type"] == "story_effect"
        for e in m["effects"]
        if e["do"] == "party"
    ]
