import asyncio
import inspect
import json
from types import SimpleNamespace
from typing import Any

import pytest

from gothic_ai.chronicle import Chronicle
from gothic_ai.crimes import Crimes
from gothic_ai.dialogue import DialogueManager
from gothic_ai.llm import ToolCall, ToolResult
from gothic_ai.llm.fake import FakeBackend, FakeStep
from gothic_ai.memory import MemoryStore
from gothic_ai.perception import PerceptionDesk
from gothic_ai.persona import PersonaProvider
from gothic_ai.places import PlaceBook
from gothic_ai.protocol import (
    BROKEN_TRACE_PROTOCOL,
    NOISE_PROTOCOL,
    TRACE_PROTOCOL,
    Loading,
    NpcEvent,
    NpcView,
    NpcViews,
)
from gothic_ai.sightings import Sightings, when
from gothic_ai.social import Rumors
from gothic_ai.trust import Trust
from tests.fake_game import FakeGame, connected
from tests.fakes import Clock
from tests.test_dialogue import _until
from tests.test_memory import close_memory_stores as close_memory_stores
from tests.test_perception import HILDA, LOBART, Social, armor, look, physical_trace

DAY = 1440
AT_8 = DAY + 8 * 60 + 10  # the second day, 08:10

BOOK = PlaceBook(
    {
        "world": "NEWWORLD.ZEN",
        "places": [
            {"id": "FARM1_PASTURE", "ru": "пастбище", "where_ru": "на пастбище у входа на ферму Лобарта",
             "en": "the pasture", "where_en": "on the pasture"},
            {"id": "FARM1_OUT", "ru": "двор", "where_ru": "во дворе фермы Лобарта",
             "en": "the yard", "where_en": "in Lobart's farmyard"},
        ],
        "points": {"NW_FARM1_PATH_01": 0, "NW_FARM1_OUT_01": 1, "NW_FARM1_OUT_02": 1},
    }
)  # fmt: skip


def view(t: int, hero: dict | None = None, *sees: dict) -> NpcView:
    """Lobart's view at game minute `t`: the hero on the pasture, Hilda in the yard, a stranger."""
    hero = hero or look("PLAYER", "Я", 8, seen=True, waypoint="NW_FARM1_PATH_01")
    return NpcView.model_validate(
        {
            "npc": LOBART,
            "name": "Лобарт",
            "waypoint": "NW_FARM1_OUT_01",
            "time": t,
            "hero": hero,
            "sees": list(sees)
            or [
                look(HILDA, "Хильда", 6, state="ZS_COOK_CAULDRON", waypoint="NW_FARM1_OUT_02"),
                look("SLD_800_X", "Наёмник", 12, guild=7, armor=armor("ITAR_SLD_L", "Доспех")),
            ],
        }
    )


class Link:
    def __init__(self) -> None:
        self.listeners: list[Any] = []
        self.session = SimpleNamespace(branch="A", parent=None, parent_seq=0)
        self.world_generation = 0
        self.sent: list[Any] = []

    async def send(self, msg: Any) -> None:
        self.sent.append(msg)


async def push(seen: Sightings, *views: NpcView) -> None:
    """The game's once-a-second push of the views (npc_views), as the link hands it out."""
    for listener in seen.link.listeners:
        result = listener(NpcViews(type="npc_views", t=0, views=list(views)))
        if inspect.isawaitable(result):
            await result


async def sightings(
    store: MemoryStore, link: Link | None = None, clock: Clock | None = None,
) -> Sightings:
    link = link or Link()
    desk = PerceptionDesk(link, social=Social, places=BOOK, clock=clock or Clock())  # type: ignore[arg-type]
    chronicle = Chronicle(link, store)  # type: ignore[arg-type]
    await chronicle.initialize()
    seen = Sightings(link, store, desk, chronicle)  # type: ignore[arg-type]
    await seen.prepare(LOBART)
    await seen.prepare(HILDA)
    return seen


async def record_theft(
    chronicle: Chronicle, *, id: str = "C1", witness: str = HILDA, hour: int = 8,
) -> None:
    await chronicle.record({
        "fact": "crime", "id": id, "kind": "theft", "victim": LOBART, "owner": LOBART,
        "community": "farm", "item": "ITFO_CHEESE", "count": 1, "waypoint": "NW_FARM1_OUT_01",
        "day": 1, "hour": hour, "minute": 10, "witnesses": [witness],
    })
    await chronicle.record({"fact": "crime_seen", "npc": witness, "crime": id})


