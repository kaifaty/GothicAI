import copy
import json
from pathlib import Path

import pytest

from gothic_ai import characters
from gothic_ai.characters import Card
from gothic_ai.gamedata.storygraph import Node, StoryGraph, load
from gothic_ai.llm import ToolCall
from gothic_ai.llm.fake import FakeBackend, FakeStep
from gothic_ai.persona import PersonaProvider, render_instructions
from tests.test_director import GRAPH, HILDA, LOBART, XARDAS, _line, _node

TRADER = "VLK_416_MATTEO"


@pytest.fixture
def graph(tmp_path: Path) -> StoryGraph:
    g = copy.deepcopy(GRAPH)
    g["nodes"] += [
        _node(
            "DIA_MATTEO_TRADE",
            TRADER,
            [_line("Смотри, но руками не трогай."), _line("Что берёшь?", "OTHER")],
            trade=True,
        ),
        _node(
            "DIA_MATTEO_PRICE", TRADER, [_line("Дёшево не отдам, у меня товар хороший, не то что у Зуриса.")]
        ),
    ]
    path = tmp_path / "story.json"
    path.write_text(json.dumps(g, ensure_ascii=False), encoding="utf-8")
    return load(path)


PEOPLE = [
    {
        "npc": TRADER,
        "name": "Маттео",
        "guild": 3,
        "routine": [{"state": "ZS_STAND_ARMSCROSSED"}, {"state": "ZS_SLEEP"}],
    },
    {"npc": "VLK_999_NOBODY", "name": "Никто", "guild": 3, "routine": []},  # no lines of its own
]


def _card(npc: str = TRADER, **change) -> dict:
    card = {
        "npc": npc,
        "ru": "Ты — Маттео, торговец с рыночной площади. Прижимист и горд своим товаром.",
        "en": "A proud, tight-fisted market trader.",
        "greed": 5,
        "honesty": 3,
        "courage": 2,
        "talk": 3,
        "pushy": 2,
        "strangers": 2,
    }
    return {**card, **change}


def test_what_the_model_is_told_of_an_npc(graph):
    (s,) = characters.subjects(graph, PEOPLE)
    assert (s.npc, s.name, s.guild) == (TRADER, "Маттео", 3)
    assert s.does == ["торгует"] and s.day == ["sleep", "stand armscrossed"]
    # its own lines only, not the hero's
    assert "Смотри, но руками не трогай." in s.lines and "Что берёшь?" not in s.lines
    text = s.text()
    assert text.startswith(f"## {TRADER} — Маттео, горожанин Хориниса") and "Для героя: торгует." in text
    assert characters.subjects(graph, PEOPLE, only={LOBART}) == []


def test_samples_exclude_freed_dialogs_and_their_choices(graph):
    brago = "BDT_1014_BANDIT_L"
    ambush = "DIA_BDT_1014_BANDIT_KILLER"
    choice = ambush + "_ATTACK"
    nested = choice + "_AGAIN"
    graph.nodes[ambush] = Node(
        ambush,
        "dialog",
        brago,
        "",
        [_line("Это он, точно! Держи его!"), {"do": "choice", "node": choice}],
        set(),
    )
    graph.nodes[choice] = Node(
        choice,
        "choice",
        brago,
        "",
        [_line("Хватайте его!"), {"do": "choice", "node": nested}],
        set(),
        parents=[ambush],
    )
    graph.nodes[nested] = Node(
        nested, "choice", brago, "", [_line("Он не уйдёт!"), {"do": "set_guild"}], set(), parents=[choice]
    )
    people = [*PEOPLE, {"npc": brago, "name": "Браго"}]
    (trader,) = characters.subjects(graph, people)
    assert trader.npc == TRADER
    assert "Смотри, но руками не трогай." in trader.lines


def test_law_conditions_do_not_disable_card_samples(graph):
    andre = "MIL_311_ANDRE"
    node = "DIA_ANDRE_PETZMASTER"
    graph.nodes[node] = Node(
        node,
        "dialog",
        andre,
        "",
        [_line("Тебе придётся заплатить штраф.")],
        set(),
        condition=["fn", "B_GETGREATESTPETZCRIME"],
    )
    (subject,) = characters.subjects(graph, [{"npc": andre, "name": "Андре"}])
    assert subject.lines == ["Тебе придётся заплатить штраф."]


