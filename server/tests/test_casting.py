from typing import Any

from gothic_ai.authoring import Author
from gothic_ai.cast import Cast
from gothic_ai.casting import PROPOSE, Casting
from gothic_ai.characters import Cards
from gothic_ai.chronicle import Chronicle
from gothic_ai.director import Director, Pacing
from gothic_ai.llm import ToolCall
from gothic_ai.llm.fake import FakeBackend, FakeStep
from gothic_ai.memory import MemoryStore
from gothic_ai.places import PlaceBook
from gothic_ai.story import StoryPatches
from tests.fake_game import FakeGame, connected
from tests.test_authoring import BOOK, _quest, graph  # noqa: F401 - the graph fixture
from tests.test_dialogue import _until
from tests.test_director import HILDA, NAMES, SYMBOLS

TOWNSMAN = "VLK_4000_BUERGER"


def _character(**change: Any) -> ToolCall:
    args = {
        "kind": "townsman",
        "name": "Гарет",
        "head": "Hum_Head_Pony",
        "face": "FACE_L_NORMALBART01",
        "armor": "ITAR_VLK_L",
        "day_point": "NW_FARM1_OUT_02",
        "day_state": "ZS_STAND_ARMSCROSSED",
        "night_point": "NW_FARM1_OUT_01",
        "night_state": "ZS_STAND_WP",
        "items": [{"item": "ITFO_CHEESE", "count": 2}],
        "gold": 30,
        "ru": "Ты — Гарет, бродячий торговец. Скуп на слова, но честен в цене.",
        "en": "A taciturn peddler who keeps his prices fair.",
        "greed": 3,
        "honesty": 4,
        "courage": 2,
        "talk": 2,
        "pushy": 2,
        "strangers": 4,
        "voice": "A weathered peddler in his forties with a low, dry male voice",
        "past": "Ты пришёл из Венгарда с последним кораблём.",
        "knows": ["У ступеней к башне Ксардаса засели бандиты."],
        "ties": [{"npc": HILDA, "how": "Хильда: берёшь у неё молоко"}],
        "reason": "торговец на дороге к городу",
        **change,
    }
    return ToolCall(PROPOSE, args, "c1")


