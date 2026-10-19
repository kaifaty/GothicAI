from dataclasses import replace
from types import SimpleNamespace

from gothic_ai.chronicle import Fact
from gothic_ai.people_knowledge import SOURCES, PeopleKnowledge, rumor_subject, source_event

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


async def test_turn_extension_has_only_situation():
    desk = _desk(_exposed())
    await desk.prepare(LOBART)
    assert desk.situation(LOBART) == desk.context(LOBART)
    assert desk.tools(LOBART) == [] and desk.all_tools == ()
    assert await desk.handle(None, None, None, None) is None