async def test_a_crime_seen_is_evidence_for_the_same_caught_memory_trust_and_rumor_path():
    fake = FakeGame(branch="A")
    fake.people_near = [{"npc": HILDA, "name": "Хильда", "distance": 400, "guild": 0}]
    llm = FakeBackend(script=[
        FakeStep("Хм.", [ToolCall("catch_lie", {"said": "убил волка", "seen": "Д1"}, "wrong")]),
        FakeStep("Я сам видел!", [ToolCall("catch_lie", {"said": "я ничего не брал", "seen": "D1"}, "c")]),
        FakeStep("Больше на слово не поверю."),
    ], delay=0)
    store = await MemoryStore.open(":memory:")
    async with connected(fake) as link:
        dialogue = DialogueManager(link, llm, PersonaProvider(None), memory=store)
        try:
            await dialogue.initialize()
            chronicle = Chronicle(link, store)
            await chronicle.initialize()
            crimes = Crimes(link, chronicle)
            rumors = Rumors(link, dialogue, chronicle, crimes=crimes)
            desk = PerceptionDesk(link, social=Social, places=BOOK)
            seen = Sightings(link, store, desk, chronicle, crimes=crimes)
            dialogue.extensions += [seen, Trust(chronicle)]
            await record_theft(chronicle, witness=LOBART)
            await dialogue.open(LOBART, "Лобарт")
            await dialogue.say_as_player(LOBART, "Я ничего не брал.")
            await _until(lambda: chronicle.facts("caught") != [], 5)
            await asyncio.gather(*dialogue.conversations[LOBART].actions)

            assert "Д1:" in str(llm.calls[0][1][-2].content)
            outputs = [i.output for c in llm.calls for i in c[1] if isinstance(i, ToolResult)]
            assert outputs[0].startswith("Отклонено: он этого не говорил")
            caught = chronicle.facts("caught")
            assert len(caught) == 1 and caught[0]["crime"] == "C1" and caught[0]["said"]
            assert caught[0]["how"].startswith("сам видел:")
            assert chronicle.facts("belief") == []  # ordinary denials are not quest completion beliefs
            assert any(
                r.kind == "caught" and r.crime == "C1" and r.known[LOBART] == 0
                for r in rumors.rumors.values()
            )
            assert dialogue.conversations[LOBART].intent.attitude == "wary"
            assert any(
                "Ты уличил героя во лжи" in e.content
                for e in dialogue.conversations[LOBART].memory.entries
            )
            assert "он уже лгал тебе" in (Trust(chronicle).situation(LOBART) or "")
        finally:
            await dialogue.close()
    await store.close()


async def test_crime_rumors_and_suspicions_do_not_become_own_seen_evidence():
    store = await MemoryStore.open(":memory:")
    await store.ensure_branch("A")
    seen = await sightings(store)
    seen.crimes = Crimes(seen.link, seen.chronicle)
    await record_theft(seen.chronicle)
    await seen.chronicle.record({
        "fact": "rumor", "crime": "C1", "kind": "theft", "from": HILDA, "to": LOBART, "hops": 1,
    })
    await seen.chronicle.record({
        "fact": "crime_suspected", "npc": LOBART, "crime": "C1", "suspect": "PLAYER",
    })
    assert await seen.situation(LOBART) is None
    assert seen.tools(LOBART) == []
    assert "Д1:" in (await seen.situation(HILDA) or "")
    assert [tool.name for tool in seen.tools(HILDA)] == ["catch_lie"]
    await asyncio.gather(*seen._background, *seen.chronicle._background)
    await store.close()


