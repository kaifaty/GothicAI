"""A chronicler sees the hero's facts and spends only the current save branch's budget."""

import asyncio
import json
from contextlib import asynccontextmanager
from pathlib import Path

import pytest

from gothic_ai import feats as feats_module
from gothic_ai.chronicle import Chronicle
from gothic_ai.feat_art import STYLE_VERSION, Artist, Picture
from gothic_ai.feats import Feats, Proposal, canon, hero_view, notable
from gothic_ai.journal import Journal
from gothic_ai.llm import LLMError, ToolCall
from gothic_ai.llm.fake import FakeBackend, FakeStep
from gothic_ai.memory import MemoryStore
from gothic_ai.places import PlaceBook
from gothic_ai.protocol import Loading, Session
from tests.fake_game import FakeGame, connected
from tests.test_dialogue import _until


def proposal(**changes):
    return {
        "verdict": "feat",
        "title": "Чужой кошель",
        "text": "Украл кошель у Лобарта.",
        "category": "theft",
        "evidence": [1],
        "emblem": "An open purse beside a rusty lock",
        "reason": "Первая кража героя.",
        **changes,
    }


def crime(**changes):
    return {
        "fact": "crime", "id": "C1", "kind": "theft", "victim": "LOBART",
        "victim_name": "Лобарт", **changes,
    }


def feat(**changes):
    return {
        "fact": "feat", "id": "old", "title": "Старое дело", "text": "Вернул чужое.",
        "chapter": 1, "day": 1, "hour": 8, "minute": 0, "trigger": 99,
        "evidence": [99], **changes,
    }


class FakeArtist(Artist):
    def __init__(self, root: Path, *, picture: bool = True):
        super().__init__(root)
        self.picture = picture
        self.started, self.release, self.finished = asyncio.Event(), asyncio.Event(), asyncio.Event()
        self.cancelled = False
        self.calls = []

    async def draw(self, feat_id: str, emblem: str) -> Picture | None:
        self.calls.append((feat_id, emblem))
        self.started.set()
        try:
            await self.release.wait()
            if not self.picture:
                return None
            out = self.root / feat_id
            out.mkdir()
            icon = out / "icon.png"
            icon.write_bytes(b"icon")
            return Picture(out, out / "engraving.jpg", icon, "0123456789abcdef", STYLE_VERSION)
        except asyncio.CancelledError:
            self.cancelled = True
            raise
        finally:
            self.finished.set()


@asynccontextmanager
async def world(
    *answers, gather_s=0, history=(), backend=None, artist=None, image_wait_s=150, places=None,
):
    fake = FakeGame(branch="A")
    fake.day = 10
    store = MemoryStore(":memory:")
    store.ensure_branch("A")
    for fact in history:
        store.add_fact("A", json.dumps(fact, ensure_ascii=False))
    llm = backend or FakeBackend(
        script=[FakeStep(tool_calls=[ToolCall("write_feat", answer)]) for answer in answers], delay=0,
    )
    journal = []
    async with connected(fake) as link:
        chronicle = Chronicle(link, store)
        chronicler = Feats(
            link, chronicle, llm, journal=Journal(journal.append), gather_s=gather_s,
            artist=artist, image_wait_s=image_wait_s, places=places,
        )
        try:
            yield fake, chronicle, chronicler, llm, journal
        finally:
            await chronicler.close()


async def settled(chronicler):
    await _until(lambda: chronicler._work is None or chronicler._work.done())


@pytest.mark.parametrize("fact", [
    {"fact": "blamed", "truth": False},
    {"fact": "found_out", "npc": "SECRET"},
    {"fact": "npc_intent", "plan": "SECRET"},
    {"fact": "died", "killer": "OTHER", "name": "SECRET"},
    {"fact": "surrender", "status": "offered"},
    {"fact": "feat", "title": "SECRET"},
    {"fact": "challenge", "kind": "no_blood", "people": ["SECRET"]},
    {"fact": "challenge_failed", "kind": "no_blood", "why": "SECRET"},
])
def test_hidden_facts_have_no_hero_view(fact):
    assert hero_view(1, fact) is None


