"""Chronicle data follows save forks without losing alternate deeds or revealing secrets."""

import asyncio
import json
import sqlite3
import time
from contextlib import asynccontextmanager
from pathlib import Path
from unittest.mock import AsyncMock

import pytest

from gothic_ai import fates as fates_module
from gothic_ai.chronicle import Chronicle
from gothic_ai.fatecard import FALLBACK, FateCards
from gothic_ai.fates import Fates, collect_fates
from gothic_ai.llm import ToolCall
from gothic_ai.llm.fake import FakeBackend, FakeStep
from gothic_ai.llm.off import OffBackend
from gothic_ai.memory import MemoryStore
from gothic_ai.protocol import FateEnd
from tests.fake_game import FakeGame, connected
from tests.test_dialogue import _until


@pytest.fixture
def store():
    memory = MemoryStore(":memory:")
    memory.ensure_branch("A")
    try:
        yield memory
    finally:
        memory.close()


def record(store, branch="A", **fact):
    return store.add_fact(branch, json.dumps(fact, ensure_ascii=False))


def award(store, id, branch="A", **changes):
    return record(store, branch, **{
        "fact": "feat", "id": id, "title": "Чужой кошель", "text": "Вернул Лобарту золото.",
        "category": "gold", "chapter": 1, "day": 2, **changes,
    })


def end(store, branch="A", **changes):
    return record(store, branch, **{
        "fact": "fate_end", "kind": "death", "title": "Конец пути",
        "text": "Здесь путь героя оборвался.", "name": "Волк", "chapter": 2, "day": 9,
        **changes,
    })


def ids(feats):
    return [feat.id for feat in feats]


def test_one_branch_keeps_chronicle_order_and_complete_fields(store, tmp_path, monkeypatch):
    monkeypatch.setattr(fates_module, "CATEGORY_ICONS", tmp_path / "missing")
    award(store, "first", day=9)
    record(store, fact="feat_image", id="first", hash="hash", style=1)
    award(store, "second", day=3)
    fate, others = collect_fates(store, "A", art_root=tmp_path)
    assert ids(fate) == ["first", "second"]
    assert fate[0].model_dump() == {
        "id": "first", "title": "Чужой кошель", "text": "Вернул Лобарту золото.",
        "category": "gold", "chapter": 1, "day": 9, "engraving": "", "icon": "", "epilogue": "",
    }
    assert others == []


def test_early_load_uses_last_shared_day_and_chapter_then_late_load_restores_fate(store):
    award(store, "shared")
    record(store, fact="chapter", value=3, day=7)
    saved = record(store, fact="visited", place="FARM", day=8)
    award(store, "lost", chapter=4, day=15)
    end(store, chapter=4, day=16)
    store.ensure_branch("early", "A", saved)
    fate, [fork] = collect_fates(store, "early")
    assert ids(fate) == ["shared"]
    assert (fork.chapter, fork.day, ids(fork.feats)) == (3, 8, ["lost"])
    assert [ending.model_dump() for ending in fork.ends] == [{
        "kind": "death", "title": "Конец пути", "text": "Здесь путь героя оборвался.",
        "name": "Волк", "chapter": 4, "day": 16,
    }]

    store.ensure_branch("late", "A", store.head("A"))
    fate, others = collect_fates(store, "late")
    assert ids(fate) == ["shared", "lost"]
    assert others == []


def test_copies_beyond_one_fork_are_deduplicated_and_epilogue_must_be_shown_on_a_copy(store):
    saved = record(store, fact="visited", place="FARM", day=4)
    store.ensure_branch("B", "A", saved)
    shared = award(store, "other", "B")
    record(store, "B", fact="feat_epilogue", feat="other", text="Лобарт узнал правду.")
    store.ensure_branch("C", "B", store.head("B"))
    store.ensure_branch("D", "B", shared)
    award(store, "third", "C")
    award(store, "fourth", "D")
    award(store, "fifth", "B")  # A later write on an older branch must still come last.
    fate, [fork] = collect_fates(store, "A")
    assert fate == []
    assert (fork.chapter, fork.day) == (1, 4)
    assert ids(fork.feats) == ["other", "third", "fourth", "fifth"]
    assert fork.feats[0].epilogue == ""
    record(store, "C", fact="feat_epilogue_shown", feat="other")
    _, [fork] = collect_fates(store, "A")
    assert fork.feats[0].epilogue == "Лобарт узнал правду."
    # A reveal in another branch cannot disclose the current fate's hidden epilogue.
    fate, _ = collect_fates(store, "B")
    assert fate[0].epilogue == ""


