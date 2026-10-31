import pytest

from gothic_ai.chronicle import Chronicle
from gothic_ai.concerns import CRIME_WITNESS_EN, Concerns
from gothic_ai.crimes import Crimes
from gothic_ai.deals import Deals
from gothic_ai.dialogue import DialogueManager
from gothic_ai.executor import AGENDA_MAX
from gothic_ai.llm.fake import FakeBackend
from gothic_ai.memory import MemoryStore
from gothic_ai.persona import PersonaProvider
from tests.fake_game import FakeGame, connected
from tests.test_dialogue import XARDAS, _until

LOBART, GOLD, POTION = "BAU_950_LOBART", "ITMI_GOLD", "ITPO_HEALTH_01"
WOLF = f"found_out:{LOBART}:MIS_WOLF"
BOOK = f"caught:{XARDAS}:MIS_BOOK"


async def _setup(fake: FakeGame, *, with_crimes: bool = False):
    store = MemoryStore(":memory:")
    link_cm = connected(fake)
    link = await link_cm.__aenter__()
    dialogue = DialogueManager(link, FakeBackend("…", delay=0), PersonaProvider(None), memory=store)
    chronicle = Chronicle(link, store)
    crimes = Crimes(link, chronicle) if with_crimes else None
    deals = Deals(link, dialogue, chronicle, crimes=crimes)
    concerns = Concerns(link, chronicle, deals)
    return link_cm, chronicle, deals, concerns


def _debt(chronicle: Chronicle, deals: Deals, debt: str, *, npc: str = LOBART, due: int = 2, **give) -> None:
    """A debt written past the deals, as the e2e seeds one."""
    give = give or {"item": GOLD, "count": 25, "name": "Золото"}
    chronicle.record({"fact": "debt", "id": debt, "npc": npc, "npc_name": npc, **give, "due": due, "day": 0})
    deals.reload()


def _lie(kind: str, npc: str, quest: str, title: str) -> dict:
    return {"fact": kind, "quest": quest, "npc": npc, "name": npc, "title": title, "how": "сам", "day": 1}


