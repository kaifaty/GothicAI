"""A chronicler sees the hero's facts and spends only the current save branch's budget."""

import asyncio
import json
from contextlib import asynccontextmanager
from pathlib import Path
from unittest.mock import patch

import pytest

from gothic_ai import feats as feats_module
from gothic_ai.chronicle import Chronicle
from gothic_ai.feat_art import STYLE_VERSION, Artist, Picture
from gothic_ai.feats import Feats, Proposal, canon, epilogue_canon, hero_view, notable
from gothic_ai.journal import Journal
from gothic_ai.llm import LLMError, ToolCall
from gothic_ai.llm.fake import FakeBackend, FakeStep
from gothic_ai.memory import MemoryStore
from gothic_ai.places import PlaceBook
from gothic_ai.protocol import Loading, NpcRoster, Session
from gothic_ai.quests import ATTACK_CM
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
    fame=True, hero_distance=lambda npc: None,
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
            artist=artist, image_wait_s=image_wait_s, places=places, fame=fame,
            hero_distance=hero_distance,
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
    {"fact": "feat_fame", "feat": "F1", "title": "SECRET"},
    {"fact": "rumor", "kind": "fame", "title": "SECRET"},
    {"fact": "feat_epilogue", "feat": "old", "text": "SECRET"},
    {"fact": "feat_epilogue_shown", "feat": "old"},
    {"fact": "challenge", "kind": "no_blood", "people": ["SECRET"]},
    {"fact": "challenge_hint", "kind": "no_blood", "npc": "SECRET", "witnesses": ["SECRET"]},
    {"fact": "challenge_dropped", "kind": "no_blood", "line": 1, "why": "no_fight", "people": ["SECRET"]},
    {"fact": "challenge_failed", "kind": "no_blood", "why": "SECRET"},
])
def test_hidden_facts_have_no_hero_view(fact):
    assert hero_view(1, fact) is None
    assert not notable(1, fact, [(1, fact)])


@pytest.mark.parametrize(("history", "occasion", "evidence", "witnesses", "fame"), [
    ([], crime(witnesses=["PLAYER", "MALETH", "MALETH"]), [1], ["MALETH"], True),
    ([], {"fact": "quest_outcome", "npc": "LOBART", "how": "amended", "pay": "none"},
     [1], ["LOBART"], True),
    ([{"fact": "quest_outcome", "npc": "LOBART", "how": "amended", "pay": "none"},
      {"fact": "quest_given", "npc": "XARDAS"}],
     {"fact": "challenge_met", "kind": "no_reward", "evidence": [1, 2]},
     [3], ["LOBART"], True),
    ([], crime(), [1], [], True),
    ([crime(witnesses=["MALETH"])],
     {"fact": "died", "npc": "LOBART", "killer": "PLAYER"}, [1, 2], ["MALETH"], True),
    ([], crime(kind="attack"), [1], ["LOBART"], True),
    ([{"fact": "lie", "npc": "LOBART", "quest": "WOLVES"}],
     crime(witnesses=["LOBART"]), [1, 2], [], True),
    ([{"fact": "claim", "npc": "LOBART", "quest": "WOLVES", "truth": False}], crime(witnesses=["LOBART"]),
     [1, 2], [], True),
    ([crime(witnesses=["LOBART"])],
     {"fact": "challenge_met", "kind": "secret_kept", "evidence": [1], "challenge": 8},
     [1, 2], [], True),
    ([{"fact": "died", "npc": "LOBART", "killer": "OTHER"}],
     crime(witnesses=["LOBART"]), [2], [], True),
    ([], crime(witnesses=["LOBART"]), [1], [], False),
    ([{"fact": "crime_seen", "crime": "C1", "npc": "MALETH"},
      {"fact": "crime_seen", "crime": "OTHER", "npc": "LOBART"}],
     crime(), [3], ["MALETH"], True),
    ([{"fact": "testimony", "witness": "MALETH", "npc": "LOBART",
       "crime_saw": ["C1"], "crime_told": ["C1"]},
      {"fact": "testimony", "witness": "OTHER", "npc": "ANDRE", "crime_told": ["C2"]}],
     crime(), [3], ["LOBART", "MALETH"], True),
    ([{"fact": "initiative", "npc": "LOBART", "seen_by": ["PLAYER", "MALETH"]}],
     crime(), [1, 2], [], True),
    ([{"fact": "vanilla_quest", "quest": "Q1", "giver": "XARDAS"},
      {"fact": "quest_given", "quest": "Q1", "npc": "XARDAS"}],
     {"fact": "died", "npc": "BANDIT", "killer": "PLAYER"}, [1, 2, 3], [], True),
])
async def test_fame_uses_occasion_and_living_crime_witnesses(history, occasion, evidence, witnesses, fame):
    async with world(proposal(evidence=evidence), history=history, fame=fame) as (
        _, chronicle, chronicler, _, _,
    ):
        chronicle.record(occasion)
        await settled(chronicler)
        [award] = chronicle.facts("feat")
        expected = [{
            "fact": "feat_fame", "feat": award["id"], "title": award["title"],
            "text": award["text"], "witnesses": witnesses, "day": award["day"],
        }] if witnesses else []
        assert chronicle.facts("feat_fame") == expected
        if any(fact["fact"] in {"lie", "claim"} for fact in history):
            chronicle.record(revelation(npc="LOBART", name="Лобарт"))
            chronicler.hero_talk("LOBART")
            await _until(lambda: bool(chronicle.facts("feat_epilogue_shown")))
            assert not chronicle.facts("feat_fame")


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
            {"fact": "feat_epilogue", "feat": "old", "text": "СКРЫТЫЙ_ЭПИЛОГ"},
            {"fact": "feat_epilogue_shown", "feat": "СКРЫТЫЙ_ПОКАЗ"},
            {"fact": "died", "killer": "OTHER", "name": "ЧУЖАЯ_ЖЕРТВА"},
        ]:
            chronicle.record(fact)
        assert llm.calls == []
        chronicle.record(crime(witnesses=["ТАЙНЫЙ_СВИДЕТЕЛЬ"]))
        await settled(chronicler)
        assert len(llm.calls) == 1
        text = llm.calls[0][1][0].content
        for hidden in ("СКРЫТОЕ_ОБВИНЕНИЕ", "СКРЫТОЕ_РАСКРЫТИЕ", "СКРЫТЫЙ_ЗАМЫСЕЛ",
                       "ЧУЖАЯ_ЖЕРТВА", "ТАЙНЫЙ_СВИДЕТЕЛЬ", "СКРЫТЫЙ_ЭПИЛОГ", "СКРЫТЫЙ_ПОКАЗ"):
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


