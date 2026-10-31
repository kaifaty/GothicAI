"""Data-driven limits on an npc's gifts and loans to the hero (docs/contracts.md)."""

from __future__ import annotations

import json
import re
from collections.abc import Collection, Iterable
from pathlib import Path
from typing import TYPE_CHECKING, Any

from .chronicle import Chronicle, Fact
from .contracts import GOLD, STASH_FILE, Give

if TYPE_CHECKING:
    from .deals import Deal, Debt
    from .dialogue import Conversation

_MONEY = re.compile(
    r"\b(?:золот|монет|денег|деньг|денеж|деньж|грош|кошел|материальн|одолж|займ|заем|взайм|ссуд)"
    r"|\b(?:долг|долга|долгу|долгом|долги|долгов|долгами|долгах)\b"
)


def meeting_words(conv: Conversation) -> tuple[str, ...]:
    """Only the hero's words since the last meeting note, including the uncommitted turn."""
    words = []
    for entry in reversed(conv.memory.entries):
        if entry.role == "developer" and entry.content.startswith("[Новая встреча с героем"):
            break
        if entry.role == "user":
            words.append(entry.content)
    words.reverse()
    if conv.trigger is not None and conv.trigger.role == "user":
        words.append(conv.trigger.content)
    return tuple(words)


def _gold(gives: Iterable[Give]) -> int:
    return sum(g.count for g in gives if g.item == GOLD)


def _fact_gold(items: Iterable[dict[str, Any]]) -> int:
    return sum(int(g["count"]) for g in items if g.get("item") == GOLD)


def _gift(fact: Fact) -> int:
    given = _fact_gold(fact.get("npc_gives", ()))
    given += _fact_gold((fact.get("npc_later") or {}).get("items", ()))
    return max(0, given - _fact_gold(fact.get("later", ())) - _fact_gold(fact.get("hero_gives", ())))


class Purse:
    def __init__(self, chronicle: Chronicle, path: Path = STASH_FILE) -> None:
        self.chronicle = chronicle
        self.rules: dict[str, dict[str, Any]] = {}
        if path.is_file():
            data = json.loads(path.read_text(encoding="utf-8"))
            self.rules = {npc.upper(): limits for npc, limits in data.get("purse", {}).items()}

    def error(self, deal: Deal, debts: Iterable[Debt], hero_words: Collection[str]) -> str:
        limits = self.rules.get(deal.npc)
        if limits is None:
            return ""
        given = _gold(deal.npc_gives) + (_gold(deal.npc_later.gives) if deal.npc_later else 0)
        if (
            given > 0
            and limits["asked"]
            and not any(_MONEY.search(words.lower().replace("ё", "е")) for words in hero_words)
        ):
            return "Отклонено: герой не просил у тебя денег, а сам ты их не предлагаешь."
        later = _gold(x.give for x in deal.later)
        gift = max(0, given - later - _gold(deal.hero_gives))
        spent = sum(_gift(f) for f in self.chronicle.facts("deal") if f.get("npc") == deal.npc)
        if gift > 0 and spent + gift > limits["gift"]:
            left = max(0, limits["gift"] - spent)
            return (
                f"Отклонено: даром ты даёшь герою не больше {limits['gift']} золотых за всё время, "
                f"осталось {left}. Больше — только в долг: что он вернёт, впиши в hero_later."
            )
        repaid = {d.id for d in deal.repay}
        owed = sum(
            d.give.count
            for d in debts
            if d.npc == deal.npc and not d.paid and d.give.item == GOLD and d.id not in repaid
        )
        if later > 0 and owed + later > limits["loan"]:
            return (
                f"Отклонено: в долг — не больше {limits['loan']} золотых; сейчас он должен тебе "
                f"{owed}, с этой сделкой будет {owed + later}."
            )
        return ""