async def test_what_it_saw_stays_where_and_how_and_follows_the_save():
    store = await MemoryStore.open(":memory:")
    await store.ensure_branch("A")
    seen = await sightings(store)
    for t in (AT_8, AT_8 + 2, AT_8 + 4):  # the hero on the pasture for four minutes
        await push(seen, view(t))
    await seen._sweep(AT_8 + 64)  # an hour later: he has long gone
    militia = armor("ITAR_MIL_L", "Лёгкий доспех ополчения")
    hero = look("PLAYER", "Я", 3, seen=True, waypoint="NW_FARM1_OUT_01", armor=militia, weapon="melee")
    await push(seen, view(AT_8 + 64, hero, look(HILDA, "Хильда", 30, waypoint="NW_FARM1_OUT_02")))

    assert await seen.situation(LOBART) == (
        "Что ты видел сам, своими глазами (не с чужих слов):\n"
        "Героя (имени его ты не знаешь, но в лицо узнаёшь):\n"
        "- В1: сейчас — во дворе фермы Лобарта; на нём: Лёгкий доспех ополчения (по виду ополченец); "
        "с оружием наголо.\n"
        "- В2: сегодня 08:10–08:14, около часа назад — на пастбище у входа на ферму Лобарта; без доспеха."
    )  # Hilda is before its eyes now: not a memory; the stranger is not kept
    assert [t.name for t in seen.tools(LOBART)] == ["catch_lie"]

    # in the memory of the save branch: written when it starts and when it ends, not every second
    # (the hero and Hilda: the first sightings, started and ended; the second ones, started)
    assert len(await store.seen("A", LOBART)) == 2 + 2 + 1 + 1
    again = await sightings(store)
    trail = again.hero_trail(LOBART)
    assert [(s.start, s.end, s.places) for s in trail] == [
        (AT_8 + 64, AT_8 + 64, ["во дворе фермы Лобарта"]),
        (AT_8, AT_8 + 4, ["на пастбище у входа на ферму Лобарта"]),
    ]
    # a load to before it: it never saw him
    await store.ensure_branch("B", "A", 0)
    link = Link()
    link.session.branch = "B"
    assert await (await sightings(store, link)).situation(LOBART) is None
    await store.close()


async def test_the_people_it_knows_last_seen_and_a_body():
    store = await MemoryStore.open(":memory:")
    await store.ensure_branch("A")
    clock = Clock()
    seen = await sightings(store, clock=clock)
    hero = look("PLAYER", "Я", 9, seen=False)
    await push(seen, view(AT_8, hero, look(HILDA, "Хильда", 6, waypoint="NW_FARM1_OUT_02")))
    await push(
        seen,
        view(AT_8 + 30, hero, look(HILDA, "Хильда", 6, kind="body", body="dead", waypoint="NW_FARM1_OUT_02")),
    )
    await seen._sweep(AT_8 + DAY)
    clock.t += 60  # its view of then is long gone
    assert await seen.situation(LOBART) == (
        "Что ты видел сам, своими глазами (не с чужих слов):\n"
        "Знакомых, последний раз:\n"
        "- тело: Хильда — вчера 08:40, утром — во дворе фермы Лобарта."
    )
    assert "Хильда — вчера 08:10, утром" in (seen.recall(LOBART, "где Хильда?") or "")
    assert seen.recall(LOBART, "волк") is None
    assert seen.tools(LOBART) == []  # never saw the hero: nothing to catch him with
    await store.close()


async def test_strangers_and_beasts_worth_remembering():
    # step M2: Cavalorn saw the bandits in their cave, came out, and «там бандиты, разберись»
    store = await MemoryStore.open(":memory:")
    await store.ensure_branch("A")
    clock = Clock()
    seen = await sightings(store, clock=clock)
    bdt = armor("ITAR_BDT_M", "Доспех бандита")
    hero = look("PLAYER", "Я", 9, seen=False)
    brago = look(
        "BDT_1014_BANDIT_L",
        "Браго",
        10,
        guild=9,
        att="hostile",
        armor=bdt,
        state="ZS_SIT_CAMPFIRE",
        waypoint="NW_FARM1_PATH_01",
    )
    sheep = look("SHEEP", "Овца", 7, kind="monster", guild=40, state="ZS_MM_ALLSCHEDULER")
    passer = look("VLK_400_X", "Горожанин", 8, guild=3)  # nobody to remember
    await push(seen, view(AT_8, hero, brago, sheep, passer))
    await push(seen, view(AT_8 + 2, hero, brago, sheep, passer))
    await push(seen, view(AT_8 + 20, hero, look(HILDA, "Хильда", 6, waypoint="NW_FARM1_OUT_02")))
    clock.t += 1

    assert await seen.situation(LOBART) == (
        "Что ты видел сам, своими глазами (не с чужих слов):\n"
        "Недавно видел, сейчас не видишь:\n"
        "- незнакомец, по виду бандит (id BDT_1014_BANDIT_L) — сегодня 08:10–08:12, 20 минут назад — на "
        "пастбище у входа на ферму Лобарта; враг тебе; сидит у костра."
    )  # fmt: skip
    # its intent may point to him; a companion set on him goes there first (orders.py)
    assert seen.targets(LOBART) == {
        "BDT_1014_BANDIT_L": "незнакомец, по виду бандит, видел 20 минут назад — "
        "на пастбище у входа на ферму Лобарта"
    }
    assert seen.where_seen(LOBART, "bdt_1014_bandit_l") == "FARM1_PASTURE"
    assert "незнакомец, по виду бандит" in (seen.recall(LOBART, "где бандиты?") or "")
    # it is kept across a load, and fades
    again = await sightings(store)
    await again._sweep(AT_8 + 20)
    assert again.where_seen(LOBART, "BDT_1014_BANDIT_L") == "FARM1_PASTURE"
    await seen._sweep(AT_8 + 4 * 60)
    assert seen.targets(LOBART) == {} and seen.where_seen(LOBART, "BDT_1014_BANDIT_L") is None
    assert "Недавно видел" not in (await seen.situation(LOBART) or "")
    await store.close()