async def test_a_broken_debt_is_the_lenders_concern():
    fake = FakeGame(branch="A")
    link_cm, chronicle, deals, concerns = await _setup(fake)
    try:
        _debt(chronicle, deals, "d1")
        assert concerns.of(LOBART) == []  # not due yet, nor today
        deals.overdue(3)
        c = concerns.top(LOBART)
        assert c is not None and (c.key, c.npc, c.rank) == ("debt_broken:d1", LOBART, 0)
        assert c.en == "make the hero pay back the 25 gold he owes you; his day has passed"
        assert len(c.en.split()) <= 15 and len(c.en) <= AGENDA_MAX
        assert c.ru == "герой не вернул тебе долг в срок: 25 золотых до конца дня 3"
        assert concerns.agenda(LOBART) == c.en
        # the hero owes nothing to the others
        assert concerns.of(XARDAS) == [] and concerns.agenda(XARDAS) is None

        # something else borrowed
        _debt(chronicle, deals, "d2", npc=XARDAS, item=POTION, count=2, name="Эликсир лечения")
        deals.overdue(3)
        c = concerns.top(XARDAS)
        assert c is not None and "Эликсир лечения ×2" in c.ru
        assert c.en == "make the hero pay back what he borrowed from you; his day has passed"
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_a_debt_due_today_reminds_and_not_before():
    fake = FakeGame(branch="A")
    link_cm, chronicle, deals, concerns = await _setup(fake)
    try:
        seen: list[tuple[str, str | None]] = []  # as the planner reads the agenda when told
        concerns.on_change.append(lambda npc: seen.append((npc, concerns.agenda(npc))))
        _debt(chronicle, deals, "d1", due=2)
        seen.clear()
        deals._set_day(1)
        assert seen == [(LOBART, None)]
        deals._set_day(1)  # the same day: nothing new
        assert len(seen) == 1
        fake.day = 2
        deals._set_day(2)
        reminder = "remind the hero to pay back 25 gold to you by tonight"
        assert seen[-1] == (LOBART, reminder)
        (c,) = concerns.of(LOBART)
        assert (c.key, c.rank) == ("debt_due:d1", 2)
        assert c.ru == "сегодня последний день: герой должен вернуть тебе 25 золотых"

        # he pays it back in a deal: the deals know before the facts are told
        o = await deals.propose(LOBART, "Лобарт", {"repay": ["d1"]}, None)
        assert o.status == "accepted"
        await _until(lambda: any(m["type"] == "deal_show" for m in fake.deals))
        fake.hero_items = {GOLD: 25}
        await deals.accept(deals.open[LOBART].id, 1)
        assert deals.debts["d1"].paid and seen[-1] == (LOBART, None)
        assert concerns.of(LOBART) == []
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_a_lie_found_out_or_caught_until_forgiven():
    fake = FakeGame(branch="A")
    link_cm, chronicle, deals, concerns = await _setup(fake)
    try:
        chronicle.record(_lie("found_out", LOBART, "MIS_WOLF", "Убить волка"))
        c = concerns.top(LOBART)
        assert c is not None and (c.key, c.rank) == (f"lied:{WOLF}", 1)
        assert c.en == "make the hero answer for lying to you and taking your reward"
        assert c.ru == "герой обманул тебя: взял награду за «Убить волка», а дела не сделал"
        chronicle.record(_lie("caught", XARDAS, "MIS_BOOK", "Принести книгу"))
        c = concerns.top(XARDAS)
        assert c is not None and c.key == f"lied:{BOOK}"
        assert c.en == "make the hero prove what he claims; never take his word again"
        assert c.ru == "герой обманул тебя: уверял, что сделал «Принести книгу», а это неправда"
        assert all(len(e.split()) <= 15 for e in (concerns.agenda(LOBART), c.en) if e)

        # forgiven in a deal
        chronicle.record({"fact": "deal", "npc": LOBART, "name": "Лобарт", "forgive": [WOLF], "silence": []})
        assert concerns.of(LOBART) == []
        # amends made for it
        chronicle.record({"fact": "amends", "id": "A1", "npc": XARDAS, "to": XARDAS, "gold": 10, "lie": BOOK})
        assert concerns.of(XARDAS) == []
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_rumors_about_others_are_not_concerns():
    fake = FakeGame(branch="A")
    link_cm, chronicle, deals, concerns = await _setup(fake)
    try:
        chronicle.record(_lie("caught", XARDAS, "MIS_BOOK", "Принести книгу"))
        rumor = {
            "fact": "rumor",
            "kind": "caught",
            "to": LOBART,
            "from": XARDAS,
            "about": XARDAS,
            "about_name": "Ксардас",
            "quest": "MIS_BOOK",
            "title": "Принести книгу",
        }
        chronicle.record(rumor)
        assert BOOK in deals.lies_of(LOBART)[0]  # he knows of it
        assert concerns.of(LOBART) == [] and concerns.top(LOBART) is None
        assert [c.key for c in concerns.of(XARDAS)] == [f"lied:{BOOK}"]
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_rank_and_on_change_from_facts():
    fake = FakeGame(branch="A")
    link_cm, chronicle, deals, concerns = await _setup(fake)
    try:
        changed: list[str] = []
        concerns.on_change.append(changed.append)
        _debt(chronicle, deals, "due", due=2)
        chronicle.record(_lie("caught", LOBART, "MIS_FIELD", "Вспахать поле"))
        _debt(chronicle, deals, "late", due=1)
        deals._set_day(2)
        deals.overdue(2)
        mind = concerns.of(LOBART)
        assert [(c.key, c.rank) for c in mind] == [
            ("debt_broken:late", 0),
            (f"lied:caught:{LOBART}:MIS_FIELD", 1),
            ("debt_due:due", 2),
        ]
        assert concerns.top(LOBART) == mind[0] and concerns.of(LOBART) is mind  # kept

        # facts of other kinds change nothing
        changed.clear()
        chronicle.record({"fact": "belief", "npc": LOBART, "quest": "MIS_FIELD", "believed": False})
        chronicle.record({"fact": "died", "npc": "WOLF", "killer": "PLAYER"})
        assert changed == [] and concerns.of(LOBART) is mind
        # both sides of the amends are told
        chronicle.record({"fact": "amends", "id": "A1", "npc": XARDAS, "to": LOBART, "lie": "caught:X:Q"})
        assert changed == [XARDAS, LOBART] and concerns.of(LOBART) is not mind

        # a load: the branch loaded has none of it
        await fake.send(type="session", branch="B", parent=None, parent_seq=0, reason="load")
        await _until(lambda: deals.debts == {})
        assert concerns.of(LOBART) == []
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_a_debt_read_again_is_told_at_once():
    """A debt written past the deals and then read again: whoever cached the lender's agenda in between (the
    planner, told by the fact) hears of it from the reload, not on the next day."""
    fake = FakeGame(branch="A")
    link_cm, chronicle, deals, concerns = await _setup(fake)
    try:
        seen: list[tuple[str, str | None]] = []
        concerns.on_change.append(lambda npc: seen.append((npc, concerns.agenda(npc))))
        deals._set_day(2)
        _debt(chronicle, deals, "d1", due=2)  # due today
        assert seen[0] == (LOBART, None)  # the fact came before the deals read it
        assert seen[-1] == (LOBART, "remind the hero to pay back 25 gold to you by tonight")
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_the_agenda_set_for_a_lie_ends_with_it():
    """quests.py keeps the lie's agenda in the npc's intent; once the lie is forgiven it is no agenda of the
    npc's any more. An agenda of the conversation's own stays."""
    fake = FakeGame(branch="A")
    link_cm, chronicle, deals, concerns = await _setup(fake)
    try:
        caught = "make the hero prove what he claims; never take his word again"
        chronicle.record(_lie("caught", XARDAS, "MIS_BOOK", "Книга"))
        assert concerns.current(XARDAS, caught) == caught  # the lie stands
        chronicle.record({"fact": "deal", "npc": XARDAS, "forgive": [BOOK], "day": 1})
        assert concerns.top(XARDAS) is None
        assert concerns.current(XARDAS, caught) is None
        assert (
            concerns.current(XARDAS, "get the hero to bring me a book") == "get the hero to bring me a book"
        )
        assert concerns.current(XARDAS, None) is None
    finally:
        await link_cm.__aexit__(None, None, None)


