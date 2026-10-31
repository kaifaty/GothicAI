"""What an npc saw itself (docs/npc-tools.md «Память увиденного», wave 1 step 6).

Perception (perception.py) tells what the npc sees now; here what it saw stays. A sighting is someone seen
without a break (GAP_MIN game minutes unseen end it): when, the places he was seen at (the book of places),
how he looked (his armor) and anything notable — a weapon drawn, blood, a fight, a body. Kept are the hero
(whoever sees him: one that does not know his name still knows his face), the people the npc knows by
name, and (step M2 of docs/npc-tools.md, «Видел недавно») strangers and creatures worth remembering: a body,
one hostile to it or to the hero, one with a weapon drawn, a beast at someone — with what they were doing.

Sightings are the npc's own events in the memory log of the save branch (memory.py, kind `seen`), so a load
brings back exactly what it had seen before that save. One is written when it starts, every SAVE_MIN game
minutes while it goes on and when it ends; the latest record of it wins.

Visible physical traces (v37) keep their first observation's time and place, with stable ids across a
load. They are its own evidence (В lines), without a culprit. Impacts heard (v36) are remembered alongside
them but told separately as sounds, without visual evidence ids or an inferred cause.

For the conversational model (a TurnExtension):
- the situation, «Что ты видел сам»: the hero's last sightings, each with an id (В1, В2…), and the last
  sighting of each one it knows that it does not see now;
- `catch_lie{said, seen}`: the npc calls the hero a liar with what it saw itself — «я был в городе», when it
  saw him on the farm an hour ago. The code checks both halves: the sighting is the npc's own, and the hero
  did say those words. Then it is a lie caught (quests.caught): the npc takes his word no more, is wary of
  him, the people around hear it. What a witness it asked saw (witnesses.py, a line С1…) counts the same;
- recall: the sightings of whom or where the query names;
- «Недавно видел, сейчас не видишь»: the strangers and creatures of the last `FADE_MIN` game minutes, with
  their ids: they may be its intent's target (perception.py), and a companion set on one goes where it saw
  him first (orders.py: «там бандиты, разберись» after it came out of their cave).

What npc's far from the hero see is not kept: the game pushes the views of those near him (npc_views).
"""

from __future__ import annotations

import asyncio
import json
import logging
from collections.abc import Callable, Coroutine
from dataclasses import dataclass, field
from typing import TYPE_CHECKING, Any, Protocol

from .chronicle import Chronicle, Fact
from .game import GameError, GameLink
from .journal import Journal
from .knowledge import roots
from .llm import Tool, ToolCall
from .memory import MemoryStore
from .perception import WEAPON, _doing, armor_look, trace_name
from .protocol import (
    BROKEN_TRACE_PROTOCOL,
    NOISE_PROTOCOL,
    TRACE_PROTOCOL,
    GameMessage,
    Loading,
    Look,
    MemoryCmd,
    NpcEvent,
    NpcView,
    NpcViews,
    Session,
    Snapshot,
    Trace,
)
from .quests import Lie, caught
from .turn import ToolOutcome

if TYPE_CHECKING:
    from .crimes import Crimes
    from .dialogue import Conversation, DialogueManager
    from .perception import PerceptionDesk

log = logging.getLogger(__name__)

HERO = "PLAYER"
GAP_MIN = 10  # game minutes unseen that end a sighting (about 40 s of real time)
SAVE_MIN = 15  # a sighting that goes on is written down again this often (game minutes)
KEEP = 40  # sightings an npc has in mind; the log keeps them all
HERO_SHOWN = 3  # the hero's latest sightings in the situation
PEOPLE_SHOWN = 5  # people it knows, in the situation
FORGET_MIN = 3 * 24 * 60  # others seen longer ago than this are not in the situation
NEAREST = 6  # people it knows, kept from one view (the nearest)
PLACES_MAX = 4
HEARD = 8  # the hero's latest lines a lie must be in
RECALLED = 8
NOTED = 6  # strangers and creatures worth remembering, kept from one view (the nearest)
NOTED_SHOWN = 5  # ...in the situation
TRACES_SHOWN = 6
HEARD_SHOWN = 4
FADE_MIN = 180  # ...not longer ago than this (game minutes, about 12 minutes of real time)
HOSTILE = ("hostile", "angry")

