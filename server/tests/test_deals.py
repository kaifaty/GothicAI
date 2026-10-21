import asyncio
from types import SimpleNamespace
from unittest.mock import AsyncMock

import pytest

import gothic_ai.deals
from gothic_ai.chronicle import Chronicle
from gothic_ai.contracts import gold_text
from gothic_ai.crimes import Crimes
from gothic_ai.deals import Deals, LieRef
from gothic_ai.dialogue import DialogueManager
from gothic_ai.llm import ToolCall
from gothic_ai.llm.fake import FakeBackend, FakeStep
from gothic_ai.memory import MemoryStore
from gothic_ai.parley import Parley
from gothic_ai.persona import PersonaProvider
from gothic_ai.talk import Talk, TalkDesk
from gothic_ai.trust import Trust
from tests.fake_game import FakeGame, connected
from tests.test_dialogue import XARDAS, _until

POTION, GOLD = "ITPO_HEALTH_01", "ITMI_GOLD"
BRAGO = "BDT_1014_BANDIT_L"


def _offer(npc_gives: int, gold: int, call_id: str = "c1") -> ToolCall:
    args = {
        "npc_gives": [{"item": POTION, "count": npc_gives}],
        "hero_gives": [{"item": GOLD, "count": gold}],
    }
    return ToolCall("propose_deal", args, call_id)


async def _setup(fake: FakeGame, llm: FakeBackend, store: MemoryStore, *, gates=None):
    fake.symbols = {GOLD: "item", POTION: "item"}
    fake.item_names = {GOLD: "Золото", POTION: "Эликсир лечения"}
    fake.npc_items = {XARDAS: {POTION: 3}}
    link_cm = connected(fake)
    link = await link_cm.__aenter__()
    dialogue = DialogueManager(link, llm, PersonaProvider(None), memory=store)
    chronicle = Chronicle(link, store)
    deals = Deals(link, dialogue, chronicle, gates=gates)
    dialogue.extensions.append(deals)
    await link.snapshot(XARDAS)
    dialogue.open(XARDAS, "Ксардас")
    return link_cm, dialogue, chronicle, deals


def _shown(fake: FakeGame) -> list[dict]:
    return [m for m in fake.deals if m["type"] == "deal_show"]


