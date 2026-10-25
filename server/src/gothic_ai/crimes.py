"""The hero's deeds and their actual witnesses, independent of the on-scene reaction.

The bridge supplies ownership and pre-reaction combat state. Truth and each npc's knowledge are read
from the Chronicle's save branch; crime_react.py settles held deeds. Community law lives in
crime_law.py and supplies the scripts' PETZ counters through the existing story commands.
Rules were checked against GOTHIC.DAT: C_IsTakenItemMyPossession, B_AssessTheft,
B_AssessDamage/B_Attack, B_AssessMurder, and the lifted C_NpcBelongsTo* helpers.
"""

from __future__ import annotations

from collections import Counter
from collections.abc import Callable, Iterable, Mapping
from dataclasses import dataclass
from typing import TYPE_CHECKING, Any, Literal

from .chronicle import Chronicle, Fact
from .crime_sides import CrimeSides
from .game import GameError, GameLink
from .gamedata.storygraph import StoryGraph
from .mercy import fresh_surrender
from .people_knowledge import rumor_subject
from .promise_breach import silence_facts
from .protocol import DeedItem, DeedSnapshot, GameMessage, HeroDeed, Session

if TYPE_CHECKING:
    from .blame import Blame
    from .concerns import Concern
    from .crime_react import CrimeReact
    from .dialogue import DialogueManager
    from .places import PlaceBook
    from .sightings import Sightings

SUSPECT_MIN = 30  # a remembered presence within half an hour of the theft, at the same place


@dataclass(frozen=True)
class CrimeKnowledge:
    crime: Fact
    degree: Literal["seen", "heard", "suspects"]
    source: str = ""
    hops: int = 0
    suspect: str = "PLAYER"
    forgiven: bool = False
    silenced: bool = False
    excuse: str = ""
    excuse_by: str = ""
    excuse_by_name: str = ""


HERO = {"PLAYER", "HERO", "PC_HERO"}
PUBLIC_GUILDS = {-1, 0, 15}  # absent, GIL_NONE, GIL_PUBLIC
TOUGH_GUILDS = {6, 7, 9, 13}  # GIL_DJG, GIL_SLD, GIL_BDT, GIL_PIR (C_NpcIsToughGuy)


@dataclass(frozen=True)
class Community:
    name: str
    helper: str
    lawkeepers: tuple[str, ...]


COMMUNITIES = {
    "city": Community("город", "C_NPCBELONGSTOCITY", ("MIL_311_ANDRE", "PAL_200_HAGEN")),
    "farm": Community("ферма", "C_NPCBELONGSTOFARM", ("SLD_800_LEE",)),
    "monastery": Community("монастырь", "C_NPCBELONGSTOMONASTERY", ("KDF_504_PARLAN",)),
    "oldcamp": Community("старый лагерь", "C_NPCBELONGSTOOLDCAMP", ("PAL_250_GAROND",)),
    "bl": Community("бандиты", "C_NPCBELONGSTOBL", ()),
}


def _inventory(items: Iterable[DeedItem | Mapping[str, Any]]) -> Counter[str]:
    result: Counter[str] = Counter()
    for entry in items:
        item, count = (
            (entry.item, entry.count)
            if isinstance(entry, DeedItem)
            else (str(entry["item"]), int(entry["count"]))
        )
        result[item.upper()] += count
    return result


def pickpocket_outcome(
    before: Iterable[DeedItem | Mapping[str, Any]],
    after: Iterable[DeedItem | Mapping[str, Any]],
    picked_before: int = 0,
    picked_after: int = 0,
    caught: bool = False,
) -> Literal["success", "failure"] | None:
    """Resolve an executed pickpocket choice from its original script's before/after state.

    B_Beklauen marks AIV_PlayerHasPickedMyPocket only on success. The 28 bespoke choices give
    actual items and use AR_THEFT when caught. A real gain proves success even if AR_THEFT remains
    from an earlier attempt. A marked pocket without a gain or a caught attempt proves no new deed.
    """
    if _inventory(after) - _inventory(before):
        return "success"
    if caught:
        return "failure"
    if not picked_before and not picked_after:
        return "failure"
    return None


