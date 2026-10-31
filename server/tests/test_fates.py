"""Chronicle data follows save forks without losing alternate deeds or revealing secrets."""

import json
from pathlib import Path

import pytest

from gothic_ai import fates as fates_module
from gothic_ai.chronicle import Chronicle
from gothic_ai.fates import Fates, collect_fates
from gothic_ai.memory import MemoryStore
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
    store.ensure_branch("early", "A", saved)
    fate, [fork] = collect_fates(store, "early")
    assert ids(fate) == ["shared"]
    assert (fork.chapter, fork.day, ids(fork.feats)) == (3, 8, ["lost"])

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
    store.ensure_branch("empty", "A", 0)
    store.ensure_branch("no-deeds", "A", store.head("A"))
    record(store, "no-deeds", fact="visited", place="CITY", day=8)
    fate, others = collect_fates(store, "A")
    assert ids(fate) == ["ours"]
    assert others == []
    fate, [fork] = collect_fates(store, "empty")
    assert fate == []
    assert (fork.chapter, fork.day, ids(fork.feats)) == (1, 0, ["ours"])
    store.ensure_branch("unknown-parent", "unknown", 10)
    assert collect_fates(store, "unknown-parent") == ([], [])


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


async def test_chronicle_get_replies_with_fate_forks_and_same_req_through_fake_game(store):
    saved = award(store, "shared")
    store.ensure_branch("other", "A", saved)
    award(store, "lost", "other", chapter=2, day=4)
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
