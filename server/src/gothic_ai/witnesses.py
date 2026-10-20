"""Asking a witness (docs/npc-tools.md, wave 2 step 3): an npc checks the hero's words by asking someone it
knows what it saw of him.

The hero tells Lobart he was in the city all morning. Lobart did not see him, but Hilda may have. Lobart's
conversational model calls `ask_witness{who, said}`, and code does the rest.

Whom it may ask: the people it knows (whom it meets or has dealings with, its order's people), alive. How
depends on where they are:
- near, within NEAR_CM: it calls them over («иди сюда»), they come, they talk;
- a walk away, within WALK_CM: it goes to them and comes back, as check_claim does;
- far, if they meet by their daily routines: it asks at their next meeting (Tie.occasions). The answer
  comes then as a fact and a remark, with no model call (principle 5: far away, the outcome as a fact).
Plot npc's and those at their posts are neither called over nor walk (storyguard.py), nor is anyone under
our control already (following the hero, held for a job); a far question moves nobody.

What the witness says is code's. Its evidence is what it saw itself (sightings.py) of the day before the
hero's words. Accepted cover deals hide their evidence and supply an alibi; a timid witness who knows
the hero's violence denies seeing anything. Near the hero, the witness answers aloud in its own voice
(one call, within the budget), and the asker reacts (one more).

The answer is a fact, `testimony`: the asker, the witness, the hero's words it asked about and when he said
them, what the witness saw and what it told. Cover changes only `told` and `crime_told`, retaining the
truth and the cover's source in the fact. The asker reads it as a line С1… in «Что тебе рассказали о герое
другие». catch_lie
(sightings.py) takes a С line as it takes a В line: the hero did say those words, and the witness did see
him. Then it is a lie caught «со слов: Хильда», as heavy as one the npc saw with its own eyes (the
author's call, 2026-10-13).

A far answer that saw the hero is also a concern (concerns.py) for CONCERN_MIN: the npc may come up to him
about it (initiative.py), once.
"""

from __future__ import annotations

import asyncio
import logging
from collections.abc import Callable, Mapping
from dataclasses import dataclass, field
from typing import TYPE_CHECKING, Any

from .budget import Budget
from .characters import Card
from .chronicle import Chronicle, Fact
from .concerns import RANK, Concern
from .fear import afraid
from .game import GameError, GameLink
from .journal import Journal
from .knowledge import roots
from .llm import Tool, ToolCall
from .npc_cheat import betrayed, keeps
from .people_knowledge import PeopleKnowledge
from .promise_breach import broken_promises, silence_facts
from .protocol import GameMessage, Loading, Session, Snapshot
from .sightings import Sighting, Sightings, Told, heard
from .storyguard import StoryGuard
from .turn import ToolOutcome

if TYPE_CHECKING:
    from .crimes import Crimes
    from .dialogue import Conversation, DialogueManager
    from .perception import PerceptionDesk
    from .social import Social

log = logging.getLogger(__name__)

HERO = "PLAYER"
DAY = 24 * 60
NEAR_CM = 1500  # called over: within earshot of the asker (perception.HEARD_CM)
WALK_CM = 6000  # a walk away: it goes and comes back (vanilla.CHECK_CM)
TALK_CM = 500  # this close they talk where they stand: nobody has to move
COME_CM = 200  # a witness called over comes this close
GO_CM = 400  # the asker goes up to a witness this close (indoors, by a stove, a path may not get closer)
BACK_CM = 300  # the asker comes back to the hero this close
HEARS_CM = 1000  # the hero hears the witness's answer this close (talk.OVERHEAR_CM)
MEET_WITHIN = 2 * DAY  # a far witness must meet the asker within this
TRAIL_MIN = DAY  # what a witness tells of: the day before the hero's words
JUST_MIN = 10  # sightings that began this shortly before his words are the scene at hand, not news
TOLD = 3  # sightings a witness tells of
SHOWN = 3  # testimonies in the situation
KEPT_MIN = 2 * DAY  # older testimonies are not in the situation
CANDIDATES = 4  # near and a walk away, the nearest
FAR = 2  # and far, by their meetings: the farm's people would crowd out the city's
ASKED = 16  # acquaintances asked about in one story_state
ASK_AGAIN_MIN = 60  # the same question to the same witness within this: the old answer
CONCERN_MIN = 12 * 60  # a far answer is on the asker's mind this long
WAIT_S = 20.0  # for the witness's words
STATE_S = 2.5  # story_state in prepare: within the turn's PREPARE_S
TICK_S = 5.0

ASK = "ask_witness"
HERO_ASK_TOOL = Tool(
    ASK,
    "Расспросить знакомого о герое, когда сам ты не видел, а его слова проверить хочешь (кого можно спросить "
    "и где он — в обстановке). Рядом — окликнешь, и он подойдёт; поодаль — сходишь к нему и вернёшься; "
    "далеко — спросишь при встрече, ответ узнаешь потом. Сначала скажи вслух, кого и о чём спросишь. Ответ "
    "придёт ремаркой и строкой С1 в «Что тебе рассказали о герое другие»; уличить по ней — catch_lie.",
    {
        "type": "object",
        "properties": {
            "who": {"type": "string", "description": "кого спрашиваешь: имя, как в обстановке"},
            "said": {
                "type": "string",
                "description": "его слова, которые проверяешь, коротко: «всё утро был в городе»",
            },
        },
        "required": ["who", "said"],
        "additionalProperties": False,
    },
)

ASK_TOOL = Tool(
    ASK,
    HERO_ASK_TOOL.description + " С about — что свидетель видел о другом человеке; не о герое.",
    {
        **HERO_ASK_TOOL.parameters,
        "properties": {
            **HERO_ASK_TOOL.parameters["properties"],
            "about": {
                "type": ["string", "null"],
                "description": "о ком спрашиваешь: имя человека из обстановки; null — о герое",
            },
        },
        "required": ["who", "said", "about"],
    },
)


@dataclass(frozen=True)
class Candidate:
    """Someone the asker may ask, as it is now."""

    npc: str
    name: str
    tier: str  # near | walk | far
    cm: float | None  # how far from the asker
    moves: bool  # may be called over (not a plot npc, not at its post, not under our control)
    meets: int | None = None  # far: the game minute of their next meeting