async def test_the_director_brings_in_a_character_who_gives_a_quest(graph):  # noqa: F811
    fake = FakeGame(branch="A")
    fake.vars = {"KAPITEL": 1, "XP_AMBIENT": 50}
    fake.npc_states = {n: {"state": "alive", "name": name} for n, name in NAMES.items()}
    fake.symbols = {
        **SYMBOLS,
        TOWNSMAN: "npc",
        "GIL_VLK": "const",
        "FACE_L_NORMALBART01": "const",
        "ITAR_VLK_L": "item",
        "ITMI_GOLD": "item",
        "ITFO_CHEESE": "item",
        "ITFO_APPLE": "item",
        "ZS_STAND_ARMSCROSSED": "func",
        "ZS_STAND_WP": "func",
        "ITMI_SAW": "item",
        "WLD_INSERTITEM": "func",
    }
    fake.item_names = {"ITFO_CHEESE": "Сыр", "ITFO_APPLE": "Яблоко", "ITMI_SAW": "Пила"}
    fake.people = [
        {"npc": HILDA, "name": "Хильда", "routine": [{"waypoint": "NW_FARM1_OUT_01"}]},
        {"npc": TOWNSMAN, "name": "Горожанин", "routine": [{"waypoint": "NW_CITY_01"}]},
    ]
    # a knight is no stranger of the farm, and the city is no point of the yard; then a peddler
    llm = FakeBackend(
        script=[
            FakeStep("", [_character(kind="knight", day_point="NW_CITY_01")]),
            FakeStep("", [_character()]),
        ],
        delay=0,
    )
    store = MemoryStore(":memory:")
    async with connected(fake) as link:
        patches = StoryPatches(link, store)
        chronicle = Chronicle(link, store)
        cast = Cast(link, patches, chronicle)
        places = PlaceBook(BOOK)
        casting = Casting(link, cast, llm, places, chronicle)
        await _until(lambda: patches.branch is not None and chronicle.branch is not None)

        premise = "На ферму пришёл торговец после ограбления. Теперь ему нужна помощь с товаром."
        spec = await casting.character("FARM1_ENTRANCE", premise=premise)
        assert spec is not None and spec.npc == "DIR_NPC_1" and spec.like == TOWNSMAN
        brief = llm.calls[0][1][0].content
        assert "Место: во дворе перед домом Лобарта (FARM1_ENTRANCE)." in brief
        assert "- NW_FARM1_OUT_01" in brief and "- NW_FARM1_FIELD_01" not in brief
        assert "- townsman — горожанин" in brief and "farmer" not in brief  # the world has no farmer to copy
        assert f"- {HILDA} — Хильда" in brief and "- ITFO_CHEESE — Сыр" in brief
        assert f"Замысел режиссёра: {premise}" in brief
        assert f"Замысел режиссёра: {premise}" in llm.calls[1][1][0].content
        rejected = llm.calls[1][1][-1].content
        assert (
            "вида 'knight' нет в сводке" in rejected and "точки 'NW_CITY_01' (днём) нет в сводке" in rejected
        )

        # the code's: the template, the guild, the skin of the face, the hours, the gold among the things
        made = fake.cast["DIR_NPC_1"]
        assert made["guild"] == "GIL_VLK" and made["look"]["skin"] == "BODYTEX_L"
        assert made["items"] == [{"item": "ITMI_GOLD", "count": 30}, {"item": "ITFO_CHEESE", "count": 2}]
        assert [(e["start"], e["end"], e["waypoint"]) for e in made["routine"]] == [
            ("06:00", "21:00", "NW_FARM1_OUT_02"),
            ("21:00", "06:00", "NW_FARM1_OUT_01"),
        ]
        person = cast.person("DIR_NPC_1")
        assert person is not None and person.ties[0].npc == HILDA
        assert [f["fact"] for f in chronicle.facts() if f["fact"].startswith("cast")] == [
            "cast_rejected",
            "cast",
            "cast_reason",
        ]

        # he gives the author's quest, and the author knows him by his card
        quests = FakeBackend(script=[FakeStep("", [_quest(title="Сыр Гарета")])], delay=0)
        director = Director(link, graph, patches, chronicle, quests, pacing=Pacing(cooldown_s=0), settle_s=0)
        author = Author(
            link, graph, patches, chronicle, director, quests, places=places, cards=Cards({}, cast.cards)
        )
        q = await author.quest("DIR_NPC_1")
        assert q is not None and q.giver == "DIR_NPC_1" and q.name(q.giver) == "Гарет"
        assert "Его характер (написан ему самому): Ты — Гарет" in quests.calls[0][1][0].content
        give = next(op["node"] for m in fake.graph for op in m["ops"] if op.get("op") == "add_node")
        assert give["npc"] == "DIR_NPC_1" and give["id"] == "DIR_Q1_GIVE"


BANDIT = "BDT_1013_BANDIT_L"


def _bandit_world(fake: FakeGame) -> None:
    fake.npc_states = {HILDA: {"state": "alive", "name": "Хильда"}}
    fake.people = [
        {"npc": TOWNSMAN, "name": "Горожанин", "routine": []},
        {"npc": BANDIT, "name": "Бандит", "routine": []},
        {"npc": "BDT_1000_BANDIT_L", "name": "Браго", "routine": []},
    ]
    fake.symbols = {
        TOWNSMAN: "npc",
        BANDIT: "npc",
        "GIL_BDT": "const",
        "GIL_VLK": "const",
        "ITAR_BDT_M": "item",
        "ITAR_VLK_L": "item",
        "ITFO_CHEESE": "item",
        "ITMI_GOLD": "item",
        "ITMI_SAW": "item",
        "FACE_N_NORMAL01": "const",
        "FACE_N_NORMAL02": "const",
        "FACE_L_NORMALBART01": "const",
        "ZS_STAND_GUARDING": "func",
        "ZS_STAND_ARMSCROSSED": "func",
        "ZS_STAND_WP": "func",
    }


async def test_a_named_bandit_keeps_the_authors_role_and_carried_goods():
    fake = FakeGame(branch="A")
    _bandit_world(fake)
    llm = FakeBackend(
        script=[
            FakeStep("", [_character()]),
            FakeStep("", [_character(kind="bandit", armor="ITAR_BDT_M", name="Рутгер")]),
        ],
        delay=0,
    )
    store = MemoryStore(":memory:")
    async with connected(fake) as link:
        patches = StoryPatches(link, store)
        chronicle = Chronicle(link, store)
        cast = Cast(link, patches, chronicle)
        casting = Casting(link, cast, llm, PlaceBook(BOOK), chronicle)
        await _until(lambda: patches.branch is not None and chronicle.branch is not None)
        spec = await casting.character(
            "FARM1_ENTRANCE", kind="bandit", about="Вор, укравший товар торговца.", carries=["ITMI_SAW"]
        )
        assert spec is not None and spec.like == BANDIT and spec.name == "Рутгер"
        assert (spec.guild, spec.look.armor) == ("GIL_BDT", "ITAR_BDT_M")
        assert {t.item: t.count for t in spec.items}["ITMI_SAW"] == 1
        assert "kind — bandit; about — Вор, укравший товар торговца." in llm.calls[0][1][0].content
        assert "ITMI_SAW" in llm.calls[0][1][0].content
        assert "Замысел режиссёра:" not in llm.calls[0][1][0].content
        assert "вид задан автором: bandit" in llm.calls[1][1][-1].content


