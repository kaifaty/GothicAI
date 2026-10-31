"""Match a real disclosure to the accepted silence or cover that an NPC intended to break."""

from __future__ import annotations

from .chronicle import Chronicle, Fact
from .npc_cheat import keeps


def silence_facts(chronicle: Chronicle, fact: Fact, *, cover: bool = True) -> list[Fact]:
    """Project kept crime promises, including raw deals whose deception switch was later disabled."""
    kind = fact.get("fact")
    if kind == "silenced":
        how = "cover" if fact.get("cover") else "silence"
        return [fact] if (cover or how != "cover") and keeps(chronicle, fact.get("deal", ""), how) else []
    if kind != "deal":
        return []
    deal = str(fact.get("deal", ""))
    out = []
    if keeps(chronicle, deal, "silence"):
        for key in fact.get("silence", ()):
            if isinstance(key, str) and key.startswith("crime:"):
                plain = {key: value for key, value in fact.items() if key != "cover"}
                out.append({**plain, "fact": "silenced", "crime": key.removeprefix("crime:")})
    promise = fact.get("cover")
    if cover and isinstance(promise, dict) and promise.get("crime") and keeps(chronicle, deal, "cover"):
        out.append({**fact, "fact": "silenced", "crime": promise["crime"], "cover": deal})
    return out


def broken_promises(
    chronicle: Chronicle,
    npc: str,
    *,
    crime: str = "",
    rumor: str = "",
    cover: bool = True,
    at: int | None = None,
) -> list[Fact]:
    """Crime cover includes silence; day cover affects traces in testimony, not every crime that day."""
    npc = npc.upper()
    keys = {key for key in (rumor, f"crime:{crime}" if crime else "") if key}
    out = []
    for f in chronicle.facts("deal"):
        if str(f.get("npc", "")).upper() != npc:
            continue
        if at is not None:
            when = f.get("at")
            if when is None and all(key in f for key in ("day", "hour", "minute")):
                when = int(f["day"]) * 1440 + int(f["hour"]) * 60 + int(f["minute"])
            if when is not None and int(when) > at:
                continue  # an overdue report may already have arrived before this promise was made
        deal = str(f.get("deal", ""))
        silence = bool(keys.intersection(f.get("silence", ()))) and not keeps(chronicle, deal, "silence")
        covering = (
            cover and bool(crime) and isinstance(f.get("cover"), dict)
            and f["cover"].get("crime") == crime and not keeps(chronicle, deal, "cover")
        )
        if silence or covering:
            out.append(f)
    return out