NOTABLE = {
    "weapon": "с оружием наголо",
    "hurt": "в крови",
    "fight": "в драке",
    "down": "без сознания",
    "hostile": "враг тебе",
}

CATCH = "catch_lie"
CATCH_TOOL = Tool(
    CATCH,
    "Уличить героя во лжи тем, что ты видел своими глазами: он говорит, что был где-то или делал что-то, "
    "а ты сам видел его в другом месте или другим (строка из «Что ты видел сам», например В2) — или его "
    "видел тот, кого ты расспросил (строка С1 из «Что тебе рассказали о герое другие»). "
    "Уликой может быть и след из «Следы, которые ты видел», если слова героя прямо ему противоречат "
    "(например, он говорит, что стрела старая, а ты видел, что она свежая). "
    "Или говорит, что забор сломан с весны, а ты видел свежие обломки. Дело героя, которое ты видел сам "
    "(строка Д1), тоже улика, если он отрицает его. Только если его слова прямо противоречат "
    "увиденному. Ты это запомнишь, люди вокруг услышат.",
    {
        "type": "object",
        "properties": {
            "said": {
                "type": "string",
                "description": "его слова, в которых ложь, коротко, как он сказал: «всё утро был в городе»",
            },
            "seen": {
                "type": "string",
                "description": "номер строки: что ты видел сам или тебе рассказали, например В2, Д1 или С1",
            },
        },
        "required": ["said", "seen"],
        "additionalProperties": False,
    },
)


@dataclass
class Sighting:
    """Someone the npc saw without a break."""

    who: str  # PLAYER or the npc's symbol
    name: str
    start: int  # game minutes since the first day began
    end: int
    places: list[str] = field(default_factory=list)  # where, in words: «на пастбище у входа на ферму…»
    armor: list[str] = field(default_factory=list)  # what he wore, in order: armor symbols, "" for none
    armor_names: list[str] = field(default_factory=list)
    notable: list[str] = field(default_factory=list)  # NOTABLE keys, and "dead": a body
    place_ids: list[str] = field(default_factory=list)  # the same places by the book's ids (FARM1_PASTURE)
    known: bool = True  # the hero or one it knows by name; False: a stranger or a creature, by its look
    doing: str = ""  # what it was doing when last seen: «сидит у костра»
    saved: int = -1  # the end last written down; -1: never
    trace_kind: str = ""  # physical evidence, first seen at start; not a person or proof of a culprit
    trace_age: int | None = None  # broken: its age when first seen, not time elapsed since seeing it

    @property
    def key(self) -> str:
        return f"{self.who}@{self.start}"

    def record(self) -> dict[str, Any]:
        return {
            "who": self.who,
            "name": self.name,
            "start": self.start,
            "end": self.end,
            "places": self.places,
            "place_ids": self.place_ids,
            "armor": self.armor,
            "armor_names": self.armor_names,
            "notable": self.notable,
            **({} if self.known else {"known": False}),
            **({"doing": self.doing} if self.doing else {}),
            **({"trace_kind": self.trace_kind} if self.trace_kind else {}),
            **({"trace_age": self.trace_age} if self.trace_kind == "broken" else {}),
        }

    @classmethod
    def of(cls, record: dict[str, Any]) -> Sighting:
        s = cls(
            str(record["who"]),
            str(record.get("name", "")),
            int(record["start"]),
            int(record["end"]),
            [str(p) for p in record.get("places", [])],
            [str(a) for a in record.get("armor", [])],
            [str(a) for a in record.get("armor_names", [])],
            [str(n) for n in record.get("notable", [])],
            place_ids=[str(p) for p in record.get("place_ids", [])],
            known=bool(record.get("known", True)),
            doing=str(record.get("doing", "")),
            trace_kind=str(record.get("trace_kind", "")),
            trace_age=record.get("trace_age"),
        )
        s.saved = s.end
        return s


@dataclass(frozen=True)
class Heard:
    """An impact the npc heard itself; this says nothing about who made it."""

    time: int
    place: str
    place_id: str = ""

    def record(self) -> dict[str, Any]:
        return {"observation": "heard", "time": self.time, "place": self.place, "place_id": self.place_id}

    @classmethod
    def of(cls, record: dict[str, Any]) -> Heard:
        return cls(int(record["time"]), str(record["place"]), str(record.get("place_id", "")))