def test_forks_are_latest_first_and_same_day_does_not_merge_distinct_cutoffs(store):
    record(store, fact="chapter", value=2, day=5)
    early = record(store, fact="visited", place="FARM", day=6)
    later = record(store, fact="visited", place="CITY", day=6)
    store.ensure_branch("early", "A", early)
    award(store, "early-deed", "early")
    store.ensure_branch("later", "A", later)
    award(store, "later-deed", "later")
    _, forks = collect_fates(store, "A")
    assert [(fork.chapter, fork.day, ids(fork.feats)) for fork in forks] == [
        (2, 6, ["later-deed"]), (2, 6, ["early-deed"]),
    ]


def test_load_before_parent_fork_groups_copied_ancestry_at_its_original_place(store):
    early = record(store, fact="visited", place="FARM", day=3)
    later = record(store, fact="visited", place="CITY", day=7)
    store.ensure_branch("B", "A", later)
    award(store, "later-deed", "B")
    store.ensure_branch("C", "B", early)
    store.ensure_branch("D", "A", early)
    award(store, "other-deed", "D")
    _, forks = collect_fates(store, "C")
    assert len(forks) == 1
    assert (forks[0].chapter, forks[0].day) == (1, 3)
    assert ids(forks[0].feats) == ["later-deed", "other-deed"]


def test_other_playthrough_empty_branches_and_branches_without_deeds_add_no_forks(store):
    award(store, "ours")
    store.ensure_branch("other-game")
    award(store, "foreign", "other-game")
    end(store, "other-game")
    store.ensure_branch("empty", "A", 0)
    end(store, "empty")
    store.ensure_branch("no-deeds", "A", store.head("A"))
    record(store, "no-deeds", fact="visited", place="CITY", day=8)
    end(store, "no-deeds")  # Its inherited deed is already in the current fate.
    fate, others = collect_fates(store, "A")
    assert ids(fate) == ["ours"]
    assert others == []
    fate, [fork] = collect_fates(store, "empty")
    assert fate == []
    assert (fork.chapter, fork.day, ids(fork.feats)) == (1, 0, ["ours"])
    store.ensure_branch("unknown-parent", "unknown", 10)
    assert collect_fates(store, "unknown-parent") == ([], [])


def test_ends_deduplicate_copies_and_order_by_recording_time_with_inherited_deeds(store):
    saved = record(store, fact="visited", place="FARM", day=4)
    store.ensure_branch("B", "A", saved)
    inherited = award(store, "other", "B")
    store.ensure_branch("C", "B", inherited)
    end(store, "C", kind="wrath", title="Вечная служба", name="Ксардас", day=8)
    end(store, "B", kind="finale", title="Путь пройден", name="", day=3)
    store.ensure_branch("copy", "C", store.head("C"))

    fate, [fork] = collect_fates(store, "A")
    assert fate == [] and ids(fork.feats) == ["other"]
    assert (fork.chapter, fork.day) == (1, 4)
    assert [(ending.kind, ending.name, ending.day) for ending in fork.ends] == [
        ("wrath", "Ксардас", 8), ("finale", "", 3),
    ]


def test_current_end_and_its_inherited_copies_are_not_other_fate_ends(store):
    award(store, "ours")
    end(store)
    store.ensure_branch("copy", "A", store.head("A"))
    award(store, "other", "copy")

    fate, [fork] = collect_fates(store, "A")
    assert ids(fate) == ["ours"] and ids(fork.feats) == ["other"]
    assert fork.ends == []


def test_old_feat_index_is_replaced_on_open_and_reads_fate_end(tmp_path):
    path = tmp_path / "memory.sqlite3"
    memory = MemoryStore(path)
    memory.ensure_branch("A")
    award(memory, "ours")
    ended = end(memory)
    memory.close()
    with sqlite3.connect(path) as db:
        db.executescript("""
            DROP INDEX events_by_feat;
            CREATE INDEX events_by_feat ON events (branch, seq)
            WHERE kind='fact'
            AND json_extract(content, '$.fact') IN ('feat', 'feat_epilogue', 'feat_epilogue_shown');
        """)
    memory = MemoryStore(path)
    try:
        facts = memory.feat_facts("A")
        assert [(seq, json.loads(raw)["fact"]) for seq, _, raw in facts] == [
            (1, "feat"), (ended, "fate_end"),
        ]
        with sqlite3.connect(path) as db:
            [definition] = db.execute(
                "SELECT sql FROM sqlite_master WHERE name='events_by_feat'",
            ).fetchone()
        assert "'fate_end'" in definition
    finally:
        memory.close()


