import pytest

from gothic_ai.cast import Cast, Person, PersonTie, person_problems, problems
from gothic_ai.characters import Card, Cards, Traits
from gothic_ai.chronicle import Chronicle
from gothic_ai.memory import MemoryStore
from gothic_ai.persona import PersonaProvider
from gothic_ai.protocol import NpcLook, NpcSpec, NpcThing, RoutineEntry
from gothic_ai.story import StoryPatches
from tests.fake_game import FakeGame, connected
from tests.test_dialogue import _until
from tests.test_memory import close_memory_stores as close_memory_stores

LIKE = "VLK_4000_BUERGER"
SYMBOLS = {
    LIKE: "npc",
    "BAU_950_LOBART": "npc",
    "GIL_VLK": "const",
    "FACE_N_NORMAL05": "const",
    "FACE_N_LEE": "const",
    "ITAR_VLK_L": "item",
    "ITMI_GOLD": "item",
    "ITFO_APPLE": "item",
    "ZS_STAND_ARMSCROSSED": "func",
    "ZS_SLEEP": "func",
}


def _peddler(**change: object) -> NpcSpec:
    spec = {
        "npc": "DIR_NPC_1",
        "like": LIKE,
        "at": "NW_FARM1_PATH_01",
        "name": "Гарет",
        "guild": "GIL_VLK",
        "look": NpcLook(
            gender="MALE", head="Hum_Head_Pony", face="FACE_N_NORMAL05", skin="BODYTEX_N", armor="ITAR_VLK_L"
        ),
        "items": [NpcThing(item="ITMI_GOLD", count=40), NpcThing(item="ITFO_APPLE", count=5)],
        "routine": [
            RoutineEntry(
                start="06:00", end="21:00", state="ZS_STAND_ARMSCROSSED", waypoint="NW_FARM1_PATH_01"
            ),
            RoutineEntry(start="21:00", end="06:00", state="ZS_SLEEP", waypoint="NW_FARM1_OUT_01"),
        ],
        **change,
    }
    return NpcSpec.model_validate(spec)


def test_a_description_is_checked():
    assert problems(_peddler(), SYMBOLS, set()) == []
    # a person of the plot is no template; the face of one is no face for a stranger
    bad = problems(
        _peddler(
            like="BAU_950_LOBART",
            look=NpcLook(gender="MALE", head="Hum_Head_Unicorn", face="FACE_N_LEE", skin="BODYTEX_X"),
        ),
        SYMBOLS,
        set(),
    )
    assert (
        "шаблон — безымянный крестьянин, горожанин, наёмник или бандит скриптов, а не BAU_950_LOBART" in bad
    )
    assert any(p.startswith("голова Hum_Head_Unicorn не годится") for p in bad)
    assert "лицо FACE_N_LEE не годится: только лица безымянных" in bad
    assert any(p.startswith("кожа — одна из") for p in bad)
    # the day is whole: no gap between the entries
    gap = [
        RoutineEntry(start="06:00", end="20:00", state="ZS_STAND_ARMSCROSSED", waypoint="WP"),
        RoutineEntry(start="21:00", end="06:00", state="ZS_SLEEP", waypoint="WP"),
    ]
    assert problems(_peddler(routine=gap), SYMBOLS, set()) == [
        "распорядок: отрезки должны занимать ровно сутки"
    ]
    # names the game has not, a taken id, too much gold
    odd = problems(
        _peddler(npc="DIR_NPC_1", items=[NpcThing(item="ITMI_GOLD", count=5000), NpcThing(item="ITMI_NONE")]),
        SYMBOLS,
        {"DIR_NPC_1"},
    )
    assert odd == ["персонаж DIR_NPC_1 уже есть", "ITMI_GOLD: от 1 до 1000", "предмета ITMI_NONE нет в игре"]
    assert problems(_peddler(npc="NPC_1"), SYMBOLS, set()) == [
        "персонаж режиссёра называется DIR_NPC_…: NPC_1"
    ]


def test_a_bandit_has_a_nameless_template_guild_and_armor():
    symbols = {**SYMBOLS, "BDT_1013_BANDIT_L": "npc", "GIL_BDT": "const", "ITAR_BDT_M": "item"}
    spec = _peddler(
        like="BDT_1013_BANDIT_L",
        name="Бандит",
        guild="GIL_BDT",
        look=NpcLook(
            gender="MALE",
            head="HUM_HEAD_FIGHTER",
            face="FACE_N_NORMAL05",
            skin="BODYTEX_N",
            armor="ITAR_BDT_M",
        ),
    )
    assert problems(spec, symbols, set()) == []
    assert problems(spec.model_copy(update={"like": "BDT_1090_ADDON_RAVEN"}), symbols, set())