@pytest.mark.parametrize(("fact", "removed"), [
    (crime(witnesses=["SECRET"], place=[1, 2, 3]), {"witnesses", "place"}),
    ({"fact": "hero_yield", "npc": "N", "t": 1, "deadline": 2}, {"t", "deadline"}),
    ({"fact": "yield_broken", "npc": "N", "t": 1}, {"t"}),
    ({"fact": "humiliated", "npc": "N", "t": 1, "witnesses": []}, {"t", "witnesses"}),
    ({"fact": "surrender", "status": "spared", "deadline": 1}, {"deadline"}),
    ({"fact": "initiative", "npc": "N", "seen_by": ["SECRET"]}, {"seen_by"}),
    ({"fact": "bounty_betrayal", "witnesses": [], "witness_names": []},
     {"witnesses", "witness_names"}),
    ({"fact": "quest", "tag": "SECRET", "instance": "SECRET"}, {"tag", "instance"}),
    ({"fact": "claim", "believed": True, "blamed": "N", "blame_crime": "C", "checked": True},
     {"believed", "blamed", "blame_crime", "checked"}),
    ({"fact": "died", "killer": "PLAYER", "npc": "N", "name": "Имя", "monster": False,
      "guild": 8, "waypoint": "TOWER", "tag": "SECRET"}, {"killer", "tag"}),
])
def test_hero_view_removes_private_fields_without_mutating_the_fact(fact, removed):
    assert hero_view(1, fact) == {key: value for key, value in fact.items() if key not in removed}
    assert removed <= fact.keys()


def test_notable_first_repeated_and_unusual_outcomes():
    entries = [(1, crime())]
    assert notable(1, entries[-1][1], entries)
    entries.append((2, crime(id="C2")))
    assert not notable(2, entries[-1][1], entries)
    entries.append((3, crime(id="C3")))
    assert notable(3, entries[-1][1], entries)
    entries.append((4, crime(id="C4", victim="HILDA")))
    assert not notable(4, entries[-1][1], entries)
    ordinary = {"fact": "quest_outcome", "quest": "Q", "how": "done", "pay": "full"}
    assert not notable(5, ordinary, [(5, ordinary)])
    for changes in ({"how": "amended"}, {"how": "dropped"}, {"pay": "none"}):
        unusual = {**ordinary, **changes}
        assert notable(6, unusual, [(5, ordinary), (6, unusual)])
    monster = {"fact": "died", "npc": "MONSTER", "killer": "PLAYER", "monster": True}
    for flag in (False, None):
        human = {"fact": "died", "npc": "HUMAN", "killer": "PLAYER", "monster": flag}
        assert notable(2, human, [(1, monster), (2, human)])
    # The first murder of a peaceful man stays notable after killing a bandit in a fair fight.
    bandit = crime(kind="murder", victim="BRAGO", lawful_fight=True)
    farmer = crime(id="C2", kind="murder")
    assert notable(2, farmer, [(1, bandit), (2, farmer)])
    assert not notable(3, crime(id="C3", kind="murder", victim="HILDA"), [
        (1, bandit), (2, farmer), (3, crime(id="C3", kind="murder", victim="HILDA")),
    ])
    met = {
        "fact": "challenge_met", "kind": "no_blood", "line": 1,
        "challenge": 2, "people": ["LOBART"], "evidence": [3],
    }
    assert hero_view(4, met) == {key: value for key, value in met.items() if key != "challenge"}
    assert notable(5, met, [(4, met), (5, met)])