@dataclass
class Testimony:
    """What a witness told the asker of the hero (the fact `testimony`)."""

    id: str
    npc: str
    name: str
    witness: str
    witness_name: str
    said: str  # the hero's words it asked about
    tier: str
    minute: int  # when it was told
    at: int  # when the hero said those words
    saw: list[Sighting] = field(default_factory=list)
    told: list[Sighting] = field(default_factory=list)
    crime_saw: list[str] = field(default_factory=list)  # actual crime_seen, never rumors or suspicions
    crime_told: list[str] = field(default_factory=list)
    cover: str = ""  # deal id, or fear; saw and crime_saw retain the truth
    vouch: str = ""
    about: str = HERO  # the person asked about; old facts used about as a timestamp
    about_name: str = ""

    @classmethod
    def of(cls, f: Fact) -> Testimony:
        minute = int(f.get("minute", 0))
        return cls(
            str(f.get("id", "")),
            str(f.get("npc", "")).upper(),
            str(f.get("name", "")),
            str(f.get("witness", "")).upper(),
            str(f.get("witness_name", "")),
            str(f.get("said", "")),
            str(f.get("tier", "")),
            minute,
            int(f.get("at", f.get("about", minute) if not isinstance(f.get("about"), str) else minute)),
            [Sighting.of(r) for r in f.get("saw", [])],
            [Sighting.of(r) for r in f.get("told", [])],
            [str(crime) for crime in f.get("crime_saw", [])],
            [str(crime) for crime in f.get("crime_told", [])],
            str(f.get("cover", "")),
            str(f.get("vouch", "")),
            str(f.get("about") or HERO).upper() if isinstance(f.get("about"), str) else HERO,
            str(f.get("about_name", "")),
        )


@dataclass
class Inquiry:
    """A question to a far witness, asked at their next meeting."""

    id: str
    npc: str
    name: str
    witness: str
    witness_name: str
    said: str
    minute: int  # when the hero said it and the asker meant to ask
    meets: int  # their meeting, when it is asked
    about: str = HERO
    about_name: str = ""