async def test_haggle_then_accept_what_he_saw():
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    llm = FakeBackend(
        script=[
            FakeStep("Эликсир — за пятьдесят золотых.", [_offer(1, 50)]),
            FakeStep("Ладно, тридцать.", [_offer(1, 30, "c2")]),
            FakeStep("По рукам."),
        ],
        delay=0,
    )
    link_cm, dialogue, chronicle, deals = await _setup(fake, llm, store)
    try:
        assert "propose_deal" in [t.name for t in dialogue._offered(XARDAS)]
        assert "withdraw_deal" not in [t.name for t in dialogue._offered(XARDAS)]
        await dialogue.say_as_player(XARDAS, "Продай эликсир.")
        await _until(lambda: len(_shown(fake)) == 1)
        v1 = _shown(fake)[0]
        assert (v1["version"], v1["npc"]) == (1, XARDAS)
        assert v1["npc_gives"] == [{"item": POTION, "count": 1, "text": "Эликсир лечения"}]
        assert v1["hero_gives"] == [{"item": GOLD, "count": 50, "text": "50 золотых"}]
        assert "withdraw_deal" in [t.name for t in dialogue._offered(XARDAS)]

        # haggling changes the terms in the same window
        await dialogue.say_as_player(XARDAS, "Дорого.")
        await _until(lambda: len(_shown(fake)) == 2)
        v2 = _shown(fake)[1]
        assert (v2["deal"], v2["version"], v2["hero_gives"][0]["count"]) == (v1["deal"], 2, 30)
        situation = llm.calls[1][1][-2].content
        assert (
            "Твоё предложение герою ждёт его согласия: ты отдаёшь — Эликсир лечения; герой отдаёт — 50"
            in (situation)
        )

        # he has too little: nothing changes hands, the offer stays
        fake.hero_items = {GOLD: 10}
        await fake.send(type="deal_accept", deal=v1["deal"], version=2)
        await _until(lambda: fake.statuses.count("notice") == 1)
        assert deals.open[XARDAS].state == "open"
        assert fake.hero_items == {GOLD: 10} and chronicle.facts("deal") == []

        # what he saw a moment ago is not what is shown now: not carried out, the window shows v2 again
        fake.hero_items = {GOLD: 40}
        await fake.send(type="deal_accept", deal=v1["deal"], version=1)
        await _until(lambda: len(_shown(fake)) == 3)
        assert _shown(fake)[2]["version"] == 2 and fake.hero_items == {GOLD: 40}

        # he accepts v2: everything changes hands at once, the npc answers
        await fake.send(type="deal_accept", deal=v1["deal"], version=2)
        await _until(lambda: chronicle.facts("deal") != [])
        assert fake.hero_items == {GOLD: 10, POTION: 1} and fake.npc_items[XARDAS] == {POTION: 2, GOLD: 30}
        effects = [m for m in fake.story if m["type"] == "story_effect"][-1]
        assert effects["check"] is True
        assert {"type": "deal_hide", "deal": v1["deal"]} in fake.deals
        fact = chronicle.facts("deal")[0]
        assert (fact["npc"], fact["version"], fact["hero_gives"][0]["count"]) == (XARDAS, 2, 30)
        await _until(lambda: len(llm.calls) == 3)
        assert "сделка состоялась" in llm.calls[2][1][-1].content
        assert [t.name for t in llm.calls[2][2]] == ["set_intent"]  # a wish fulfilled may be dropped
        notes = [e.content for e in dialogue.conversations[XARDAS].memory.entries]
        assert any("Герой принял твою сделку" in n and "30 золотых" in n for n in notes)
        assert XARDAS not in deals.open and deals.situation(XARDAS) is None
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_declined_the_npc_offers_otherwise():
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    llm = FakeBackend(
        script=[
            FakeStep("Эликсир — за пятьдесят.", [_offer(1, 50)]),
            FakeStep("Ну, тогда сорок.", [_offer(1, 40, "c2")]),
        ],
        delay=0,
    )
    link_cm, dialogue, chronicle, deals = await _setup(fake, llm, store)
    try:
        await dialogue.say_as_player(XARDAS, "Продай эликсир.")
        await _until(lambda: len(_shown(fake)) == 1)
        # N: the window is gone in the game already; the npc hears of it and may offer again
        await fake.send(type="deal_decline", deal="D1", version=1)
        await _until(lambda: len(llm.calls) == 2)
        assert "Герой отказался от сделки" in llm.calls[1][1][-1].content
        assert [t.name for t in llm.calls[1][2]] == ["propose_deal"]
        # offered less, the request's tools are the same: the start of the npc's requests does not change
        every = [t.name for t in llm.sent_tools[0]]
        assert "propose_deal" in every and "withdraw_deal" in every and "set_intent" in every
        assert [[t.name for t in tools] for tools in llm.sent_tools] == [every, every]
        assert llm.cache_keys == [XARDAS, XARDAS]
        await _until(lambda: len(_shown(fake)) == 2)
        again = _shown(fake)[1]
        assert (again["deal"], again["version"], again["hero_gives"][0]["count"]) == ("D2", 1, 40)
        assert {"type": "deal_hide", "deal": "D1"} in fake.deals
        notes = [e.content for e in dialogue.conversations[XARDAS].memory.entries]
        assert any("Герой отказался от твоего предложения" in n and "50 золотых" in n for n in notes)
        assert chronicle.facts("deal") == []
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_what_cannot_be_offered():
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    link_cm, dialogue, chronicle, deals = await _setup(fake, FakeBackend("…", delay=0), store)
    try:
        snap = dialogue.link.snapshots[XARDAS]

        async def offer(npc_gives, hero_gives):
            return await deals.propose(
                XARDAS, "Ксардас", {"npc_gives": npc_gives, "hero_gives": hero_gives}, snap
            )

        o = await offer([{"item": "ITAR_PAL_H", "count": 1}], [])
        assert (
            o.status == "rejected" and "у тебя нет ITAR_PAL_H" in o.output and "Эликсир лечения" in o.output
        )
        o = await offer([{"item": POTION, "count": 5}], [])
        assert o.status == "rejected" and "у тебя только 3" in o.output
        o = await offer([{"item": "ITMW_ZWEIHAENDER1", "count": 1}], [])  # in its hands
        assert o.status == "rejected"
        o = await offer([], [{"item": "ITMI_NOTHING", "count": 1}])
        assert o.status == "rejected" and "нет таких предметов: ITMI_NOTHING" in o.output
        o = await offer([], [])
        assert o.status == "rejected" and "ничего нет" in o.output
        o = await offer([{"item": POTION, "count": 0}], [])
        assert o.status == "rejected"
        assert _shown(fake) == []

        # the same item twice is added up; the same terms again change nothing
        o = await offer(
            [{"item": POTION, "count": 1}, {"item": POTION, "count": 1}], [{"item": GOLD, "count": 5}]
        )
        assert o.status == "accepted"
        await _until(lambda: _shown(fake) != [])
        assert _shown(fake)[-1]["npc_gives"][0]["count"] == 2
        o = await offer([{"item": POTION, "count": 2}], [{"item": GOLD, "count": 5}])
        assert "уже видит" in o.output and len(_shown(fake)) == 1

        # withdrawn: the window goes
        call = ToolCall("withdraw_deal", {}, "w1")
        conv = dialogue.conversations[XARDAS]
        o = await deals.handle(dialogue, conv, call, snap)
        assert o is not None and o.status == "accepted" and XARDAS not in deals.open
        await _until(lambda: fake.deals[-1] == {"type": "deal_hide", "deal": "D1"})
        o = await deals.handle(dialogue, conv, call, snap)
        assert o is not None and o.status == "rejected"
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_a_thing_of_its_story_by_a_guessed_id():
    # 2026-10-14: Cavalorn asked for his letter as ITWR_CAVALORNLETTER twice, and no window came
    sealed, opened, map_ = (
        "ITWR_SATURASFIRSTMESSAGE_ADDON_SEALED",
        "ITWR_SATURASFIRSTMESSAGE_ADDON",
        "ITWR_MAP_NEWWORLD_ORNAMENTS_ADDON",
    )
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    link_cm, dialogue, chronicle, deals = await _setup(fake, FakeBackend("…", delay=0), store)
    fake.symbols.update({sealed: "item", opened: "item", map_: "item", "DIA_X": "dialog"})
    fake.item_names.update({sealed: "Запечатанное послание", opened: "Вскрытое послание", map_: "Карта"})
    deals.things = lambda npc: {sealed, opened, map_, GOLD, "ITMI_NOT_IN_THE_GAME", "DIA_X"}
    try:
        snap = dialogue.link.snapshots[XARDAS]

        async def offer(hero_gives, bring=()):
            args = {
                "npc_gives": [{"item": POTION, "count": 1}],
                "hero_gives": hero_gives,
                "task": {"kill": [], "bring": list(bring), "days": 0},
            }
            return await deals.propose(XARDAS, "Ксардас", args, snap)

        letter = {"item": "ITWR_CAVALORNLETTER", "count": 1}
        fake.hero_items = {map_: 1, sealed: 1, GOLD: 30}  # the map is a letter too, but not his: one of his
        deals.things = lambda npc: {sealed, opened, GOLD}
        o = await offer([letter, {"item": GOLD, "count": 5}])
        assert o.status == "accepted"
        assert "Предмета ITWR_CAVALORNLETTER нет" in o.output
        assert f"Запечатанное послание ({sealed})" in o.output
        await _until(lambda: _shown(fake) != [])
        assert [t["item"] for t in _shown(fake)[-1]["hero_gives"]] == [sealed, GOLD]

        # he has none of its letters, or two: it is told the things of its story, of the kind it asked first
        deals.things = lambda npc: {sealed, opened, map_, GOLD, "ITMI_NOT_IN_THE_GAME", "DIA_X"}
        for items in ({GOLD: 30}, {sealed: 1, map_: 1}):
            fake.hero_items = items
            o = await offer([letter])
            assert o.status == "rejected" and o.output == (
                "Отклонено: нет таких предметов: ITWR_CAVALORNLETTER. Вещи из твоих дел: "
                f"Карта ({map_}); Вскрытое послание ({opened}); Запечатанное послание ({sealed}); "
                f"Золото ({GOLD})."
            )
        # what it wants brought some day is not looked for on him
        fake.hero_items = {sealed: 1}
        o = await offer([], bring=[letter])
        assert o.status == "rejected" and "нет таких предметов: ITWR_CAVALORNLETTER." in o.output
        # with no story graph it is just told there is no such thing
        deals.things = None
        o = await offer([letter])
        assert o.output == "Отклонено: нет таких предметов: ITWR_CAVALORNLETTER."
        assert len(_shown(fake)) == 1
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_an_offer_not_taken_goes_with_the_conversation():
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    link_cm, dialogue, chronicle, deals = await _setup(fake, FakeBackend("…", delay=0), store)
    try:
        snap = dialogue.link.snapshots[XARDAS]
        args = {"npc_gives": [{"item": POTION, "count": 1}], "hero_gives": [{"item": GOLD, "count": 20}]}
        await deals.propose(XARDAS, "Ксардас", args, snap)
        deals.end(XARDAS, "walked_away")
        await _until(lambda: [m["type"] for m in fake.deals] == ["deal_show", "deal_hide"])
        notes = [e.content for e in dialogue.conversations[XARDAS].memory.entries]
        assert any("Герой ушёл, не приняв его, твоё предложение снято" in n for n in notes)
        # accepting it now does nothing
        fake.hero_items = {GOLD: 50}
        await fake.send(type="deal_accept", deal="D1", version=1)
        await asyncio.sleep(0.2)
        assert not any(m["type"] == "story_effect" for m in fake.story)
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_any_end_of_the_conversation_takes_the_offer():
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    link_cm, dialogue, chronicle, deals = await _setup(fake, FakeBackend("…", delay=0), store)
    try:
        talk = TalkDesk(dialogue.link, dialogue)
        talk.on_talk_end.append(deals.end)
        snap = dialogue.link.snapshots[XARDAS]
        args = {"npc_gives": [{"item": POTION, "count": 1}], "hero_gives": [{"item": GOLD, "count": 20}]}
        await deals.propose(XARDAS, "Ксардас", args, snap)
        talk.partner = Talk(XARDAS, "Ксардас", 0.0, 0.0)
        talk._end("switched", "Диего")  # the hero turned to someone else
        await _until(lambda: XARDAS not in deals.open)
        await _until(lambda: fake.deals[-1] == {"type": "deal_hide", "deal": "D1"})
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_the_npc_no_longer_has_it():
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    llm = FakeBackend("Ох, а эликсира-то уже нет.", delay=0)
    link_cm, dialogue, chronicle, deals = await _setup(fake, llm, store)
    try:
        snap = dialogue.link.snapshots[XARDAS]
        args = {"npc_gives": [{"item": POTION, "count": 1}], "hero_gives": [{"item": GOLD, "count": 20}]}
        await deals.propose(XARDAS, "Ксардас", args, snap)
        fake.npc_items[XARDAS] = {}  # sold it to someone else meanwhile
        fake.hero_items = {GOLD: 50}
        await fake.send(type="deal_accept", deal="D1", version=1)
        await _until(lambda: XARDAS not in deals.open)
        assert fake.hero_items == {GOLD: 50} and chronicle.facts("deal") == []
        assert fake.deals[-1] == {"type": "deal_hide", "deal": "D1"}
        await _until(lambda: len(llm.calls) == 1)
        assert "Сделка сорвалась" in llm.calls[0][1][-1].content
    finally:
        await link_cm.__aexit__(None, None, None)