async def test_a_character_comes_once_and_stays_with_its_saves():
    store = await MemoryStore.open(":memory:")
    fake = FakeGame(branch="A")
    fake.symbols = dict(SYMBOLS)
    async with connected(fake) as link:
        patches = StoryPatches(link, store)
        await patches.initialize()
        chronicle = Chronicle(link, store)
        await chronicle.initialize()
        cast = Cast(link, patches, chronicle)
        await cast.prepare()
        await _until(lambda: patches.branch is not None and chronicle.branch is not None)
        await patches.apply({"op": "disable", "node": "DIA_LOBART_WORKNOW"})
        await _until(lambda: fake.heads[-1:] == [("A", 1)])  # a save made before he came

        assert await cast.next_id() == "DIR_NPC_1"
        made, issues = await cast.introduce(_peddler())
        assert issues == [] and made == {"npc": "DIR_NPC_1", "state": "made", "index": 74650}
        assert (
            fake.cast["DIR_NPC_1"]["name"] == "Гарет"
            and fake.cast["DIR_NPC_1"]["routine"][1]["state"] == "ZS_SLEEP"
        )
        assert [c.npc for c in await cast.characters()] == [
            "DIR_NPC_1"
        ] and await cast.next_id() == "DIR_NPC_2"
        # he is no change of the graph
        assert await patches.ops() == [{"op": "disable", "node": "DIA_LOBART_WORKNOW"}]
        assert [f["npc"] for f in chronicle.facts("cast")] == ["DIR_NPC_1"]
        # once only
        again, issues = await cast.introduce(_peddler())
        assert again is None and issues == ["персонаж DIR_NPC_1 уже есть"]

        # a new session of the branch: he is asked for first, before the graph, and the game has him
        graphs = len(fake.graph)
        await fake.send(type="session", branch="C", parent="A", parent_seq=10, reason="load")
        await _until(lambda: len(fake.graph) > graphs)
        assert fake.cast_asked == ["DIR_NPC_1", "DIR_NPC_1"] and fake.graph[-1]["ops"] == [
            {"op": "disable", "node": "DIA_LOBART_WORKNOW"}
        ]
        assert [c.npc for c in await cast.characters()] == ["DIR_NPC_1"]

        # the save made before he came: no one asks for him
        graphs = len(fake.graph)
        await fake.send(type="session", branch="B", parent="A", parent_seq=1, reason="load")
        await _until(lambda: len(fake.graph) > graphs)
        assert fake.cast_asked == ["DIR_NPC_1", "DIR_NPC_1"] and await cast.characters() == []


async def test_a_rejected_character_is_not_kept():
    fake = FakeGame(branch="A")
    fake.symbols = dict(SYMBOLS)
    async with connected(fake) as link:
        patches = StoryPatches(link, await MemoryStore.open(":memory:"))
        await patches.initialize()
        cast = Cast(link, patches)
        await cast.prepare()
        await _until(lambda: patches.branch is not None)
        made, issues = await cast.introduce(_peddler(like="BAU_950_LOBART"))
        assert made is None and issues and fake.cast == {} and await cast.characters() == []


@pytest.mark.parametrize("name", ["Хильда", " хИлЬдА "])
async def test_a_new_character_cannot_take_a_name_of_any_person_of_the_world(name):
    fake = FakeGame(branch="A")
    fake.symbols = dict(SYMBOLS)
    fake.people = [{"npc": "BAU_951_HILDA", "name": "Хильда", "routine": []}]
    async with connected(fake) as link:
        patches = StoryPatches(link, await MemoryStore.open(":memory:"))
        await patches.initialize()
        cast = Cast(link, patches)
        await cast.prepare()
        await _until(lambda: patches.branch is not None)
        made, issues = await cast.introduce(_peddler(name=name))
        assert made is None and issues == [f"имя «{name.strip()}» уже есть у человека мира"]
        assert fake.cast == {} and await cast.characters() == []


async def test_a_new_character_cannot_take_a_name_of_a_directors_character():
    fake = FakeGame(branch="A")
    fake.symbols = dict(SYMBOLS)
    async with connected(fake) as link:
        patches = StoryPatches(link, await MemoryStore.open(":memory:"))
        await patches.initialize()
        cast = Cast(link, patches)
        await cast.prepare()
        await _until(lambda: patches.branch is not None)
        made, issues = await cast.introduce(_peddler())
        assert made is not None and issues == []
        made, issues = await cast.introduce(_peddler(npc="DIR_NPC_2"))
        assert made is None and issues == ["имя «Гарет» уже есть у человека мира"]
        assert len(await cast.characters()) == 1