@dataclass
class Mind:
    """What one npc saw: its sightings, oldest first, and those going on."""

    sightings: list[Sighting] = field(default_factory=list)
    going: dict[str, Sighting] = field(default_factory=dict)  # who -> the sighting going on
    traces: dict[str, Sighting] = field(default_factory=dict)  # (trace id, visible kind) -> first observation
    heard: list[Heard] = field(default_factory=list)


@dataclass(frozen=True)
class Told:
    """What a witness the npc asked told it of the hero (witnesses.py): a line С1… to catch a lie with."""

    witness: str
    witness_name: str
    sightings: list[Sighting]  # what it saw of him; empty: it did not see him
    words: str  # the line, in words
    said: str  # the hero's words it was asked about (they were his: checked when it was asked)
    id: str  # the testimony's
    crime: str = ""  # the exact deed in this evidence row, witnessed by the source itself


class Hearsay(Protocol):
    """What witnesses told the npc (witnesses.Witnesses)."""

    def told(self, npc: str, sid: str) -> Told | None:
        """The testimony behind a С line of the npc's latest situation."""
        ...

    def any(self, npc: str) -> bool:
        """Whether a witness told it of seeing the hero."""
        ...


class Sightings:
    """What npc's saw, from the views (perception.py), kept in the memory of the save branch."""

    def __init__(
        self,
        link: GameLink,
        memory: MemoryStore,
        desk: PerceptionDesk,
        chronicle: Chronicle,
        *,
        knows_hero: Callable[[str], bool] = lambda npc: False,
        journal: Journal | None = None,
        crimes: Crimes | None = None,
    ) -> None:
        self.link = link
        self.memory = memory
        self.desk = desk
        self.chronicle = chronicle
        self.knows_hero = knows_hero
        self.journal = journal or Journal()
        self.crimes = crimes
        self.branch: str | None = link.session.branch if link.session is not None else None
        self.now = 0  # the game's clock as the latest view had it (minutes)
        self._clock_known = False
        self._minds: dict[str, Mind] = {}
        self._shown: dict[str, dict[str, Sighting]] = {}  # npc -> the hero's sightings by id, last turn
        self._crime_shown: dict[str, dict[str, Fact]] = {}  # npc -> its own crime_seen evidence, last turn
        self._caught: set[tuple[str, str]] = set()  # (npc, his words) caught already
        self.hearsay: Hearsay | None = None  # what witnesses told the npc (witnesses.py)
        self._background: set[asyncio.Task[Any]] = set()
        desk.on_view.append(self.saw)
        link.listeners.append(self._on_message)

    def _on_message(self, msg: GameMessage) -> None:
        match msg:
            case Session():
                self.branch = msg.branch
                self._forget()
            case Loading():
                self._forget()
            case NpcViews():
                times = [v.time for v in msg.views if v.time is not None]
                if times:
                    self._sweep(max(times))
            case NpcEvent(kind="noise"):
                self.heard(msg)

    def _forget(self) -> None:
        self._minds.clear()
        self._shown.clear()
        self._crime_shown.clear()
        self._caught.clear()
        self.now = 0
        self._clock_known = False

    # --- seeing ---------------------------------------------------------------------------------

    def saw(self, view: NpcView) -> None:
        """A view of the npc's (pushed or asked): whom it sees goes into its sightings."""
        if self.branch is None or view.time is None:
            return
        now = view.time
        self.now = max(self.now, now)
        self._clock_known = True
        viewer = view.npc.upper()
        mind = self.mind(viewer)
        for who, look, known in self._subjects(view):
            s = mind.going.get(who)
            if s is not None and now - s.end > GAP_MIN:
                self._close(viewer, mind, s)
                s = None
            if s is None:
                name = "Герой" if who == HERO else look.name if known else self.desk.label(viewer, look)
                s = Sighting(who, name, now, now, known=known)
                mind.sightings.append(s)
                del mind.sightings[:-KEEP]
                mind.going[who] = s
            s.end = max(s.end, now)
            where, place = self._where(view, look)
            if len(s.places) < PLACES_MAX and where not in s.places and place:
                _add(s.place_ids, place)
            _add(s.places, where, PLACES_MAX)
            armor = look.armor.item if look.armor else ""
            if armor not in s.armor:
                s.armor.append(armor)
                s.armor_names.append(look.armor.name if look.armor else "")
            for flag in _notable(look):
                _add(s.notable, flag)
            if doing := _doing(look.state, look.body, 0):
                s.doing = doing
            if s.saved < 0 or s.end - s.saved >= SAVE_MIN:
                self._write(viewer, s)
        hello = getattr(self.link, "hello", None)
        if hello is None or hello.protocol >= TRACE_PROTOCOL:
            for trace in view.traces:
                if trace.kind == "broken" and hello is not None and hello.protocol < BROKEN_TRACE_PROTOCOL:
                    continue
                who = f"TRACE:{trace.id}:{trace.kind}"
                s = mind.traces.get(who)
                if s is None:
                    where, place = self._where(view, trace)
                    s = Sighting(
                        who,
                        trace.name,
                        now,
                        now,
                        [where],
                        place_ids=[place] if place else [],
                        trace_kind=trace.kind,
                        trace_age=trace.age if trace.kind == "broken" else None,
                    )
                    mind.traces[who] = s
                    self._write(viewer, s)

    def heard(self, event: NpcEvent) -> None:
        """An actual noise event, kept separately from things seen and never usable as a В line."""
        hello = getattr(self.link, "hello", None)
        if self.branch is None or (hello is not None and hello.protocol < NOISE_PROTOCOL):
            return
        now = event.time if event.time is not None else self.now if self._clock_known else None
        if now is None:
            return
        self.now = max(self.now, now)
        self._clock_known = True
        book = self.desk.places
        place = book.of(event.place) if book is not None and event.place else None
        where = place.where_ru if place is not None else f"у точки {event.place}" if event.place else "рядом"
        sound = Heard(now, where, place.id if place is not None else "")
        viewer = event.npc.upper()
        mind = self.mind(viewer)
        if sound in mind.heard:
            return
        mind.heard.append(sound)
        del mind.heard[:-KEEP]
        self._remember(viewer, sound.record())

    def _subjects(self, view: NpcView) -> list[tuple[str, Look, bool]]:
        """Whom of those it sees it keeps: the hero, the people it knows (the nearest), and the strangers and
        creatures worth remembering (the nearest); whether it knows them."""
        out: list[tuple[str, Look, bool]] = []
        if view.hero is not None and view.hero.seen:
            out.append((HERO, view.hero, True))
        known = [o for o in view.sees if o.kind in ("human", "body") and self.desk.knows(view.npc, o)]
        out += [(o.id.upper(), o, True) for o in known[:NEAREST]]
        others = [o for o in view.sees if o not in known and _noticeable(o)]
        out += [(o.id.upper(), o, False) for o in others[:NOTED]]
        return out

    def _where(self, view: NpcView, look: Look | Trace) -> tuple[str, str]:
        """Where someone was seen, in words and by the book's id (''): his place by the book, else where the
        npc is."""
        book = self.desk.places
        place = book.of(look.waypoint) if book is not None and look.waypoint else None
        if place is None and not look.waypoint:
            place = self.desk.place(view)  # an old bridge: not where he is, where the npc is
        if place is not None:
            return place.where_ru, place.id
        wp = look.waypoint or view.waypoint or view.nearest_waypoint
        return (f"у точки {wp}" if wp else "где-то рядом"), ""

    def _sweep(self, now: int) -> None:
        """Sightings unseen for long enough are over: their end is written down."""
        self.now = max(self.now, now)
        self._clock_known = True
        for viewer, mind in self._minds.items():
            for s in list(mind.going.values()):
                if self.now - s.end > GAP_MIN:
                    self._close(viewer, mind, s)

    def _close(self, viewer: str, mind: Mind, s: Sighting) -> None:
        if s.saved != s.end:
            self._write(viewer, s)
        mind.going.pop(s.who, None)

    def _write(self, viewer: str, s: Sighting) -> None:
        self._remember(viewer, s.record())
        s.saved = s.end

    def _remember(self, viewer: str, record: dict[str, Any]) -> None:
        if self.branch is None:
            return
        seq = self.memory.add_seen(self.branch, viewer, json.dumps(record, ensure_ascii=False))
        # the head goes into the next save, so a load brings this back
        self._spawn(self.link.send(MemoryCmd(branch=self.branch, seq=seq)))

    def mind(self, npc: str) -> Mind:
        """What the npc saw, read from the save branch the first time."""
        npc = npc.upper()
        mind = self._minds.get(npc)
        if mind is None:
            latest: dict[str, Sighting] = {}
            sounds: list[Heard] = []
            for raw in self.memory.seen(self.branch, npc) if self.branch is not None else []:
                try:
                    record = json.loads(raw)
                    if not isinstance(record, dict):
                        raise ValueError("observation is not an object")
                    if record.get("observation") == "heard":
                        sounds.append(Heard.of(record))
                        continue
                    s = Sighting.of(record)
                except (ValueError, TypeError, KeyError) as err:
                    log.warning("sightings of %s: a broken record: %s", npc, err)
                    continue
                latest[s.key] = s
            people = [s for s in latest.values() if not s.trace_kind]
            traces = {s.who: s for s in latest.values() if s.trace_kind}
            mind = self._minds[npc] = Mind(
                sorted(people, key=lambda s: s.start)[-KEEP:], traces=traces, heard=sounds[-KEEP:]
            )
        return mind

    # --- in words ---------------------------------------------------------------------------------

    def witnesses(self) -> list[str]:
        """The npc's in mind now that have seen the hero."""
        return [npc for npc, mind in self._minds.items() if any(s.who == HERO for s in mind.sightings)]

    def hero_trail(self, npc: str) -> list[Sighting]:
        """The hero's latest sightings, the latest first."""
        return [s for s in reversed(self.mind(npc).sightings) if s.who == HERO]

    def words(self, s: Sighting, now: int | None = None, bare: bool = True) -> str:
        """A sighting in words: «сегодня 08:10–08:14, около часа назад — на пастбище у…; без доспеха».
        `bare=False` leaves out «без доспеха» when he wore nothing all along (a witness's answer)."""
        now = max(self.now if now is None else now, s.end)
        parts = [", потом ".join(s.places) or "где-то"]
        if s.trace_kind:
            age = ""
            if s.trace_kind == "broken":
                age = "; давность при наблюдении " + (
                    f"{s.trace_age} мин" if s.trace_age is not None else "неизвестна"
                )
            return f"{_observed_when(s.start, now)} — {trace_name(s.trace_kind, s.name)}; {parts[0]}{age}"
        if s.who == HERO:
            parts.append(_wore(s) if bare or any(s.armor) else "")
        parts += [NOTABLE[n] for n in s.notable if n in NOTABLE]
        return f"{when(s.start, s.end, now)} — " + "; ".join(p for p in parts if p)

    def _other(self, s: Sighting, now: int) -> str:
        who = f"тело: {s.name}" if "dead" in s.notable else s.name
        return f"{who} — {self.words(s, now)}"

    # --- TurnExtension --------------------------------------------------------------------------

    async def prepare(self, npc: str) -> None:
        pass  # perception asks the view afresh before the turn; it comes here as any view

    def situation(self, npc: str) -> str | None:
        npc = npc.upper()
        if self.branch is None:
            return None
        lines = []
        shown: dict[str, Sighting] = {}
        trail = self.hero_trail(npc)[:HERO_SHOWN]
        if trail:
            lines.append(
                "Героя:" if self.knows_hero(npc) else "Героя (имени его ты не знаешь, но в лицо узнаёшь):"
            )
            for i, s in enumerate(trail, 1):
                shown[f"В{i}"] = s
                lines.append(f"- В{i}: {self.words(s)}.")
        traces = sorted(self.mind(npc).traces.values(), key=lambda s: s.start, reverse=True)[:TRACES_SHOWN]
        if traces:
            lines.append("Следы, которые ты видел:")
            for s in traces:
                sid = f"В{len(shown) + 1}"
                shown[sid] = s
                lines.append(f"- {sid}: {self.words(s)}.")
        deeds = self.crimes.seen(npc) if self.crimes is not None else []
        self._crime_shown[npc] = {f"Д{i}": deed for i, deed in enumerate(reversed(deeds), 1)}
        if deeds:
            lines.append("Дела героя, которые ты видел сам (улики для catch_lie):")
            lines += [
                f"- {sid}: ты видел, как {self.crimes.describe(deed)}."
                for sid, deed in self._crime_shown[npc].items()
            ]
        view = self.desk.view(npc)
        in_sight = {o.id.upper() for o in view.sees} if view is not None else set()
        others: list[Sighting] = []
        told: set[str] = set()
        for s in reversed(self.mind(npc).sightings):
            if s.who == HERO or s.who in told or not s.known:
                continue
            told.add(s.who)
            if s.who not in in_sight and self.now - s.end <= FORGET_MIN:
                others.append(s)
            if len(others) >= PEOPLE_SHOWN:
                break
        if others:
            lines.append("Знакомых, последний раз:")
            lines += [f"- {self._other(s, self.now)}." for s in others]
        if noted := self.noted(npc, in_sight):
            lines.append("Недавно видел, сейчас не видишь:")
            lines += [
                f"- {s.name} (id {s.who}) — {self.words(s)}{f'; {s.doing}' if s.doing else ''}."
                for s in noted
            ]
        self._shown[npc] = shown
        seen = "Что ты видел сам, своими глазами (не с чужих слов):\n" + "\n".join(lines) if lines else ""
        sounds = self.mind(npc).heard[-HEARD_SHOWN:]
        if sounds:
            heard = "Что ты слышал сам (звук не говорит, кто его вызвал):\n" + "\n".join(
                f"- {_observed_when(sound.time, max(self.now, sound.time))} — слышал грохот {sound.place}."
                for sound in reversed(sounds)
            )
            return "\n".join(part for part in (seen, heard) if part)
        return seen or None

    def noted(self, npc: str, in_sight: set[str] | None = None) -> list[Sighting]:
        """The strangers and creatures it saw lately and does not see now, the latest first: the latest
        sighting of each, of the last FADE_MIN game minutes."""
        if in_sight is None:
            view = self.desk.view(npc)
            in_sight = {o.id.upper() for o in view.sees} if view is not None else set()
        out: list[Sighting] = []
        told: set[str] = set()
        for s in reversed(self.mind(npc).sightings):
            if s.known or s.who in told:
                continue
            told.add(s.who)
            if s.who not in in_sight and self.now - s.end <= FADE_MIN:
                out.append(s)
            if len(out) >= NOTED_SHOWN:
                break
        return out

    def targets(self, npc: str) -> dict[str, str]:
        """Whom it saw lately and may point its intent to though it does not see them now (perception.py):
        id -> how it knows him. Not the dead."""
        return {
            s.who: f"{s.name}, видел {ago(self.now - s.end)} — {', потом '.join(s.places) or 'где-то'}"
            for s in self.noted(npc)
            if "dead" not in s.notable
        }

    def where_seen(self, npc: str, who: str) -> str | None:
        """The place (the book's id) where the npc last saw `who`, lately."""
        who = who.upper()
        for s in reversed(self.mind(npc).sightings):
            if s.who == who:
                ok = self.now - s.end <= FADE_MIN and s.place_ids
                return s.place_ids[-1] if ok else None
        return None

    all_tools: tuple[Tool, ...] = (CATCH_TOOL,)

    def tools(self, npc: str) -> list[Tool]:
        told = self.hearsay is not None and self.hearsay.any(npc)
        deeds = self.crimes is not None and bool(self.crimes.seen(npc))
        return [CATCH_TOOL] if self.hero_trail(npc) or self.mind(npc).traces or told or deeds else []

    async def handle(
        self, dialogue: DialogueManager, conv: Conversation, call: ToolCall, snap: Snapshot | None
    ) -> ToolOutcome | None:
        if call.name != CATCH:
            return None
        sid = _sid(str(call.arguments.get("seen", "")))
        said = str(call.arguments.get("said", "")).strip().strip("«»\"' .")
        extra: dict[str, Any] = {}
        told: Told | None = None
        if sid.startswith("С"):
            told = self.hearsay.told(conv.npc, sid) if self.hearsay is not None else None
            if told is None:
                return ToolOutcome(
                    "Отклонено: такого тебе не рассказывали — укажи строку из «Что тебе рассказали о герое "
                    "другие» (С1, С2…).",
                    "rejected",
                )
            if not told.sightings and not told.crime:
                return ToolOutcome(
                    f"Отклонено: со слов: {told.witness_name} — героя не видели, уличить нечем.", "rejected"
                )
            how = f"узнал со слов: {told.words}"
            extra = {"witness": told.witness, "witness_name": told.witness_name, "testimony": told.id}
            if told.crime:
                if self.crimes is None or self.crimes.crime(told.crime) is None:
                    return ToolOutcome("Отклонено: в этой ветке нет такого дела.", "rejected")
                extra["crime"] = told.crime
        elif sid.startswith("Д"):
            deed = self._crime_shown.get(conv.npc, {}).get(sid)
            if deed is None or self.crimes is None:
                return ToolOutcome(
                    "Отклонено: такого дела ты сам не видел — укажи строку Д1, Д2….", "rejected"
                )
            how = f"сам видел: {self.crimes.describe(deed)}"
            extra = {"crime": deed["id"]}
        else:
            s = self._shown.get(conv.npc, {}).get(sid)
            if s is None:
                return ToolOutcome(
                    "Отклонено: такого ты сам не видел — укажи строку из «Что ты видел сам» (В1, В2…).",
                    "rejected",
                )
            how = f"сам видел: {self.words(s)}" if s.trace_kind else f"сам видел его {self.words(s)}"
        if not said:
            return ToolOutcome("Отклонено: скажи, в каких его словах ложь.", "rejected")
        # his words: in his latest lines, or those a witness was asked about (checked when it was asked)
        asked = told is not None and _matches(said, roots(told.said))
        if not asked and not heard(conv, said):
            return ToolOutcome(
                "Отклонено: он этого не говорил — уличить можно только в его собственных словах.", "rejected"
            )
        key = (conv.npc, " ".join(sorted(roots(said))))
        if key in self._caught or self._caught_before(conv.npc, said):
            return ToolOutcome("Отклонено: в этом ты его уже уличил.", "rejected")
        self._caught.add(key)
        lie = Lie(
            f"SAID_{conv.npc}_{self.now}",
            conv.npc,
            conv.persona.name,
            said,
            "",
            said=True,
            crime=str(extra.get("crime", "")),
        )

        async def job() -> tuple[str, bool]:
            await caught(self.link, dialogue, self.chronicle, self.journal, lie, how, extra=extra)
            return f"Ты уличил героя во лжи: «{said}»", True

        dialogue.after_speech(conv, job)
        return ToolOutcome("Принято: ты уличил его; ты это запомнишь, люди вокруг услышат.", "accepted")

    def _caught_before(self, npc: str, said: str) -> bool:
        """Its words were caught as a lie by this npc already, in this branch (a load forgets `_caught`)."""
        words = roots(said)
        return any(
            f.get("npc") == npc and f.get("said") and roots(str(f.get("title", ""))) == words
            for f in self.chronicle.facts("caught")
        )

    def recall(self, npc: str, query: str) -> str | None:
        """The sightings of whom or where the query names: «где ты видел героя», «Хильда»."""
        q = query.lower()
        words = roots(query)
        hits = []
        mind = self.mind(npc)
        observations = sorted([*mind.sightings, *mind.traces.values()], key=lambda s: s.start, reverse=True)
        for s in observations:
            if s.who == HERO:
                hit = "геро" in q
            elif s.known:
                hit = s.name.lower()[:4] in q
            else:  # «бандиты»: by its look
                hit = any(w in words for w in roots(s.name) if len(w) >= 4)
            hit = hit or any(w in words for p in s.places for w in roots(p) if len(w) >= 5)
            if hit:
                hits.append(s)
            if len(hits) >= RECALLED:
                break
        if not hits:
            return None
        lines = [
            f"- героя: {self.words(s)}" if s.who == HERO else f"- {self._other(s, self.now)}" for s in hits
        ]
        return "Что ты видел сам:\n" + "\n".join(lines)

    def _spawn(self, coro: Coroutine[Any, Any, Any]) -> None:
        async def run() -> None:
            try:
                await coro
            except GameError:
                pass  # the game is gone; it learns the head again on reconnect

        task = asyncio.create_task(run())
        self._background.add(task)
        task.add_done_callback(self._background.discard)