def test_time_in_words():
    assert when(AT_8, AT_8 + 4, AT_8 + 6) == "сегодня 08:10–08:14, только что"
    assert when(AT_8, AT_8, AT_8 + 25) == "сегодня 08:10, 25 минут назад"
    assert when(AT_8, AT_8 + 20, AT_8 + 200) == "сегодня 08:10–08:30, 3 часа назад"
    assert when(AT_8, AT_8 + 3, AT_8 + 3) == "сейчас, с 08:10"
    assert when(AT_8 + 11 * 60, AT_8 + 11 * 60, AT_8 + DAY) == "вчера 19:10, вечером"
    assert when(AT_8, AT_8, AT_8 + 5 * DAY) == "5 дней назад 08:10, утром"


def trace_view(time: int, kind: str = "moved", **extra) -> NpcView:
    return NpcView.model_validate({
        "npc": LOBART, "name": "Лобарт", "time": time, "waypoint": "NW_FARM1_OUT_01",
        "traces": [physical_trace(kind=kind)], **extra,
    })


async def test_trace_memory_keeps_first_seen_time_place_and_save_branch(tmp_path, monkeypatch):
    path = str(tmp_path / "memory.sqlite3")
    store = await MemoryStore.open(path)
    await store.ensure_branch("A")
    seen = await sightings(store)
    await push(seen, trace_view(AT_8))
    await push(seen, trace_view(AT_8 + 30))
    assert len(await store.seen("A", LOBART)) == 1
    assert await seen.situation(LOBART) == (
        "Что ты видел сам, своими глазами (не с чужих слов):\n"
        "Следы, которые ты видел:\n"
        "- В1: сегодня 08:10, 30 минут назад — сдвинутый предмет: Стол; во дворе фермы Лобарта."
    )
    assert await seen.situation(HILDA) is None  # no view: nobody told Hilda what Lobart saw
    await push(seen, trace_view(
        AT_8 + 31, "tilted",
        traces=[physical_trace(kind="tilted", waypoint="NW_FARM1_PATH_01")],
    ))
    await asyncio.gather(*seen._background)
    head = await store.head("A")
    assert head == 2  # a different visible state has its own first observation
    await store.close()

    # A new store and observer process read only what the saved memory head contains.
    store = await MemoryStore.open(path)
    await store.ensure_branch("loaded", "A", head)
    link = Link()
    link.session.branch = "loaded"
    again = await sightings(store, link)
    await again._sweep(AT_8 + 60)
    situation = await again.situation(LOBART) or ""
    assert (
        "В1: сегодня 08:41, 30 минут назад — наклонённый предмет: Стол; "
        "на пастбище у входа на ферму Лобарта"
    ) in situation
    assert (
        "В2: сегодня 08:10, около часа назад — сдвинутый предмет: Стол; во дворе фермы Лобарта"
    ) in situation
    assert "08:10, около часа назад — наклонённый предмет" not in situation
    await push(again, trace_view(AT_8 + 61, "tilted"))
    assert await store.head("loaded") == head
    assert "Стол" in (again.recall(LOBART, "где стол?") or "")
    assert set(again.targets(LOBART)) == set()  # no trace becomes a companion or combat target
    await store.ensure_branch("before", "A", 0)
    before = Link()
    before.session.branch = "before"
    assert await (await sightings(store, before)).situation(LOBART) is None
    records = [json.loads(raw) for raw in await store.seen("loaded", LOBART)]
    assert all("other" not in record and "culprit" not in record for record in records)
    assert {(record["trace_kind"], record["start"], tuple(record["place_ids"])) for record in records} == {
        ("moved", AT_8, ("FARM1_OUT",)), ("tilted", AT_8 + 31, ("FARM1_PASTURE",)),
    }
    await store.close()

    for invalidation in ("read_loading", "write_loading", "write_session"):
        store = await MemoryStore.open(":memory:")
        await store.ensure_branch("A")
        seen = await sightings(store)
        if invalidation == "read_loading":
            await seen.saw(trace_view(AT_8))
            seen._forget()
        entered, release = asyncio.Event(), asyncio.Event()
        sent_before = len(seen.link.sent)

        if invalidation == "read_loading":
            read = store.seen

            async def delayed_read(branch, npc, *, read=read, entered=entered, release=release):
                records = await read(branch, npc)
                entered.set()
                await release.wait()
                return records

            monkeypatch.setattr(store, "seen", delayed_read)
            pending = asyncio.create_task(seen.prepare(LOBART))
        else:
            write = store.add_seen

            async def delayed_write(branch, npc, record, *, write=write, entered=entered, release=release):
                entered.set()
                await release.wait()
                return await write(branch, npc, record)

            monkeypatch.setattr(store, "add_seen", delayed_write)
            pending = asyncio.create_task(seen.saw(trace_view(AT_8)))

        try:
            await asyncio.wait_for(entered.wait(), 1)
            assert all(not mind.traces for mind in seen._minds.values())
            if invalidation == "write_session":
                # The reader updates its session before this listener's Session callback runs.
                seen.link.session = SimpleNamespace(branch="A", parent=None, parent_seq=0)
            else:
                await seen._on_message(Loading(type="loading"))
        finally:
            release.set()
            await pending
        assert all(not mind.traces for mind in seen._minds.values())
        assert len(seen.link.sent) == sent_before
        if invalidation != "write_session":
            with pytest.raises(KeyError):
                seen.mind(LOBART)
        # The old write may commit, but cannot publish into the new world.
        assert await store.head("A") == 1
        await store.close()