def test_turns_require_the_related_earlier_deed():
    entries = [(1, {"fact": "lie", "quest": "Q", "npc": "LOBART"})]
    made_good = {"fact": "lie_made_good", "quest": "Q", "npc": "LOBART"}
    # The second made-good is noteworthy because it follows a lie about this quest.
    entries.append((2, {**made_good, "quest": "OTHER"}))
    entries.append((3, made_good))
    assert notable(3, made_good, entries)
    unknown = {**made_good, "quest": "UNKNOWN", "npc": "HILDA"}
    assert not notable(4, unknown, [*entries, (4, unknown)])
    for kind, earlier, reference in [("yield_broken", "hero_yield", "yield"),
                                     ("truce_broken", "truce", "truce")]:
        old = {"fact": earlier, "id": "T", "npc": "LEADER", "members": ["MEMBER"]}
        broken = {"fact": kind, reference: "T", "npc": "MEMBER"}
        history = [(1, old), (2, {**broken, reference: "OTHER"})]
        assert notable(3, broken, [*history, (3, broken)])


@pytest.mark.parametrize(("changes", "previous"), [
    ({"category": "unknown"}, []),
    ({"title": ""}, []),
    ({"title": "Один два три четыре пять"}, []),
    ({"title": "Я" * 41}, []),
    ({"text": ""}, []),
    ({"text": "Я" * 141}, []),
    ({"text": "Первая строка\nВторая строка"}, []),
    ({"title": "Gold"}, []),
    ({"text": "Взял gold."}, []),
    ({"title": "Кошель!"}, []),
    ({"text": "Взял кошель!"}, []),
    ({"title": "Кошель 🗡"}, []),
    ({"text": "Взял кошель 🗡"}, []),
    ({"title": "Поздравляем"}, []),
    ({"text": "Достижение героя."}, []),
    ({"title": "Ачивка"}, []),
    ({"title": "ЧУЖОЙ КОШЕЛЬ"}, [feat(title="Чужой кошель")]),
    ({"evidence": []}, []),
    ({"evidence": [1, 2, 3, 4, 5, 6, 7]}, []),
    ({"evidence": [1, 99]}, []),
    ({"evidence": [2]}, []),
    ({"evidence": [1, 2]}, [feat(trigger=2)]),
    ({"title": "Кошель Хильда"}, []),
    ({"title": "Кошель Хильды"}, []),
    ({"text": "Обокрал Хильду."}, []),
    ({"emblem": ""}, []),
    ({"emblem": "purse " * 16}, []),
    ({"emblem": "Чужой кошель"}, []),
    *[({"emblem": f"An old {word}"}, []) for word in
      ("blood", "gore", "face", "text", "letter", "rune", "FACES", "letters", "runes")],
    ({"text": "Метка ©️."}, []),
])
def test_canon_rejects_each_invalid_field(changes, previous):
    window = [(1, crime()), (2, {"fact": "deal", "npc": "HILDA", "name": "Хильда"})]
    errors = canon(Proposal.of(proposal(**changes)), 1, window, previous)
    assert errors and all(isinstance(error, str) and any("а" <= c.lower() <= "я" for c in error)
                          for error in errors)


@pytest.mark.parametrize(("name", "text"), [
    ("Хильда", "Обокрал Хильду."),
    ("Хильда", "Взял кошель у Хильды."),
    ("Лобарт", "Взял кошель у Лобарта."),
    ("Онар", "Договорился с Онаром."),
    ("Лорд Хаген", "Обокрал лорда Хагена."),
])
def test_canon_accepts_names_from_evidence_and_unchanged_proposal(name, text):
    raw = proposal()
    assert canon(Proposal.of(raw), 1, [(1, crime())], []) == []
    raw["text"] = text
    window = [(1, {"fact": "deal"}), (2, {"fact": "deal", "name": name})]
    assert canon(Proposal.of(raw), 1, window, []) == [
        f"Имя «{name.casefold()}» отсутствует в доказательствах",
    ]
    raw["evidence"] = [1, 2]
    assert canon(Proposal.of(raw), 1, window, []) == []


@pytest.mark.parametrize("changes", [
    {"emblem": "A purse on a wooden surface"},
    {"emblem": "A purse made of textile"},
    {"emblem": "A prune beside a purse"},
    {"text": "Поручение №2."},
    {"text": "Повернул на 90°."},
    {"text": "Метка ©."},
])
def test_canon_accepts_ordinary_words_and_symbols(changes):
    assert canon(Proposal.of(proposal(**changes)), 1, [(1, crime())], []) == []