def _theft(chronicle: Chronicle, *, witness: str = LOBART, victim: str = XARDAS, **metadata):
    chronicle.record(
        {
            "fact": "crime",
            "id": "C1",
            "kind": "theft",
            "victim": victim,
            "owner": victim,
            "item": POTION,
            "count": 2,
            "day": 1,
            "hour": 14,
            "minute": 20,
            "waypoint": "FARM",
            "community": "farm",
        }
    )
    chronicle.record({"fact": "crime_seen", "npc": witness, "crime": "C1", **metadata})


async def test_a_crime_witness_has_a_choice_without_a_forced_quest():
    fake = FakeGame(branch="A")
    link_cm, chronicle, deals, concerns = await _setup(fake, with_crimes=True)
    try:
        _theft(chronicle, guild=8, npctype=0)
        (c,) = concerns.of(LOBART)
        assert (c.key, c.npc, c.rank, c.en) == ("crime_witness:C1", LOBART, 1, CRIME_WITNESS_EN)
        assert len(c.en.split()) <= 15 and len(c.en) <= AGENDA_MAX
        assert all(
            t in c.ru for t in ("ты видел сам", POTION, "рассказать", "промолчать", "плату", "crime:C1")
        )
        assert "silence" in c.ru and "hero_gives" in c.ru
        assert not chronicle.facts("line") and not chronicle.facts("amends")
        # Stable key: duplicate delivery does not create a second concern.
        chronicle.record({"fact": "crime_seen", "npc": LOBART, "crime": "C1", "guild": 8, "npctype": 0})
        assert [c.key for c in concerns.of(LOBART)] == ["crime_witness:C1"]
        chronicle.record({"fact": "rumor", "to": "HEARER", "from": LOBART, "crime": "C1"})
        chronicle.record({"fact": "crime_suspected", "npc": "SUSPECTER", "crime": "C1"})
        assert concerns.of("HEARER") == [] and concerns.of("SUSPECTER") == []
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_recorded_side_clears_only_the_allied_witness_concern():
    ally = "BAU_4300_ADDON_CAVALORN"
    link_cm, chronicle, deals, concerns = await _setup(FakeGame(branch="A"), with_crimes=True)
    try:
        chronicle.record({"fact": "crime", "id": "C1", "kind": "murder", "victim": XARDAS})
        chronicle.record({"fact": "crime_seen", "npc": ally, "crime": "C1", "guild": 8, "npctype": 0})
        chronicle.record({"fact": "crime_seen", "npc": LOBART, "crime": "C1", "guild": 8, "npctype": 0})
        assert concerns.agenda(ally) == CRIME_WITNESS_EN
        outsider = concerns.of(LOBART)
        assert [c.key for c in outsider] == ["crime_witness:C1"]
        changed = []
        concerns.on_change.append(changed.append)

        chronicle.record({"fact": "crime_side", "npc": ally, "crime": "C1", "why": "fought"})

        assert changed == [ally]
        assert concerns.of(ally) == []
        assert concerns.current(ally, CRIME_WITNESS_EN) is None
        assert concerns.of(LOBART) is outsider
        assert concerns.agenda(LOBART) == CRIME_WITNESS_EN
    finally:
        await link_cm.__aexit__(None, None, None)


