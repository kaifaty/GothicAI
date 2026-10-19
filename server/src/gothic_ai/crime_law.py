"""Community law, projected only from the current save branch's Chronicle.

The runtime supplies the game's clock and writes the resulting vanilla counters. This module owns no
world state: save/load replaces its evidence, including report deadlines, payments and event clocks.
"""

from __future__ import annotations

from collections import Counter
from collections.abc import Iterable, Mapping
from dataclasses import dataclass
from typing import Any, Literal

from .chronicle import Fact
from .crimes import COMMUNITIES, Crimes
from .people_knowledge import rumor_subject

DAY_MINUTES = 24 * 60
REPORT_DELAYS = {"theft": DAY_MINUTES, "sheep": DAY_MINUTES, "attack": DAY_MINUTES // 2, "murder": 120}
LAW_COMMUNITIES = tuple(key for key, value in COMMUNITIES.items() if value.lawkeepers)
COUNTER_KINDS = {"theft": "THEFT", "attack": "ATTACK", "murder": "MURDER", "sheep": "SHEEPKILLER"}
_GUARD_GUILDS = {"city": {1, 2}, "oldcamp": {1, 2}, "farm": {7}, "monastery": {4}}
_TIMED_FACTS = {"died", "forgiven", "silenced", "rumor", "testimony"}


def game_minutes(fact: Mapping[str, Any]) -> int | None:
    """A full clock only: a day alone cannot order a pardon against a two-hour deadline."""
    if isinstance(fact.get("at"), int):
        return int(fact["at"])
    if all(key in fact for key in ("day", "hour", "minute")):
        return int(fact["day"]) * DAY_MINUTES + int(fact["hour"]) * 60 + int(fact["minute"])
    return None


@dataclass(frozen=True)
class LawDelivery:
    community: str
    crime: Fact
    via: str
    due: int
    how: Literal["guard", "witness", "rumor"]
    status: Literal["pending", "known", "stopped", "absolved"]
    reason: str = ""


class CrimeLaw:
    def __init__(self, crimes: Crimes) -> None:
        self.crimes = crimes
        self.chronicle = crimes.chronicle
        self._branch: str | None = None
        self._now: int | None = None

    @property
    def now(self) -> int | None:
        return self._now if self._branch == self.chronicle.branch else None

    def community_of(self, npc: str, metadata: Mapping[str, Any] | None = None) -> str | None:
        npc = npc.upper()
        for community in LAW_COMMUNITIES:
            if npc in COMMUNITIES[community].lawkeepers:
                return community
        person = {**self.crimes.people.get(npc, {}), **(metadata or {})}
        guild = int(person.get("guild", -1))
        if guild in {1, 2, 3} and "npctype" not in person:
            return None  # city and OldCamp cannot be distinguished from guild alone
        return self.crimes.community(guild, int(person.get("npctype", 0)))

    def is_guard(self, npc: str, community: str | None, metadata: Mapping[str, Any] | None = None) -> bool:
        if community not in LAW_COMMUNITIES:
            return False
        npc = npc.upper()
        if npc in COMMUNITIES[community].lawkeepers:
            return True
        person = {**self.crimes.people.get(npc, {}), **(metadata or {})}
        return (
            int(person.get("guild", -1)) in _GUARD_GUILDS[community]
            and self.community_of(npc, person) == community
        )

    def known(self, community: str) -> list[Fact]:
        ids = {f.get("crime") for f in self.chronicle.facts("crime_known") if f.get("community") == community}
        return [f for f in self.crimes.facts() if f.get("community") == community and f.get("id") in ids]

    def _absolved(self, community: str) -> set[str]:
        return {
            crime
            for f in self.chronicle.facts("absolved")
            if f.get("community") == community
            for crime in f.get("crimes", [])
        }

    def unpaid(self, community: str) -> list[Fact]:
        paid = self._absolved(community)
        return [f for f in self.known(community) if f["id"] not in paid]

    def counters(self, community: str) -> dict[str, int]:
        counts = Counter(f.get("kind") for f in self.unpaid(community))
        return {counter: counts[kind] for kind, counter in COUNTER_KINDS.items()}

    def debt(self, community: str) -> int:
        counters = self.counters(community)
        total = sum(counters.values())
        if total and total == counters["SHEEPKILLER"]:
            return 100  # PETZMASTER's CRIME_SHEEPKILLER branch replaces the computed total
        return min(1000, 50 * total + (500 if counters["MURDER"] else 0))

    def inform(self, crime: str, *, via: str, how: str, at: int) -> Fact | None:
        """The common arrival path: delayed reports and an on-scene call teach law once."""
        deed = self.crimes.crime(crime)
        if deed is None or (community := deed.get("community")) not in LAW_COMMUNITIES:
            return None
        if any(f.get("id") == crime for f in self.known(community)):
            return None
        fact: Fact = {
            "fact": "crime_known", "community": community, "crime": crime,
            "how": how, "via": via.upper(), "at": at,
        }
        self.chronicle.record(fact)
        return fact

    def called(
        self, crime: str, via: str, guards: Iterable[str], *, at: int | None = None
    ) -> list[Fact]:
        """Teach guards who heard the witness and deliver that call to their community law."""
        deed = self.crimes.crime(crime)
        if deed is None or deed.get("community") not in LAW_COMMUNITIES:
            return []
        when = at if at is not None else self.now
        if when is None:
            when = game_minutes(deed)
        if when is None:
            return []
        via = via.upper()
        listeners = sorted({npc.upper() for npc in guards})
        source = next(
            (f["npc"] for f in self.chronicle.facts("crime_seen")
             if f.get("crime") == crime and f.get("npc")),
            via,
        )
        out = []
        for npc in listeners:
            if not self.is_guard(npc, deed["community"]):
                continue
            if not any(k.crime["id"] == crime and k.degree != "suspects" for k in self.crimes.known(npc)):
                fact: Fact = {
                    "fact": "rumor", "crime": crime, "from": via, "to": npc,
                    "hops": 1, "at": when, "how": "called",
                    "quest": f"crime:{crime}", "kind": deed["kind"], "about": source,
                    "about_name": self.crimes.names(source), "title": self.crimes.describe(deed),
                    "day": when // DAY_MINUTES,
                }
                self.chronicle.record(fact)
                out.append(fact)
        if any(self.is_guard(npc, deed["community"]) for npc in listeners):
            if arrival := self.inform(crime, via=via, how="called", at=when):
                out.append(arrival)
        return out

    def _times(self, facts: list[Fact]) -> dict[int, int]:
        stamps = {
            int(f["event"]): int(f["at"])
            for f in facts
            if f.get("fact") == "crime_law_time" and "event" in f and "at" in f
        }
        return {
            index: at
            for index, fact in enumerate(facts)
            if (at := game_minutes(fact)) is not None or (at := stamps.get(index)) is not None
        }

    def tick(self, now: int) -> list[Fact]:
        """Advance reports using this branch's game minutes, preserving missing event clocks once.

        Event ordinals are stable when Chronicle copies a branch at a saved head. They identify the
        original fact without mutating shared facts or relying on process-local evidence after a load.
        """
        self._branch, self._now = self.chronicle.branch, now
        facts = self.chronicle.facts()
        times = self._times(facts)
        for index, fact in enumerate(facts):
            if fact.get("fact") == "rumor" and rumor_subject(fact) != "PLAYER":
                continue
            if fact.get("fact") in _TIMED_FACTS and index not in times:
                self.chronicle.record({"fact": "crime_law_time", "event": index, "at": now})
        out = []
        known = {(f.get("community"), f.get("crime")) for f in self.chronicle.facts("crime_known")}
        # The earliest surviving route wins, including when a clock jump crosses several deadlines.
        for report in sorted(self.deliveries(), key=lambda d: d.due):
            key = (report.community, report.crime["id"])
            if report.status != "pending" or report.due > now or key in known:
                continue
            if fact := self.inform(
                report.crime["id"], via=report.via, how=report.how, at=report.due
            ):
                known.add(key)
                out.append(fact)
        return out

    def deliveries(self) -> list[LawDelivery]:
        facts = self.chronicle.facts()
        times = self._times(facts)
        crimes = {f["id"]: f for f in facts if f.get("fact") == "crime"}
        known = {(f.get("community"), f.get("crime")) for f in facts if f.get("fact") == "crime_known"}
        paid = {community: self._absolved(community) for community in LAW_COMMUNITIES}
        reports: list[LawDelivery] = []
        routes: set[tuple[str, str, str, int]] = set()
        for index, fact in enumerate(facts):
            if fact.get("fact") == "rumor" and rumor_subject(fact) != "PLAYER":
                continue
            entries: list[tuple[str, str]] = []
            kind = fact.get("fact")
            if kind == "crime_seen":
                entries.append((fact.get("crime", ""), fact.get("npc", "")))
            elif kind == "rumor" and fact.get("crime"):
                entries.append((fact["crime"], fact.get("to", "")))
            elif kind == "testimony":
                entries += [(crime, fact.get("npc", "")) for crime in self.crimes.testimony_crimes(fact)]
            for crime_id, npc in entries:
                crime = crimes.get(crime_id)
                if crime is None or (community := crime.get("community")) not in LAW_COMMUNITIES:
                    continue
                npc = npc.upper()
                guard = self.is_guard(npc, community, fact)
                if kind == "crime_seen":
                    if not guard and self.community_of(npc, fact) != community:
                        continue
                    at = game_minutes(crime)
                    if at is None:
                        continue
                    how: Literal["guard", "witness", "rumor"] = "guard" if guard else "witness"
                    due = at if guard else at + REPORT_DELAYS[crime["kind"]]
                else:
                    if not guard or index not in times:
                        continue
                    how, due = "rumor", times[index]
                key = (community, crime_id, npc, due)
                if key in routes:
                    continue
                routes.add(key)
                status: Literal["pending", "known", "stopped", "absolved"] = "pending"
                reason = ""
                if crime_id in paid[community]:
                    status, reason = "absolved", "absolved"
                elif (community, crime_id) in known:
                    status = "known"
                elif reason := self._blocker(crime, npc, due, facts, times):
                    status = "stopped"
                reports.append(LawDelivery(community, crime, npc, due, how, status, reason))
        return reports

    def _blocker(self, crime: Fact, via: str, due: int, facts: list[Fact], times: dict[int, int]) -> str:
        candidates = []
        victim = crime.get("victim") or crime.get("owner")
        for index, fact in enumerate(facts):
            at = times.get(index)
            if at is None or at >= due:
                continue
            kind, npc = fact.get("fact"), fact.get("npc", "").upper()
            if kind == "died" and npc == via:
                candidates.append((at, index, "died"))
            elif fact.get("crime") == crime["id"]:
                if kind == "silenced" and npc == via and (self.crimes.cover or not fact.get("cover")):
                    candidates.append((at, index, "silenced"))
                elif kind == "forgiven" and npc == victim:
                    candidates.append((at, index, "repaid" if fact.get("paid") else "forgiven"))
        return min(candidates)[2] if candidates else ""

    def absolve(
        self,
        community: str | None,
        *,
        crimes: Iterable[str] | None = None,
        how: str = "paid",
    ) -> list[Fact]:
        """Pay known cases, or grant LOC_ALL absolution to every extant case, including pending ones."""
        selected = set(crimes) if crimes is not None else None
        out = []
        for target in LAW_COMMUNITIES if community is None else (community,):
            if target not in LAW_COMMUNITIES:
                continue
            candidates = (
                self.crimes.facts() if community is None or selected is not None else self.unpaid(target)
            )
            paid = self._absolved(target)
            ids = [
                f["id"]
                for f in candidates
                if f.get("community") == target
                and f["id"] not in paid
                and (selected is None or f["id"] in selected)
            ]
            if not ids:
                continue
            fact: Fact = {"fact": "absolved", "community": target, "crimes": ids, "how": how}
            if self.now is not None:
                fact["at"] = self.now
            self.chronicle.record(fact)
            out.append(fact)
        return out

    def summary(self) -> str:
        lines = []
        reports = self.deliveries()
        reasons = {
            "died": "свидетель мёртв",
            "silenced": "свидетель обещал молчать",
            "forgiven": "пострадавший простил",
            "repaid": "пострадавшему возместили",
        }
        for community in LAW_COMMUNITIES:
            known = self.known(community)
            routes = [r for r in reports if r.community == community and r.status in {"pending", "stopped"}]
            if not known and not routes:
                continue
            unpaid = {f["id"] for f in self.unpaid(community)}
            lines.append(f"Закон: {COMMUNITIES[community].name}; долг {self.debt(community)} золотых.")
            for crime in known:
                settled = "; оплачено" if crime["id"] not in unpaid else ""
                lines.append(f"  знает {crime['id']}: {self.crimes.describe(crime)}{settled}")
            for report in routes:
                if report.status == "stopped":
                    text = "остановлено: " + reasons.get(report.reason, report.reason)
                else:
                    text = (
                        f"в пути до дня {report.due // DAY_MINUTES}, "
                        f"{report.due % DAY_MINUTES // 60:02}:{report.due % 60:02}"
                    )
                lines.append(f"  {report.crime['id']} через {self.crimes.names(report.via)}: {text}")
        return "\n".join(lines) if lines else "Закон общин: известных дел и вестей в пути нет."
