from collections import defaultdict
from dataclasses import replace
from types import SimpleNamespace

from gothic_ai.people_knowledge import SOURCES, PeopleKnowledge
from gothic_ai.social import DAY, Rumor, Rumors, Social
from tests.fake_game import FakeGame, connected
from tests.test_cover import SAID, VOUCH, CoverWorld
from tests.test_people_knowledge import HILDA, LOBART, VINO, _exposed

ANDRE, VATRAS = "MIL_311_ANDRE", "VLK_439_VATRAS"


def _rumors(facts):
    names = {LOBART: "Лобарт", HILDA: "Хильда", VINO: "Вино", ANDRE: "Андре", VATRAS: "Ватрас"}
    social = Social([{"npc": npc, "name": name} for npc, name in names.items()])
    for a, b in [(LOBART, HILDA), (LOBART, VINO), (VINO, ANDRE), (ANDRE, VATRAS)]:
        social.tie(a, b).meets = [(0, DAY)]
    r = Rumors.__new__(Rumors)
    r.social, r.dead, r.rumors, r.tells = social, set(), {}, defaultdict(int)
    r.chronicle = SimpleNamespace(
        record=facts.append, facts=lambda kind=None: [f for f in facts if kind is None or f["fact"] == kind],
    )
    r.remembered = []
    r.dialogue = SimpleNamespace(remember=lambda *args: r.remembered.append(args))
    r.gate = lambda *args: True
    r.clock = DAY + 600
    r._load()
    return r


def test_exposure_seeds_cover_and_spreads_two_hops_subject_cannot_retell():
    facts = _exposed()
    desk = _rumors(facts)
    [r] = desk.rumors.values()
    assert r.kind == "cover" and r.subject == HILDA and r.subject_name == "Хильда"
    assert r.known == {LOBART: 0} and r.hops == 2
    assert r.told("Лобарт") == (
        "Слух (со слов: Лобарт): Хильда покрывал(а) героя и лгал(а) человеку по имени Лобарт"
    )
    assert desk.spread(DAY + 600, DAY + 601) == 3
    assert r.known == {LOBART: 0, HILDA: 1, VINO: 1, ANDRE: 2}
    assert VATRAS not in r.known
    assert not r.tells(HILDA, 1)
    assert not desk.can_tell(r, HILDA, VATRAS)
    assert desk.tell(r, HILDA, VATRAS) == "no"
    rumors = [f for f in facts if f["fact"] == "rumor"]
    assert {f["subject"] for f in rumors} == {HILDA}
    assert {f["subject_name"] for f in rumors} == {"Хильда"}
    assert all(f["from"] != HILDA for f in rumors)
    assert all("Хильда" in args[-1] for args in desk.remembered)


def test_keys_separate_hero_and_npc_and_old_rumor_defaults_to_hero():
    hero = Rumor("Q1", "caught", LOBART, "Лобарт", "не брал")
    npc = Rumor("Q1", "caught", LOBART, "Лобарт", "не брал", subject=HILDA, subject_name="Хильда")
    assert hero.key == f"caught:{LOBART}:Q1" and hero.key != npc.key
    desk = _rumors([])
    desk.rumors = {hero.key: hero, npc.key: npc}
    old = {"fact": "rumor", "quest": "Q1", "kind": "caught", "about": LOBART, "to": VINO, "hops": 1}
    desk._on_fact(old)
    assert hero.known == {VINO: 1} and npc.known == {}
    desk._on_fact({**old, "subject": HILDA, "to": ANDRE})
    assert hero.known == {VINO: 1} and npc.known == {ANDRE: 1}
    assert "Хильда обманул" in npc.description()


def test_load_reconstructs_cover_knowledge_and_counts_without_duplicate_facts():
    facts = _exposed()
    desk = _rumors(facts)
    desk.spread(DAY + 600, DAY + 601)
    [r] = desk.rumors.values()
    before = (dict(r.known), dict(r.since), dict(desk.tells), len(facts))
    knowledge = PeopleKnowledge(desk.chronicle).of(VINO)
    for _ in range(2):
        desk._load()
        [r] = desk.rumors.values()
        assert (r.known, r.since, dict(desk.tells), len(facts)) == before
        assert PeopleKnowledge(desk.chronicle).of(VINO) == knowledge
        assert desk.spread(DAY + 601, DAY + 602) == 0
    facts.clear()
    desk._load()
    assert not desk.rumors and PeopleKnowledge(desk.chronicle).of(VINO) == []


def test_source_registry_also_drives_rumor_seed_and_words(monkeypatch):
    monkeypatch.setitem(
        SOURCES, "another_discovery",
        replace(SOURCES["exposed"], kind="another_kind", text="{subject_name}: новое знание"),
    )
    facts = _exposed()
    facts[-1]["fact"] = "another_discovery"
    [rumor] = _rumors(facts).rumors.values()
    assert rumor.kind == "another_kind" and rumor.subject == HILDA
    assert rumor.told("Лобарт") == "Слух (со слов: Лобарт): Хильда: новое знание"


async def test_no_cover_has_neither_exposure_nor_cover_rumor():
    fake = FakeGame(branch="A")
    async with connected(fake) as link:
        world = CoverWorld(link, fake, cover=False)
        rumors = Rumors(link, world.dialogue, world.chronicle, crimes=world.crimes)
        world.saw()
        world.covered(day=1, vouch=VOUCH)
        await world.testify()
        world.chronicle.record({
            "fact": "caught", "quest": "proof", "npc": LOBART, "title": SAID,
            "said": True, "day": 1,
        })
        assert world.chronicle.facts("exposed") == []
        assert all(r.kind != "cover" for r in rumors.rumors.values())
        assert PeopleKnowledge(world.chronicle).context(LOBART) is None
