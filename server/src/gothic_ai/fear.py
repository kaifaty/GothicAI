"""Fear of the hero comes from certain, personally unforgiven knowledge of human violence."""

from __future__ import annotations

from collections.abc import Mapping
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from .characters import Card
    from .crimes import Crimes


def knows_violence(npc: str, crimes: Crimes | None) -> bool:
    """Seeing or hearing a human assault counts; a suspicion or someone else's pardon does not."""
    return crimes is not None and any(
        k.degree in {"seen", "heard"} and k.suspect == "PLAYER" and not k.forgiven
        and k.crime.get("kind") in {"attack", "murder"}
        and not crimes.does_not_blame(npc, k.crime)
        for k in crimes.known(npc.upper())
    )


def afraid(npc: str, cards: Mapping[str, Card], crimes: Crimes | None, *, courage: int) -> bool:
    card = cards.get(npc.upper())
    return card is not None and card.courage <= courage and knows_violence(npc, crimes)