def test_gold_reads_right():
    assert [gold_text(n) for n in (1, 2, 11, 21, 30)] == [
        "1 золотой",
        "2 золотых",
        "11 золотых",
        "21 золотой",
        "30 золотых",
    ]


BOOK = f"caught:{XARDAS}:MIS_BOOK"


async def test_he_forgives_a_lie_and_keeps_quiet_about_it():
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    link_cm, dialogue, chronicle, deals = await _setup(fake, FakeBackend("…", delay=0), store)
    try:
        snap = dialogue.link.snapshots[XARDAS]
        caught = {
            "fact": "caught",
            "quest": "MIS_BOOK",
            "npc": XARDAS,
            "name": "Ксардас",
            "title": "Принести книгу",
        }
        chronicle.record({**caught, "how": "видел сам", "day": 1})
        assert "он уже лгал тебе" in Trust(chronicle).situation(XARDAS)
        situation = deals.situation(XARDAS) or ""
        assert f"- {BOOK}: он обманул тебя: уверял, что сделал «Принести книгу», а это неправда" in situation

        async def offer(**terms):
            args = {
                "npc_gives": [],
                "hero_gives": [],
                "forgive": [],
                "silence": [],
                "hero_later": [],
                "repay": [],
            }
            return await deals.propose(XARDAS, "Ксардас", {**args, **terms}, snap)

        o = await offer(forgive=["caught:NOBODY:MIS_X"])
        assert o.status == "rejected" and "ты не знаешь такой лжи" in o.output and BOOK in o.output
        o = await offer(forgive=[BOOK], silence=[BOOK], hero_gives=[{"item": GOLD, "count": 30}])
        assert o.status == "accepted" and "прощаешь ему ложь про «Принести книгу»" in o.output
        await _until(lambda: _shown(fake) != [])
        shown = _shown(fake)[-1]
        assert [t["text"] for t in shown["npc_gives"]] == [
            "Прощает ложь про «Принести книгу»",
            "Молчит о лжи про «Принести книгу»",
        ]
        assert shown["npc_gives"][0]["item"] == ""  # nothing to check in his purse

        fake.hero_items = {GOLD: 30}
        await deals.accept(shown["deal"], 1)
        fact = chronicle.facts("deal")[0]
        assert (fact["forgive"], fact["silence"], fact["gold"]) == ([BOOK], [BOOK], 30)
        line = Trust(chronicle).situation(XARDAS) or ""
        assert (
            "он уже лгал тебе" not in line
            and "он загладил вину за ложь: «Принести книгу» — заплатил 30" in line
        )
        # forgiven: no longer among what he can forgive
        assert BOOK not in (deals.situation(XARDAS) or "")
        o = await offer(forgive=[BOOK])
        assert o.status == "rejected" and "уже простил" in o.output
    finally:
        await link_cm.__aexit__(None, None, None)


def _theft(chronicle: Chronicle, *, witness: str, victim: str = XARDAS, crime_id: str = "C1"):
    chronicle.record(
        {
            "fact": "crime", "id": crime_id, "kind": "theft", "victim": victim, "owner": victim,
            "item": POTION, "count": 2, "day": 1, "hour": 14, "minute": 20,
            "place": {"x": 0, "y": 0, "z": 0}, "waypoint": "FARM", "community": "farm",
        }
    )
    chronicle.record({"fact": "crime_seen", "npc": witness, "crime": crime_id})