def test_epilogue_is_exposed_only_after_reveal_in_that_branch(store):
    award(store, "F")
    record(store, fact="feat_epilogue", feat="F", text="Лобарт узнал правду.")
    fate, _ = collect_fates(store, "A")
    assert fate[0].epilogue == ""
    record(store, fact="feat_epilogue_shown", feat="F")
    fate, _ = collect_fates(store, "A")
    assert fate[0].epilogue == "Лобарт узнал правду."


def test_picture_paths_use_existing_own_files_then_category_files(store, tmp_path, monkeypatch):
    root, categories = tmp_path / "art", tmp_path / "categories"
    monkeypatch.setattr(fates_module, "CATEGORY_ICONS", categories)
    award(store, "own")
    award(store, "fallback")
    award(store, "missing", category="word")
    (root / "own").mkdir(parents=True)
    (categories / "gold").mkdir(parents=True)
    for path in (root / "own/engraving.jpg", categories / "gold/engraving.jpg", categories / "gold/icon.png"):
        path.write_bytes(b"picture")
    fate, _ = collect_fates(store, "A", art_root=root)
    assert fate[0].engraving == str(root / "own/engraving.jpg")
    assert fate[0].icon == str(categories / "gold/icon.png")
    assert fate[1].engraving == str(categories / "gold/engraving.jpg")
    assert fate[1].icon == str(categories / "gold/icon.png")
    assert fate[2].engraving == fate[2].icon == ""
    assert all(Path(fate[0].model_dump()[key]).is_absolute() for key in ("engraving", "icon"))


def test_thirty_quickloads_with_thousands_of_facts_stay_below_200_ms(store, tmp_path):
    def fill(branch, count):
        for n in range(1, count + 1):
            if n % 100 == 0:
                award(store, f"{branch}-{n}", branch, day=n)
            else:
                record(store, branch, fact="visited", place="FARM", day=n)

    fill("A", 3000)
    current = "A"
    for n in range(30):
        child = f"load-{n}"
        store.ensure_branch(child, current, store.head(current) - 50)
        fill(child, 200)
        current = child
    started = time.perf_counter()
    fate, others = collect_fates(store, current, art_root=tmp_path)
    elapsed = time.perf_counter() - started
    assert elapsed < 0.2, f"chronicle_get took {elapsed * 1000:.1f} ms after 30 quickloads"
    assert len(fate) == 60
    assert len(others) == sum(len(fork.feats) for fork in others) == 30
    assert len({feat.id for feat in fate + [feat for fork in others for feat in fork.feats]}) == 90


async def test_chronicle_get_replies_with_fate_forks_and_same_req_through_fake_game(store):
    saved = award(store, "shared")
    store.ensure_branch("other", "A", saved)
    award(store, "lost", "other", chapter=2, day=4)
    end(store, "other")
    fake = FakeGame(branch="A")
    async with connected(fake) as link:
        chronicle = Chronicle(link, store)
        fates = Fates(link, chronicle)
        try:
            await fake.send(type="chronicle_get", req="journal-1")
            await _until(lambda: bool(fake.received))
            [reply] = fake.received
            assert reply["type"] == "chronicle" and reply["req"] == "journal-1"
            assert [feat["id"] for feat in reply["fate"]] == ["shared"]
            assert [feat["id"] for feat in reply["others"][0]["feats"]] == ["lost"]
            assert reply["others"][0]["chapter"] == 1 and reply["others"][0]["day"] == 2
            assert reply["fate"][0]["title"] == "Чужой кошель"
            assert reply["others"][0]["ends"][0]["name"] == "Волк"
        finally:
            await fates.close()


@pytest.mark.parametrize(("enabled", "branch", "deeds"), [
    (False, "A", True), (True, "A", False), (True, None, True),
])
async def test_chronicle_get_always_answers_even_disabled_empty_or_without_session(
    store, enabled, branch, deeds,
):
    if deeds:
        award(store, "hidden-when-disabled")
    fake = FakeGame(branch=branch)
    async with connected(fake) as link:
        fates = Fates(link, Chronicle(link, store), enabled=enabled)
        try:
            await fake.send(type="chronicle_get", req="empty")
            await _until(lambda: bool(fake.received))
            assert fake.received == [{"type": "chronicle", "req": "empty", "fate": [], "others": []}]
        finally:
            await fates.close()