def test_challenge_canon_accepts_its_supporting_evidence():
    met = {"fact": "challenge_met", "kind": "mercy", "evidence": [1]}
    p = Proposal.of(proposal(evidence=[1, 50]))
    assert canon(p, 50, [(50, met)], [feat(trigger=1)]) == []
    assert canon(p, 50, [(50, {**met, "evidence": [2]})], [])


async def test_complete_path_retries_records_toast_and_journal_then_rolls_back():
    async with world(proposal(title="Кошель!"), proposal()) as (fake, chronicle, chronicler, llm, journal):
        seq = chronicle.record(crime(witnesses=["SECRET"]))
        await settled(chronicler)
        await _until(lambda: any(m["type"] == "feat_toast" for m in fake.received))
        [award] = chronicle.facts("feat")
        assert seq == 1 and award["evidence"] == [seq] and award["trigger"] == seq
        assert len(award["id"]) == 12 and int(award["id"], 16) >= 0
        assert (award["chapter"], award["day"], award["hour"], award["minute"]) == (1, 10, 8, 0)
        assert award["emblem"] == proposal()["emblem"] and award["reason"] == proposal()["reason"]
        [toast] = [m for m in fake.received if m["type"] == "feat_toast"]
        icon = Path(feats_module.__file__).parent / "assets/feats/categories/theft/icon.png"
        assert icon.is_file() and toast["icon"] == str(icon.resolve())
        assert toast["feat"] == award["id"] and toast["title"] == award["title"]
        assert toast["text"] == award["text"]
        assert [{k: v for k, v in entry.items() if k != "t"} for entry in journal] == [{
            "kind": "feat", "id": award["id"], "title": award["title"],
            "text": award["text"], "category": "theft",
        }]
        assert len(llm.calls) == 2 and llm.cache_keys == ["chronicler", "chronicler"]
        assert "Отклонено" in llm.calls[1][1][-1].content
        assert [tool.name for tool in llm.calls[0][2]] == ["write_feat"]
        assert "SECRET" not in llm.calls[0][1][0].content
        assert chronicle.entries("feat")[0][0] > seq
        chronicle.memory.ensure_branch("B", "A", seq)
        link = chronicle.link
        link._dispatch(Session(type="session", branch="B", parent="A", parent_seq=seq, reason="load"))
        assert chronicler.feats == [] and chronicle.facts("feat") == []
        assert len(chronicle.entries()) == 1


@pytest.mark.parametrize(("waypoint", "with_places", "where"), [
    ("TOWER", True, "в башне Ксардаса"),
    ("UNKNOWN", True, None),
    ("TOWER", False, None),
])
async def test_chronicler_brief_describes_victims_and_places(waypoint, with_places, where, caplog):
    caplog.set_level("INFO", logger="gothic_ai.feats")
    places = PlaceBook({
        "points": {"TOWER": 0},
        "places": [{
            "id": "XARDAS", "ru": "башня Ксардаса", "where_ru": "в башне Ксардаса",
            "en": "Xardas's tower", "where_en": "in Xardas's tower",
        }],
    }) if with_places else None
    theft = crime(victim_guild=12, place=[1, 2, 3], waypoint=waypoint)
    visit = {"fact": "visited", "waypoint": waypoint}
    death = {
        "fact": "died", "npc": "LOBART", "name": "Лобарт", "killer": "PLAYER",
        "monster": False, "guild": 8, "waypoint": waypoint,
    }
    async with world(proposal(verdict="none"), history=[theft, visit], places=places) as (
        _, chronicle, chronicler, llm, _,
    ):
        chronicle.record(death)
        await settled(chronicler)
        [(_, messages, _)] = llm.calls
        assert messages[0].content in caplog.text
        lines = messages[0].content.splitlines()
        occasion, *window = [
            json.loads(line[line.index("{"):])
            for line in lines if line.startswith(("Повод:", "#"))
        ]
        assert occasion == window[-1]
        assert occasion["кто"] == "крестьянин" and occasion["guild"] == 8
        assert window[0]["кто жертва"] == "житель окрестностей"
        assert "place" not in window[0]
        for view in [occasion, *window]:
            if where is None:
                assert "где" not in view
            else:
                assert view["где"] == where
        assert [fact for _, fact in chronicle.entries()] == [theft, visit, death]