@pytest.mark.parametrize("message", [
    Loading(type="loading"), Session(type="session", branch="B", reason="load"),
])
async def test_loading_cancels_the_wait_and_an_inflight_model(message):
    for gather_s, model_delay in [(60, 0), (0, 60)]:
        backend = FakeBackend(
            script=[FakeStep(tool_calls=[ToolCall("write_feat", proposal())])],
            first=model_delay, delay=0,
        )
        async with world(backend=backend, gather_s=gather_s) as (_, chronicle, chronicler, llm, _):
            chronicle.record(crime())
            if model_delay:
                await _until(lambda: bool(llm.calls))
            chronicle.record({"fact": "challenge_met", "kind": "no_blood", "evidence": [1]})
            assert len(chronicler._pending_challenges) == 1
            chronicle.link._dispatch(message)
            await settled(chronicler)
            assert not chronicler._pending_challenges
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


async def test_pending_challenges_survive_newer_ordinary_occasions(monkeypatch):
    backend = FakeBackend(
        script=[FakeStep(tool_calls=[ToolCall("write_feat", proposal(verdict="none"))])], delay=0,
    )
    started, release = asyncio.Event(), asyncio.Event()
    stream = backend.stream

    async def wait_first(**kwargs):
        if not started.is_set():
            started.set()
            await release.wait()
        async for event in stream(**kwargs):
            yield event

    monkeypatch.setattr(backend, "stream", wait_first)
    async with world(backend=backend) as (_, chronicle, chronicler, llm, _):
        chronicle.record(crime())
        await _until(started.is_set)
        first = chronicle.record({"fact": "challenge_met", "kind": "no_blood", "evidence": [1]})
        chronicle.record({"fact": "hero_yield", "npc": "N"})
        second = chronicle.record({"fact": "challenge_met", "kind": "mercy", "evidence": [1]})
        chronicle.record({"fact": "truce", "npc": "N"})
        for seq, title in [(first, "Тихий путь"), (second, "Мягкая рука")]:
            backend.script.append(FakeStep(tool_calls=[ToolCall(
                "write_feat", proposal(title=title, text="Завершил дело.", evidence=[seq]),
            )]))
        release.set()
        await settled(chronicler)
        assert [fact["trigger"] for fact in chronicle.facts("feat")] == [first, second]
        assert len(llm.calls) == 3
        for call, seq in zip(llm.calls[1:], [first, second], strict=True):
            assert call[1][0].content.startswith(f"Повод: #{seq} challenge_met ")
        assert not chronicler._pending_challenges
        assert chronicler._pending is None


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