@asynccontextmanager
async def fate_world(store, backend, *, enabled=True):
    fake = FakeGame(branch="A")
    async with connected(fake) as link:
        chronicle = Chronicle(link, store)
        cards = FateCards(link, chronicle, backend, enabled=enabled)
        try:
            yield fake, chronicle, cards
        finally:
            await cards.close()


def fate_answer(**changes):
    return {"title": "Сдержавший слово", "text": "Герой вернул Лобарту золото.",
            "best": ["first", "third"], **changes}


def fate_responses(fake):
    return [m for m in fake.received if m["type"] == "fate_card"]


async def test_fate_end_model_card_repeats_and_early_load_starts_a_new_ending(store):
    shared = record(store, fact="crime", victim_name="Лобарт", witnesses=["SECRET"])
    for id in ("first", "second", "third", "fourth"):
        award(store, id)
    record(store, fact="feat_epilogue", feat="first", name="Лобарт", text="Позже Лобарт узнал правду.")
    record(store, fact="feat_epilogue_shown", feat="first")
    record(store, fact="feat_epilogue", feat="second", name="Тайный", text="HIDDEN_EPILOGUE")
    for n in range(45):
        record(store, fact="visited", place=f"PLACE_{n}", day=8)
    record(store, fact="npc_intent", name="Тайный", plan="HIDDEN_PLAN")
    backend = FakeBackend(script=[FakeStep(tool_calls=[ToolCall(
        "write_fate", fate_answer(best=["third", "first"]),
    )]), FakeStep(tool_calls=[ToolCall("write_fate", fate_answer(best=[]))])], delay=0)
    async with fate_world(store, backend) as (fake, chronicle, _):
        fake.vars["KAPITEL"] = 4
        fake.day = 12
        # Concurrent requests share the same in-flight model call and each get one response.
        for req in ("one", "two"):
            await fake.send(type="fate_end", req=req, kind="death", killer="WOLF", name="Волк")
        await _until(lambda: len(fate_responses(fake)) == 2)
        assert [m["req"] for m in fate_responses(fake)] == ["one", "two"]
        card = fate_responses(fake)[0]
        assert (card["title"], card["text"], ids_of_dicts(card["feats"])) == (
            fate_answer()["title"], fate_answer()["text"], ["first", "third"],
        )
        assert (card["chapter"], card["day"], card["count"], card["off"]) == (4, 12, 4, False)
        [ending] = chronicle.facts("fate_end")
        assert ending == {"fact": "fate_end", "kind": "death", "killer": "WOLF", "name": "Волк",
                          **fate_answer(best=["third", "first"]), "chapter": 4, "day": 12,
                          "fallback": False}
        assert len(backend.calls) == 1 and backend.cache_keys == ["chronicler"]
        context = json.loads(backend.calls[0][1][0].content)
        assert len(context["facts"]) == 40 and context["facts"][0]["place"] == "PLACE_5"
        assert context["kind"] == "death" and context["name"] == "Волк"
        assert context["feats"][0]["epilogue"] == "Позже Лобарт узнал правду."
        assert context["feats"][1]["epilogue"] == ""
        assert "HIDDEN_PLAN" not in json.dumps(context) and "SECRET" not in json.dumps(context)
        await fake.send(type="fate_end", req="repeat", kind="finale")
        await _until(lambda: len(fate_responses(fake)) == 3)
        assert len(backend.calls) == 1 and len(chronicle.facts("fate_end")) == 1
        assert fate_responses(fake)[-1]["title"] == card["title"]
        await fake.send(type="session", branch="early", parent="A", parent_seq=shared, reason="load")
        await _until(lambda: chronicle.branch == "early")
        await fake.send(type="fate_end", req="new-fate", kind="finale")
        await _until(lambda: len(fate_responses(fake)) == 4)
        assert len(backend.calls) == 2
        assert fate_responses(fake)[-1]["feats"] == []
        assert chronicle.facts("fate_end")[0]["kind"] == "finale"


def ids_of_dicts(feats):
    return [f["id"] for f in feats]