@pytest.mark.parametrize("protocol", [TRACE_PROTOCOL - 1, TRACE_PROTOCOL])
async def test_trace_memory_obeys_hello_even_when_given_a_view_directly(protocol):
    store = await MemoryStore.open(":memory:")
    await store.ensure_branch("A")
    link = Link()
    link.hello = SimpleNamespace(protocol=protocol)
    seen = await sightings(store, link)
    await seen.saw(trace_view(AT_8))
    assert bool(await seen.situation(LOBART)) == (protocol >= TRACE_PROTOCOL)
    assert bool(seen.tools(LOBART)) == (protocol >= TRACE_PROTOCOL)
    await store.close()


@pytest.mark.parametrize("protocol", [TRACE_PROTOCOL - 1, TRACE_PROTOCOL, BROKEN_TRACE_PROTOCOL])
async def test_broken_memory_obeys_hello_without_disabling_old_traces(protocol):
    store = await MemoryStore.open(":memory:")
    await store.ensure_branch("A")
    link = Link()
    link.hello = SimpleNamespace(protocol=protocol)
    seen = await sightings(store, link)
    await seen.saw(trace_view(AT_8, traces=[
        physical_trace(), physical_trace("broken:7", "broken", "сломанный забор"),
    ]))
    situation = await seen.situation(LOBART) or ""
    assert ("Стол" in situation) == (protocol >= TRACE_PROTOCOL)
    assert ("сломанный забор" in situation) == (protocol >= BROKEN_TRACE_PROTOCOL)
    await asyncio.gather(*seen._background)
    await store.close()