async def test_crime_deals_require_the_victim_or_an_actual_witness():
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    link_cm, dialogue, chronicle, deals = await _setup(fake, FakeBackend("…", delay=0), store)
    deals.crimes = Crimes(dialogue.link, chronicle)
    try:
        _theft(chronicle, witness=XARDAS, victim="LESTER")
        snap = dialogue.link.snapshots[XARDAS]
        result = await deals.propose(XARDAS, "Ксардас", {"forgive": ["crime:C1"]}, snap)
        assert result.status == "rejected" and "только пострадавший" in result.output
        result = await deals.propose(XARDAS, "Ксардас", {"silence": ["crime:invented"]}, snap)
        assert result.status == "rejected"
        assert not chronicle.facts("forgiven") and not chronicle.facts("silenced")

        # Knowing a rumor does not make him a witness who can sell his silence.
        chronicle.record({
            "fact": "rumor", "kind": "theft", "crime": "C1", "to": "LOBART", "from": XARDAS, "hops": 1,
        })
        result = await deals.propose("LOBART", "Лобарт", {"silence": ["crime:C1"]}, None)
        assert result.status == "rejected" and "только видевший" in result.output

        result = await deals.propose(XARDAS, "Ксардас", {"silence": ["crime:C1"]}, snap)
        assert result.status == "accepted" and "crime:C1" in (deals.situation(XARDAS) or "")
        assert not chronicle.facts("silenced")  # the offer alone changes nothing
        offer = deals.open[XARDAS]
        await deals.accept(offer.id, offer.version)
        assert chronicle.facts("silenced")[0]["crime"] == "C1"
        assert deals.crimes.known(XARDAS)[0].silenced
    finally:
        await link_cm.__aexit__(None, None, None)


@pytest.mark.parametrize(
    "cover,expected",
    [
        ({"crime": "crime:C1"}, {"crime": "C1"}),
        ({"day": 2}, {"day": 1}),
        ({"day": 1}, {"day": 0}),
        ({"vouch": "Всё утро был со мной."}, {"day": 1, "vouch": "Всё утро был со мной."}),
        ({"vouch": ""}, {"day": 1, "vouch": ""}),
        (
            {"crime": "C1", "day": 2, "vouch": "Он был в поле."},
            {"crime": "C1", "day": 1, "vouch": "Он был в поле."},
        ),
    ],
)
async def test_cover_terms_are_shown_and_recorded_only_on_acceptance(cover, expected):
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    link_cm, dialogue, chronicle, deals = await _setup(fake, FakeBackend("…", delay=0), store)
    deals.crimes = Crimes(dialogue.link, chronicle)
    try:
        if "crime" in cover:
            _theft(chronicle, witness=XARDAS)
        outcome = await deals.propose(XARDAS, "Ксардас", {"cover": cover}, None)
        assert outcome.status == "accepted"
        assert not chronicle.facts("covered") and not chronicle.facts("silenced")
        offer = deals.open[XARDAS]
        assert "прикрыть тебя:" in offer.show().npc_gives[0].text
        assert "обещаешь прикрыть его" in offer.terms()
        await deals.accept(offer.id, offer.version)
        assert chronicle.facts("covered") == [
            {"fact": "covered", "npc": XARDAS, "deal": offer.id, **expected}
        ]
        assert chronicle.facts("deal")[0]["cover"] == expected
        if "crime" in cover:
            [silenced] = chronicle.facts("silenced")
            assert (silenced["crime"], silenced["cover"]) == ("C1", offer.id)
            assert deals.crimes.known(XARDAS)[0].silenced
        else:
            assert not chronicle.facts("silenced")
        deals.reload()
        result = await deals.propose(XARDAS, "Ксардас", {"cover": {"vouch": ""}}, None)
        assert result.status == "accepted" and deals.open[XARDAS].id != offer.id
    finally:
        await link_cm.__aexit__(None, None, None)


@pytest.mark.parametrize(
    "cover,why",
    [
        ({}, "хотя бы одно"),
        ({"crime": None, "day": None, "vouch": None}, "хотя бы одно"),
        ({"crime": "invented"}, "только видевший"),
        ({"day": 4}, "сегодня"),
        ({"day": 0}, "сегодня"),
        ({"day": True}, "сегодня"),
        ({"vouch": "я" * 201}, "200"),
    ],
)
async def test_cover_rejects_unknown_crime_remote_day_and_empty_terms(cover, why):
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    link_cm, dialogue, chronicle, deals = await _setup(fake, FakeBackend("…", delay=0), store)
    deals.crimes = Crimes(dialogue.link, chronicle)
    try:
        result = await deals.propose(XARDAS, "Ксардас", {"cover": cover}, None)
        assert result.status == "rejected" and why in result.output
        assert not deals.open and not chronicle.facts("covered")
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_cover_requires_seen_crime_and_can_follow_a_silence_deal():
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    link_cm, dialogue, chronicle, deals = await _setup(fake, FakeBackend("…", delay=0), store)
    deals.crimes = Crimes(dialogue.link, chronicle)
    try:
        _theft(chronicle, witness="LESTER")
        chronicle.record(
            {
                "fact": "rumor",
                "kind": "theft",
                "crime": "C1",
                "to": XARDAS,
                "from": "LESTER",
            }
        )
        result = await deals.propose(XARDAS, "Ксардас", {"cover": {"crime": "C1"}}, None)
        assert result.status == "rejected" and "только видевший" in result.output
        chronicle.record({"fact": "crime_seen", "npc": XARDAS, "crime": "C1"})
        chronicle.record({"fact": "silenced", "npc": XARDAS, "crime": "C1", "deal": "old"})
        result = await deals.propose(XARDAS, "Ксардас", {"cover": {"crime": "C1"}}, None)
        assert result.status == "accepted"
    finally:
        await link_cm.__aexit__(None, None, None)


