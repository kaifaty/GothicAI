from dataclasses import replace
from types import SimpleNamespace

from gothic_ai.chronicle import Fact
from gothic_ai.people_knowledge import SOURCES, PeopleKnowledge, rumor_subject, source_event, source_events

LOBART, HILDA, VINO = "BAU_950_LOBART", "BAU_951_HILDA", "BAU_952_VINO"


def _exposed(subject=HILDA, name="Хильда", key="T1", **extra) -> list[Fact]:
    return [
        {
            "fact": "testimony", "id": key, "npc": LOBART, "name": "Лобарт",
            "witness": subject, "witness_name": name, "minute": 1500,
        },
        {"fact": "exposed", "npc": LOBART, "liar": subject, "testimony": key, **extra},
    ]


def _rumor(subject=HILDA, name="Хильда", key="T1", **extra) -> Fact:
    return {
        "fact": "rumor", "kind": "cover", "subject": subject, "subject_name": name,
        "quest": f"exposed:{key}", "about": LOBART, "about_name": "Лобарт",
        "from": LOBART, "from_name": "Лобарт", "to": VINO, "day": 1, "hops": 1, **extra,
    }


def _desk(facts: list[Fact]) -> PeopleKnowledge:
    return PeopleKnowledge(SimpleNamespace(facts=lambda: list(facts)))


def test_accusation_rumor_preserves_owner_belief_without_proving_theft():
    fact = {
        "fact": "npc_accused", "id": "suspect:C1:VINO", "crime": "C1", "kind": "suspect",
        "npc": LOBART, "name": "Лобарт", "target": VINO, "target_name": "Вино",
        "witnesses": [LOBART, HILDA], "witness_names": {LOBART: "Лобарт", HILDA: "Хильда"},
    }
    entries = source_events(fact, [fact])
    assert len(entries) == 2 and all(e.kind == "npc_suspected" for e in entries)
    assert entries[0].description() == "Лобарт думает, что Вино украл(а) его вещь"
    heard = _rumor(VINO, "Вино", kind="npc_suspected", quest="npc_accused:suspect:C1:VINO",
                   about=LOBART, about_name="Лобарт", to=HILDA)
    assert "Лобарт думает" in _desk([heard]).context(HILDA)
    assert all(e.kind != "npc_theft" for e in _desk([fact, heard]).of(HILDA))


def test_source_names_and_day_restore_from_old_testimony():
    facts = _exposed()
    entry = source_event(facts[-1], facts)
    assert entry is not None
    assert (entry.subject_name, entry.about_name, entry.day) == ("Хильда", "Лобарт", 1)
    assert entry.source == LOBART and entry.direct and entry.kind == "cover"
    assert "Хильда" in entry.description() and "Лобарт" in entry.description()
    facts[-1].update(day=5, name="Лобарт", subject_name="Хильда")
    assert source_event(facts[-1], facts).day == 5


def test_who_knows_about_whom_and_source():
    facts = _exposed()
    desk = _desk(facts)
    assert desk.of(VINO) == [] and desk.of(HILDA) == []
    assert "ты раскрыл это сам" in desk.context(LOBART)
    assert "Хильда уже лгал(а) тебе, покрывая героя" in desk.warning(LOBART, HILDA)
    facts.append(_rumor())
    entry = desk.of(VINO)[0]
    assert entry.subject == HILDA and not entry.direct and entry.source == LOBART
    assert "со слов: Лобарт" in desk.context(VINO)
    assert "ты слышал, что Хильда" in desk.warning(VINO, HILDA)


def test_context_three_freshest_living_people_except_self():
    facts = [_rumor(f"N{i}", f"Человек {i}", f"T{i}") for i in range(5)]
    facts += [_rumor(VINO, "Вино", "self"), {"fact": "died", "npc": "N4"}]
    desk = _desk(facts)
    context = desk.context(VINO)
    assert context is not None
    assert len(context.splitlines()) == 4
    assert context.index("Человек 3") < context.index("Человек 2") < context.index("Человек 1")
    assert all(name not in context for name in ["Человек 0", "Человек 4", "Вино"])
    assert desk.warning(VINO, VINO) is None and desk.warning(VINO, "N4") is None


def test_reload_rebuilds_without_duplicates_and_direct_beats_hearsay():
    facts = _exposed() + [_rumor(), _rumor()]
    desk = _desk(facts)
    expected = desk.of(VINO)
    assert len(expected) == 1 and _desk(facts).of(VINO) == expected
    facts.append(_rumor(to=LOBART, **{"from": VINO, "from_name": "Вино"}))
    assert len(desk.of(LOBART)) == 1 and desk.of(LOBART)[0].direct
    # A load of the earlier branch drops the exposure and everything learned from it.
    facts.clear()
    assert desk.context(VINO) is None and desk.context(LOBART) is None


def test_old_rumor_is_about_player_and_not_other_people():
    fact = _rumor()
    del fact["subject"]
    assert rumor_subject(fact) == "PLAYER"
    assert _desk([fact]).context(VINO) is None
    assert rumor_subject({"subject": ""}) == "PLAYER"