# --- time in words ------------------------------------------------------------------------------


def when(start: int, end: int, now: int) -> str:
    """«сейчас, с 09:12», «сегодня 08:10–08:14, около часа назад», «вчера 19:05–19:40, вечером»."""
    span = _hhmm(start) + (f"–{_hhmm(end)}" if end - start >= 2 else "")
    if now - end <= 1:
        return f"сейчас, с {_hhmm(start)}" if end - start >= 2 else "сейчас"
    days = now // 1440 - start // 1440
    if days == 0:
        return f"сегодня {span}, {ago(now - end)}"
    label = (
        "вчера"
        if days == 1
        else "позавчера"
        if days == 2
        else f"{days} {_plural(days, 'день', 'дня', 'дней')} назад"
    )
    return f"{label} {span}, {_part_of_day(start % 1440)}"


def _observed_when(time: int, now: int) -> str:
    """A single observation keeps its clock time even while it is still fresh."""
    words = when(time, time, now)
    return f"сегодня {_hhmm(time)}, только что" if words == "сейчас" else words


def ago(minutes: int) -> str:
    if minutes < 3:
        return "только что"
    if minutes < 50:
        return f"{max(5, round(minutes / 5) * 5)} минут назад"
    if minutes < 90:
        return "около часа назад"
    h = round(minutes / 60)
    return f"{h} {_plural(h, 'час', 'часа', 'часов')} назад"