def lie(**changes):
    return {"fact": "lie", "quest": "WOLVES", "npc": "LOTAR", "name": "Лотар", **changes}


def revelation(**changes):
    return {
        "fact": "found_out", "quest": "WOLVES", "npc": "LOTAR", "name": "Лотар",
        "how": "нашёл волков живыми", "day": 10, **changes,
    }


@pytest.mark.parametrize(("kind", "evidence_kind", "npc", "name"), [
    ("found_out", "lie", "LOTAR", "Лотар"), ("caught", "claim", "HILDA", "Хильда"),
])
async def test_epilogue_matches_a_lie_in_evidence_and_stays_hidden_far_away(kind, evidence_kind, npc, name):
    history = [lie(fact=evidence_kind, npc=npc, name=name), feat(evidence=[1], category="deceit")]
    async with world(history=history, hero_distance=lambda npc: ATTACK_CM + 1) as (
        fake, chronicle, chronicler, llm, journal,
    ):
        trigger = chronicle.record(revelation(fact=kind, npc=npc, name=name))
        await _until(lambda: bool(chronicle.facts("feat_epilogue")))
        [epilogue] = chronicle.facts("feat_epilogue")
        assert epilogue == {
            "fact": "feat_epilogue", "feat": "old", "text": "Позже обман раскрылся.",
            "trigger": trigger, "npc": npc, "name": name, "quest": "WOLVES", "day": 10,
        }
        assert chronicler.feats == [history[1]] and not llm.calls
        chronicler.hero_talk("STRANGER")
        chronicle.link._dispatch(NpcRoster(type="npc_roster", t=1, npcs=[]))
        await asyncio.sleep(0)
        assert not chronicle.facts("feat_epilogue_shown")
        assert not any(msg["type"] == "feat_toast" for msg in fake.received)
        assert not any(entry["kind"] == "feat_epilogue" for entry in journal)


@pytest.mark.parametrize("evidence", [crime(), lie(quest="OTHER")])
async def test_epilogue_requires_matching_lie_evidence(evidence):
    async with world(history=[evidence, feat(evidence=[1], category="deceit")]) as (
        _, chronicle, chronicler, llm, _,
    ):
        chronicle.record(revelation())
        await settled(chronicler)
        await asyncio.sleep(0)
        assert not chronicle.facts("feat_epilogue") and not llm.calls


async def test_only_one_epilogue_per_feat_even_for_another_lie():
    history = [lie(), lie(quest="SHEEP"), feat(evidence=[1, 2], category="deceit")]
    async with world(history=history) as (_, chronicle, _, _, _):
        trigger = chronicle.record(revelation())
        await _until(lambda: bool(chronicle.facts("feat_epilogue")))
        chronicle.record(revelation(fact="caught"))
        chronicle.record(revelation(quest="SHEEP"))
        await asyncio.sleep(0)
        [epilogue] = chronicle.facts("feat_epilogue")
        assert epilogue["trigger"] == trigger


@pytest.mark.parametrize("learning", ["near", "talk"])
async def test_revelation_before_feat_is_recovered_when_the_feat_is_written(learning):
    distances = {"LOTAR": ATTACK_CM - 1 if learning == "near" else ATTACK_CM + 1}
    async with world(history=[lie()], hero_distance=distances.get) as (_, chronicle, chronicler, _, _):
        chronicle.record(revelation())
        if learning == "talk":
            chronicler.hero_talk("LOTAR")
        distances["LOTAR"] = ATTACK_CM + 1
        chronicle.record(feat(evidence=[1], category="deceit"))
        await _until(lambda: bool(chronicle.facts("feat_epilogue")))
        assert chronicle.facts("feat_epilogue")[0]["trigger"] == 2
        await _until(lambda: bool(chronicle.facts("feat_epilogue_shown")))