@pytest.mark.parametrize("age", [2, None])
async def test_broken_memory_keeps_observed_age_after_a_load(tmp_path, age):
    path = str(tmp_path / "memory.sqlite3")
    store = await MemoryStore.open(path)
    await store.ensure_branch("A")
    seen = await sightings(store)
    first = trace_view(AT_8, traces=[physical_trace("broken:7", "broken", "обломки ящика", age=age)])
    await seen.saw(first)
    await seen.saw(first.model_copy(update={"time": AT_8 + 20}))
    await asyncio.gather(*seen._background)
    head = await store.head("A")
    assert head == 1
    record = json.loads((await store.seen("A", LOBART))[0])
    assert record["trace_age"] == age
    assert record["trace_kind"] == "broken"
    assert "culprit" not in record and "other" not in record
    await store.close()

    store = await MemoryStore.open(path)
    await store.ensure_branch("loaded", "A", head)
    link = Link()
    link.session.branch = "loaded"
    again = await sightings(store, link)
    await again._sweep(AT_8 + 60)
    age_words = f"{age} мин" if age is not None else "неизвестна"
    assert (
        "В1: сегодня 08:10, около часа назад — обломки ящика; во дворе фермы Лобарта; "
        f"давность при наблюдении {age_words}."
    ) in (await again.situation(LOBART) or "")
    assert again.targets(LOBART) == {}
    await store.close()


@pytest.mark.parametrize("protocol", [NOISE_PROTOCOL - 1, NOISE_PROTOCOL, TRACE_PROTOCOL])
async def test_noise_is_heard_memory_with_time_and_place_never_visual_evidence(protocol):
    store = await MemoryStore.open(":memory:")
    await store.ensure_branch("A")
    link = Link()
    link.hello = SimpleNamespace(protocol=protocol)
    seen = await sightings(store, link)
    # v36 has no event time: use the last game's clock. v37 can arrive before any npc_view.
    if protocol < TRACE_PROTOCOL:
        await push(seen, NpcView(npc=LOBART, name="Лобарт", time=AT_8))
    noise = NpcEvent(
        type="npc_event", npc=LOBART, kind="noise", place="NW_FARM1_OUT_02",
        time=AT_8 if protocol >= TRACE_PROTOCOL else None,
    )
    for listener in link.listeners:
        result = listener(noise)
        if inspect.isawaitable(result):
            await result
    if protocol < NOISE_PROTOCOL:
        assert await seen.situation(LOBART) is None and await store.seen("A", LOBART) == []
        await store.close()
        return
    assert await seen.situation(LOBART) == (
        "Что ты слышал сам (звук не говорит, кто его вызвал):\n"
        "- сегодня 08:10, только что — слышал грохот во дворе фермы Лобарта."
    )
    assert seen.tools(LOBART) == [] and seen._shown[LOBART] == {}
    assert "PLAYER" not in (await store.seen("A", LOBART))[0]
    assert await seen.situation(HILDA) is None
    again = await sightings(store)
    await again._sweep(AT_8 + 20)
    assert "сегодня 08:10, 20 минут назад — слышал грохот во дворе фермы Лобарта" in (
        await again.situation(LOBART) or ""
    )
    # Loading clears in-process observation state; the active branch restores the saved record.
    await again._on_message(Loading(type="loading"))
    await again.prepare(LOBART)
    assert len(again.mind(LOBART).heard) == 1
    await store.close()


@pytest.mark.parametrize("kind,name,lie,description", [
    ("moved", "Стол", "я не трогал стол", "сдвинутый предмет: Стол"),
    ("broken", "сломанный забор", "этот забор давно сломан, ещё с весны", "сломанный забор"),
])
async def test_a_seen_trace_is_a_catch_lie_evidence_line_in_the_model_prompt(kind, name, lie, description):
    observed = trace_view(AT_8, traces=[physical_trace(kind=kind, name=name, age=2)])
    fake = FakeGame(branch="A")
    fake.views = {LOBART: observed.model_copy(update={"time": AT_8 + 20}).model_dump()}
    llm = FakeBackend(script=[
        FakeStep(
            "Я видел свежий след.", [ToolCall("catch_lie", {"said": lie, "seen": "В1"}, "c1")]
        ),
        FakeStep("Я запомню твои слова."),
    ], delay=0)
    store = await MemoryStore.open(":memory:")
    async with connected(fake) as link:
        dialogue = DialogueManager(link, llm, PersonaProvider(None), memory=store)
        await dialogue.initialize()
        chronicle = Chronicle(link, store)
        await chronicle.initialize()
        desk = PerceptionDesk(link, social=Social, places=BOOK)
        seen = Sightings(link, store, desk, chronicle)
        dialogue.extensions += [desk, seen]
        dialogue.sight = desk.sight
        await seen.saw(observed)
        await dialogue.open(LOBART, "Лобарт")
        await dialogue.say_as_player(LOBART, lie)
        await _until(lambda: chronicle.facts("caught") != [], 5)
    prompt = str(llm.calls[0][1][-2].content)
    assert "Что ты видел сам, своими глазами" in prompt
    assert f"В1: сегодня 08:10, 20 минут назад — {description}; во дворе фермы Лобарта" in prompt
    assert f"Следы: {description}" in prompt
    if kind == "broken":
        assert "давность при наблюдении 2 мин" in prompt
    catch = next(tool for tool in llm.calls[0][2] if tool.name == "catch_lie")
    assert "Следы, которые ты видел" in catch.description
    caught = chronicle.facts("caught")
    assert len(caught) == 1
    assert caught[0]["title"] == lie
    assert f"{description}; во дворе фермы Лобарта" in caught[0]["how"]
    assert caught[0]["how"].startswith("сам видел: ")  # the trace, without claiming to have seen its maker
    await store.close()


