"""NPC promises: a private, deterministic intention fixed when the hero accepts a deal."""

from __future__ import annotations

from collections.abc import Mapping, Sequence
from typing import TYPE_CHECKING, Any

from .chronicle import Chronicle, Fact
from .fear import afraid

if TYPE_CHECKING:
    from .characters import Card
    from .crimes import Crimes
    from .dialogue import DialogueManager
    from .protocol import RosterEntry

HEARD_CM = 1500
INTENT_LABELS = {
    "silence": "молчание",
    "cover": "прикрытие",
    "contract": "плату за поручение",
    "npc_later": "передачу обещанных вещей позже",
    "meet": "встречу",
}


def decide(
    chronicle: Chronicle,
    npc: str,
    deal: str,
    how: Sequence[str],
    *,
    cards: Mapping[str, Card],
    crimes: Crimes | None = None,
    enabled: bool = True,
) -> Fact | None:
    """Keep an existing decision, or record it once. Disabled and immediate deals have no intent."""
    existing = next((f for f in chronicle.facts("npc_intent") if f.get("deal") == deal), None)
    if existing is not None:
        return existing
    if not enabled or not how:
        return None
    npc = npc.upper()
    card = cards.get(npc)
    holds, why = True, "no_card"
    if card is not None:
        holds = card.honesty >= 3 or (card.honesty == 2 and card.greed < 4)
        why = "honesty" if holds else "dishonesty"
        if not holds and crimes is not None and afraid(npc, cards, crimes, courage=3):
            holds, why = True, "fear"
    fact = {
        "fact": "npc_intent",
        "deal": deal,
        "npc": npc,
        "keeps": holds,
        "how": list(dict.fromkeys(how)),
        "why": why,
    }
    chronicle.record(fact)
    return fact


def keeps(chronicle: Chronicle, deal_id: str, how: str) -> bool:
    """Old deals and obligations outside the recorded intent keep their previous honest behavior."""
    if not getattr(chronicle, "npc_cheat_enabled", True):
        return True
    return not any(
        f.get("deal") == deal_id and not f.get("keeps", True) and how in f.get("how", ())
        for f in chronicle.facts("npc_intent")
    )


def breach(chronicle: Chronicle, npc: str, deal: str, how: str, **fields: Any) -> Fact | None:
    if any(f.get("deal") == deal and f.get("how") == how for f in chronicle.facts("npc_broken")):
        return None
    fact = {**fields, "fact": "npc_broken", "npc": npc.upper(), "deal": deal, "how": how}
    chronicle.record(fact)
    return fact


def betrayed(chronicle: Chronicle, npc: str, deal: str, how: str, to: str) -> Fact | None:
    if any(f.get("deal") == deal for f in chronicle.facts("betrayed")):
        return None
    fact = {"fact": "betrayed", "npc": npc.upper(), "deal": deal, "how": how, "to": to}
    chronicle.record(fact)
    return fact


def hearers(
    dialogue: DialogueManager, npc: str, roster: Mapping[str, RosterEntry] | None = None
) -> list[str]:
    """People hearing a refusal now, from the live roster (or fresh perception without a roster)."""
    if not roster:
        talk = getattr(getattr(dialogue, "floor", None), "talk", None)
        roster = getattr(talk, "people", None)
    if roster:
        return sorted(
            {
                entry.id.upper()
                for entry in roster.values()
                if entry.id.upper() not in {npc.upper(), "PLAYER"}
                and entry.activity != "down"
                and entry.earshot <= HEARD_CM
            }
        )
    perception = getattr(getattr(dialogue, "sight", None), "__self__", None)
    views = perception.views() if hasattr(perception, "views") else []
    return sorted(
        {
            view.npc.upper()
            for view in views
            if view.npc.upper() not in {npc.upper(), "PLAYER"}
            and view.hp > 0
            and view.hero is not None
            and view.hero.distance <= HEARD_CM
        }
    )