@pytest.mark.parametrize("text", [
    "Лотар нашёл волков живыми.", "Позже Лотар нашёл wolves.", "Позже Хильда узнала правду.",
    "Позже Лотар узнал правду!", "Позже Лотар узнал правду 🐺.",
    "Позже записали достижение.", "Позже " + "я" * 140,
])
def test_epilogue_canon_rejects_invalid_text(text):
    evidence = [(1, lie()), (3, revelation())]
    entries = [*evidence, (4, {"fact": "deal", "name": "Хильда"})]
    assert epilogue_canon(text, evidence, entries)
    assert not epilogue_canon("Позже Лотар нашёл волков живыми.", evidence, entries)


def test_epilogue_canon_uses_third_person():
    evidence = [(1, lie(npc="MATTEO", name="Маттео")), (3, revelation(npc="MATTEO", name="Маттео"))]
    for pronoun in ("мне", "МНЕ"):
        assert epilogue_canon(f"Позже Маттео открыл ящик при {pronoun}.", evidence, evidence) == [
            "Эпилог пишется в третьем лице",
        ]
    assert not epilogue_canon("Позже Маттео открыл ящик при герое.", evidence, evidence)


async def test_epilogue_model_gets_full_disclosure_and_retries_before_fallback():
    backend = FakeBackend(tool_calls=[ToolCall("write_epilogue", {
        "text": "Лотар нашёл волков.", "reason": "Раскрытие лжи.",
    })], delay=0)
    backend.name = "test"
    disclosure = revelation(blamed="B1", witness_name="Грег", hidden_detail="полная правда")
    history = [lie(), feat(evidence=[1], category="deceit")]
    async with world(backend=backend, history=history) as (_, chronicle, _, llm, _):
        chronicle.record(disclosure)
        await _until(lambda: bool(chronicle.facts("feat_epilogue")))
        [epilogue] = chronicle.facts("feat_epilogue")
        assert epilogue["text"] == "Позже обман раскрылся."
        assert len(llm.calls) == 2
        instructions, messages, tools = llm.calls[0]
        assert instructions == (
            "Допиши к записи летописи одну строку эпилога: что стало известно позже. "
            "Пиши просто, в духе мира, в прошедшем времени, начни со слова «Позже». "
            "Пиши со стороны летописи: о людях в третьем лице, героя называй «герой» или не называй; "
            "поля фактов написаны со стороны NPC, их «ты» и «тебе» — это герой. "
            "Не пересказывай саму запись и не придумывай событий сверх показанных фактов."
        )
        assert [tool.name for tool in tools] == ["write_epilogue"]
        brief = messages[0].content
        assert history[1]["title"] in brief and history[1]["text"] in brief
        for key, value in disclosure.items():
            assert key in brief and str(value) in brief
        assert "Отклонено" in llm.calls[1][1][-1].content


async def test_epilogue_model_can_correct_its_first_answer():
    backend = FakeBackend(script=[
        FakeStep(tool_calls=[ToolCall("write_epilogue", {"text": "Неверно.", "reason": ""})]),
        FakeStep(tool_calls=[ToolCall("write_epilogue", {
            "text": "Позже Лотар нашёл волков живыми.", "reason": "Сам увидел волков.",
        })]),
    ], delay=0)
    backend.name = "test"
    async with world(backend=backend, history=[lie(), feat(evidence=[1], category="deceit")]) as (
        _, chronicle, _, _, _,
    ):
        chronicle.record(revelation())
        await _until(lambda: bool(chronicle.facts("feat_epilogue")))
        assert chronicle.facts("feat_epilogue")[0]["text"] == "Позже Лотар нашёл волков живыми."
        assert len(backend.calls) == 2


async def test_epilogue_retry_yields_to_an_ordinary_feat(monkeypatch):
    backend = FakeBackend(script=[
        FakeStep(tool_calls=[ToolCall("write_epilogue", {"text": "Неверно.", "reason": ""})]),
        FakeStep(tool_calls=[ToolCall("write_feat", proposal(verdict="none"))]),
        FakeStep(tool_calls=[ToolCall("write_epilogue", {
            "text": "Позже Лотар нашёл волков живыми.", "reason": "Раскрытие лжи.",
        })]),
    ], delay=0)
    backend.name = "test"
    started, release, model_ready = asyncio.Event(), asyncio.Event(), asyncio.Event()
    stream = backend.stream

    async def hold_first_call(**kwargs):
        if not started.is_set():
            started.set()
            await release.wait()
        async for event in stream(**kwargs):
            yield event

    monkeypatch.setattr(backend, "stream", hold_first_call)
    async with world(backend=backend, history=[lie(), feat(evidence=[1], category="deceit")]) as (
        _, chronicle, chronicler, _, _,
    ):
        story_state = chronicle.link.story_state

        async def ready(**kwargs):
            state = await story_state(**kwargs)
            model_ready.set()
            return state

        monkeypatch.setattr(chronicle.link, "story_state", ready)
        chronicle.record(revelation())
        await _until(started.is_set)
        chronicle.record(crime())
        await _until(model_ready.is_set)
        release.set()
        await _until(lambda: bool(chronicle.facts("feat_epilogue")))
        await settled(chronicler)
        assert [tools[0].name for _, _, tools in backend.calls] == [
            "write_epilogue", "write_feat", "write_epilogue",
        ]