async def test_a_bandit_group_uses_distinct_points_of_one_place_without_a_model():
    fake = FakeGame(branch="A")
    _bandit_world(fake)
    llm = FakeBackend(script=[], delay=0)
    store = MemoryStore(":memory:")
    async with connected(fake) as link:
        patches = StoryPatches(link, store)
        chronicle = Chronicle(link, store)
        cast = Cast(link, patches, chronicle)
        places = PlaceBook(BOOK)
        casting = Casting(link, cast, llm, places, chronicle)
        await _until(lambda: patches.branch is not None and chronicle.branch is not None)
        group, errors = await casting.bandits("FARM1_FIELD", 2, ["ITMI_SAW"])
        assert errors == [] and len(group) == 2 and llm.calls == []
        assert len({s.at for s in group}) == 2
        assert {s.name for s in group} == {"Бандит"} and {s.like for s in group} == {BANDIT}
        for spec in group:
            assert places.of(spec.at).id == "FARM1_FIELD"
            assert {r.waypoint for r in spec.routine} == {spec.at}
            assert [(r.start, r.end) for r in spec.routine] == [("06:00", "21:00"), ("21:00", "06:00")]
            assert [i.item for i in spec.items] == ["ITMI_SAW"]
            assert cast.person(spec.npc) is None
        assert len(cast.characters()) == 2
        assert (await casting.bandits("FARM1_FIELD", 3))[0] == []
        assert len(cast.characters()) == 2


async def test_a_bandit_group_returns_already_introduced_characters_on_refusal(monkeypatch):
    fake = FakeGame(branch="A")
    _bandit_world(fake)
    store = MemoryStore(":memory:")
    async with connected(fake) as link:
        patches = StoryPatches(link, store)
        chronicle = Chronicle(link, store)
        cast = Cast(link, patches, chronicle)
        casting = Casting(link, cast, FakeBackend(script=[], delay=0), PlaceBook(BOOK), chronicle)
        await _until(lambda: patches.branch is not None and chronicle.branch is not None)
        introduce = cast.introduce

        async def first_only(spec, person=None):
            if cast.characters():
                return None, ["игра не сделала второго бандита"]
            return await introduce(spec, person)

        monkeypatch.setattr(cast, "introduce", first_only)
        group, errors = await casting.bandits("FARM1_FIELD", 2)
        assert len(group) == 1 and errors == ["игра не сделала второго бандита"]
        assert [c.npc for c in cast.characters()] == [group[0].npc]
        monkeypatch.setattr(cast, "introduce", introduce)
        rest, errors = await casting.bandits("FARM1_FIELD", 1, occupied_points=[group[0].at])
        assert errors == [] and len(rest) == 1 and rest[0].at != group[0].at
        assert len(cast.characters()) == 2


async def test_a_director_card_applies_to_delayed_deceit_and_greedy_deeds():
    from gothic_ai.npc_cheat import decide
    from gothic_ai.npc_will import MOTIVES

    fake = FakeGame(branch="A")
    _bandit_world(fake)
    llm = FakeBackend(script=[FakeStep("", [_character(honesty=1, greed=5)])], delay=0)
    store = MemoryStore(":memory:")
    async with connected(fake) as link:
        patches = StoryPatches(link, store)
        chronicle = Chronicle(link, store)
        cast = Cast(link, patches, chronicle)
        casting = Casting(link, cast, llm, PlaceBook(BOOK), chronicle)
        await _until(lambda: patches.branch is not None and chronicle.branch is not None)
        spec = await casting.character("FARM1_ENTRANCE")
        assert spec is not None
        cards = Cards({}, cast.cards)
        card = cards[spec.npc]
        assert (card.honesty, card.greed) == (1, 5)
        intent = decide(chronicle, spec.npc, "DIRECTOR_DEAL", ["npc_later"], cards=cards)
        assert intent is not None and intent["keeps"] is False and intent["why"] == "dishonesty"
        assert MOTIVES["greed"].action(card) == "steal"