@pytest.mark.parametrize(
    "expected",
    [
        Card(
            npc="BDT_1014_BANDIT_L",
            name="Браго",
            ru=(
                "Ты — Браго, главарь маленькой шайки. Говоришь коротко и властно, "
                "привык, что тебя слушаются. Ты жаден и расчётлив: сначала прикидываешь, "
                "чего стоит чужак и сколько с него можно содрать, "
                "а дерёшься, когда это выгодно или когда тебе перечат после твоего последнего слова. "
                "Слабых давишь, сильных проверяешь на испуг, лжецов не прощаешь. "
                "О своих делах и о тех, кто тебе платит, держишь язык за зубами."
            ),
            en=(
                "Curt, commanding gang leader; greedy and calculating — sizes a stranger up and squeezes "
                "him before fighting; fights when it pays or when defied after his last word; bullies the "
                "weak, tests the strong, never forgives a liar; keeps his paymasters secret."
            ),
            greed=4,
            honesty=2,
            courage=4,
            talk=2,
            pushy=4,
            strangers=1,
        ),
        Card(
            npc="BDT_1015_BANDIT_L",
            name="Бандит",
            ru=(
                "Ты — бандит из шайки Браго, подозрительный и грубый. Разглядываешь каждую незнакомую рожу "
                "и спрашиваешь без церемоний. Решать оставляешь Браго и поддакиваешь ему, но своя шкура тебе "
                "дороже чужой награды: когда запахнет жареным, торгуешься или отходишь."
            ),
            en=(
                "Brago's henchman; suspicious and blunt, scrutinizes strangers, defers to Brago, values "
                "his own skin over a bounty — haggles or backs off when it gets dangerous."
            ),
            greed=3,
            honesty=3,
            courage=2,
            talk=3,
            pushy=2,
            strangers=2,
        ),
    ],
)
def test_bandit_cards(expected):
    assert characters.load()[expected.npc] == expected


async def test_cards_from_the_models_answer(graph):
    subjects = characters.subjects(graph, PEOPLE)
    llm = FakeBackend(script=[FakeStep("", [ToolCall("write_cards", {"cards": [_card()]}, "c1")])], delay=0)
    cards, errors = await characters.write(llm, subjects)
    assert errors == [] and cards[TRADER].greed == 5 and cards[TRADER].name == "Маттео"
    assert "Маттео" in llm.calls[0][1][0].content and "Никакого сюжета" in llm.calls[0][0]
    # out of the list, a scale out of 1..5, nothing at all
    bad = [_card(npc=HILDA), _card(greed=7)]
    llm = FakeBackend(script=[FakeStep("", [ToolCall("write_cards", {"cards": bad}, "c1")])], delay=0)
    cards, errors = await characters.write(llm, subjects)
    assert cards == {} and any("not in the list" in e for e in errors) and any("1..5" in e for e in errors)
    cards, errors = await characters.write(FakeBackend("нет", delay=0), subjects)
    assert cards == {} and errors == ["no write_cards call"]


def test_a_card_in_the_persona(tmp_path):
    card = Card(TRADER, "Маттео", **{k: v for k, v in _card().items() if k != "npc"})
    path = tmp_path / "characters.json"
    characters.save({TRADER: card}, path)
    assert characters.load(path) == {TRADER: card}
    assert characters.load(tmp_path / "none.json") == {}

    assert card.traits_ru() == (
        "Твой характер: жадность — очень высокая; честность — средняя; смелость — низкая; "
        "разговорчивость — средняя; назойливость — низкая; к чужакам — подозрителен."
    )
    text = render_instructions(TRADER, "Маттео", [], card)
    assert card.ru in text and "жадность — очень высокая" in text and "житель острова" not in text
    # a hand-written biography goes first
    assert "некромант" in render_instructions(XARDAS, "Ксардас", [], card)

    personas = PersonaProvider(None, {TRADER: card})
    p = personas.get(TRADER, "Маттео")
    assert p.traits == "A proud, tight-fisted market trader." and card.ru in p.instructions
    assert "житель острова" in personas.get(LOBART, "Лобарт").instructions  # no card


def test_the_voices_command_prints_the_tts_voices(capsys):
    """`gothic-ai voices`: what the shorts' dub designs its English voices from."""
    import argparse

    from gothic_ai import cli
    from gothic_ai.persona import DEFAULT_VOICE, VOICES

    assert cli.voices(argparse.Namespace(npc=["none_100_xardas", "NO_SUCH_NPC"], game_dir=None)) == 0
    got = json.loads(capsys.readouterr().out)
    assert got["NONE_100_XARDAS"] == {"voice_id": "NONE_100_XARDAS", "voice": VOICES["NONE_100_XARDAS"]}
    assert got["NO_SUCH_NPC"]["voice"] == DEFAULT_VOICE