class Crimes:
    def __init__(
        self,
        link: GameLink,
        chronicle: Chronicle,
        *,
        graph: StoryGraph | None = None,
        dialogue: DialogueManager | None = None,
        places: PlaceBook | None = None,
        names: Callable[[str], str] | None = None,
        cover: bool = True,
        mercy: bool = True,
    ) -> None:
        self.link = link
        self.chronicle = chronicle
        self.graph = graph
        self.dialogue = dialogue
        self.places = places
        self.names = names or (lambda npc: npc)
        self.cover = cover
        self.mercy = mercy
        self.sightings: Sightings | None = None
        self.blame: Blame | None = None
        self.react: CrimeReact | None = None
        self.people: dict[str, dict[str, Any]] = {}  # static world metadata, never beliefs
        self.sides = CrimeSides(self)
        self._noncrimes: set[str] = set()  # completed negative classifications, only in this world
        self.on_noncrime: list[Callable[[str], None]] = []
        chronicle.on_fact.append(self._on_fact)
        link.listeners.append(self._on_message)

    def facts(self) -> list[Fact]:
        """The current save branch's deeds, including those inherited at its saved head."""
        return self.chronicle.facts("crime")

    async def close(self) -> None:
        if self._on_message in self.link.listeners:
            self.link.listeners.remove(self._on_message)
        if self._on_fact in self.chronicle.on_fact:
            self.chronicle.on_fact.remove(self._on_fact)

    def crime(self, crime: str) -> Fact | None:
        return next((f for f in self.facts() if f.get("id") == crime), None)

    def side(self, npc: str, crime: str) -> Fact | None:
        """A witness's recorded side, inherited with the save branch like the deed itself."""
        return next((
            fact for fact in self.chronicle.facts("crime_side")
            if fact.get("npc") == npc.upper() and fact.get("crime") == crime
        ), None)

    def learn_side(self, npc: str, crime: Fact) -> Fact | None:
        """Record a listener's own side before its knowledge can trigger consequences."""
        npc = npc.upper()
        side = self.side(npc, crime["id"])
        if side is not None or crime.get("kind") != "murder":
            return side
        why = self.sides.heard_reason(npc, crime)
        if why is None:
            return None
        side = {"fact": "crime_side", "npc": npc, "crime": crime["id"], "why": why}
        self.chronicle.record(side)
        return side

    def _side_excuse(self, npc: str, crime: str) -> dict[str, str]:
        side = self.side(npc, crime)
        if side is None:
            return {}
        return {
            "excuse": side["why"],
            "excuse_by": side["npc"],
            "excuse_by_name": self.names(side["npc"]),
        }

    def _testimony_excuse(self, testimony: Fact, crime: str) -> dict[str, str]:
        return testimony.get("crime_excuses", {}).get(crime) or self._side_excuse(
            testimony.get("witness", ""), crime
        )

    def excuse(self, npc: str, crime: str) -> dict[str, str]:
        """The teller's own side wins; otherwise preserve its first justified source."""
        npc = npc.upper()
        if side := self._side_excuse(npc, crime):
            return side
        for fact in self.chronicle.facts():
            if (
                fact.get("fact") == "rumor" and fact.get("to") == npc
                and fact.get("crime") == crime and rumor_subject(fact) == "PLAYER"
                and fact.get("excuse")
            ):
                return {key: fact.get(key, "") for key in ("excuse", "excuse_by", "excuse_by_name")}
            if (
                fact.get("fact") == "testimony" and fact.get("npc") == npc
                and crime in self.testimony_crimes(fact)
            ):
                if excuse := self._testimony_excuse(fact, crime):
                    return excuse
        return {}

    def excuse_text(self, crime: Fact, excuse: Mapping[str, str]) -> str:
        by = excuse.get("excuse_by_name") or self.names(excuse.get("excuse_by", ""))
        victim = self.names(crime.get("victim", ""))
        reasons = {
            "fought": f"{victim} дрался с ним",
            "companion": f"{victim} напал на героя и его спутников",
            "hostile": f"{victim} был его врагом",
            "quest": f"сам хотел, чтобы {victim} умер",
        }
        reason = reasons.get(excuse.get("excuse", ""))
        return f"{by} говорит: {reason}" if reason else ""

    def does_not_blame(self, npc: str, crime: Fact) -> bool:
        """A personal side, or an unrelated listener accepting a justified account."""
        if self.side(npc, crime["id"]) is not None:
            return True
        if (
            crime.get("kind") != "murder" or self.react is None
            or self.react.base_role(npc, crime) != "stranger"
        ):
            return False
        return any(
            k.crime["id"] == crime["id"] and k.suspect == "PLAYER"
            and k.degree == "heard" and k.excuse
            for k in self.known(npc)
        )

    def blamed(self, npc: str, crime: str) -> bool:
        """An active belief in another culprit suspends this victim's demand against the hero."""
        return self.blame is not None and self.blame.active(npc, crime) is not None

    def classified_noncrime(self, deed: str) -> bool:
        """An explicit classification result; an unrecorded or duplicate deed is not one."""
        return deed in self._noncrimes

    def _noncrime(self, deed: str) -> None:
        if deed not in self._noncrimes:
            self._noncrimes.add(deed)
            for listener in self.on_noncrime:
                listener(deed)

    def seen(self, npc: str) -> list[Fact]:
        ids = {f.get("crime") for f in self.chronicle.facts("crime_seen") if f.get("npc") == npc.upper()}
        return [f for f in self.facts() if f.get("id") in ids]

    def testimony_crimes(self, testimony: Fact) -> list[str]:
        """With cover disabled old testimony yields its truth, while ordinary silence still applies."""
        about = testimony.get("about")
        if isinstance(about, str) and about.upper() not in HERO:
            return []  # testimony about another NPC never proves anything about the hero
        if self.cover or not testimony.get("cover"):
            return list(testimony.get("crime_told", []))
        silent = set()
        for fact in self.chronicle.facts():
            if fact == testimony:
                break
            silent.update(
                promise.get("crime")
                for promise in silence_facts(self.chronicle, fact, cover=False)
                if promise.get("npc") == testimony.get("witness")
            )
        return [c for c in testimony.get("crime_saw", []) if c not in silent]

    def known(self, npc: str) -> list[CrimeKnowledge]:
        """The single view of an npc's knowledge. Ownership alone never teaches it anything."""
        npc = npc.upper()
        facts = self.chronicle.facts()
        crimes = {f["id"]: f for f in facts if f.get("fact") == "crime"}
        silences = {
            (promise.get("npc"), promise.get("crime"))
            for f in facts for promise in silence_facts(self.chronicle, f, cover=self.cover)
        }
        out: dict[tuple[str, str], CrimeKnowledge] = {}
        rank = {"suspects": 0, "heard": 1, "seen": 2}
        for f in facts:
            if f.get("fact") == "rumor" and rumor_subject(f) != "PLAYER":
                continue
            entries: list[tuple[str, str, str, int, str]] = []
            if f.get("fact") == "crime_seen" and f.get("npc") == npc:
                entries.append((f["crime"], "seen", npc, 0, "PLAYER"))
            elif f.get("fact") == "rumor" and f.get("to") == npc and f.get("crime"):
                entries.append((f["crime"], "heard", f.get("from", ""), int(f.get("hops", 1)), "PLAYER"))
            elif f.get("fact") == "testimony" and f.get("npc") == npc:
                told = self.testimony_crimes(f)
                entries += [(c, "heard", f.get("witness", ""), 1, "PLAYER") for c in told]
            elif f.get("fact") == "crime_suspected" and f.get("npc") == npc:
                entries.append((f["crime"], "suspects", npc, 0, f.get("suspect", "PLAYER")))
            for crime, degree, source, hops, suspect in entries:
                if crime not in crimes:
                    continue
                key = (crime, suspect)
                previous = out.get(key)
                excuse = {}
                if degree == "heard":
                    excuse = (
                        self._testimony_excuse(f, crime) if f.get("fact") == "testimony"
                        else {key: f.get(key, "") for key in ("excuse", "excuse_by", "excuse_by_name")}
                    )
                if (
                    previous is None or rank[degree] > rank[previous.degree]
                    or (degree == previous.degree == "heard" and excuse.get("excuse") and not previous.excuse)
                ):
                    forgiven = any(
                        s.get("fact") == "forgiven" and s.get("npc") == npc and s.get("crime") == crime
                        for s in facts
                    )
                    silenced = (npc, crime) in silences
                    out[key] = CrimeKnowledge(
                        crimes[crime], degree, source, hops, suspect, forgiven, silenced,
                        excuse.get("excuse", ""), excuse.get("excuse_by", ""),
                        excuse.get("excuse_by_name", ""),
                    )
        return list(out.values())

    def describe(self, crime: Fact) -> str:
        """Describe a real deed; callers decide whether it is known or merely suspected."""
        victim = self.names(crime.get("victim") or crime.get("owner") or "")
        item = crime.get("item_name") or crime.get("item") or "чужую вещь"
        count = int(crime.get("count", 0))
        amount = f" ×{count}" if count > 1 else ""
        match crime.get("kind"):
            case "theft":
                action = f"взял {item}{amount}"
                if crime.get("outcome") == "failure":
                    action = "пытался обчистить карман"
                elif crime.get("method") == "pickpocket":
                    action += " из кармана"
                elif crime.get("chest"):
                    action += " из сундука"
                if victim:
                    action += f" (пострадавший: {victim})"
            case "attack":
                action = f"напал на {victim}"
            case "murder":
                action = (f"убил того, кто сдался: {victim}" if crime.get("surrendered")
                          else f"убил человека: {victim}")
            case _:
                action = "убил чужую овцу"
        place = self.places.of(crime.get("waypoint")) if self.places is not None else None
        where = f" {place.where_ru}" if place is not None else ""
        when = f"день {crime.get('day', 0)}, {int(crime.get('hour', 0)):02}:{int(crime.get('minute', 0)):02}"
        return f"герой {action}{where} ({when})"

    def situation(self, npc: str) -> str | None:
        lines = []
        for k in self.known(npc):
            if k.suspect != "PLAYER":
                continue
            what = self.describe(k.crime)
            if k.degree == "seen":
                if k.crime.get("surrendered"):
                    what = "он " + what.removeprefix("герой ")
                line = f"Ты видел сам: {what}."
            elif k.degree == "heard":
                line = f"Ты слышал (со слов: {self.names(k.source)}): {what}; переходов: {k.hops}."
                if k.excuse:
                    line += " " + self.excuse_text(k.crime, {
                        "excuse": k.excuse, "excuse_by": k.excuse_by, "excuse_by_name": k.excuse_by_name,
                    }) + "."
            else:
                line = f"Ты подозреваешь: {what}. Ты видел героя рядом, самого дела не видел; это догадка."
            if self.side(npc, k.crime["id"]) is not None:
                line += " Жертва была твоим врагом: ты не винишь героя."
            elif self.does_not_blame(npc, k.crime):
                line += " Ты не винишь героя."
            if k.forgiven:
                line += " Ты простил это дело."
            if k.silenced:
                line += " Ты обещал молчать об этом деле."
            lines.append(line)
        return "Что ты знаешь о делах героя:\n" + "\n".join(lines) if lines else None

    def distrust(self, npc: str) -> list[str]:
        """Only direct proof, or a victim's learned grievance, breaks its reliance on his word."""
        community = self._person_community(npc)
        return [
            self.describe(k.crime)
            for k in self.known(npc)
            if k.suspect == "PLAYER"
            and k.degree != "suspects"
            and not k.forgiven
            and not self.does_not_blame(npc, k.crime)
            and (
                k.degree == "seen"
                or k.crime.get("victim") == npc.upper()
                or (k.crime.get("kind") == "murder" and community and k.crime.get("community") == community)
            )
        ]

    def attitude(self, npc: str) -> str | None:
        """The initial reaction to this knowledge, not the npc's current intent."""
        attitudes = {self._attitude_of(npc, k) for k in self.known(npc)}
        return "hostile" if "hostile" in attitudes else "wary" if "wary" in attitudes else None

    def _attitude_of(self, npc: str, knowledge: CrimeKnowledge) -> str | None:
        if knowledge.suspect != "PLAYER" or knowledge.degree == "suspects" or knowledge.forgiven:
            return None
        if self.does_not_blame(npc, knowledge.crime):
            return None
        community = self._person_community(npc)
        crime = knowledge.crime
        if crime.get("kind") == "murder" and community and crime.get("community") == community:
            return "hostile"
        return "wary" if knowledge.degree == "seen" or crime.get("victim") == npc.upper() else None

    def _person_community(self, npc: str) -> str | None:
        person = self.people.get(npc.upper(), {})
        guild = int(person.get("guild", -1))
        if guild in {1, 2, 3} and "npctype" not in person:
            return None  # do not mistake an OldCamp member for a townsman before reading its type
        return self.community(guild, int(person.get("npctype", 0)))

    async def prepare(self, npc: str) -> None:
        npc = npc.upper()
        # story_people has guilds, but OldCamp/BL membership also depends on NPCTYPE. Read both
        # fields through the existing evaluator rather than guessing a community from a name.
        if self.known(npc) and "npctype" not in self.people.get(npc, {}):
            branch = self.chronicle.branch
            try:
                values = await self.link.story_eval(
                    npc,
                    [["field", ["inst", "SELF"], "GUILD"], ["field", ["inst", "SELF"], "NPCTYPE"]],
                )
            except (GameError, TimeoutError):
                values = []
            if (
                self.chronicle.branch == branch
                and len(values) == 2
                and all(isinstance(v, int) for v in values)
            ):
                self.people[npc] = {**self.people.get(npc, {}), "guild": values[0], "npctype": values[1]}
        # Only a deferred murder reaction may begin here. Ordinary turns never reassert wariness.
        self._apply_attitude(npc, hostile_only=True)

    def _apply_attitude(
        self,
        npc: str,
        *,
        crimes: set[str] | None = None,
        hostile_only: bool = False,
    ) -> None:
        if self.dialogue is None:
            return
        npc = npc.upper()
        applied = {
            (f.get("crime"), f.get("attitude"))
            for f in self.chronicle.facts("crime_attitude")
            if f.get("npc") == npc
        }
        pending = []
        for knowledge in self.known(npc):
            crime = knowledge.crime["id"]
            if crimes is not None and crime not in crimes:
                continue
            attitude = self._attitude_of(npc, knowledge)
            if (
                attitude is None
                or (hostile_only and attitude != "hostile")
                or (crime, attitude) in applied
                or (crime, "hostile") in applied
            ):
                continue
            pending.append((crime, attitude))
        if not pending:
            return
        attitude = "hostile" if any(a == "hostile" for _, a in pending) else "wary"
        name = self.names(npc)
        current = self.dialogue.open(npc, name).intent.attitude
        if current != "hostile" and current != attitude:
            # This is a belief change only; vanilla still owns every physical reaction in this layer.
            self.dialogue.change_intent(npc, name, body=False, attitude=attitude)
        # Consume the reaction even when an existing wary/hostile intent already covers it. The
        # conversational model may soften later, and a load must not revive the old reaction.
        for crime, reaction in pending:
            self.chronicle.record(
                {"fact": "crime_attitude", "npc": npc, "crime": crime, "attitude": reaction}
            )

    def _on_fact(self, fact: Fact) -> None:
        if fact.get("fact") == "rumor" and rumor_subject(fact) != "PLAYER":
            return
        if fact.get("fact") in {"crime_seen", "rumor", "testimony"}:
            npc = fact.get("npc") or fact.get("to")
            crimes = set(fact.get("crime_told", []))
            if fact.get("crime"):
                crimes.add(fact["crime"])
            if npc and crimes:
                self._apply_attitude(npc, crimes=crimes)

    def suspect_chest(
        self,
        npc: str,
        chest: str,
        missing: Mapping[str, int],
        *,
        waypoint: str = "",
    ) -> list[Fact]:
        """A missing item plus a remembered presence is suspicion, never proof of the thief."""
        if self.sightings is None:
            return []
        npc = npc.upper()
        out = []
        for crime in self.facts():
            if (
                crime.get("kind") != "theft"
                or crime.get("chest") != chest
                or crime.get("victim") != npc
                or crime.get("item") not in missing
            ):
                continue
            at = (
                int(crime.get("day", 0)) * 1440 + int(crime.get("hour", 0)) * 60 + int(crime.get("minute", 0))
            )
            place = self.places.of(crime.get("waypoint") or waypoint) if self.places is not None else None
            if place is None:
                continue  # no spatial evidence is better than accusing everyone seen that day
            for s in self.sightings.mind(npc).sightings:
                if (
                    s.trace_kind
                    or s.end < at - SUSPECT_MIN
                    or s.start > at + SUSPECT_MIN
                    or place.id not in s.place_ids
                ):
                    continue
                if any(
                    f.get("npc") == npc and f.get("crime") == crime["id"] and f.get("suspect") == s.who
                    for f in self.chronicle.facts("crime_suspected")
                ):
                    continue
                fact = {
                    "fact": "crime_suspected",
                    "npc": npc,
                    "crime": crime["id"],
                    "suspect": s.who,
                    "chest": chest,
                    "sighting": s.record(),
                }
                self.chronicle.record(fact)
                out.append(fact)
        return out

    def concerns(self, npc: str) -> list[Concern]:
        from .concerns import Concern

        raised = {
            f.get("occasion") for f in self.chronicle.facts("initiative") if f.get("npc") == npc.upper()
        }
        return [
            Concern(
                f"crime_suspected:{k.crime['id']}",
                npc.upper(),
                1,
                "ask the hero about missing property; you suspect him, without proof",
                f"из твоего сундука пропало; подозреваешь героя: {self.describe(k.crime)}",
            )
            for k in self.known(npc)
            if k.degree == "suspects"
            and k.suspect == "PLAYER"
            and not k.forgiven
            and f"crime_suspected:{k.crime['id']}" not in raised
        ]

    all_tools = ()

    def tools(self, npc: str) -> list:
        return []

    async def handle(self, dialogue: Any, conv: Any, call: Any, snap: Any) -> None:
        return None

    def lawkeepers(self, community: str | None) -> tuple[str, ...]:
        definition = COMMUNITIES.get(community or "")
        if definition is None:
            return ()
        if self.graph is None:
            return definition.lawkeepers
        # Use the actual graph's PETZMASTER nodes, not a name inferred from an npc's guild.
        found = {node.npc for node in self.graph.nodes.values() if node.id.endswith("_PETZMASTER")}
        return tuple(npc for npc in definition.lawkeepers if npc in found)

    def community(self, guild: int, npctype: int = 0) -> str | None:
        """The vanilla community of a victim; a guild-only owner has ordinary NPCTYPE 0."""
        if self.graph is not None:
            for key, definition in COMMUNITIES.items():
                function = self.graph.functions.get(definition.helper)
                if function and function.get("lifted"):
                    if self._community_expr(function["body"], guild, npctype):
                        return key
            # Missing helpers in a partial graph do not make ordinary people stateless.
        if guild in {1, 2, 3}:
            fallback = "oldcamp" if npctype in {3, 4} else "city"
        elif guild in {7, 8}:
            fallback = "farm"
        elif guild in {4, 5}:
            fallback = "monastery"
        else:
            fallback = "bl" if npctype in {5, 7} else None
        if fallback is not None and self.graph is not None:
            function = self.graph.functions.get(COMMUNITIES[fallback].helper)
            if function and function.get("lifted"):
                return None  # the actual DAT's helper rejected this vanilla fallback
        return fallback

    def _community_expr(self, expr: list, guild: int, npctype: int) -> int | bool:
        match expr:
            case ["int", value, *_]:
                return int(value)
            case ["field", _, "GUILD"]:
                return guild
            case ["field", _, "NPCTYPE"]:
                return npctype
            case ["fn", name, *_] if self.graph is not None:
                return self._community_expr(self.graph.functions[name]["body"], guild, npctype)
            case ["!", operand]:
                return not self._community_expr(operand, guild, npctype)
            case ["==", left, right]:
                return self._community_expr(left, guild, npctype) == self._community_expr(
                    right, guild, npctype
                )
            case ["&&", *operands]:
                return all(self._community_expr(x, guild, npctype) for x in operands)
            case ["||", *operands]:
                return any(self._community_expr(x, guild, npctype) for x in operands)
            case _:
                raise ValueError(f"unsupported vanilla community expression: {expr!r}")

    def _npc(self, symbol: str) -> str:
        symbol = symbol.upper()
        return self.graph.npc(symbol) if self.graph is not None else symbol

    def _ownership(self, deed: DeedSnapshot, *, culprit: str = "") -> tuple[str, int, int] | None:
        """Whose property was taken, strongest explicit ownership first; None means lawful.

        An explicitly owned item stays its owner's even in a public room. Conversely a dropped
        unowned item is no one's property, as C_IsTakenItemMyPossession's ITEM_DROPPED check says.
        """
        sources = [
            (deed.owner, deed.owner_npc_guild, deed.owner_npctype, deed.owner_guild),
        ]
        if deed.item_dropped and not deed.owner:
            return None
        if deed.chest:
            sources.append(
                (deed.chest_owner, deed.chest_owner_guild, deed.chest_owner_npctype, deed.chest_guild)
            )
        sources.append((deed.room_owner, deed.room_owner_guild, deed.room_owner_npctype, deed.room_guild))
        for owner, npc_guild, npctype, guild in sources:
            if owner:
                owner = self._npc(owner)
                own = owner == culprit if culprit else owner in HERO
                return None if own else (owner, npc_guild, npctype)
            if guild not in PUBLIC_GUILDS:
                actor_guild = (
                    getattr(deed, "culprit_guild", deed.hero_guild) if culprit else deed.hero_guild
                )
                return None if guild == actor_guild else ("", guild, 0)
        return None

    def _on_message(self, msg: GameMessage) -> None:
        self.sides.on_message(msg)
        if isinstance(msg, HeroDeed):
            self.record(msg)
        elif isinstance(msg, Session):
            self.people.clear()
            self._noncrimes.clear()

    def record(self, deed: HeroDeed) -> Fact | None:
        """Classify a raw bridge deed and keep one truth plus one fact per real witness."""
        if self.chronicle.branch is None:
            return None
        # Reconnect delivery or duplicate perceptions cannot turn one deed into two crimes. The
        # branch query also survives a save/load, unlike a process-wide set of event ids.
        if any(f.get("id") == deed.id for f in self.facts()):
            return None

        victim, owner = self._npc(deed.victim), ""
        community = None
        witnesses = list(dict.fromkeys(self._npc(npc) for npc in deed.witnesses if npc))
        item, count = deed.item.upper(), deed.count
        extra: dict[str, Any] = {}
        match deed.kind:
            case "take_item":
                if not item or count <= 0:
                    return None
                ownership = self._ownership(deed)
                if ownership is None:
                    return self._noncrime(deed.id)
                owner, guild, npctype = ownership
                if deed.owner:
                    deaths = [
                        fact for fact in self.chronicle.facts("died")
                        if self._npc(fact.get("npc", "")) == owner
                    ]
                    violence = {
                        fact.get("kind") for fact in self.facts()
                        if self._npc(fact.get("victim", "")) == owner
                        and not (fact.get("kind") == "murder" and fact.get("lawful_fight"))
                    }
                    if (
                        deed.owner_npc_guild > 16
                        or any(fact.get("monster") is True for fact in deaths)
                        or (deaths and "murder" not in violence)
                        or (
                            not violence.intersection({"attack", "murder"})
                            and self.sides.fought_hero_group(owner, deed)
                        )
                    ):
                        return self._noncrime(deed.id)
                victim = owner
                community = self.community(guild, npctype)
                kind = "theft"
            case "attack":
                if not victim:
                    return None
                if (
                    deed.victim_is_monster
                    or deed.victim_guild > 16
                    or deed.victim_in_fight
                    or deed.victim_enemy
                    or deed.victim_started_fight
                    or deed.victim_npctype == 2
                    or deed.victim_tough_guy
                    or deed.victim_guild in TOUGH_GUILDS
                ):
                    return self._noncrime(deed.id)
                community = self.community(deed.victim_guild, deed.victim_npctype)
                kind = "attack"
            case "murder":
                if not victim:
                    return None
                if deed.victim_is_sheep or deed.victim_guild == 18:
                    if deed.victim_tough_guy:  # bought/following sheep, including Betsy
                        return self._noncrime(deed.id)
                    kind = "sheep"
                    # Vanilla has no sheep -> npc owner. Its crime belongs to a witness's
                    # community. Keep an unwitnessed sheep's community unknown, without guessing
                    # a farmer from proximity or the sheep's waypoint.
                    community = next(
                        (
                            candidate
                            for npc in deed.witnesses_metadata
                            if self._npc(npc.npc) in witnesses
                            if (candidate := self.community(npc.guild, npc.npctype)) is not None
                        ),
                        None,
                    )
                elif deed.victim_is_monster or deed.victim_guild > 16:
                    return self._noncrime(deed.id)
                else:
                    kind = "murder"
                    community = self.community(deed.victim_guild, deed.victim_npctype)
                    if self.mercy and fresh_surrender(
                        self.chronicle, victim, deed.day * 1440 + deed.hour * 60 + deed.minute,
                    ):
                        extra["surrendered"] = True
                    elif (
                        deed.victim_enemy
                        or deed.victim_started_fight
                        or deed.victim_tough_guy
                        or deed.victim_guild in TOUGH_GUILDS
                    ):
                        extra["lawful_fight"] = True
                    if deed.victim_guild >= 0:
                        extra["victim_guild"] = deed.victim_guild
            case "pickpocket":
                outcome = pickpocket_outcome(
                    deed.inventory_before,
                    deed.inventory_after,
                    deed.picked_pocket_before,
                    deed.picked_pocket_after,
                    deed.caught,
                )
                if outcome is None or not victim:
                    return None
                kind, owner = "theft", victim
                community = self.community(deed.victim_guild, deed.victim_npctype)
                extra = {"method": "pickpocket", "outcome": outcome}
                gains = _inventory(deed.inventory_after) - _inventory(deed.inventory_before)
                if outcome == "success":
                    witnesses = []
                    item, count = next(iter(gains.items()))
                    if len(gains) > 1:
                        extra["items"] = [
                            {"item": symbol, "count": amount} for symbol, amount in gains.items()
                        ]
                else:
                    witnesses = list(dict.fromkeys([victim, *witnesses]))
                    item, count = "", 0
            case _:
                return None

        fact = {
            "fact": "crime",
            "id": deed.id,
            "kind": kind,
            "victim": victim,
            "owner": owner,
            "community": community,
            "item": item,
            "count": count,
            "place": deed.position.model_dump(),
            "waypoint": deed.waypoint,
            "day": deed.day,
            "hour": deed.hour,
            "minute": deed.minute,
            "witnesses": witnesses,
            **extra,
            **({"chest": deed.chest} if deed.chest else {}),
        }
        self._noncrimes.discard(deed.id)
        self.chronicle.record(fact)
        metadata = {self._npc(p.npc): p for p in deed.witnesses_metadata}
        if kind == "murder":
            # These facts must precede every crime_seen: listeners immediately change attitudes
            # and deliver news to the law when knowledge is recorded.
            for npc in witnesses:
                if why := self.sides.reason(npc, deed, surrendered=bool(extra.get("surrendered"))):
                    self.chronicle.record(
                        {"fact": "crime_side", "npc": npc, "crime": deed.id, "why": why}
                    )
        for npc in witnesses:
            person = metadata.get(npc)
            self.chronicle.record(
                {
                    "fact": "crime_seen", "npc": npc, "crime": deed.id,
                    **({"guild": person.guild, "npctype": person.npctype} if person else {}),
                }
            )
        return fact