class Witnesses:
    """ask_witness for the conversational model (a TurnExtension); the far questions on the game's clock."""

    def __init__(
        self,
        link: GameLink,
        dialogue: DialogueManager,
        sightings: Sightings,
        desk: PerceptionDesk,
        chronicle: Chronicle,
        guard: StoryGuard,
        *,
        social: Callable[[], Social | None] = lambda: None,
        budget: Budget | None = None,
        hero_cm: Callable[[str], float | None] = lambda npc: None,
        state_of: Callable[[str], str] = lambda npc: "",
        controlled: Callable[[str], bool] = lambda npc: False,
        partner: Callable[[], str | None] = lambda: None,
        talking: Callable[[str, bool], None] | None = None,
        journal: Journal | None = None,
        crimes: Crimes | None = None,
        cover: bool = True,
        cards: Mapping[str, Card] | None = None,
        people_knowledge: PeopleKnowledge | None = None,
        npc_crimes: bool = True,
    ) -> None:
        self.link = link
        self.dialogue = dialogue
        self.sightings = sightings
        self.desk = desk
        self.chronicle = chronicle
        self.guard = guard
        self.social = social
        self.budget = budget
        self.hero_cm = hero_cm  # how far the hero is from an npc (the roster), None: not around
        self.state_of = state_of  # an npc's script state (the roster)
        self.controlled = controlled  # an npc under our control already (attached: following, held)
        self.partner = partner  # whom the hero talks to now (talk.py)
        self.talking = talking  # the planner leaves an npc alone while it talks (planner.talking)
        self.journal = journal or Journal()
        self.crimes = crimes if crimes is not None else sightings.crimes
        self.cover_enabled = cover
        self.npc_crimes_enabled = npc_crimes
        self.all_tools = (ASK_TOOL if npc_crimes else HERO_ASK_TOOL,)
        self.cards = cards if cards is not None else {}
        self.people_knowledge = people_knowledge or PeopleKnowledge(chronicle)
        self.on_change: list[Callable[[str], None]] = []  # an asker whose concerns changed with time
        self.now = 0  # the game's clock as the latest story_state had it (minutes); 0: not known
        self.inquiries: dict[str, Inquiry] = {}  # far questions not asked yet
        self._near: dict[str, list[Candidate]] = {}  # asker -> whom it may ask, from the last prepare
        self._shown: dict[str, dict[str, Told]] = {}  # asker -> exact testimony evidence row, last turn
        self._npc_rows: set[tuple[str, str]] = set()  # NPC testimony cannot catch a hero lie by itself
        self._asking: set[str] = set()  # askers with a question under way
        self._expired: set[str] = set()  # far answers whose concern is over, notified
        sightings.hearsay = self
        link.listeners.append(self._on_message)
        chronicle.on_fact.append(self._on_fact)
        self._load()

    def _on_message(self, msg: GameMessage) -> None:
        match msg:
            case Session():
                self.now = 0
                self._load()
            case Loading():
                self.now = 0
                self._near.clear()
                self._shown.clear()
                self._npc_rows.clear()
                self._asking.clear()

    def _load(self) -> None:
        """The far questions of the branch not asked yet, from its facts."""
        self.inquiries.clear()
        self._near.clear()
        self._shown.clear()
        self._npc_rows.clear()
        self._expired.clear()
        done = {
            f.get("id") for f in self.chronicle.facts() if f.get("fact") in ("testimony", "inquiry_lapsed")
        }
        for f in self.chronicle.facts("inquiry"):
            if f.get("id") not in done:
                q = _inquiry(f)
                if q.about == HERO or self.npc_crimes_enabled:
                    self.inquiries[q.id] = q

    # --- whom it may ask ---------------------------------------------------------------------------

    async def prepare(self, npc: str) -> None:
        npc = npc.upper()
        self._near[npc] = []  # a prepare cut short offers nobody, not the last turn's list
        try:
            self._near[npc] = await self.candidates(npc)
        except (GameError, TimeoutError) as err:
            log.info("witnesses of %s: %s", npc, err)

    async def candidates(self, npc: str) -> list[Candidate]:
        """The people it knows that it may ask now, the nearest first, the far ones by their meeting."""
        social = self.social()
        ties = social.ties.get(npc, {}) if social is not None else {}  # read only: Social.tie() makes ties
        names = dict(social.names) if social is not None else {}
        view = self.desk.view(npc)
        seen = []
        for o in view.sees if view is not None else []:
            if o.kind == "human" and self.desk.knows(npc, o):
                seen.append(o.id.upper())
                names.setdefault(o.id.upper(), o.name)
        dead = {str(f.get("npc", "")).upper() for f in self.chronicle.facts("died")}
        known = [n for n in dict.fromkeys([*seen, *sorted(ties)]) if n not in (npc, HERO) and n not in dead]
        if not known:
            return []
        state = await self.link.story_state(npcs=known[:ASKED], near=npc, timeout=STATE_S)
        self._clock(state)
        conv = self.dialogue.conversations.get(npc)
        following = conv is not None and conv.intent.goal == "follow_hero"
        walks = self.guard.body(npc, self.state_of(npc)) is None and not following
        out = []
        for other in known[:ASKED]:
            st = (state.get("npcs") or {}).get(other) or {}
            if st.get("state") != "alive":
                continue
            name = st.get("name") or names.get(other) or other
            cm = float(st["distance"]) if st.get("distance") is not None else None
            moves = self.guard.body(other, self.state_of(other)) is None and not self.controlled(other)
            if cm is not None and cm <= NEAR_CM and (moves or cm <= TALK_CM or walks):
                out.append(Candidate(other, name, "near" if moves or cm <= TALK_CM else "walk", cm, moves))
            elif cm is not None and cm <= WALK_CM and walks:
                out.append(Candidate(other, name, "walk", cm, moves))
            elif (tie := ties.get(other)) is not None and self.now:
                # their next meeting: one going on now is no meeting to ask at, they are far apart
                later = tie.occasions(npc, other, self.now + 1, self.now + MEET_WITHIN)
                meets = next((m for m in later if m > self.now + 1), None)
                if meets is not None:
                    out.append(Candidate(other, name, "far", cm, moves, meets))
        close = sorted((c for c in out if c.tier != "far"), key=lambda c: (c.tier != "near", c.cm or 0))
        far = sorted((c for c in out if c.tier == "far"), key=lambda c: c.meets or 0)
        return close[:CANDIDATES] + far[:FAR]

    # --- the conversation (TurnExtension) ----------------------------------------------------------

    def situation(self, npc: str) -> str | None:
        npc = npc.upper()
        parts = []
        near = self._near.get(npc, [])
        if near:
            lines = [
                f"- {c.name} — {self._where(c)}{self._warning(npc, c.npc)}" for c in near
            ]
            parts.append("Кого из знакомых можно расспросить о герое (ask_witness):\n" + "\n".join(lines))
        waiting = [q for q in self.inquiries.values() if q.npc == npc]
        if waiting:
            lines = [
                f"- {q.witness_name} — "
                + (f"о человеке {q.about_name}, " if q.about != HERO else "")
                + f"о его словах «{q.said}» (встретитесь {self._at(q.meets)})"
                for q in waiting
            ]
            parts.append("Ты собирался расспросить при встрече:\n" + "\n".join(lines))
        shown: dict[str, Told] = {}
        self._npc_rows = {row for row in self._npc_rows if row[0] != npc}
        for t in self.testimonies(npc):
            first = len(shown)
            if t.cover:
                shown[f"С{len(shown) + 1}"] = Told(
                    t.witness, t.witness_name, [], self._cover_words(t), t.said, t.id
                )
            # A testimony may include several deeds. Each gets its own evidence row so catch_lie
            # refers to the exact crime, rather than guessing which one the hero denied.
            deeds = self._deeds(t)
            for crime in [str(f["id"]) for f in deeds]:
                shown[f"С{len(shown) + 1}"] = Told(
                    t.witness, t.witness_name, [], self._told(t, crime), t.said, t.id, crime
                )
            if t.told or (not deeds and not t.cover):
                shown[f"С{len(shown) + 1}"] = Told(
                    t.witness, t.witness_name, t.told, self._told(t, ""), t.said, t.id
                )
            if t.about != HERO:
                self._npc_rows.update((npc, sid) for sid in list(shown)[first:])
        self._shown[npc] = shown
        if shown:
            lines = [
                f"- {sid}: {t.words}.{self._warning(npc, t.witness)}" for sid, t in shown.items()
            ]
            subject = "" if any(row[0] == npc for row in self._npc_rows) else " о герое"
            parts.append(
                f"Что тебе рассказали{subject} другие (с их слов, сам ты этого не видел):\n"
                + "\n".join(lines)
            )
        return "\n".join(parts) or None

    def _warning(self, npc: str, witness: str) -> str:
        warning = self.people_knowledge.warning(npc, witness)
        return f" {warning}" if warning else ""

    all_tools: tuple[Tool, ...] = (ASK_TOOL,)

    def tools(self, npc: str) -> list[Tool]:
        return list(self.all_tools) if self._near.get(npc.upper()) else []

    async def handle(
        self, dialogue: DialogueManager, conv: Conversation, call: ToolCall, snap: Snapshot | None
    ) -> ToolOutcome | None:
        if call.name != ASK:
            return None
        npc = conv.npc
        who = str(call.arguments.get("who", "")).strip()
        said = str(call.arguments.get("said", "")).strip().strip("«»\"' .")
        about, about_name = HERO, ""
        if "about" in call.arguments:
            if not self.npc_crimes_enabled:
                return ToolOutcome("Отклонено: расспросы о других людях выключены.", "rejected")
            if call.arguments["about"] is not None:
                subject = self._subject(npc, str(call.arguments["about"]).strip())
                if subject is None:
                    return ToolOutcome(
                        "Отклонено: about — имя человека из обстановки, кроме героя.", "rejected"
                    )
                about, about_name = subject
        near = self._near.get(npc, [])
        c = _find(near, who)
        if c is None:
            names = ", ".join(c.name for c in near) or "никого"
            return ToolOutcome(
                f"Отклонено: спросить можно только знакомых из обстановки: {names}.", "rejected"
            )
        if not said:
            return ToolOutcome("Отклонено: скажи, какие его слова проверяешь.", "rejected")
        if not heard(conv, said):
            return ToolOutcome(
                "Отклонено: он этого не говорил — проверить можно только его собственные слова.", "rejected"
            )
        if npc in self._asking:
            return ToolOutcome("Отклонено: ты уже ждёшь ответа на свой вопрос.", "rejected")
        if any(q.npc == npc and q.witness == c.npc and q.about == about for q in self.inquiries.values()):
            return ToolOutcome(f"Отклонено: ты уже собирался спросить при встрече: {c.name}.", "rejected")
        if (old := self._asked(npc, c.npc, said, about)) is not None:
            return ToolOutcome(f"Ты уже спрашивал: {self._told(old)}.", "read")
        now = self._now()
        if c.tier == "far":
            q = Inquiry(
                f"{npc}:{c.npc}:{about}:{now}", npc, conv.persona.name, c.npc, c.name, said, now,
                c.meets or 0,
                about, about_name,
            )
            self._inquire(q)
            return ToolOutcome(
                f"Принято: спросишь при встрече ({self._at(q.meets)}): {c.name}; ответ узнаешь потом.",
                "accepted",
            )
        if self.dialogue.speaking(c.npc):
            return ToolOutcome(f"Отклонено: {c.name} сейчас говорит.", "rejected")
        try:
            view = await self.desk.fresh(c.npc)
        except (GameError, TimeoutError):
            view = None
        if view is not None and (why := _busy(view.state, view.body)) is not None:
            return ToolOutcome(f"Отклонено: {c.name} {why}.", "rejected")
        self._asking.add(npc)
        asking = conv.reply
        self.journal.add(
            "ask_witness", npc=npc, name=conv.persona.name, witness=c.npc, tier=c.tier, said=said
        )

        async def job() -> tuple[str, bool]:
            try:
                return await self._ask(conv, c, said, now, asking, about, about_name)
            finally:
                self._asking.discard(npc)

        dialogue.after_speech(conv, job)
        how = "окликнешь — подойдёт" if c.tier == "near" else "дойдёшь, спросишь и вернёшься"
        return ToolOutcome(f"Принято: после реплики {how}; ответ придёт ремаркой.", "accepted")

    def recall(self, npc: str, query: str) -> str | None:
        """What it was told of the hero, by whom the query names, or all of it."""
        q = query.lower()
        recent = self.testimonies(npc, KEPT_MIN * 3, 10)
        told = [t for t in recent if t.witness_name.lower()[:4] in q
                or (t.about_name and t.about_name.lower()[:4] in q)]
        told = told or ([t for t in recent if t.about == HERO] if "геро" in q else [])
        if not told:
            return None
        subject = "" if any(t.about != HERO for t in told) else " о герое"
        return f"Что тебе рассказали{subject} другие:\n" + "\n".join(f"- {self._told(t)}" for t in told)

    # --- for sightings.catch_lie (sightings.Hearsay) -----------------------------------------------

    def told(self, npc: str, sid: str) -> Told | None:
        """The testimony behind a С line of the npc's latest situation."""
        npc = npc.upper()
        if (npc, sid) in self._npc_rows:
            return None
        return self._shown.get(npc, {}).get(sid)

    def any(self, npc: str) -> bool:
        """Whether a witness told it of seeing the hero (catch_lie is offered then)."""
        return any(t.about == HERO and (t.told or t.cover or self._deeds(t)) for t in self.testimonies(npc))

    # --- concerns.py -----------------------------------------------------------------------------

    def concerns(self, npc: str) -> list[Concern]:
        """A far answer that saw the hero, not spoken of to him yet: the npc may come up to him about it."""
        npc = npc.upper()
        if not self._now():
            return []  # the clock is not known yet (a load): nothing is fresh or stale
        spoken = {f.get("occasion") for f in self.chronicle.facts("initiative") if f.get("npc") == npc}
        caught = [f for f in self.chronicle.facts("caught") if f.get("npc") == npc and f.get("said")]
        settled = {f.get("testimony") for f in caught}
        words = {frozenset(roots(str(f.get("title", "")))) for f in caught}
        out = []
        for t in self.testimonies(npc, CONCERN_MIN):
            key = f"witness:{t.id}"
            if (t.about != HERO or t.tier != "far" or not (t.told or self._deeds(t))
                    or key in spoken or t.id in settled):
                continue
            if frozenset(roots(t.said)) in words:
                continue  # caught for those words by what it saw itself
            ru = f"{self._told(t)} — а он говорил тебе «{t.said}»"
            en = "tell the hero what a witness saw of him; he may have lied to you"
            out.append(Concern(key, npc, RANK["witness"], en, ru))
        return out

    # --- asking -----------------------------------------------------------------------------------

    async def _ask(
        self, conv: Conversation, c: Candidate, said: str, at: int, asking: asyncio.Task[None] | None,
        about: str = HERO, about_name: str = "",
    ) -> tuple[str, bool]:
        """Near: it calls the witness over; a walk: it goes to the witness and back. Then the witness tells
        what it saw (aloud, if the hero hears) and the asker reacts."""
        npc, name = conv.npc, conv.persona.name
        wconv = self.dialogue.open(c.npc, c.name)
        held = await self.dialogue.borrow_body(conv, "witness")
        wheld = False
        t: Testimony | None = None
        if self.talking is not None:
            self.talking(c.npc, True)
        try:
            if c.tier == "walk":
                if not held:
                    return "Ты не смог отойти: ты занят", False
                failed = await self._act(npc, "go_to_npc", npc=c.npc, radius=GO_CM)
                if failed and ((cm := await self._cm(npc, c.npc)) is None or cm > NEAR_CM):
                    await self._act(npc, "go_to_npc", npc=HERO, radius=BACK_CM)
                    return f"Ты не дошёл ({failed}): {c.name}", False  # out of call: nobody to ask
            elif held:
                await self._act(npc, "turn_to", npc=c.npc)
                await self._act(npc, "play_ani", name="T_COMEOVERHERE")
            if c.tier == "near" and c.moves and (c.cm or 0) > TALK_CM:
                wheld = await self.dialogue.borrow_body(wconv, "witness")
                if wheld:
                    await self._act(c.npc, "go_to_npc", npc=npc, radius=COME_CM)
            if wheld:
                await self._act(c.npc, "turn_to", npc=npc)
            else:
                await self._soft(c.npc, "turn_to", npc=npc)
            t = await self._testify(None, npc, name, c.npc, c.name, said, c.tier, at, about, about_name)
            await self._speak(wconv, t)
            if c.tier == "walk":
                await self._act(npc, "go_to_npc", npc=HERO, radius=BACK_CM)
        finally:
            spoken_to = self.partner() == c.npc  # the hero turned to the witness: talk.py has it now
            if wheld:
                if spoken_to:
                    await self.dialogue.borrow_body(wconv, "listen")  # held on, for the conversation
                self.dialogue.return_body(wconv, "witness")
            if self.talking is not None and not spoken_to:
                self.talking(c.npc, False)
            if held:
                if c.tier == "near":
                    await self._act(npc, "turn_to", npc=HERO)
                self.dialogue.return_body(conv, "witness")
        if t is None:
            return "", False
        if asking is not None and not asking.done() and asking is not asyncio.current_task():
            await asyncio.wait({asking}, timeout=WAIT_S)  # the reaction is a turn of its own, not pending
        self._react(npc, t)
        return "", True

    async def _speak(self, wconv: Conversation, t: Testimony) -> None:
        """The witness says what it saw, if the hero is there to hear it, it is not in a turn of its own and
        the budget allows. Cut off by the hero, it is over: the asker still hears the answer."""
        if (cm := self.hero_cm(t.witness)) is None or cm > HEARS_CM:
            return
        if self.dialogue.speaking(wconv.npc):
            return  # its own turn: event_turn would leave the cue to its next one
        if self.budget is not None and not self.budget.allow(t.witness):
            return
        saw = "; ".join(self.sightings.words(s, t.minute) for s in t.told)
        if self.crimes is not None or t.about != HERO:
            deeds = "; ".join(self._describe(t, f) for f in self._deeds(t))
            saw = "; ".join(part for part in (saw, deeds) if part)
        cue = (
            f"[{t.name} спрашивает тебя о герое: тот говорит «{t.said}». Что ты видел сам: "
            f"{saw or 'героя ты не видел за сутки до того'}. Ответь коротко тому, кто спрашивает, своими "
            "словами и в своём характере — только то, что видел сам.]"
        )
        if t.about != HERO:
            cue = (
                f"[{t.name} спрашивает тебя о человеке {t.about_name}: герой говорит «{t.said}». "
                f"Что ты видел сам: {saw or 'этого человека ты тогда не видел'}. "
                "Ответь коротко тому, кто спрашивает, своими словами и в своём характере — "
                "только то, что видел сам.]"
            )
        if t.cover:
            decision = (
                "ты боишься героя: скажи, что ничего не видел"
                if t.cover == "fear"
                else f"ты обещал прикрыть героя: скажи «{t.vouch}»"
                if t.vouch
                else "ты обещал прикрыть героя: скажи, что не видел"
            )
            cue = (
                f"[{t.name} спрашивает тебя о герое: тот говорит «{t.said}». {decision}. "
                + (f"Остальное, о чём можешь рассказать: {saw}. " if saw else "")
                + "Ответь коротко тому, кто спрашивает, своими словами и в своём характере.]"
            )
        # a free witness answers on the move; one held for this stands and talks
        task = self.dialogue.event_turn(wconv.npc, cue, overlay=not self.controlled(wconv.npc), tools=())
        if task is None:
            return
        done, _ = await asyncio.wait({task}, timeout=WAIT_S)
        if not done:
            log.info("witness %s: its words took too long", wconv.npc)
            return
        if not task.cancelled() and (err := task.exception()) is not None:
            log.info("witness %s: %s", wconv.npc, err)  # retrieved: a failed turn is no reason to stop
        try:
            if wconv.voice is not None:
                await asyncio.wait_for(wconv.voice.join(), WAIT_S)
            await _until(lambda: not self.dialogue.speaking(wconv.npc), WAIT_S)
        except TimeoutError:
            log.info("witness %s: its words took too long", wconv.npc)

    def _react(self, npc: str, t: Testimony) -> None:
        """The asker hears the answer: whether it contradicts the hero is its own judgment."""
        cue = (
            f"[Ответ: {self._told(t)}. Улики — строки С1… в «Что тебе рассказали о герое другие». "
            "Противоречит ли это "
            f"словам героя «{t.said}» и что сказать ему — решай сам.]"
        )
        if t.about != HERO:
            cue = (
                f"[Ответ о человеке {t.about_name}: {self._told(t)}. "
                "Показание — строки С1… в «Что тебе рассказали другие». "
                "Кого считать виновным и что сказать герою — решай сам.]"
            )
        if self.budget is None or self.budget.allow(npc):
            self.dialogue.event_turn(
                npc, cue, tools=(("catch_lie" if t.about == HERO else "blame"), "set_intent"), kind="witness"
            )
        else:
            self.dialogue.note(npc, cue.strip("[]"))

    async def _testify(
        self, id: str | None, npc: str, name: str, witness: str, wname: str, said: str, tier: str, at: int,
        about: str = HERO, about_name: str = "",
    ) -> Testimony | None:
        """What the witness tells: its own evidence before the hero's words (`at`). The fact,
        and both remember. `id`: the far question's, else a new one. None if a load came meanwhile."""
        if about != HERO and not self.npc_crimes_enabled:
            return None
        branch = self.chronicle.branch
        state = await self._state()
        if self.chronicle.branch != branch:
            return None  # another world: this answer belongs to none of its facts
        now = self._now()
        at = at or now
        about = about.upper()
        saw = _trail(self.sightings.hero_trail(witness), at) if about == HERO else []
        t = Testimony(
            id or f"{npc}:{witness}:{about}:{now}", npc, name, witness, wname, said, tier, now, at, saw,
            about=about, about_name=about_name,
        )
        t.told = list(saw)
        if self.crimes is not None and about == HERO:
            deeds = [
                f
                for f in reversed(self.crimes.seen(witness))
                if at - TRAIL_MIN <= _crime_minute(f) <= at
            ][:TOLD]
            t.crime_saw = [str(f["id"]) for f in deeds]
            silent = {str(k.crime["id"]) for k in self.crimes.known(witness) if k.silenced}
            t.crime_told = [crime for crime in t.crime_saw if crime not in silent]
        if about != HERO:
            self._npc_evidence(t)
        elif self.cover_enabled:
            self._cover(t)
        crime_excuses = {}
        if self.crimes is not None and about == HERO:
            for crime_id in t.crime_told:
                if crime := self.crimes.crime(crime_id):
                    # Recording knowledge synchronously changes attitudes and informs the law.
                    self.crimes.learn_side(npc, crime)
                if self.crimes.side(witness, crime_id) is not None:
                    crime_excuses[crime_id] = {
                        **self.crimes.excuse(witness, crime_id), "excuse_by_name": wname,
                    }
        self.chronicle.record(
            {
                "fact": "testimony",
                "id": t.id,
                "npc": npc,
                "name": name,
                "witness": witness,
                "witness_name": wname,
                "said": said,
                "tier": tier,
                "at": at,
                **({"about": about, "about_name": about_name} if about != HERO else {}),
                "saw": [s.record() for s in t.saw],
                "told": [s.record() for s in t.told],
                **({"crime_saw": t.crime_saw, "crime_told": t.crime_told} if t.crime_saw else {}),
                **({"crime_excuses": crime_excuses} if crime_excuses else {}),
                **({"cover": t.cover, "vouch": t.vouch} if t.cover else {}),
                "minute": now,
                **({"day": int(state["day"])} if "day" in state else {}),
            }
        )
        if about == HERO:
            self._betrayal(t)
        self.journal.add("testimony", npc=npc, name=name, witness=witness, saw=len(t.saw), tier=tier)
        self.dialogue.remember(
            witness, wname,
            f"Тебя расспрашивали о {about_name or 'герое'} ({name}): герой говорит «{said}»",
        )
        if tier == "far":
            self.dialogue.remember(npc, name, f"Ты расспросил при встрече: {self._told(t)}")
        return t

    def _subject(self, npc: str, who: str) -> tuple[str, str] | None:
        """Resolve a named human, including one absent from the current view; never the hero."""
        if not who or who.upper() in {HERO, "HERO", "PC_HERO"} or who.lower() in {"герой", "героя", "я"}:
            return None
        social = self.social()
        names = dict(social.names) if social is not None else {}
        names.update({key: card.name for key, card in self.cards.items()})
        if view := self.desk.view(npc):
            names.update({o.id.upper(): o.name for o in view.sees if o.kind == "human"})
        people = [Candidate(key.upper(), name, "", None, False)
                  for key, name in names.items() if key.upper() not in {HERO, "HERO", "PC_HERO"}]
        person = _find(people, who)
        return (person.npc, person.name) if person else None

    def _npc_seen(self, witness: str) -> list[Fact]:
        seen = {f.get("crime") for f in self.chronicle.facts("npc_crime_seen")
                if str(f.get("npc", "")).upper() == witness}
        return [f for f in self.chronicle.facts("npc_crime") if f.get("id") in seen]

    def _case(self, t: Testimony) -> Fact | None:
        """The asker's known case the words concern. Hidden crimes do not narrow a witness's answer."""
        facts = self.chronicle.facts()
        known = {f.get("crime") for f in facts
                 if str(f.get("npc", "")).upper() == t.npc
                 and f.get("fact") in {"npc_crime_seen", "npc_crime_suspected"}}
        known.update(f.get("crime") for f in facts
                     if f.get("fact") == "rumor" and str(f.get("to", "")).upper() == t.npc)
        known.update(crime for f in facts
                     if f.get("fact") == "testimony" and str(f.get("npc", "")).upper() == t.npc
                     for crime in f.get("crime_told", []))
        known.update(k.key.removeprefix("npc_crime_seen:") for k in self.people_knowledge.of(t.npc)
                     if k.key.startswith("npc_crime_seen:"))
        if self.crimes is not None:
            known.update(k.crime["id"] for k in self.crimes.known(t.npc))
        cases = [f for f in facts if f.get("fact") in {"crime", "npc_crime"}
                 and f.get("id") in known and t.npc in {f.get("victim"), f.get("owner")}
                 and _crime_minute(f) <= t.at]
        words = roots(t.said)
        return max(cases, key=lambda f: (
            str(f.get("id", "")).lower() in t.said.lower(),
            len(words & roots(str(f.get("item_name") or f.get("item") or ""))),
            _crime_minute(f),
        ), default=None)

    def _npc_evidence(self, t: Testimony) -> None:
        case = self._case(t)
        lo, hi = (_crime_minute(case) - 60, _crime_minute(case) + 60) if case else (t.at - TRAIL_MIN, t.at)
        hi = min(hi, t.at)  # nothing observed after the words being checked
        t.saw = [s for s in reversed(self.sightings.mind(t.witness).sightings)
                 if s.who == t.about and not s.trace_kind and s.end >= lo and s.start <= hi][:TOLD]
        t.told = list(t.saw)
        t.crime_saw = [str(f["id"]) for f in reversed(self._npc_seen(t.witness))
                       if f.get("culprit") == t.about and lo <= _crime_minute(f) <= hi][:TOLD]
        t.crime_told = list(t.crime_saw)

    def _describe(self, t: Testimony, crime: Fact) -> str:
        if t.about == HERO and self.crimes is not None:
            return self.crimes.describe(crime)
        if self.crimes is not None:
            description = self.crimes.describe(crime)
            return description.replace("герой ", f"{t.about_name} ", 1) + f" (дело {crime['id']})"
        victim = "на тебя" if crime.get("victim") == t.npc else "на другого человека"
        item = str(crime.get("item_name") or crime.get("item") or "чужую вещь")
        where = " из твоего сундука" if crime.get("chest") and crime.get("owner") == t.npc else ""
        action = f"взял {item}{where}" if crime.get("kind") == "theft" else f"напал {victim}"
        return f"{t.about_name} {action} ({self._day(_crime_minute(crime), t.minute)}; дело {crime['id']})"

    def _npc_told(self, t: Testimony, crime: str | None, now: int) -> str:
        parts = []
        if crime != "":
            deeds = [f for f in self._deeds(t) if not crime or f["id"] == crime]
            if deeds:
                parts.append("видел собственными глазами: " + "; ".join(self._describe(t, f) for f in deeds))
        if not crime:
            parts.extend(f"видел: {t.about_name} — {self.sightings.words(s, now)}" for s in t.told)
        what = "; ".join(parts) or f"{t.about_name} тогда не видел"
        return f"{t.witness_name}, {self._day(t.minute, now)}, о человеке {t.about_name}: {what}"

    def _cover(self, t: Testimony) -> None:
        """Accepted promises, then fear. Neither changes the witness's actual evidence."""
        covers = []
        for f in self._covers():
            if str(f.get("npc", "")).upper() != t.witness:
                continue
            day = f.get("day")
            crime = self.crimes.crime(str(f.get("crime", ""))) if self.crimes is not None else None
            day_matches = day is not None and (
                int(day) * DAY <= t.at and (int(day) + 1) * DAY > t.at - TRAIL_MIN
            )
            crime_matches = crime is not None and (
                _crime_minute(crime) - 60 <= t.at
                and _crime_minute(crime) + 60 >= t.at - TRAIL_MIN
            )
            if day_matches or crime_matches:
                covers.append(f)
        if covers:
            # Several promises may hide different evidence. The latest supplies the spoken alibi.
            t.cover = str(covers[-1]["deal"])
            t.vouch = str(covers[-1].get("vouch", ""))
            for f in covers:
                crime_id = str(f.get("crime", ""))
                t.crime_told = [c for c in t.crime_told if c != crime_id]
                crime = self.crimes.crime(crime_id) if self.crimes is not None else None
                places = (self.crimes.places if self.crimes is not None else None) or self.desk.places
                place = places.of(crime.get("waypoint")) if places is not None and crime else None
                at = _crime_minute(crime) if crime else 0

                t.told = [s for s in t.told if not _covered(s, f.get("day"), place.id if place else "", at)]
        elif self._afraid(t.witness):
            t.cover = "fear"
            t.told = []
            t.crime_told = []

    def _afraid(self, npc: str) -> bool:
        return afraid(npc, self.cards, self.crimes, courage=2)

    def _covers(self) -> list[Fact]:
        """Read kept promises from both their applied facts and the original accepted deal."""
        facts = self.chronicle.facts()
        applied = {f.get("deal") for f in facts if f.get("fact") == "covered"}
        covers = []
        for f in facts:
            if not keeps(self.chronicle, str(f.get("deal", "")), "cover"):
                continue
            if f.get("fact") == "covered":
                covers.append(f)
            elif (
                f.get("fact") == "deal" and
                isinstance(f.get("cover"), dict) and f.get("deal") not in applied
            ):
                covers.append({"npc": f.get("npc"), "deal": f.get("deal"), **f["cover"]})
        return covers

    def _betrayal(self, t: Testimony) -> None:
        """Only evidence actually disclosed reveals a broken silence or cover in the Chronicle."""
        deals = {
            str(f.get("deal", ""))
            for crime in t.crime_told
            for f in broken_promises(
                self.chronicle, t.witness, crime=crime, cover=self.cover_enabled, at=t.minute,
            )
        }
        if self.cover_enabled:
            for f in self.chronicle.facts("deal"):
                promise = f.get("cover")
                deal = str(f.get("deal", ""))
                if (
                    str(f.get("npc", "")).upper() != t.witness
                    or not isinstance(promise, dict) or keeps(self.chronicle, deal, "cover")
                ):
                    continue
                crime = self.crimes.crime(str(promise.get("crime", ""))) if self.crimes else None
                places = (self.crimes.places if self.crimes else None) or self.desk.places
                place = places.of(crime.get("waypoint")) if places is not None and crime else None
                at = _crime_minute(crime) if crime else 0
                if any(
                    _covered(s, promise.get("day"), place.id if place else "", at)
                    for s in t.told
                ):
                    deals.add(deal)
        for deal in deals:
            betrayed(self.chronicle, t.witness, deal, "testified", t.npc)

    def _on_fact(self, f: Fact) -> None:
        """Catching the same words by another route also reveals the covering witness."""
        if not self.cover_enabled or f.get("fact") != "caught" or not f.get("said"):
            return
        npc = str(f.get("npc", "")).upper()
        words = roots(str(f.get("title", "")))
        by = f.get("quest", "")
        for fact in self.chronicle.facts("testimony"):
            if (
                fact.get("npc") != npc or not fact.get("cover")
                or not words or not words <= roots(str(fact.get("said", "")))
            ):
                continue
            t = Testimony.of(fact)
            lied = (
                bool(t.vouch)
                or any(s.record() not in [r.record() for r in t.told] for s in t.saw)
                or bool(set(t.crime_saw) - set(t.crime_told))
            )
            if not lied or f.get("testimony") == t.id:
                continue
            if any(
                e.get("npc") == npc and e.get("testimony") == t.id
                for e in self.chronicle.facts("exposed")
            ):
                continue
            now = self._now()
            self.chronicle.record({
                "fact": "exposed", "npc": npc, "liar": t.witness, "testimony": t.id,
                "by": by, "cover": t.cover, "name": t.name, "subject_name": t.witness_name,
                "day": now // DAY, "at": now,
            })
            what = (
                f"{t.witness_name} побоялся(ась) сказать правду"
                if t.cover == "fear"
                else f"{t.witness_name} тебя обманул(а), покрывая героя"
            )
            self.dialogue.remember(npc, t.name, what)

    def _inquire(self, q: Inquiry) -> None:
        self.inquiries[q.id] = q
        self.chronicle.record(
            {
                "fact": "inquiry",
                "id": q.id,
                "npc": q.npc,
                "name": q.name,
                "witness": q.witness,
                "witness_name": q.witness_name,
                "said": q.said,
                "minute": q.minute,
                "meets": q.meets,
                **({"about": q.about, "about_name": q.about_name} if q.about != HERO else {}),
            }
        )
        self.journal.add("ask_witness", npc=q.npc, name=q.name, witness=q.witness, tier="far", said=q.said)

    # --- the far questions, on the game's clock ---------------------------------------------------

    async def run(self) -> None:
        while True:
            await asyncio.sleep(TICK_S)
            if not self.inquiries and not self._windows():
                continue
            try:
                await self.tick()
            except (GameError, TimeoutError) as err:
                log.info("witnesses: %s", err)

    async def tick(self) -> None:
        """The questions whose meeting has come, asked in their order (a clock jump asks them all); the far
        answers whose time on the asker's mind is over."""
        branch = self.chronicle.branch
        await self._state()
        dead = {str(f.get("npc", "")).upper() for f in self.chronicle.facts("died")}
        for q in sorted(self.inquiries.values(), key=lambda q: q.meets):
            if self.chronicle.branch != branch:
                return  # a load: the new branch's questions are taken at the next tick
            if q.npc in dead or q.witness in dead:
                self._lapse(q, "dead")
            elif self.now >= q.meets and self.inquiries.pop(q.id, None) is not None:
                await self._testify(
                    q.id, q.npc, q.name, q.witness, q.witness_name, q.said, "far", q.minute,
                    q.about, q.about_name,
                )
        for npc in self._expire():
            for listener in self.on_change:
                listener(npc)

    def _windows(self) -> bool:
        """Far answers still on someone's mind (their concern ends with time, which nobody else notices)."""
        return any(
            f.get("tier") == "far"
            and (f.get("told") or f.get("crime_told"))
            and f.get("id") not in self._expired
            for f in self.chronicle.facts("testimony")
        )

    def _expire(self) -> list[str]:
        """The askers whose far answer has been on their mind long enough."""
        out = []
        for f in self.chronicle.facts("testimony"):
            if (
                f.get("tier") != "far"
                or not (f.get("told") or f.get("crime_told"))
                or f.get("id") in self._expired
            ):
                continue
            if self.now and self.now - int(f.get("minute", 0)) > CONCERN_MIN:
                self._expired.add(str(f.get("id")))
                out.append(str(f.get("npc", "")).upper())
        return list(dict.fromkeys(out))

    def _lapse(self, q: Inquiry, why: str) -> None:
        self.inquiries.pop(q.id, None)
        self.chronicle.record({"fact": "inquiry_lapsed", "id": q.id, "why": why})
        self.dialogue.remember(
            q.npc,
            q.name,
            f"Ты так и не расспросил о словах героя «{q.said}» того, кого хотел: {q.witness_name}",
        )

    # --- helpers --------------------------------------------------------------------------------

    def testimonies(self, npc: str, within: int = KEPT_MIN, n: int = SHOWN) -> list[Testimony]:
        """What witnesses told it of the hero, the latest first."""
        npc = npc.upper()
        now = self._now()
        out = [
            Testimony.of(f) for f in self.chronicle.facts("testimony") if str(f.get("npc", "")).upper() == npc
        ]
        out = [t for t in out if (not now or now - t.minute <= within)
               and (self.npc_crimes_enabled or t.about == HERO)]
        if not self.cover_enabled:
            silent: set[tuple[str, str]] = set()
            before: dict[str, set[tuple[str, str]]] = {}
            for f in self.chronicle.facts():
                silent.update(
                    (str(p.get("npc", "")).upper(), str(p.get("crime")))
                    for p in silence_facts(self.chronicle, f, cover=False)
                )
                if f.get("fact") == "testimony":
                    before[str(f.get("id", ""))] = silent.copy()
            for t in out:
                if t.cover:
                    t.told = list(t.saw)
                    t.crime_told = [c for c in t.crime_saw if (t.witness, c) not in before.get(t.id, set())]
                    t.cover = t.vouch = ""
        return sorted(out, key=lambda t: t.minute, reverse=True)[:n]

    def _asked(self, npc: str, witness: str, said: str, about: str = HERO) -> Testimony | None:
        """The same question to the same witness within the hour: its answer."""
        words = roots(said)
        now = self._now()
        for t in self.testimonies(npc, ASK_AGAIN_MIN, 10):
            if (t.witness == witness and t.about == about and now - t.minute <= ASK_AGAIN_MIN
                    and roots(t.said) == words):
                return t
        return None

    def _deeds(self, t: Testimony) -> list[Fact]:
        """The testimony's deeds still backed by the witness's own evidence in this branch."""
        if t.about != HERO:
            witnessed = {
                str(f["id"]): f for f in self._npc_seen(t.witness) if f.get("culprit") == t.about
            }
            return [witnessed[crime] for crime in t.crime_told if crime in witnessed]
        if self.crimes is None:
            return []
        witnessed = {str(f["id"]): f for f in self.crimes.seen(t.witness)}
        return [witnessed[crime] for crime in t.crime_told if crime in witnessed]

    def _told(self, t: Testimony, crime: str | None = None) -> str:
        """«Хильда, сегодня в 09:35, о его словах «…»: героя видели сегодня 08:10–08:30 — у сарая; без
        доспеха»; when the hero said them, if it was not then."""
        now = max(self._now(), t.minute)
        if t.about != HERO:
            return self._npc_told(t, crime, now)
        spoke = f" (он сказал их {self._day(t.at, now)})" if t.minute - t.at > 30 else ""
        seen = "; ".join(self.sightings.words(s, now) for s in t.told)
        parts = [f"героя видели {seen}"] if seen and not crime else []
        if crime != "" and self.crimes is not None and (deeds := self._deeds(t)):
            deeds = [f for f in deeds if not crime or f["id"] == crime]
            parts.append("видел собственными глазами: " + "; ".join(self.crimes.describe(f) for f in deeds))
        if t.cover and crime is None:
            parts.insert(0, self._cover_words(t))
        what = "; ".join(parts) or "героя не видели за сутки до того"
        return f"{t.witness_name}, {self._day(t.minute, now)}, о его словах «{t.said}»{spoke}: {what}"

    @staticmethod
    def _cover_words(t: Testimony) -> str:
        return (
            f"{t.witness_name} говорит: «{t.vouch}»"
            if t.vouch else f"{t.witness_name} говорит, что не видел"
        )

    def _where(self, c: Candidate) -> str:
        if c.tier == "near":
            return f"рядом, в {_m(c.cm or 0)} м: окликнешь" + ("" if c.moves else ", но с места не сойдёт")
        if c.tier == "walk":
            return f"в {_m(c.cm or 0)} м: дойдёшь и спросишь"
        return f"далеко: спросишь при встрече ({self._at(c.meets or 0)})"

    def _at(self, minute: int) -> str:
        now = self._now()
        days = minute // DAY - now // DAY
        day = "сегодня" if days <= 0 else "завтра" if days == 1 else "послезавтра"
        return f"{day} около {minute % DAY // 60:02d}:{minute % 60:02d}"

    @staticmethod
    def _day(minute: int, now: int) -> str:
        """«сегодня в 09:35», «вчера в 20:10»."""
        days = now // DAY - minute // DAY
        label = (
            "сегодня"
            if days <= 0
            else "вчера"
            if days == 1
            else "позавчера"
            if days == 2
            else f"{days} дн. назад"
        )
        return f"{label} в {minute % DAY // 60:02d}:{minute % 60:02d}"

    def _now(self) -> int:
        return self.now or self.sightings.now

    async def _state(self) -> dict[str, Any]:
        try:
            state = await self.link.story_state()
        except (GameError, TimeoutError) as err:
            log.info("witnesses: no clock: %s", err)
            return {}
        self._clock(state)
        return state

    def _clock(self, state: dict[str, Any]) -> None:
        if all(k in state for k in ("day", "hour", "minute")):
            self.now = int(state["day"]) * DAY + int(state["hour"]) * 60 + int(state["minute"])

    async def _act(self, npc: str, action: str, /, **args: Any) -> str:
        """An act of a held npc's body; what went wrong, or ''. The game keeps its own deadlines (a walk:
        60 s), the wait is longer."""
        try:
            res = await self.link.act(npc, action, timeout=90.0, **args)  # type: ignore[arg-type]
            return "" if res.status == "done" else res.detail or res.status
        except (GameError, TimeoutError) as err:
            return str(err) or type(err).__name__

    async def _cm(self, npc: str, other: str) -> float | None:
        """How far `other` is from the npc now."""
        try:
            st = (await self.link.story_state(npcs=[other], near=npc)).get("npcs", {}).get(
                other.upper()
            ) or {}
        except (GameError, TimeoutError):
            return None
        return float(st["distance"]) if st.get("distance") is not None else None

    async def _soft(self, npc: str, action: str, /, **args: Any) -> None:
        try:
            await self.link.npc_act(npc, action, **args)  # type: ignore[arg-type]
        except (GameError, TimeoutError) as err:
            log.info("witnesses: %s %s: %s", npc, action, err)