@pytest.mark.parametrize("crimes,cover", [(True, False), (False, True)])
async def test_cover_is_unavailable_without_cover_or_crimes(crimes, cover):
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    link_cm, dialogue, chronicle, deals = await _setup(fake, FakeBackend("…", delay=0), store)
    deals.crimes = Crimes(dialogue.link, chronicle) if crimes else None
    deals.cover = cover
    try:
        [tool] = deals.tools(XARDAS)
        assert "cover" not in tool.parameters["properties"] and "(cover)" not in tool.description
        assert "cover" not in deals.all_tools[0].parameters["properties"]
        result = await deals.propose(XARDAS, "Ксардас", {"cover": {"vouch": ""}}, None)
        assert result.status == "rejected" and "выключено" in result.output
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_a_victim_can_forgive_a_crime_he_heard_of():
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    link_cm, dialogue, chronicle, deals = await _setup(fake, FakeBackend("…", delay=0), store)
    deals.crimes = Crimes(dialogue.link, chronicle)
    try:
        _theft(chronicle, witness="LESTER")
        snap = dialogue.link.snapshots[XARDAS]
        result = await deals.propose(XARDAS, "Ксардас", {"forgive": ["crime:C1"]}, snap)
        assert result.status == "rejected"  # being the owner is not knowledge
        chronicle.record({
            "fact": "rumor", "kind": "theft", "crime": "C1", "to": XARDAS, "from": "LESTER", "hops": 1,
        })
        assert "crime:C1" in (deals.situation(XARDAS) or "")
        result = await deals.propose(XARDAS, "Ксардас", {"forgive": ["crime:C1"]}, snap)
        assert result.status == "accepted"
        offer = deals.open[XARDAS]
        await deals.accept(offer.id, offer.version)
        forgiven = chronicle.facts("forgiven")[0]
        assert (forgiven["crime"], forgiven["npc"], forgiven["paid"]) == ("C1", XARDAS, False)
        assert deals.crimes.known(XARDAS)[0].forgiven
        assert "crime:C1" not in (deals.situation(XARDAS) or "")
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_repay_a_theft_returns_the_whole_item_quantity_once():
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    link_cm, dialogue, chronicle, deals = await _setup(fake, FakeBackend("…", delay=0), store)
    deals.crimes = Crimes(dialogue.link, chronicle)
    try:
        _theft(chronicle, witness=XARDAS)
        snap = dialogue.link.snapshots[XARDAS]
        for wanted in ([], [{"item": POTION, "count": 1}]):
            result = await deals.propose(
                XARDAS, "Ксардас", {"repay": ["crime:C1"], "hero_gives": wanted}, snap
            )
            assert result.status == "rejected" and "все украденные вещи" in result.output
        result = await deals.propose(XARDAS, "Ксардас", {
            "repay": ["crime:C1"], "hero_gives": [{"item": POTION, "count": 2}],
        }, snap)
        assert result.status == "accepted"
        fake.hero_items = {POTION: 2}
        offer = deals.open[XARDAS]
        await deals.accept(offer.id, offer.version)
        assert fake.hero_items[POTION] == 0 and fake.npc_items[XARDAS][POTION] == 5
        forgiven = chronicle.facts("forgiven")[0]
        assert forgiven["paid"] and forgiven["hero_gives"][0]["count"] == 2
        assert chronicle.facts("deal")[0]["repay"] == ["crime:C1"]
        assert not chronicle.facts("debt_paid")
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_repay_a_theft_by_agreed_price_only_forgives_after_transfer():
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    link_cm, dialogue, chronicle, deals = await _setup(fake, FakeBackend("…", delay=0), store)
    deals.crimes = Crimes(dialogue.link, chronicle)
    try:
        _theft(chronicle, witness=XARDAS)
        snap = dialogue.link.snapshots[XARDAS]
        result = await deals.propose(XARDAS, "Ксардас", {
            "repay": ["crime:C1"], "hero_gives": [{"item": GOLD, "count": 30}],
        }, snap)
        assert result.status == "accepted"
        offer = deals.open[XARDAS]
        fake.hero_items = {GOLD: 29}
        await deals.accept(offer.id, offer.version)
        assert not chronicle.facts("forgiven") and fake.hero_items[GOLD] == 29
        assert not deals.crimes.known(XARDAS)[0].forgiven
        fake.hero_items[GOLD] = 30
        await deals.accept(offer.id, offer.version)
        assert fake.hero_items[GOLD] == 0 and fake.npc_items[XARDAS][GOLD] == 30
        assert chronicle.facts("forgiven")[0]["paid"]
    finally:
        await link_cm.__aexit__(None, None, None)


def test_a_lie_in_his_own_words():
    said = LieRef(f"caught:{XARDAS}:", "всё утро был в городе", XARDAS, "Ксардас", said=True)
    assert said.words(XARDAS) == "обманул тебя: сказал «всё утро был в городе», а это неправда"
    assert said.words("LESTER") == (
        "обманул человека по имени Ксардас: сказал «всё утро был в городе», а это неправда"
    )
    quest = LieRef(BOOK, "Принести книгу", XARDAS, "Ксардас")
    assert quest.words(XARDAS) == "обманул тебя: уверял, что сделал «Принести книгу», а это неправда"
    # a reward taken for nothing reads the same with or without it
    found = LieRef(f"found_out:{XARDAS}:MIS_BOOK", "Принести книгу", XARDAS, "Ксардас", said=True)
    assert "взял награду за «Принести книгу»" in found.words(XARDAS)