GARET = Person(
    ru="Ты — Гарет, бродячий торговец. Скуп на слова, но честен в цене.",
    en="A taciturn peddler who keeps his prices fair.",
    greed=3,
    honesty=4,
    courage=2,
    talk=2,
    pushy=3,
    strangers=4,
    voice="A weathered peddler in his forties with a low, dry male voice, speaking unhurriedly",
    past="Ты пришёл из Венгарда с последним кораблём и торгуешь по фермам.",
    knows=("У ступеней к башне Ксардаса сидят бандиты.",),
    ties=(PersonTie(npc="BAU_951_HILDA", how="Хильда с фермы Лобарта: берёшь у неё молоко"),),
)


def test_who_a_character_is_is_checked():
    alive = {"BAU_951_HILDA"}
    assert person_problems(GARET, _peddler(), alive) == []
    bad = GARET.model_copy(
        update={
            "ru": "Гарет — торговец.",
            "greed": 7,
            "voice": "A young woman with a bright voice, speaking fast",
            "ties": (PersonTie(npc="BAU_950_LOBART", how="он"),),
        }
    )
    assert person_problems(bad, _peddler(), alive) == [
        "greed: от 1 до 5",
        "карточка: от 20 до 600 знаков, начинается с «Ты — …»",
        "голос: скажи, что это голос мужчины (man)",
        "знакомый BAU_950_LOBART: такого живого человека нет",
        "знакомый BAU_950_LOBART: как знакомы — от 5 до 200 знаков",
    ]
    # a townswoman without a look of her own is a woman
    her = _peddler(like="VLK_4001_BUERGERIN", look=None)
    assert person_problems(GARET, her, alive) == ["голос: скажи, что это голос женщины (woman)"]


async def test_a_character_is_a_person_to_whoever_reads_npcs():
    fake = FakeGame(branch="A")
    fake.symbols = dict(SYMBOLS)
    fake.npc_states = {"BAU_951_HILDA": {"state": "alive", "name": "Хильда"}}
    async with connected(fake) as link:
        patches = StoryPatches(link, await MemoryStore.open(":memory:"))
        await patches.initialize()
        cast = Cast(link, patches)
        await cast.prepare()
        changed: list[str] = []
        cast.on_change.append(lambda: changed.append("cast"))
        await _until(lambda: patches.branch is not None)
        cards = Cards({"BAU_951_HILDA": _hilda()}, cast.cards)
        personas = PersonaProvider(None, cards=cards, people=cast.person)
        assert "DIR_NPC_1" not in cards and personas.get("DIR_NPC_1", "Гарет").voice_key == ""

        made, issues = await cast.introduce(_peddler(), GARET)
        assert issues == [] and made is not None and changed == ["cast"]
        assert cast.person("DIR_NPC_1") == GARET and cast.bonds() == {
            "DIR_NPC_1": {"BAU_951_HILDA": GARET.ties[0].how}
        }
        # his card, as a card of the scripts' npc's
        assert cards["DIR_NPC_1"].name == "Гарет" and cards["DIR_NPC_1"].greed == 3 and len(cards) == 2
        assert Traits(cards)["DIR_NPC_1"] == GARET.en
        # his persona: his card, past, knowledge and acquaintances; a voice of his own
        persona = personas.get("DIR_NPC_1", "Гарет")
        for text in (GARET.ru, GARET.past, GARET.knows[0], GARET.ties[0].how):
            assert text in persona.instructions
        assert persona.voice == GARET.voice and persona.traits == GARET.en
        assert persona.voice_id.startswith("DIR_NPC_1~") and persona.voice_id != "DIR_NPC_1"
        assert "написана для него самого" in personas.brief("DIR_NPC_1", "Гарет")
        # one with another voice is another voice, though the same symbol (another branch)
        other = GARET.model_copy(
            update={"voice": "An old man with a thin, shaky male voice, speaking slowly"}
        )
        assert personas._person("DIR_NPC_1", "Гарет", other).voice_id != persona.voice_id


def _hilda() -> Card:
    return Card("BAU_951_HILDA", "Хильда", "Ты — Хильда.", "A farm wife.", 3, 3, 3, 3, 3, 3)