@pytest.mark.parametrize("already_shown", [False, True])
async def test_roster_does_not_read_long_history_without_pending_epilogues(already_shown):
    history = [
        lie(), feat(evidence=[1], category="deceit"), revelation(),
        {"fact": "feat_epilogue", "feat": "old", "text": "Позже обман раскрылся.",
         "trigger": 3, "npc": "LOTAR", "name": "Лотар", "quest": "WOLVES", "day": 10},
        {"fact": "feat_epilogue_shown", "feat": "old"},
    ] if already_shown else []
    history += [revelation(quest="OTHER", npc="STRANGER", name="Чужой")]
    history += [{"fact": "npc_intent", "npc": "STRANGER", "plan": str(i)} for i in range(5000)]
    async with world(history=history) as (_, chronicle, chronicler, _, _):
        with (
            patch.object(chronicle, "entries", wraps=chronicle.entries) as entries,
            patch.object(chronicle, "facts", wraps=chronicle.facts) as facts,
            patch.object(chronicler, "_near", wraps=chronicler._near) as near,
        ):
            for tick in range(2):
                chronicle.link._dispatch(NpcRoster(type="npc_roster", t=tick, npcs=[]))
            assert entries.call_count == facts.call_count == near.call_count == 0


@pytest.mark.parametrize(("near_at_reveal", "with_picture"), [(True, False), (False, True)])
async def test_epilogue_toast_when_near_at_disclosure_or_on_later_approach(
    tmp_path, monkeypatch, near_at_reveal, with_picture,
):
    distances = {"LOTAR": ATTACK_CM - 1 if near_at_reveal else ATTACK_CM + 1}
    history = [lie(), feat(evidence=[1], category="deceit")]
    if not near_at_reveal:
        history += [{"fact": "npc_intent", "plan": str(i)} for i in range(5000)]
    monkeypatch.setattr(feats_module, "DEFAULT_ROOT", tmp_path)
    icon = feats_module.CATEGORY_ICONS / "deceit/icon.png"
    if with_picture:
        icon = tmp_path / "old/icon.png"
        icon.parent.mkdir()
        icon.write_bytes(b"icon")
        history.append({"fact": "feat_image", "id": "old", "hash": "image", "style": STYLE_VERSION})
    async with world(history=history, hero_distance=distances.get) as (
        fake, chronicle, chronicler, _, journal,
    ):
        chronicle.record(revelation())
        await _until(lambda: bool(chronicle.facts("feat_epilogue")))
        if not near_at_reveal:
            assert not chronicle.facts("feat_epilogue_shown")
            chronicle.record(revelation(quest="OTHER", npc="STRANGER", name="Чужой"))
            distances["LOTAR"] = ATTACK_CM - 1
            with (
                patch.object(chronicle, "entries", wraps=chronicle.entries) as entries,
                patch.object(chronicle, "facts", wraps=chronicle.facts) as facts,
                patch.object(chronicler, "_near", wraps=chronicler._near) as near,
            ):
                chronicle.link._dispatch(NpcRoster(type="npc_roster", t=1, npcs=[]))
                kinds = [call.args[0] if call.args else call.kwargs.get("kind")
                         for call in [*entries.call_args_list, *facts.call_args_list]]
                assert None not in kinds and "rumor" not in kinds
                assert [call.args for call in near.call_args_list] == [("LOTAR",)]
        await _until(lambda: bool(chronicle.facts("feat_epilogue_shown")))
        await _until(lambda: any(msg["type"] == "feat_toast" for msg in fake.received))
        [toast] = [msg for msg in fake.received if msg["type"] == "feat_toast"]
        assert toast["feat"] == "old" and toast["title"] == history[1]["title"]
        assert toast["text"] == "Позже обман раскрылся."
        assert toast["icon"] == str(icon)
        assert chronicle.facts("feat_epilogue_shown") == [{"fact": "feat_epilogue_shown", "feat": "old"}]
        assert [{k: v for k, v in entry.items() if k != "t"} for entry in journal] == [{
            "kind": "feat_epilogue", "feat": "old", "title": history[1]["title"],
            "text": "Позже обман раскрылся.",
        }]
        chronicler.hero_talk("LOTAR")
        chronicle.link._dispatch(NpcRoster(type="npc_roster", t=2, npcs=[]))
        await asyncio.sleep(0)
        assert len([msg for msg in fake.received if msg["type"] == "feat_toast"]) == 1