async def test_picture_arrives_before_toast_without_blocking_the_chronicler(tmp_path):
    artist = FakeArtist(tmp_path)
    async with world(proposal(), artist=artist) as (fake, chronicle, chronicler, _, journal):
        chronicle.record(crime())
        await _until(artist.started.is_set)
        await settled(chronicler)
        assert not artist.finished.is_set()
        assert not any(m["type"] == "feat_toast" for m in fake.received)
        [award] = chronicler.feats
        assert artist.calls == [(award["id"], award["emblem"])]
        artist.release.set()
        await _until(lambda: any(m["type"] == "feat_toast" for m in fake.received))
        [toast] = [m for m in fake.received if m["type"] == "feat_toast"]
        assert toast["icon"] == str(tmp_path / award["id"] / "icon.png")
        assert chronicle.facts("feat_image") == [{
            "fact": "feat_image", "id": award["id"], "hash": "0123456789abcdef", "style": STYLE_VERSION,
        }]
        [entry] = [entry for entry in journal if entry["kind"] == "feat_image"]
        assert entry["id"] == award["id"] and entry["dir"] == str(tmp_path / award["id"])


async def test_picture_timeout_keeps_drawing_and_records_the_late_picture(tmp_path):
    artist = FakeArtist(tmp_path)
    async with world(proposal(), artist=artist, image_wait_s=0) as (
        fake, chronicle, chronicler, _, journal,
    ):
        chronicle.record(crime())
        await _until(artist.started.is_set)
        await _until(lambda: any(m["type"] == "feat_toast" for m in fake.received))
        [award] = chronicler.feats
        [toast] = [m for m in fake.received if m["type"] == "feat_toast"]
        assert toast["icon"] == str(feats_module.CATEGORY_ICONS / "theft/icon.png")
        assert chronicle.facts("feat_image") == [] and not artist.cancelled
        artist.release.set()
        await _until(lambda: bool(chronicle.facts("feat_image")))
        [image] = chronicle.facts("feat_image")
        assert image["id"] == award["id"] and image["hash"] and image["style"] == STYLE_VERSION
        assert any(entry["kind"] == "feat_image" and entry["id"] == award["id"] for entry in journal)
        assert len([m for m in fake.received if m["type"] == "feat_toast"]) == 1


async def test_broken_picture_still_sends_the_category_toast(tmp_path):
    class BrokenArtist(Artist):
        async def draw(self, feat_id: str, emblem: str) -> Picture | None:
            raise RuntimeError("broken")

    async with world(proposal(), artist=BrokenArtist(tmp_path)) as (fake, chronicle, _, _, _):
        chronicle.record(crime())
        await _until(lambda: any(m["type"] == "feat_toast" for m in fake.received))
        [toast] = [m for m in fake.received if m["type"] == "feat_toast"]
        assert toast["icon"] == str(feats_module.CATEGORY_ICONS / "theft/icon.png")
        assert chronicle.facts("feat_image") == []


async def test_failed_picture_uses_the_category_without_an_image_fact(tmp_path):
    artist = FakeArtist(tmp_path, picture=False)
    artist.release.set()
    async with world(proposal(), artist=artist) as (fake, chronicle, _, _, journal):
        chronicle.record(crime())
        await _until(lambda: any(m["type"] == "feat_toast" for m in fake.received))
        [toast] = [m for m in fake.received if m["type"] == "feat_toast"]
        assert toast["icon"] == str(feats_module.CATEGORY_ICONS / "theft/icon.png")
        assert chronicle.facts("feat_image") == []
        assert not any(entry["kind"] == "feat_image" for entry in journal)