def test_new_source_needs_only_a_registry_entry(monkeypatch):
    monkeypatch.setitem(
        SOURCES, "new_source",
        replace(
            SOURCES["exposed"], kind="new_kind", text="{subject_name}: новое знание",
            warning_direct="{subject_name}: новая причина", warning_heard="{subject_name}: слух",
        ),
    )
    facts = _exposed()
    facts[-1]["fact"] = "new_source"
    desk = _desk(facts)
    assert "Хильда: новое знание" in desk.context(LOBART)
    assert desk.warning(LOBART, HILDA) == "(Хильда: новая причина)"
    rumor = _rumor(kind="new_kind", quest="new_source:T1")
    assert "Хильда: новое знание" in _desk([rumor]).context(VINO)


def test_source_registry_supports_several_first_observers(monkeypatch):
    monkeypatch.setitem(
        SOURCES, "shared_discovery",
        replace(
            SOURCES["exposed"], observer="witnesses", observer_name="witness_names",
            about="PLAYER", about_name="герой", requires=(("how", "seen"),),
        ),
    )
    fact = {
        "fact": "shared_discovery", "liar": HILDA, "subject_name": "Хильда", "testimony": "T1",
        "witnesses": [LOBART, VINO, LOBART, HILDA, "PLAYER"],
        "witness_names": {LOBART: "Лобарт", VINO: "Вино"}, "how": "seen",
    }
    entries = source_events(fact, [])
    assert [entry.source for entry in entries] == [LOBART, VINO]
    assert [entry.source_name for entry in entries] == ["Лобарт", "Вино"]
    assert len({(entry.key, entry.about, entry.about_name) for entry in entries}) == 1
    assert source_events({**fact, "how": "missing"}, []) == []


def test_unpaid_is_known_only_to_hearers_and_names_the_debtor():
    fact = {
        "fact": "npc_broken", "npc": HILDA, "npc_name": "Хильда", "deal": "D1",
        "how": "unpaid", "day": 2, "witnesses": [LOBART, VINO, HILDA, "PLAYER"],
        "witness_names": {LOBART: "Лобарт", VINO: "Вино"},
    }
    desk = _desk([fact])
    for npc in [LOBART, VINO]:
        [entry] = desk.of(npc)
        assert (entry.subject, entry.kind, entry.key, entry.day) == (HILDA, "npc_broken", "npc_broken:D1", 2)
        assert "Хильда не отдал(а) герою обещанное" in entry.description()
        assert "ты слышал это сам" in desk.context(npc)
        assert "Хильда при тебе не отдал(а)" in desk.warning(npc, HILDA)
    assert desk.of(HILDA) == [] and desk.of("UNINVOLVED") == []
    assert source_events({**fact, "how": "no_show"}, []) == []
    assert source_events({**fact, "witnesses": []}, []) == []


def test_unpaid_restores_direct_and_hearsay_knowledge_without_duplicates():
    event = {
        "fact": "npc_broken", "npc": HILDA, "npc_name": "Хильда", "deal": "D1",
        "how": "unpaid", "witnesses": [LOBART], "day": 1,
    }
    rumor = _rumor(
        kind="npc_broken", quest="npc_broken:D1", about="PLAYER", about_name="герой",
    )
    facts = [event, rumor, event.copy(), rumor.copy()]
    for npc in [LOBART, VINO]:
        [entry] = _desk(facts).of(npc)
        assert entry.direct == (npc == LOBART)
        assert _desk(facts).of(npc) == [entry]
    assert "со слов: Лобарт" in _desk(facts).context(VINO)
    facts.clear()
    assert _desk(facts).of(LOBART) == [] and _desk(facts).of(VINO) == []


def test_broken_gate_vouch_names_the_voucher_in_direct_news_and_rumors():
    fact = {
        "fact": "gate_vouch_broken", "id": "V1:C1", "vouch": "V1", "npc": HILDA,
        "name": "Хильда", "crime": "C1", "title": "Кража", "day": 2,
        "witnesses": [LOBART], "witness_names": {LOBART: "Лобарт"},
    }
    rumor = _rumor(
        kind="gate_vouch_broken", quest="gate_vouch_broken:V1:C1",
        about="PLAYER", about_name="герой",
    )
    desk = _desk([fact, rumor])
    for npc in (LOBART, VINO):
        [entry] = desk.of(npc)
        assert (entry.subject, entry.about, entry.kind) == (HILDA, "PLAYER", "gate_vouch_broken")
        assert entry.direct == (npc == LOBART)
        assert entry.description() == "Хильда поручился за преступника"
        assert "Хильда поручился за преступника" in desk.warning(npc, HILDA)
    assert "со слов: Лобарт" in desk.context(VINO)
    assert desk.of(HILDA) == []


async def test_turn_extension_has_only_situation():
    desk = _desk(_exposed())
    await desk.prepare(LOBART)
    assert desk.situation(LOBART) == desk.context(LOBART)
    assert desk.tools(LOBART) == [] and desk.all_tools == ()
    assert await desk.handle(None, None, None, None) is None