@pytest.mark.parametrize("bad_text", [
    "Герой встретил Диего.", "Я вернул золото.", "На моём пути было золото.",
])
async def test_fate_end_retries_invalid_name_or_person_then_records_fallback(store, bad_text):
    record(store, fact="npc_intent", name="Диего", plan="SECRET")
    for id in ("first", "second", "third", "fourth"):
        award(store, id)
    backend = FakeBackend(tool_calls=[ToolCall("write_fate", fate_answer(text=bad_text))], delay=0)
    async with fate_world(store, backend) as (fake, chronicle, _):
        await fake.send(type="fate_end", req="bad", kind="death")
        await _until(lambda: bool(fate_responses(fake)))
        [card] = fate_responses(fake)
        assert (card["req"], card["title"], card["text"]) == ("bad", *FALLBACK["death"])
        assert ids_of_dicts(card["feats"]) == ["second", "third", "fourth"]
        assert len(backend.calls) == 2
        assert len(backend.calls[-1][1]) == 2
        assert chronicle.facts("fate_end")[0]["fallback"] is True


@pytest.mark.parametrize("kind", ["death", "wrath", "finale"])
async def test_fate_end_without_model_uses_exact_fallback(store, kind):
    async with fate_world(store, OffBackend()) as (fake, chronicle, _):
        await fake.send(type="fate_end", req=kind, kind=kind)
        await _until(lambda: bool(fate_responses(fake)))
        [card] = fate_responses(fake)
        assert (card["title"], card["text"]) == FALLBACK[kind]
        assert card["req"] == kind and card["feats"] == []
        assert chronicle.facts("fate_end")[0]["fallback"] is True


async def test_fate_end_model_runtime_error_still_sends_one_card(store, monkeypatch):
    backend = FakeBackend(delay=0)

    async def failed_stream(**kwargs):
        raise RuntimeError("model unavailable")
        yield

    monkeypatch.setattr(backend, "stream", failed_stream)
    async with fate_world(store, backend) as (fake, chronicle, _):
        await fake.send(type="fate_end", req="error", kind="wrath")
        await _until(lambda: bool(fate_responses(fake)))
        [card] = fate_responses(fake)
        assert card["req"] == "error" and (card["title"], card["text"]) == FALLBACK["wrath"]
        assert chronicle.facts("fate_end")[0]["fallback"] is True


async def test_disabled_fate_card_sends_off_without_clock_model_or_fact(store):
    backend = FakeBackend(delay=0)
    async with fate_world(store, backend, enabled=False) as (fake, chronicle, _):
        await fake.send(type="fate_end", req="off", kind="finale")
        await _until(lambda: bool(fate_responses(fake)))
        assert fake.received == [{"type": "fate_card", "req": "off", "off": True, "title": "",
                                  "text": "", "feats": [], "chapter": 1, "day": 0, "count": 0}]
        assert not backend.calls and not chronicle.facts("fate_end")


async def test_fate_card_timeout_covers_both_attempts_with_fake_model_and_clock(store, monkeypatch):
    loop = asyncio.get_running_loop()
    clock = loop.time
    offset = 0
    cancelled = False
    calls = 0
    backend = FakeBackend(delay=0)

    async def stalled_retry(**kwargs):
        nonlocal offset, cancelled, calls
        calls += 1
        if calls == 1:
            offset += 12
            yield ToolCall("write_fate", fate_answer(text="Я был здесь."))
        else:
            # The second attempt has only eight seconds left in the shared budget.
            offset += 8.01
            try:
                await asyncio.Event().wait()
            finally:
                cancelled = True

    monkeypatch.setattr(backend, "stream", stalled_retry)
    async with fate_world(store, backend) as (_, chronicle, cards):
        sent = []
        async def send(command):
            sent.append(command)
        monkeypatch.setattr(cards.link, "send", send)
        monkeypatch.setattr(cards.link, "story_state", AsyncMock(return_value={"day": 9, "vars": {}}))
        monkeypatch.setattr(loop, "time", lambda: clock() + offset)
        started = loop.time()
        await cards._answer(FateEnd(type="fate_end", req="timeout", kind="death"), "A")
        elapsed = loop.time() - started
        assert 20 <= elapsed <= 21 and calls == 2 and cancelled
        [card] = [command for command in sent if command.type == "fate_card"]
        assert card.req == "timeout" and (card.title, card.text) == FALLBACK["death"]
        assert chronicle.facts("fate_end")[0]["fallback"] is True