@pytest.mark.parametrize("message", [
    Loading(type="loading"), Session(type="session", branch="B", reason="load"),
])
async def test_loading_keeps_the_drawing_without_a_toast_or_image_fact(tmp_path, message):
    artist = FakeArtist(tmp_path)
    async with world(proposal(), artist=artist) as (fake, chronicle, chronicler, _, journal):
        chronicle.record(crime())
        await _until(artist.started.is_set)
        [award] = chronicler.feats
        chronicle.memory.ensure_branch("B", "A", 0)
        chronicle.link._dispatch(message)
        assert not artist.cancelled and not artist.finished.is_set()
        artist.release.set()
        await asyncio.gather(*chronicler._drawings, *chronicler._toasts)
        assert artist.finished.is_set() and not artist.cancelled
        assert (tmp_path / award["id"] / "icon.png").is_file()
        assert not any(m["type"] == "feat_toast" for m in fake.received)
        assert chronicle.facts("feat_image") == []
        assert not any(entry["kind"] == "feat_image" for entry in journal)


async def test_close_cancels_and_joins_the_drawing(tmp_path):
    artist = FakeArtist(tmp_path)
    async with world(proposal(), artist=artist) as (_, chronicle, _, _, _):
        chronicle.record(crime())
        await _until(artist.started.is_set)
    assert artist.cancelled and artist.finished.is_set()


async def test_hidden_truth_never_reaches_the_chronicler():
    async with world(proposal(verdict="none")) as (_, chronicle, chronicler, llm, _):
        for fact in [
            {"fact": "blamed", "name": "СКРЫТОЕ_ОБВИНЕНИЕ"},
            {"fact": "found_out", "name": "СКРЫТОЕ_РАСКРЫТИЕ"},
            {"fact": "npc_intent", "name": "СКРЫТЫЙ_ЗАМЫСЕЛ"},
            {"fact": "died", "killer": "OTHER", "name": "ЧУЖАЯ_ЖЕРТВА"},
        ]:
            chronicle.record(fact)
        assert llm.calls == []
        chronicle.record(crime(witnesses=["ТАЙНЫЙ_СВИДЕТЕЛЬ"]))
        await settled(chronicler)
        assert len(llm.calls) == 1
        text = llm.calls[0][1][0].content
        for hidden in ("СКРЫТОЕ_ОБВИНЕНИЕ", "СКРЫТОЕ_РАСКРЫТИЕ", "СКРЫТЫЙ_ЗАМЫСЕЛ",
                       "ЧУЖАЯ_ЖЕРТВА", "ТАЙНЫЙ_СВИДЕТЕЛЬ"):
            assert hidden not in text
        assert not chronicle.facts("feat")


@pytest.mark.parametrize(("history", "occasion"), [
    ([feat(id=str(i), day=i) for i in range(1, 4)], crime()),
    ([feat(day=10, hour=7, minute=59)], crime()),
    ([feat(evidence=[2])], crime()),
    ([feat(id=str(i), day=i) for i in range(1, 4)],
     {"fact": "challenge_met", "kind": "no_blood", "evidence": [1]}),
])
async def test_budget_discards_without_a_model_call(history, occasion):
    async with world(proposal(), history=history) as (_, chronicle, chronicler, llm, _):
        chronicle.record(occasion)
        await settled(chronicler)
        assert llm.calls == []
        assert len(chronicle.facts("feat")) == len(history)


async def test_challenge_bypasses_daily_budget_and_shows_old_evidence():
    history = [
        feat(day=10, hour=7, minute=59, trigger=2),
        {"fact": "quest_outcome", "quest": "Q", "how": "done", "pay": "full"},
        *[{"fact": "visited", "waypoint": "TOWER"} for _ in range(41)],
    ]
    trigger = len(history) + 1
    async with world(proposal(evidence=[2, trigger]), history=history) as (
        _, chronicle, chronicler, llm, _,
    ):
        seq = chronicle.record({
            "fact": "challenge_met", "challenge": 1, "kind": "no_blood",
            "line": 1, "people": ["LOBART"], "evidence": [2],
        })
        await settled(chronicler)
        assert seq == trigger and len(llm.calls) == 1
        assert len(chronicle.facts("feat")) == 2
        assert "#2 quest_outcome " in llm.calls[0][1][0].content
        assert chronicle.facts("feat")[-1]["evidence"] == [2, trigger]


