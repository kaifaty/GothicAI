from dataclasses import replace
from types import SimpleNamespace

import pytest

from gothic_ai.characters import Card
from gothic_ai.npc_cheat import betrayed, breach, decide, hearers, keeps
from gothic_ai.protocol import RosterEntry

NPC = "BAU_900_TEST"
CARD = Card(NPC, "Бронко", "", "", 3, 3, 4, 3, 3, 3)


class Facts:
    def __init__(self):
        self.all = []

    def record(self, fact):
        self.all.append(fact)

    def facts(self, kind=None):
        return [f for f in self.all if kind is None or f["fact"] == kind]


@pytest.mark.parametrize(
    ("honesty", "greed", "expected"),
    [(h, g, h >= 3 or (h == 2 and g < 4)) for h in range(1, 6) for g in (3, 4, 5)],
)
def test_intent_rule(honesty, greed, expected):
    chronicle = Facts()
    card = replace(CARD, honesty=honesty, greed=greed)
    fact = decide(chronicle, NPC, "D1", ["silence", "cover"], cards={NPC: card})
    assert fact["keeps"] is expected
    assert fact["npc"] == NPC and fact["how"] == ["silence", "cover"]
    assert keeps(chronicle, "D1", "silence") is expected
    assert keeps(chronicle, "D1", "meet") is True
    assert keeps(chronicle, "older-deal", "silence") is True


@pytest.mark.parametrize("kind", ["attack", "murder"])
@pytest.mark.parametrize("degree", ["seen", "heard"])
@pytest.mark.parametrize("courage", [1, 2, 3, 4])
def test_fear_overrides_dishonesty(kind, degree, courage):
    chronicle = Facts()
    card = replace(CARD, honesty=1, courage=courage)
    known = SimpleNamespace(
        degree=degree, suspect="PLAYER", forgiven=False, crime={"id": "C1", "kind": kind},
    )
    crimes = SimpleNamespace(known=lambda npc: [known], does_not_blame=lambda npc, crime: False)
    intent = decide(chronicle, NPC, "D1", ["contract"], cards={NPC: card}, crimes=crimes)
    assert intent["keeps"] is (courage <= 3)


@pytest.mark.parametrize(
    ("kind", "degree", "suspect", "forgiven"),
    [
        ("theft", "seen", "PLAYER", False),
        ("attack", "suspected", "PLAYER", False),
        ("murder", "heard", "OTHER", False),
        ("attack", "heard", "PLAYER", True),
    ],
)
def test_unrelated_or_forgiven_violence_does_not_scare(kind, degree, suspect, forgiven):
    chronicle = Facts()
    card = replace(CARD, honesty=1, courage=1)
    known = SimpleNamespace(
        degree=degree, suspect=suspect, forgiven=forgiven, crime={"id": "C1", "kind": kind},
    )
    crimes = SimpleNamespace(known=lambda npc: [known], does_not_blame=lambda npc, crime: False)
    assert not decide(chronicle, NPC, "D1", ["meet"], cards={NPC: card}, crimes=crimes)["keeps"]


def test_no_card_disabled_immediate_and_fixed_after_reload():
    chronicle = Facts()
    assert decide(chronicle, NPC, "D1", ["meet"], cards={})["keeps"]
    dishonest = {NPC: replace(CARD, honesty=1)}
    assert decide(chronicle, NPC, "D1", ["meet"], cards=dishonest)["keeps"]
    assert len(chronicle.facts("npc_intent")) == 1
    assert decide(chronicle, NPC, "D2", ["meet"], cards=dishonest, enabled=False) is None
    assert decide(chronicle, NPC, "D3", [], cards=dishonest) is None
    decide(chronicle, NPC, "D4", ["meet"], cards=dishonest)
    assert not keeps(chronicle, "D4", "meet")
    chronicle.npc_cheat_enabled = False
    assert keeps(chronicle, "D4", "meet")


def test_each_kind_of_breach_and_betrayal_once_per_deal():
    chronicle = Facts()
    assert breach(chronicle, NPC, "D1", "unpaid", witnesses=["HEARER"])
    assert breach(chronicle, NPC, "D1", "unpaid", contract="another-obligation") is None
    assert breach(chronicle, NPC, "D1", "no_show")
    assert breach(chronicle, NPC, "D1", "no_show") is None
    assert betrayed(chronicle, NPC, "D1", "told", "HEARER")
    assert betrayed(chronicle, NPC, "D1", "reported", "LAW") is None
    assert [f["how"] for f in chronicle.facts("npc_broken")] == ["unpaid", "no_show"]
    assert len(chronicle.facts("betrayed")) == 1


def test_hearers_use_current_roster_and_exclude_self_far_and_down():
    roster = {
        who: RosterEntry(id=who, name=who, distance=distance, heard=heard, activity=activity)
        for who, distance, heard, activity in [
            (NPC, 100, None, "routine"),
            ("NEAR", 200, None, "routine"),
            ("WALL", 200, 20000, "routine"),
            ("FAR", 1600, None, "routine"),
            ("DOWN", 100, None, "down"),
        ]
    }
    assert hearers(SimpleNamespace(), NPC, roster) == ["NEAR"]
    assert hearers(SimpleNamespace(), NPC) == []