def _hhmm(t: int) -> str:
    t %= 1440
    return f"{t // 60:02d}:{t % 60:02d}"


def _part_of_day(minute: int) -> str:
    h = minute // 60
    return "утром" if 5 <= h < 12 else "днём" if 12 <= h < 17 else "вечером" if 17 <= h < 22 else "ночью"


def _plural(n: int, one: str, few: str, many: str) -> str:
    if n % 10 == 1 and n % 100 != 11:
        return one
    if 2 <= n % 10 <= 4 and not 12 <= n % 100 <= 14:
        return few
    return many


# --- helpers ----------------------------------------------------------------------------------


def _add(items: list[str], item: str, limit: int = 99) -> None:
    if item not in items and len(items) < limit:
        items.append(item)


def _noticeable(o: Look) -> bool:
    """A stranger or a creature worth remembering: a body, one hostile to the npc or the hero, one with a
    weapon drawn, a beast at someone. Not the sheep at pasture nor the passers-by."""
    if o.kind == "body":
        return True
    if o.att in HOSTILE or o.att_hero in HOSTILE:
        return True
    if o.kind == "human":
        return o.weapon in WEAPON
    return o.state == "ZS_MM_ATTACK"


def _notable(look: Look) -> list[str]:
    flags = []
    if look.kind == "body" or look.body == "dead":
        flags.append("dead")
    elif look.body == "down":
        flags.append("down")
    if look.kind == "human" and look.weapon in WEAPON:
        flags.append("weapon")
    if look.hurt and look.kind != "body":
        flags.append("hurt")
    if look.state == "ZS_ATTACK":
        flags.append("fight")
    if look.att in HOSTILE and look.kind != "body":
        flags.append("hostile")
    return flags