@pytest.mark.parametrize(("kind", "partner"), [("found_out", "JORA"), ("caught", "JORA"), (None, "LOTAR")])
async def test_talking_to_discoverer_or_rumor_recipient_shows_epilogue(kind, partner):
    history = [lie(), feat(evidence=[1], category="deceit")]
    async with world(history=history) as (fake, chronicle, chronicler, _, _):
        chronicle.record(revelation())
        await _until(lambda: bool(chronicle.facts("feat_epilogue")))
        if kind is not None:
            chronicle.record({"fact": "rumor", "quest": "OTHER", "kind": kind, "to": partner})
            chronicler.hero_talk(partner)
            await asyncio.sleep(0)
            assert not chronicle.facts("feat_epilogue_shown")
            chronicle.record({"fact": "rumor", "quest": "WOLVES", "kind": "lie", "to": partner})
            chronicler.hero_talk(partner)
            await asyncio.sleep(0)
            assert not chronicle.facts("feat_epilogue_shown")
            chronicle.record({"fact": "rumor", "quest": "WOLVES", "kind": kind, "to": partner})
        chronicler.hero_talk(partner)
        await _until(lambda: bool(chronicle.facts("feat_epilogue_shown")))
        await _until(lambda: any(msg["type"] == "feat_toast" for msg in fake.received))


async def test_load_before_show_can_show_again_but_load_after_show_does_not_repeat():
    history = [lie(), feat(evidence=[1], category="deceit")]
    async with world(history=history) as (fake, chronicle, chronicler, _, _):
        chronicle.record(revelation())
        await _until(lambda: bool(chronicle.facts("feat_epilogue")))
        [(before_show, _)] = chronicle.entries("feat_epilogue")
        chronicler.hero_talk("LOTAR")
        await _until(lambda: bool(chronicle.facts("feat_epilogue_shown")))
        await _until(lambda: len([msg for msg in fake.received if msg["type"] == "feat_toast"]) == 1)
        chronicle.link._dispatch(Session(
            type="session", branch="B", parent="A", parent_seq=before_show, reason="load",
        ))
        assert not chronicle.facts("feat_epilogue_shown")
        chronicle.record(revelation(fact="caught"))
        await asyncio.sleep(0)
        assert len(chronicle.facts("feat_epilogue")) == 1
        assert not chronicle.facts("feat_epilogue_shown")
        chronicler.hero_talk("LOTAR")
        await _until(lambda: bool(chronicle.facts("feat_epilogue_shown")))
        await _until(lambda: len([msg for msg in fake.received if msg["type"] == "feat_toast"]) == 2)
        [(after_show, _)] = chronicle.entries("feat_epilogue_shown")
        chronicle.link._dispatch(Session(
            type="session", branch="C", parent="B", parent_seq=after_show, reason="load",
        ))
        chronicler.hero_talk("LOTAR")
        chronicle.record(revelation())
        await asyncio.sleep(0)
        assert len([msg for msg in fake.received if msg["type"] == "feat_toast"]) == 2
        assert len(chronicle.facts("feat_epilogue_shown")) == 1
        chronicle.link._dispatch(Session(
            type="session", branch="D", parent="A", parent_seq=2, reason="load",
        ))
        assert not chronicle.facts("feat_epilogue")
        chronicle.record(revelation(fact="caught"))
        chronicler.hero_talk("LOTAR")
        await _until(lambda: bool(chronicle.facts("feat_epilogue_shown")))
        await _until(lambda: len([msg for msg in fake.received if msg["type"] == "feat_toast"]) == 3)