async def test_lies_of_keeps_his_own_words():
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    link_cm, _dialogue, chronicle, deals = await _setup(fake, FakeBackend("…", delay=0), store)
    try:
        lie = {"quest": "", "title": "всё утро был в городе", "said": True, "day": 1}
        chronicle.record({"fact": "caught", "npc": XARDAS, "name": "Ксардас", "how": "видел сам", **lie})
        chronicle.record({"fact": "caught", "npc": XARDAS, "name": "Ксардас", "quest": "MIS_BOOK"})
        heard = {"kind": "caught", "about": XARDAS, "about_name": "Ксардас", "from": XARDAS, "hops": 1}
        chronicle.record({"fact": "rumor", "to": "LESTER", **heard, **lie})
        known, _, _ = deals.lies_of(XARDAS)
        assert known[f"caught:{XARDAS}:"].said and not known[BOOK].said
        heard_of = deals.lies_of("LESTER")[0][f"caught:{XARDAS}:"]
        assert heard_of.said and heard_of.words("LESTER").endswith(
            "сказал «всё утро был в городе», а это неправда"
        )
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_a_debt_by_its_day_or_a_broken_word():
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    link_cm, dialogue, chronicle, deals = await _setup(fake, FakeBackend("…", delay=0), store)
    try:
        snap = dialogue.link.snapshots[XARDAS]
        args = {
            "npc_gives": [],
            "hero_gives": [],
            "forgive": [],
            "silence": [],
            "hero_later": [],
            "repay": [],
        }
        o = await deals.propose(
            XARDAS,
            "Ксардас",
            {
                **args,
                "npc_gives": [{"item": POTION, "count": 1}],
                "hero_later": [{"item": GOLD, "count": 25, "days": 1}],
            },
            snap,
        )
        assert o.status == "accepted" and "отдаст позже 25 золотых до конца дня 3" in o.output  # today: day 2
        await _until(lambda: _shown(fake) != [])
        shown = _shown(fake)[-1]
        assert shown["hero_gives"] == [{"item": "", "count": 0, "text": "Позже: 25 золотых — до конца дня 3"}]
        await deals.accept(shown["deal"], 1)
        assert fake.hero_items == {POTION: 1}  # the potion now, the gold later
        (debt,) = deals.debts.values()
        assert (debt.due, debt.give.count, debt.paid) == (2, 25, False)
        assert f"- {debt.id}: 25 золотых до конца дня 3" in (deals.situation(XARDAS) or "")

        # the day is over: a broken word, to him and in his memory
        assert deals.overdue(2) == []
        assert deals.overdue(3) == [debt] and deals.overdue(4) == []
        assert chronicle.facts("broken")[0]["what"] == "25 золотых до конца дня 3"
        line = Trust(chronicle).situation(XARDAS) or ""
        assert "он не сдержал слово: обещал вернуть 25 золотых до конца дня 3, а не вернул." in line
        assert "На слово ты ему больше не веришь" in line and Trust(chronicle).broken(XARDAS)
        assert "срок прошёл, он не сдержал слово" in (deals.situation(XARDAS) or "")
        notes = [e.content for e in dialogue.conversations[XARDAS].memory.entries]
        assert any("Герой не вернул тебе долг в срок" in n for n in notes)

        # late, he pays it back
        o = await deals.propose(XARDAS, "Ксардас", {**args, "repay": ["nope"]}, snap)
        assert o.status == "rejected" and debt.id in o.output
        # the debt again in his gold as well (a live model did that): told what it comes to
        twice = {**args, "repay": [debt.id], "hero_gives": [{"item": GOLD, "count": 25}]}
        o = await deals.propose(XARDAS, "Ксардас", twice, snap)
        assert (
            o.status == "accepted" and "долг уже включает своё" in o.output and "всего 50 золотых" in o.output
        )
        await deals.propose(XARDAS, "Ксардас", {**args, "repay": [debt.id]}, snap)
        await _until(lambda: len(_shown(fake)) == 3)
        shown = _shown(fake)[-1]
        assert shown["hero_gives"] == [{"item": GOLD, "count": 25, "text": "Долг: 25 золотых"}]
        fake.hero_items = {GOLD: 25}
        await deals.accept(shown["deal"], shown["version"])
        assert debt.paid and chronicle.facts("debt_paid")[0]["id"] == debt.id and fake.hero_items[GOLD] == 0
        assert "Долги героя тебе" not in (deals.situation(XARDAS) or "")
        # a load brings the debts back as they stand
        deals._load()
        assert deals.debts[debt.id].paid and deals.debts[debt.id].broken
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_day_is_told_when_it_changes(monkeypatch):
    monkeypatch.setattr(gothic_ai.deals, "DEBTS_EVERY_S", 0.01)
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    link_cm, dialogue, chronicle, deals = await _setup(fake, FakeBackend("…", delay=0), store)
    watch = None
    try:
        days: list[int] = []
        deals.on_day.append(days.append)
        assert deals.day is None
        # a deal with a debt reads the day
        later = [{"item": GOLD, "count": 25, "days": 2}]
        o = await deals.propose(XARDAS, "Ксардас", {"hero_later": later}, None)
        assert o.status == "accepted"
        await _until(lambda: _shown(fake) != [])
        await deals.accept(_shown(fake)[-1]["deal"], 1)
        assert days == [1] and deals.day == 1

        # the debts' watch reads it again: the same day is not told twice, a new one is
        watch = asyncio.create_task(deals.run())
        await asyncio.sleep(0.1)
        assert days == [1]
        fake.day = 2
        await _until(lambda: days == [1, 2])
        assert deals.day == 2 and chronicle.facts("broken") == []
        fake.day = 4
        await _until(lambda: chronicle.facts("broken") != [])
        assert days == [1, 2, 4]

        # another world may be of another day
        await fake.send(type="loading")
        await _until(lambda: deals.day is None)

        # a debt written past the deals is theirs once they reload
        chronicle.record({"fact": "debt", "id": "e2e", "npc": XARDAS, "item": GOLD, "count": 5, "due": 3})
        assert "e2e" not in deals.debts
        deals.reload()
        assert deals.debts["e2e"].give.count == 5 and len(deals.debts) == 2
    finally:
        if watch is not None:
            watch.cancel()
        await link_cm.__aexit__(None, None, None)


class _IgnoresAllowed(FakeBackend):
    """A model that calls what it sees in the request, whether it is offered or not."""

    async def stream(
        self, *, instructions, messages, tools=None, tool_choice=None, allowed=None, cache_key=None
    ):
        async for event in super().stream(
            instructions=instructions, messages=messages, tools=tools, tool_choice=tool_choice
        ):
            yield event


async def test_a_tool_not_offered_in_the_turn_is_rejected():
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    llm = _IgnoresAllowed(
        script=[FakeStep("Эликсир — за пятьдесят.", [_offer(1, 50)]), FakeStep("Ладно.")], delay=0
    )
    link_cm, dialogue, _, deals = await _setup(fake, llm, store)
    try:
        dialogue.event_turn(XARDAS, "[Герой кивнул.]", tools=("set_intent",))
        await _until(lambda: len(llm.calls) == 2)
        assert "propose_deal" in [t.name for t in llm.calls[0][2]]  # in the request
        assert "сейчас propose_deal тебе недоступен" in llm.calls[1][1][-1].output
        assert XARDAS not in deals.open and _shown(fake) == []
    finally:
        await link_cm.__aexit__(None, None, None)