async def test_a_lie_about_where_he_was_is_caught_by_what_it_saw():
    fake = FakeGame(branch="A")
    store = await MemoryStore.open(":memory:")
    wrong = FakeStep("Хм.", [ToolCall("catch_lie", {"said": "убил волка", "seen": "В2"}, "c1")])
    catch = FakeStep(
        "Врёшь! Я тебя час назад на пастбище видел.",
        [ToolCall("catch_lie", {"said": "всё утро был в городе", "seen": "B2"}, "c2")],
    )
    llm = FakeBackend(script=[wrong, catch, FakeStep("Пошёл вон.")], delay=0)
    async with connected(fake) as link:
        dialogue = DialogueManager(link, llm, PersonaProvider(None), memory=store)
        await dialogue.initialize()
        chronicle = Chronicle(link, store)
        await chronicle.initialize()
        desk = PerceptionDesk(link, social=Social, places=BOOK)
        seen = Sightings(link, store, desk, chronicle)
        dialogue.extensions += [desk, seen, Trust(chronicle)]
        dialogue.sight = desk.sight
        fake.people_near = [{"npc": HILDA, "name": "Хильда", "distance": 400, "guild": 0}]
        for t in (AT_8, AT_8 + 4):  # this morning, on the pasture
            await seen.saw(view(t))
        # an hour later he comes back and talks to Lobart (the view asked before the turn)
        now = view(AT_8 + 64, look("PLAYER", "Я", 3, seen=True, waypoint="NW_FARM1_OUT_01"))
        fake.views = {LOBART: now.model_dump()}
        await dialogue.open(LOBART, "Лобарт")
        await dialogue.say_as_player(LOBART, "Я всё утро был в городе, только что оттуда.")
        await _until(lambda: chronicle.facts("rumor") != [], 5)

    situation = str(llm.calls[0][1][-2].content)
    assert "В2: сегодня 08:10–08:14, около часа назад — на пастбище у входа на ферму Лобарта" in situation
    assert "catch_lie" in [t.name for t in llm.calls[0][2]]
    outputs = [i.output for c in llm.calls for i in c[1] if isinstance(i, ToolResult)]
    assert outputs[0].startswith("Отклонено: он этого не говорил")  # words he did not say
    c = chronicle.facts("caught")[0]
    assert (c["npc"], c["title"], c["said"]) == (LOBART, "всё утро был в городе", True)
    assert c["how"].startswith("сам видел его сегодня 08:10–08:14, около часа назад — на пастбище")
    assert dialogue.conversations[LOBART].intent.attitude == "wary"
    trust = Trust(chronicle)
    assert "он уже лгал тебе: «всё утро был в городе» — сам видел его" in (trust.situation(LOBART) or "")
    heard = [e.content for e in dialogue.conversations[HILDA].memory.entries]
    assert any("тот сказал «всё утро был в городе», а это неправда" in t for t in heard)
    await store.close()


def test_a_lie_in_words_is_retold_as_words():
    from gothic_ai.amends import Lie as AmendsLie
    from gothic_ai.social import Rumor

    rumor = Rumor("SAID_X_1", "caught", LOBART, "Лобарт", "всё утро был в городе", said=True)
    assert rumor.told("Хильда") == (
        "Слух (со слов: Хильда): герой обманул человека по имени Лобарт — "
        "сказал ему «всё утро был в городе», а это неправда"
    )
    lie = AmendsLie("SAID_X_1", "caught", LOBART, "Лобарт", "всё утро был в городе", said=True)
    assert "сказал ему «всё утро был в городе», а тот своими глазами видел" in lie.words()