@pytest.mark.parametrize("answer", [proposal(title="Кошель!"), proposal(verdict="none")])
async def test_rejected_or_declined_occasion_does_not_record_a_feat(answer):
    async with world(answer) as (_, chronicle, chronicler, llm, journal):
        seq = chronicle.record(crime())
        await settled(chronicler)
        assert len(llm.calls) == (1 if answer["verdict"] == "none" else 2)
        assert chronicle.facts("feat") == []
        assert [{key: value for key, value in entry.items() if key != "t"}
                for entry in journal if entry["kind"] == "feat_declined"] == ([{
            "kind": "feat_declined", "trigger": seq, "reason": answer["reason"],
        }] if answer["verdict"] == "none" else [])


async def test_model_error_loses_the_occasion(caplog):
    class FailedBackend(FakeBackend):
        async def stream(self, **kwargs):
            raise LLMError("test unavailable")
            yield  # pragma: no cover

    async with world(backend=FailedBackend()) as (_, chronicle, chronicler, _, _):
        chronicle.record(crime())
        await settled(chronicler)
        assert chronicle.facts("feat") == []
        assert "test unavailable" in caplog.text


async def test_loading_cancels_the_wait_and_an_inflight_model():
    for gather_s, model_delay in [(60, 0), (0, 60)]:
        backend = FakeBackend(
            script=[FakeStep(tool_calls=[ToolCall("write_feat", proposal())])],
            first=model_delay, delay=0,
        )
        async with world(backend=backend, gather_s=gather_s) as (_, chronicle, chronicler, llm, _):
            chronicle.record(crime())
            if model_delay:
                await _until(lambda: bool(llm.calls))
            chronicle.link._dispatch(Loading(type="loading"))
            chronicle.link._dispatch(Session(type="session", branch="B", reason="load"))
            await settled(chronicler)
            assert chronicle.facts("feat") == []
            assert len(llm.calls) == (1 if model_delay else 0)


async def test_while_busy_only_the_latest_occasion_waits():
    backend = FakeBackend(
        script=[FakeStep(tool_calls=[ToolCall("write_feat", proposal(verdict="none"))])],
        first=0.05, delay=0,
    )
    async with world(backend=backend) as (_, chronicle, chronicler, llm, _):
        chronicle.record(crime())
        await _until(lambda: len(llm.calls) == 1)
        chronicle.record({"fact": "hero_yield", "npc": "N", "name": "Первый"})
        latest = chronicle.record({"fact": "truce", "npc": "N", "name": "Последний"})
        await _until(lambda: len(llm.calls) == 2)
        await settled(chronicler)
        assert len(llm.calls) == 2
        text = llm.calls[1][1][0].content
        assert f"#{latest}" in text and "truce" in text
        assert not chronicle.facts("feat")


async def test_story_state_timeout_drops_only_the_active_occasion(monkeypatch, caplog):
    async with world(proposal(verdict="none")) as (_, chronicle, chronicler, llm, _):
        started, release = asyncio.Event(), asyncio.Event()
        story_state = chronicle.link.story_state

        async def timeout_once(**kwargs):
            if not started.is_set():
                started.set()
                await release.wait()
                raise TimeoutError
            return await story_state(**kwargs)

        monkeypatch.setattr(chronicle.link, "story_state", timeout_once)
        first = chronicle.record(crime())
        work = chronicler._work
        await _until(started.is_set)
        latest = chronicle.record({"fact": "truce", "npc": "N", "name": "Последний"})
        release.set()
        await settled(chronicler)
        assert work.done() and work.exception() is None
        assert chronicler._pending is None
        assert len(llm.calls) == 1
        assert llm.calls[0][1][0].content.startswith(f"Повод: #{latest} truce ")
        assert f"#{first}" in caplog.text and "TimeoutError" in caplog.text