@pytest.mark.parametrize(
    ("npc", "victim", "metadata"),
    [
        (LOBART, LOBART, {"guild": 8, "npctype": 0}),
        ("MIL_310_STADTWACHE", XARDAS, {"guild": 1, "npctype": 0}),
        ("SLD_800_LEE", XARDAS, {}),
    ],
)
async def test_victims_and_guards_do_not_get_the_witness_concern(npc, victim, metadata):
    link_cm, chronicle, deals, concerns = await _setup(FakeGame(branch="A"), with_crimes=True)
    try:
        _theft(chronicle, witness=npc, victim=victim, **metadata)
        assert concerns.of(npc) == []
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_a_priced_silence_deal_ends_the_witness_concern_when_accepted():
    fake = FakeGame(branch="A")
    fake.item_names[GOLD] = "Золото"
    fake.symbols = {GOLD: "item"}
    fake.hero_items = {GOLD: 10}
    link_cm, chronicle, deals, concerns = await _setup(fake, with_crimes=True)
    try:
        _theft(chronicle, guild=8, npctype=0)
        result = await deals.propose(
            LOBART, "Лобарт", {"silence": ["crime:C1"], "hero_gives": [{"item": GOLD, "count": 10}]}, None
        )
        assert result.status == "accepted" and concerns.top(LOBART) is not None
        offer = deals.open[LOBART]
        await deals.accept(offer.id, offer.version)
        assert chronicle.facts("silenced")[0]["crime"] == "C1"
        assert chronicle.facts("deal")[0]["gold"] == 10
        assert concerns.of(LOBART) == [] and concerns.current(LOBART, CRIME_WITNESS_EN) is None
    finally:
        await link_cm.__aexit__(None, None, None)


@pytest.mark.parametrize("delivery", ["law", "victim_rumor", "victim_testimony"])
async def test_report_delivery_ends_the_cached_witness_concern(delivery):
    link_cm, chronicle, deals, concerns = await _setup(FakeGame(branch="A"), with_crimes=True)
    try:
        _theft(chronicle, guild=8, npctype=0)
        assert concerns.agenda(LOBART) == CRIME_WITNESS_EN
        changed = []
        concerns.on_change.append(lambda npc: changed.append((npc, concerns.agenda(npc))))
        fact = {
            "law": {"fact": "crime_known", "crime": "C1", "community": "farm"},
            "victim_rumor": {"fact": "rumor", "crime": "C1", "to": XARDAS, "from": LOBART},
            "victim_testimony": {"fact": "testimony", "npc": XARDAS, "witness": LOBART, "crime_told": ["C1"]},
        }[delivery]
        chronicle.record(fact)
        assert concerns.of(LOBART) == [] and (LOBART, None) in changed
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_disabled_crimes_do_not_create_witness_concerns():
    link_cm, chronicle, deals, concerns = await _setup(FakeGame(branch="A"))
    try:
        _theft(chronicle, guild=8, npctype=0)
        assert concerns.of(LOBART) == []
    finally:
        await link_cm.__aexit__(None, None, None)


@pytest.mark.parametrize("cover", [True, False])
async def test_old_cover_testimony_is_a_delivery_only_when_cover_is_disabled(cover):
    link_cm, chronicle, deals, concerns = await _setup(FakeGame(branch="A"), with_crimes=True)
    deals.crimes.cover = cover
    try:
        _theft(chronicle, guild=8, npctype=0)
        assert concerns.agenda(LOBART) == CRIME_WITNESS_EN
        chronicle.record({
            "fact": "testimony", "npc": XARDAS, "witness": LOBART, "cover": "D1",
            "crime_saw": ["C1"], "crime_told": [],
        })
        assert bool(concerns.of(LOBART)) is cover
    finally:
        await link_cm.__aexit__(None, None, None)