@pytest.mark.parametrize("hours", [1, 24])
async def test_bandit_truce_is_a_real_deal_accepted_with_y(hours):
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    link_cm, dialogue, chronicle, deals = await _setup(fake, FakeBackend("…", delay=0), store)
    dialogue.guild_of = lambda npc: 9 if npc == BRAGO else 0
    parley = Parley(dialogue.link, dialogue, chronicle)
    deals.parley = parley
    try:
        snap = await dialogue.link.snapshot(BRAGO)
        conv = dialogue.open(BRAGO, "Браго")
        outcome = await parley.handle(
            dialogue,
            conv,
            ToolCall("ultimatum", {"demand": "Заплати сто золотых", "seconds": 30}, "u1"),
            snap,
        )
        assert outcome is not None and outcome.status == "accepted"
        fake.hero_items = {GOLD: 200}
        result = await deals.propose(
            BRAGO,
            "Браго",
            {"hero_gives": [{"item": GOLD, "count": 100}], "truce": {"hours": hours}},
            snap,
        )
        assert result.status == "accepted" and parley.truce(BRAGO) is None
        assert parley.ultimatum(BRAGO) is not None
        offer = deals.open[BRAGO]
        await _until(lambda: _shown(fake) != [])
        assert any("Перемирие" in t["text"] for t in _shown(fake)[-1]["npc_gives"])
        await fake.send(type="deal_accept", deal=offer.id, version=offer.version)
        await _until(lambda: bool(chronicle.facts("deal")))
        assert fake.hero_items[GOLD] == 100 and fake.npc_items[BRAGO][GOLD] == 100
        assert chronicle.facts("deal")[0]["truce"] == {"hours": hours}
        assert parley.ultimatum(BRAGO) is None and parley.truce(BRAGO) is not None
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_truce_without_a_clock_fails_before_any_exchange():
    fake = FakeGame(branch="A")
    fake.hero_items = {GOLD: 200}
    link_cm, dialogue, chronicle, deals = await _setup(
        fake, FakeBackend("…", delay=0), MemoryStore(":memory:")
    )
    dialogue.guild_of = lambda npc: 9
    deals.parley = Parley(dialogue.link, dialogue, chronicle)
    try:
        snap = await dialogue.link.snapshot(BRAGO, fresh=True)
        result = await deals.propose(
            BRAGO,
            "Браго",
            {"hero_gives": [{"item": GOLD, "count": 100}], "truce": {"hours": 1}},
            snap,
        )
        assert result.status == "accepted"
        offer = deals.open[BRAGO]
        dialogue.link.snapshots.clear()
        deals.parley._minute = None
        assert deals.parley.now() is None
        with pytest.raises(ValueError, match="truce needs the game clock"):
            await deals.accept(offer.id, offer.version)
        assert offer.state == "open"
        assert fake.hero_items[GOLD] == 200 and fake.npc_items.get(BRAGO, {}).get(GOLD, 0) == 0
        assert not chronicle.facts("deal") and not chronicle.facts("truce")
        assert not any(msg["type"] == "story_effect" for msg in fake.story)
    finally:
        await link_cm.__aexit__(None, None, None)


@pytest.mark.parametrize("truce", [{"hours": 0}, {"hours": 25}, {"hours": True}, {"hours": 1.5}, {}])
async def test_invalid_truce_term_is_rejected(truce):
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    link_cm, dialogue, chronicle, deals = await _setup(fake, FakeBackend("…", delay=0), store)
    dialogue.guild_of = lambda npc: 9
    deals.parley = Parley(dialogue.link, dialogue, chronicle)
    try:
        result = await deals.propose(XARDAS, "Ксардас", {"truce": truce}, None)
        assert result.status == "rejected" and "truce" in result.output
        assert not deals.open and not chronicle.facts("deal")
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_truce_requires_bandit_and_enabled_parley():
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    link_cm, dialogue, chronicle, deals = await _setup(fake, FakeBackend("…", delay=0), store)
    try:
        assert "truce" not in deals.all_tools[0].parameters["properties"]
        result = await deals.propose(XARDAS, "Ксардас", {"truce": {"hours": 1}}, None)
        assert result.status == "rejected" and "выключено" in result.output
        deals.parley = Parley(dialogue.link, dialogue, chronicle)
        assert "truce" in deals.all_tools[0].parameters["properties"]
        result = await deals.propose(XARDAS, "Ксардас", {"truce": {"hours": 1}}, None)
        assert result.status == "rejected" and "только бандит" in result.output
        assert not deals.open
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_accepting_any_deal_clears_bandit_ultimatum():
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    link_cm, dialogue, chronicle, deals = await _setup(fake, FakeBackend("…", delay=0), store)
    dialogue.guild_of = lambda npc: 9 if npc == BRAGO else 0
    parley = Parley(dialogue.link, dialogue, chronicle)
    deals.parley = parley
    try:
        snap = await dialogue.link.snapshot(BRAGO)
        conv = dialogue.open(BRAGO, "Браго")
        await parley.handle(
            dialogue,
            conv,
            ToolCall("ultimatum", {"demand": "Дай мне пять золотых", "seconds": 30}, "u1"),
            snap,
        )
        assert parley.ultimatum(BRAGO) is not None
        fake.hero_items = {GOLD: 5}
        outcome = await deals.propose(
            BRAGO, "Браго", {"hero_gives": [{"item": GOLD, "count": 5}]}, snap
        )
        assert outcome.status == "accepted" and parley.ultimatum(BRAGO) is not None
        offer = deals.open[BRAGO]
        await deals.accept(offer.id, offer.version)
        assert parley.ultimatum(BRAGO) is None and parley.truce(BRAGO) is None
    finally:
        await link_cm.__aexit__(None, None, None)


