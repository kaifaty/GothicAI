"""Decide whether a code-backed lie briefly reveals the speaker's real feeling."""

from __future__ import annotations

import logging
import random
import zlib
from typing import TYPE_CHECKING

from .protocol import Mood, SayLeak

if TYPE_CHECKING:
    from .dialogue import DialogueManager

log = logging.getLogger(__name__)


def _stakes(dialogue: DialogueManager, npc: str) -> float:
    # Imported here because the extensions themselves use DialogueManager.
    from .bounty import Bounty
    from .deals import Deals
    from .witnesses import KEPT_MIN, Witnesses

    stakes = 0.0
    for extension in dialogue.extensions:
        if isinstance(extension, Witnesses) and extension.cover_enabled:
            now = extension._now()
            for fact in extension.chronicle.facts("testimony"):
                if (
                    fact.get("witness") == npc and fact.get("cover")
                    and (not now or now - int(fact.get("minute", 0)) <= KEPT_MIN)
                ):
                    stakes = max(stakes, 1.5 if fact.get("crime_saw") else 1.0)
            for cover in extension._covers():
                if cover.get("npc") == npc:
                    stakes = max(stakes, 1.5 if cover.get("crime") else 1.0)
        elif isinstance(extension, Bounty):
            tier = extension.tier(npc)
            if tier and any(not extension.revealed(npc, secret) for secret in tier["secrets"]):
                stakes = 1.5
        elif isinstance(extension, Deals) and extension.cheat:
            if any(
                fact.get("npc") == npc and fact.get("keeps") is False
                for fact in extension.chronicle.facts("npc_intent")
            ):
                stakes = max(stakes, 1.0)
    return stakes


def choose_leak(
    dialogue: DialogueManager, npc: str, line: str, felt: Mood, word: int,
) -> SayLeak | None:
    """One reproducible trial per line, only for deception the server knows about."""
    npc = npc.upper()
    stakes = _stakes(dialogue, npc)
    if not stakes:
        log.debug("%s %s: discarded felt tag without known deception", npc, line)
        return None
    card = dialogue.personas.cards.get(npc)
    skill = card.lie_skill if card is not None else None
    if skill is None:
        skill = 0.5 if dialogue.guild_of(npc) == 9 else 0.3
    probability = min(0.6, 0.4 * (1 - skill) * stakes)
    rng = random.Random(zlib.crc32(f"{npc}:{line}".encode()))
    if rng.random() >= probability:
        return None
    return SayLeak(felt=felt, half=rng.choice(("upper", "lower")), word=word)