def _trail(trail: list[Sighting], about: int) -> list[Sighting]:
    """The hero's sightings of the day before `about`, the latest first; the scene at hand is not news."""
    day = [s for s in trail if s.end >= about - TRAIL_MIN and s.start <= about]
    return [s for s in day if about - s.start > JUST_MIN][:TOLD]


def _crime_minute(f: Fact) -> int:
    return int(f.get("day", 0)) * DAY + int(f.get("hour", 0)) * 60 + int(f.get("minute", 0))


def _covered(s: Sighting, day: int | None, place: str, at: int) -> bool:
    on_day = day is not None and s.start < (day + 1) * DAY and s.end >= day * DAY
    nearby = bool(place) and place in s.place_ids and s.start <= at + 60 and s.end >= at - 60
    return on_day or nearby


def _find(near: list[Candidate], who: str) -> Candidate | None:
    """The one the model named: its name, or the stem of it (Russian declension: «Хильду»)."""
    w = who.lower()
    for c in near:
        if c.name.lower() == w or c.npc.lower() == w:
            return c
    for c in near:
        if len(c.name) >= 3 and c.name.lower()[: max(3, min(4, len(c.name) - 1))] in w:
            return c
    return None


def _busy(state: str, body: str) -> str | None:
    """Why someone can't answer now, or None."""
    state = state.upper()
    if state.startswith("ZS_SLEEP"):
        return "спит"
    if body in ("down", "dead"):
        return "не в себе"
    if state.startswith(("ZS_ATTACK", "ZS_FLEE")):
        return "дерётся"
    if state.startswith("ZS_TALK"):
        return "разговаривает"
    return None


def _inquiry(f: Fact) -> Inquiry:
    return Inquiry(
        str(f["id"]),
        str(f.get("npc", "")).upper(),
        str(f.get("name", "")),
        str(f.get("witness", "")).upper(),
        str(f.get("witness_name", "")),
        str(f.get("said", "")),
        int(f.get("minute", 0)),
        int(f.get("meets", 0)),
        str(f.get("about") or HERO).upper(),
        str(f.get("about_name", "")),
    )


async def _until(cond: Callable[[], bool], timeout: float) -> bool:
    loop = asyncio.get_running_loop()
    end = loop.time() + timeout
    while not cond():
        if loop.time() >= end:
            return False
        await asyncio.sleep(0.2)
    return True


def _m(cm: float) -> int:
    return max(1, round(cm / 100))