def _wore(s: Sighting) -> str:
    """What the hero wore, as the npc saw it: «без доспеха», «на нём: Лёгкий доспех ополчения (по виду
    ополченец)»."""
    worn = []
    for item, name in zip(s.armor, s.armor_names, strict=False):
        if not item:
            worn.append("без доспеха")
            continue
        look = armor_look(item)
        worn.append(f"на нём: {name}" + (f" (по виду {look[0]})" if look else ""))
    return ", потом ".join(worn)


def _sid(text: str) -> str:
    """The id of a sighting as the model wrote it: «В2», «B2» (Latin), «2»; of what it was told: «С1»,
    «C1»."""
    t = text.strip().upper().replace("B", "В").replace("C", "С").replace("D", "Д")
    return t if t.startswith(("В", "С", "Д")) else f"В{t}"


def heard(conv: Conversation, said: str) -> bool:
    """The hero did say it: most of its words are in his latest lines (this one too)."""
    lines = [e.content for e in conv.memory.entries if e.role == "user"][-HEARD:]
    if conv.trigger is not None and conv.trigger.role == "user":
        lines.append(conv.trigger.content)
    heard: set[str] = set()
    for line in lines:
        heard |= roots(line)
    return _matches(said, heard)


def _matches(said: str, heard: set[str]) -> bool:
    """Most of the words of `said` are among `heard` (word roots)."""
    words = roots(said)
    return bool(words) and 2 * sum(1 for w in words if w in heard) >= len(words)