@pytest.mark.parametrize("cover", [False, True])
@pytest.mark.parametrize("parley_enabled", [False, True])
@pytest.mark.parametrize(
    "enabled,npc,on_post",
    [
        (False, "MIL_310_STADTWACHE", True),
        (True, "MIL_310_STADTWACHE", True),
        (True, "MIL_310_STADTWACHE", False),
        (True, XARDAS, False),
    ],
)
async def test_gate_deal_request_schema_is_available_only_on_post(
    enabled, npc, on_post, cover, parley_enabled
):
    fake = FakeGame(branch="A")
    gates = SimpleNamespace(on_post=lambda _: on_post) if enabled else None
    link_cm, dialogue, chronicle, deals = await _setup(
        fake, FakeBackend("…", delay=0), MemoryStore(":memory:"), gates=gates
    )
    deals.crimes = Crimes(dialogue.link, chronicle) if cover else None
    deals.parley = Parley(dialogue.link, dialogue, chronicle) if parley_enabled else None
    try:
        [tool] = [
            tool for tool in dialogue._request_tools(dialogue._offered(npc))
            if tool.name == "propose_deal"
        ]
        assert ("let_pass" in tool.parameters["properties"]) is (enabled and on_post)
        assert ("let_pass" in tool.parameters["required"]) is (enabled and on_post)
        assert ("Пропуск через ворота" in tool.description) is (enabled and on_post)
        assert ("cover" in tool.parameters["properties"]) is cover
        assert ("truce" in tool.parameters["properties"]) is parley_enabled
        assert ("truce" in tool.parameters["required"]) is parley_enabled
        assert ("Только бандитам" in tool.description) is parley_enabled
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_gate_deal_is_shown_and_paid_only_with_the_passage(monkeypatch):
    guard = "MIL_310_STADTWACHE"
    fake = FakeGame(branch="A")
    fake.hero_items = {GOLD: 100}
    gates = SimpleNamespace(
        on_post=lambda npc: npc == guard,
        bribe_error=AsyncMock(return_value=""),
        bribe=AsyncMock(),
    )
    link_cm, dialogue, chronicle, deals = await _setup(
        fake, FakeBackend("…", delay=0), MemoryStore(":memory:"), gates=gates
    )
    monkeypatch.setattr(dialogue, "event_turn", lambda *args, **kwargs: None)

    async def transfer(npc, effects=None):
        await dialogue.link.story_effect(npc, effects, check=True)

    gates.bribe.side_effect = transfer
    try:
        [tool] = deals.tools(guard)
        assert "let_pass" in tool.parameters["properties"]
        assert "let_pass" in tool.parameters["required"]
        [tool] = deals.tools(XARDAS)
        assert "let_pass" not in tool.parameters["properties"] and "let_pass" not in tool.description
        dialogue.open(guard, "Стражник")
        result = await deals.propose(
            guard, "Стражник", {"let_pass": True, "hero_gives": [{"item": GOLD, "count": 100}]}, None
        )
        assert result.status == "accepted" and "пропускаешь через ворота" in result.output
        await _until(lambda: len(_shown(fake)) == 1)
        shown = _shown(fake)[0]
        assert shown["npc_gives"] == [{"item": "", "count": 0, "text": "Пропускает через ворота"}]
        assert fake.hero_items[GOLD] == 100
        gates.bribe.assert_not_awaited()
        await deals.accept(shown["deal"], shown["version"])
        effects = [{"do": "give", "item": GOLD, "count": 100, "to": "self"}]
        gates.bribe.assert_awaited_once_with(guard, effects=effects)
        assert gates.bribe_error.await_count == 2
        assert fake.hero_items[GOLD] == 0 and fake.npc_items[guard][GOLD] == 100
        assert chronicle.facts("deal")[0]["let_pass"] is True
        assert len([m for m in fake.story if m["type"] == "story_effect"]) == 1
    finally:
        await link_cm.__aexit__(None, None, None)


@pytest.mark.parametrize(
    "guard,amount,reason",
    [
        ("MIL_310_STADTWACHE", 40, "за проход нужно не меньше 60 золотых"),
        ("MIL_305_TORWACHE", 100, "ты не берёшь взяток"),
        ("MIL_333_STADTWACHE", 100, "ты сейчас не на посту"),
        ("MIL_310_STADTWACHE", 100, "у героя известная неоплаченная кража"),
    ],
)
async def test_gate_deal_rejected_by_live_permission(guard, amount, reason):
    fake = FakeGame(branch="A")
    fake.hero_items = {GOLD: 100}
    gates = SimpleNamespace(
        on_post=lambda npc: True, bribe_error=AsyncMock(return_value=reason), bribe=AsyncMock()
    )
    link_cm, _, chronicle, deals = await _setup(
        fake, FakeBackend("…", delay=0), MemoryStore(":memory:"), gates=gates
    )
    try:
        result = await deals.propose(
            guard, "Стражник", {"let_pass": True, "hero_gives": [{"item": GOLD, "count": amount}]}, None
        )
        assert result.status == "rejected" and reason in result.output
        gates.bribe_error.assert_awaited_once_with(guard, amount)
        gates.bribe.assert_not_awaited()
        assert _shown(fake) == [] and chronicle.facts("deal") == [] and fake.hero_items[GOLD] == 100
    finally:
        await link_cm.__aexit__(None, None, None)


@pytest.mark.parametrize("reason", ["ты уже не на посту", "у героя известная неоплаченная кража"])
async def test_gate_deal_rechecked_on_y_without_losing_gold(monkeypatch, reason):
    guard = "MIL_310_STADTWACHE"
    fake = FakeGame(branch="A")
    fake.hero_items = {GOLD: 100}
    gates = SimpleNamespace(
        on_post=lambda npc: True,
        bribe_error=AsyncMock(side_effect=["", reason]),
        bribe=AsyncMock(),
    )
    link_cm, dialogue, chronicle, deals = await _setup(
        fake, FakeBackend("…", delay=0), MemoryStore(":memory:"), gates=gates
    )
    monkeypatch.setattr(dialogue, "event_turn", lambda *args, **kwargs: None)
    try:
        dialogue.open(guard, "Стражник")
        result = await deals.propose(
            guard, "Стражник", {"let_pass": True, "hero_gives": [{"item": GOLD, "count": 100}]}, None
        )
        assert result.status == "accepted"
        deal = deals.open[guard]
        await deals.accept(deal.id, deal.version)
        gates.bribe.assert_not_awaited()
        assert gates.bribe_error.await_count == 2
        assert deal.state == "withdrawn" and guard not in deals.open
        assert fake.hero_items[GOLD] == 100 and chronicle.facts("deal") == []
        assert not any(m["type"] == "story_effect" for m in fake.story)
        assert any(reason in entry.content for entry in dialogue.conversations[guard].memory.entries)
    finally:
        await link_cm.__aexit__(None, None, None)


@pytest.mark.parametrize(
    "terms",
    [
        {"hero_gives": [{"item": POTION, "count": 1}]},
        {"hero_later": [{"item": GOLD, "count": 100, "days": 1}]},
        {"hero_gives": [{"item": GOLD, "count": 100}], "npc_gives": [{"item": POTION, "count": 1}]},
    ],
)
async def test_gate_deal_requires_only_gold_now(terms):
    fake = FakeGame(branch="A")
    gates = SimpleNamespace(
        on_post=lambda npc: True, bribe_error=AsyncMock(return_value=""), bribe=AsyncMock()
    )
    link_cm, dialogue, _, deals = await _setup(
        fake, FakeBackend("…", delay=0), MemoryStore(":memory:"), gates=gates
    )
    try:
        result = await deals.propose(
            XARDAS, "Ксардас", {"let_pass": True, **terms}, dialogue.link.snapshots[XARDAS]
        )
        assert result.status == "rejected" and "золот" in result.output
        gates.bribe_error.assert_not_awaited()
        assert _shown(fake) == []
    finally:
        await link_cm.__aexit__(None, None, None)
