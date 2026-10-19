"""Two people near the hero talk about his lie in their own voices (docs/npc-tools.md, wave 2 step 2).

What passes is the code's (Rumors.spread, social.py); here only how it looks, by the hero's hearing: words
(one model call writes 2–4 lines, said in turn from one queue), sight (turns and the game's own chatter, no
model), fact (as before); wait while their bodies are busy. The fact comes first.

- words: both within WORDS_CM of the hero and together (or one walks up first). Both are held for the scene
  (an npc on a stand freepoint turns back to it between lines, a smalltalk loop pushes its own chatter into
  the gaps), except a mob user, which speaks free, and posts and escorts, whose bodies are the scripts';
- the hero's voice cuts both at once (talk.py `on_hero_voice`); they stay held a while after it, and his
  next words without a name go to one of them (`addressable`);
- both remember what was said (never as their own `assistant` lines), and so do people standing by;
- a second occasion: two acquaintances who see each other near the hero (npc_views) get the same lot as a
  meeting, and the behavioral model is asked in shadow whether one would tell the other now (`talk_to`).
"""

from __future__ import annotations

import asyncio
import itertools
import json
import logging
import math
import re
import time
from collections.abc import Callable, Coroutine
from dataclasses import dataclass, field, replace
from typing import TYPE_CHECKING, Any, Literal

from .bodies import Bodies
from .brain.planner import TAKES_BODY, NpcPlanner, NpcRecord
from .brain.situation import NpcContext, clock_text, news_text
from .brain.stimuli import normalize, policy
from .budget import Budget
from .chronicle import Chronicle, Fact
from .dialogue import DialogueManager, SceneLine, ScenePlay, mood_of, to_game_text
from .game import GameError, GameLink
from .incidents import classify
from .journal import Journal
from .llm import LLMError, Message, Tool, ToolCall, open_ahead, with_deadlines
from .people_knowledge import rumor_subject
from .perception import PerceptionDesk
from .protocol import (
    Detached,
    GameMessage,
    LineStarted,
    Loading,
    Mood,
    NpcEvent,
    NpcRoster,
    NpcView,
    Perception,
    PlayerText,
    PttCancel,
    PttStart,
    PttStop,
    RosterEntry,
    Session,
)
from .social import DAY, SMALLTALK, Rumor, Rumors, _chance
from .storyguard import StoryGuard, asleep
from .talk import OVERHEAR_CM, PERC_ACTS, TalkDesk
from .trace import TraceWriter
from .trust import Trust

if TYPE_CHECKING:
    from .initiative import InitiativeDesk

log = logging.getLogger(__name__)

PROTOCOL = 15  # say.to: an older bridge can't address a line to an npc, so every pass is a fact as before
TALK_CM = 600  # together: this close
WALK_CM = 1500  # farther apart than TALK_CM but this close: one walks up first (if it may leave its spot)
WORDS_CM = 1500  # both this close to the hero: he hears voices (talkRange 2000 from the camera behind him)
LEAVE_CM = 2000  # the nearer of the two farther than this: the hero left, the scene ends
LINES_MIN, LINES_MAX, LINE_MAX = 2, 4, 160
ATTEMPTS = 2
CACHE_KEY = "gossip"  # the scenes' own connection (openai_responses.py): the prompt cache of the instructions
GRACE_S = 10.0  # after the hero's voice ends: still held, and his unnamed words may go to one of them
PAIR_GAP_MIN = 60  # view trigger: one check per (teller, listener, rumor) per game hour
RETRY_MIN = 1  # view trigger: a pass that waited (their bodies busy) is offered again this soon
SVM_GAP_S = 4.0  # sight tier: the second smalltalk SVM after the first (the game's pairs alternate ~4 s)
SVMS = 21  # $SMALLTALK01..21
SHADOW_MS = 1500
PROBE_FRESH_S = 5.0  # a story_nearby gap is used this long
WALK_RADIUS_CM, WALK_TIMEOUT_S = 250, 25.0
TURN_TIMEOUT_S = 6.0
RELEASE_POLL_S = 0.25
HOLD = "scene"
Tier = Literal["words", "sight", "fact", "wait"]
State = Literal["writing", "playing", "cut", "done", "dropped"]
RUNNING = ("writing", "playing")


@dataclass(frozen=True)
class Staging:
    """How a live pass looks (Rumors.stage): its tier, who walks up first, where and who saw it."""

    tier: Tier
    walker: str | None = None  # who walks up first (words)
    where: str | None = None  # place id (perception.place)
    seen_by: tuple[str, ...] = ()  # PLAYER and the npc's whose views see a or b, as initiative._record
    why: str = ""  # what decided it, for the log

    def keys(self) -> dict[str, Any]:
        """What goes into the rumor fact."""
        return {"where": self.where, "seen_by": list(self.seen_by)}


@dataclass
class Scene:
    """Two npc's talking near the hero: the words tier from the holds to the notes (a sight act is one too,
    briefly)."""

    id: str
    a: str  # the teller
    b: str  # the listener
    a_name: str
    b_name: str
    rumor: str  # Rumor.key
    tier: Tier
    state: State = "writing"
    why: str = ""  # why it was cut or dropped, or why its play ended early
    lines: list[tuple[str, str, Mood]] = field(default_factory=list)  # (npc, text, mood) as written
    quiet: bool = False
    play: ScenePlay | None = None
    writing: asyncio.Future[dict[str, Any] | None] | None = None
    stop: asyncio.Event = field(default_factory=asyncio.Event)  # set by cut(): checked after every await
    held: set[str] = field(default_factory=set)
    noted: bool = False
    world: int = 0
    fact_at: float = 0.0
    cut_at: float | None = None
    errors: list[str] = field(default_factory=list)
    hero_cm: float = 0.0  # the hero's distance to the nearer of the two when it began
    news: str = ""  # what a tells, in Russian (the rumor's wording)
    brief: str = ""
    reason: str = ""  # the model's
    write_ms: float = 0.0
    ended: bool = False
    released: bool = False  # its holds are let go (or there were none to keep)


# --- the model -------------------------------------------------------------------------------------

LINES = "lines"
INSTRUCTIONS = (
    "Ты пишешь короткую сцену для Gothic II: Ночь Ворона. Двое жителей Хориниса говорят между собой рядом с "
    "безымянным героем, как написал бы сценарист игры. Первый, рассказчик, передаёт второму новость о "
    "герое; второй отвечает в своём характере. Говорят своими голосами, по-русски, просто, в духе Gothic: по "
    "одному-два предложения на реплику, без ремарок, без имён в начале реплики. Они знают, где стоит герой "
    "и видит ли он их: сплетничать при нём, понизить голос (quiet), намекнуть или замять — решает их "
    "характер. Не выдумывай ничего сверх новости и того, что о них сказано. 2–4 реплики по очереди, первая "
    "— рассказчика. Ответ — только вызов lines."
)
NO_CALL = "ответ — вызов lines"


def lines_tool(labels: tuple[str, str]) -> Tool:
    """The tool the model answers with; the speakers' enum is the two (built per call)."""
    line = {
        "type": "object",
        "properties": {
            "speaker": {"type": "string", "enum": list(labels)},
            "text": {"type": "string"},
            "mood": {"type": "string", "enum": ["neutral", "friendly", "angry", "hostile", "afraid", "pain"]},
        },
        "required": ["speaker", "text", "mood"],
        "additionalProperties": False,
    }
    return Tool(
        LINES,
        "Реплики сцены по очереди, первая — рассказчика.",
        {
            "type": "object",
            "properties": {
                "lines": {"type": "array", "items": line},
                "quiet": {
                    "type": "boolean",
                    "description": "они понизили голос: не хотят, чтобы герой слышал",
                },
                "reason": {"type": "string"},
            },
            "required": ["lines", "quiet", "reason"],
            "additionalProperties": False,
        },
    )


def labels_of(a_name: str, b_name: str) -> tuple[str, str]:
    """How the two are called in the tool: by name, or by their order when the names are the same."""
    return (a_name, b_name) if a_name != b_name else ("первый", "второй")


_BRACKETS = re.compile(r"\[[^\]]*\]")


def _clean(text: str, names: tuple[str, ...]) -> str:
    """A line as the game says it: no [remarks], no leading «Имя:», game text."""
    text = _BRACKETS.sub(" ", text).strip()
    for name in names:
        head, rest = text[: len(name)], text[len(name) :].lstrip()
        if name and head.lower() == name.lower() and rest.startswith(":"):
            text = rest[1:]
            break
    return to_game_text(text)


def check(
    args: dict[str, Any], labels: tuple[str, str], names: tuple[str, str] | None = None
) -> tuple[list[tuple[int, str, Mood]], bool, list[str]]:
    """The model's lines as (speaker: 0 the teller, 1 the listener; text; mood), whether they lowered their
    voices, and what is wrong with them (Russian, for a retry)."""
    if not args:
        return [], False, [NO_CALL]
    raw = args.get("lines")
    if not isinstance(raw, list) or not LINES_MIN <= len(raw) <= LINES_MAX:
        n = len(raw) if isinstance(raw, list) else 0
        return [], False, [f"lines: от {LINES_MIN} до {LINES_MAX} реплик, а не {n}"]
    errors: list[str] = []
    out: list[tuple[int, str, Mood]] = []
    for i, item in enumerate(raw, 1):
        item = item if isinstance(item, dict) else {}
        speaker = str(item.get("speaker", ""))
        if speaker not in labels:
            errors.append(f"реплика {i}: speaker — «{labels[0]}» или «{labels[1]}»")
            continue
        who = labels.index(speaker)
        if i == 1 and who != 0:
            errors.append(f"реплика 1: первая — рассказчика («{labels[0]}»)")
        elif out and out[-1][0] == who:
            errors.append(f"реплика {i}: говорят по очереди, а не один дважды подряд")
        text = _clean(str(item.get("text", "")), (*labels, *(names or ())))
        if not text:
            errors.append(f"реплика {i}: пустая")
        elif len(text) > LINE_MAX:
            errors.append(f"реплика {i}: длиннее {LINE_MAX} знаков")
        out.append((who, text, mood_of(str(item.get("mood", ""))) or "neutral"))
    return ([] if errors else out), bool(args.get("quiet")), errors


class SceneModel:
    """The cloud model that writes a scene: one call, the tool `lines` (as the director's amends do)."""

    def __init__(self, dialogue: DialogueManager) -> None:
        self.dialogue = dialogue

    def warm(self) -> None:
        """Its connection opened as the scene begins: the write after the holds and the walk does not wait."""
        open_ahead(self.dialogue.llm, CACHE_KEY, keep=True)

    async def write(
        self,
        brief: str,
        labels: tuple[str, str],
        previous: dict[str, Any] | None = None,
        errors: list[str] | None = None,
    ) -> dict[str, Any] | None:
        """The arguments of its `lines` call; {} if it answered without one, None if it failed."""
        llm = self.dialogue.llm  # at call time: scenarios swap it
        messages = [Message("user", brief)]
        if previous is not None and errors:
            sent = json.dumps(previous, ensure_ascii=False)
            reasons = "\n".join(f"- {e}" for e in errors)
            messages.append(Message("user", f"Твой ответ: {sent}\nОтклонено:\n{reasons}\nИсправь его."))
        calls: list[ToolCall] = []
        try:
            events = llm.stream(
                instructions=INSTRUCTIONS, messages=messages, tools=[lines_tool(labels)], cache_key=CACHE_KEY
            )
            async for ev in with_deadlines(
                events, first_s=self.dialogue.llm_first_s, idle_s=self.dialogue.llm_idle_s, name=llm.name
            ):
                if isinstance(ev, ToolCall) and ev.name == LINES:
                    calls.append(ev)
        except LLMError as err:
            log.warning("gossip: the model failed: %s", err)
            return None
        return dict(calls[0].arguments) if calls else {}


# --- the desk --------------------------------------------------------------------------------------


class GossipDesk:
    """How a rumor's pass looks near the hero (Rumors.stage / on_staged), and the scenes it plays."""

    def __init__(
        self,
        link: GameLink,
        dialogue: DialogueManager,
        talk: TalkDesk,
        rumors: Rumors,
        perception: PerceptionDesk,
        budget: Budget | None,
        chronicle: Chronicle,
        trust: Trust | None,
        guard: StoryGuard,
        bodies: Bodies,
        *,
        trace: TraceWriter | None = None,
        journal: Journal | None = None,
    ) -> None:
        self.link = link
        self.dialogue = dialogue
        self.talk = talk
        self.rumors = rumors
        self.perception = perception
        self.budget = budget
        self.chronicle = chronicle
        self.trust = trust
        self.guard = guard
        self.bodies = bodies
        self.trace = trace
        self.journal = journal or dialogue.journal
        self.model = SceneModel(dialogue)
        self.planner: NpcPlanner | None = None  # follow(): the shadow question, the veto
        self.initiative: InitiativeDesk | None = None
        self.scene: Scene | None = None  # the one being written or played
        self.last: Scene | None = None  # the latest that ended (tests, the e2e, the console)
        self.scenes = 0  # words scenes begun
        self._world = 0  # bumped on every load: a scene of the world before is over
        self._sight_busy: set[str] = set()
        self._gaps: dict[tuple[str, str], tuple[float, float]] = {}  # pair -> (probed at, cm; inf: apart)
        self._probing: set[tuple[str, str]] = set()
        # view trigger, (rumor, a, b) -> game minute: when it may be offered again; when asked in shadow
        self._checked: dict[tuple[str, str, str], int] = {}
        self._asked: dict[tuple[str, str, str], int] = {}
        # the latest stage() answer: rumor, a, b, tier, why
        self._staged: tuple[str, str, str, Tier, str] | None = None
        self._voice_end = 0.0  # when the hero's voice last ended (monotonic)
        self._voice_down = False  # the talk key is held
        self._next_allow: Callable[[NpcRecord, str], str | None] | None = None
        self._ids = itertools.count(1)
        self._background: set[asyncio.Task[Any]] = set()
        rumors.stage, rumors.on_staged = self.stage, self.on_staged
        perception.on_view.append(self._on_view)
        talk.on_hero_voice.append(lambda: self.cut("hero"))
        talk.scene_hint = self.addressable
        link.listeners.append(self._on_message)

    # --- hooks for the planner and the initiative desk ----------------------------------------------

    def follow(self, planner: NpcPlanner, initiative: InitiativeDesk | None = None) -> None:
        """The behavioral model: the shadow question, and a veto chained before the hook already there
        (InitiativeDesk's: set it up first); the initiative's first words wait for a scene."""
        self.planner = planner
        self._next_allow = planner.allow
        planner.allow = self.allow
        self.initiative = initiative
        if initiative is not None:
            initiative.floor_busy = self.floor_busy

    def allow(self, rec: NpcRecord, choice: str) -> str | None:
        """Why the planner must not apply this choice now, or None (then the hook before decides)."""
        if self.busy(rec.id) and choice not in TAKES_BODY:
            return "болтает"  # a look or a walk would turn it from the one it talks to
        if choice == "approach_talk" and self.floor_busy():
            return "герой говорит" if self._voice_down else "рядом болтают"  # no first words over them
        return self._next_allow(rec, choice) if self._next_allow is not None else None

    def busy(self, npc: str) -> bool:
        """A speaker of the running scene, or of a sight act."""
        npc = npc.upper()
        s = self.scene
        return (s is not None and s.state in RUNNING and npc in (s.a, s.b)) or npc in self._sight_busy

    def floor_busy(self) -> bool:
        """A scene is being written or played, the hero speaks (his talk key is down), or a scene his voice
        cut still holds its speakers for his words (the grace): nobody else speaks to him first meanwhile,
        and no other scene starts."""
        if self._voice_down:
            return True
        return any(
            s is not None
            and not s.released
            and (s.state in RUNNING or (s.state == "cut" and s.why == "hero"))
            for s in (self.scene, self.last)
        )

    def addressable(self) -> list[str]:
        """Who the hero's next words without a name may go to: the speakers of the scene, or of the one his
        voice cut while they are still held, the one who spoke last first; only after a line was said."""
        for s in (self.scene, self.last):
            if s is None or s.released or not (s.state in RUNNING or (s.state == "cut" and s.why == "hero")):
                continue
            said = s.play.said() if s.play is not None else []
            if said:
                last = said[-1].npc
                return [last, s.b if last == s.a else s.a]
        return []

    # --- staging (Rumors) ---------------------------------------------------------------------------

    def stage(self, r: Rumor, a: str, b: str) -> Staging:
        """How a live pass of `r` from a to b looks now, from what is known already (no request waits)."""
        staging = self._stage(r, a.upper(), b.upper())
        staged = (r.key, a.upper(), b.upper(), staging.tier, staging.why)
        if staged != self._staged:  # once per change: a pass that waits is staged every tick
            log.info("gossip %s -> %s: %s (%s)", a, b, staging.tier, staging.why or "-")
        self._staged = staged
        return staging

    def _stage(self, r: Rumor, a: str, b: str) -> Staging:
        # The live scene prompt and scene memories describe news about the hero. NPC news
        # follows the same social pass as a fact, without asking that scene model.
        if not r.is_player:
            return Staging("fact", why="news about an npc")
        hello = self.link.hello
        if hello is None or hello.protocol < PROTOCOL:
            return Staging("fact", why="old bridge")
        if self.busy(a) or self.busy(b):
            # one tells another already, or b's onward pass waits for its scene
            return Staging("wait", why="busy")
        people = self.talk.people
        ea, eb = people.get(a), people.get(b)
        if ea is None or eb is None or "down" in (ea.activity, eb.activity):
            return Staging("fact", why="not around")
        if asleep(ea.state) or asleep(eb.state):
            return Staging("fact", why="asleep")
        near = ea.distance <= WORDS_CM and eb.distance <= WORDS_CM
        seen = ea.in_view or eb.in_view
        if not near and not seen:
            return Staging("fact", why=f"far ({ea.distance / 100:.0f}, {eb.distance / 100:.0f} m)")
        gap = self._gap(a, b, ea, eb)
        if gap is None:
            self._probe(a, b)
            return Staging("wait", why="gap unknown")  # the probe answers by the next tick
        if gap > WALK_CM:
            return Staging("fact", why=f"apart ({gap / 100:.0f} m)")
        if (busy := self._bodies_busy(a, b, ea, eb, near)) is not None:
            return Staging("wait", why=busy)
        where, seen_by = self._where(a, b), self._seen_by(a, b)
        shown = Staging("sight", where=where, seen_by=seen_by) if seen else Staging("fact")
        if not near:
            return replace(shown, why="seen, not heard")
        if self.guard.on_duty(a, ea.state) or self.guard.on_duty(b, eb.state):
            return replace(
                shown, why="on duty"
            )  # a post or an escort is never held: its body is the scripts'
        walker = None
        if gap > TALK_CM:
            walker = next((x for x, e in ((a, ea), (b, eb)) if self._may_walk(x, e.state)), None)
            if walker is None:
                return replace(shown, why="neither may walk")
        if self.budget is not None and not self.budget.can_all((a, b)):
            return replace(shown, why="no budget")
        return Staging("words", walker, where, seen_by, why=f"{gap / 100:.0f} m apart")

    def _bodies_busy(self, a: str, b: str, ea: RosterEntry, eb: RosterEntry, near: bool) -> str | None:
        """What their bodies are busy with, if they would be needed and are someone else's now: a dialog, a
        hold, a fight, a walk; None if they are free."""
        if any(e.activity == "dialog" for e in self.talk.people.values()):
            return "a dialog"
        if {ea.activity, eb.activity} & {"attached", "fight", "body"} or {a, b} & set(self.talk.held):
            return "held or fighting"
        if self.initiative is not None and self.initiative.active in (a, b):
            return "walks up to the hero"
        if self.bodies.busy(a) or self.bodies.busy(b):
            return "a walk"  # a soft walk keeps the roster's "routine"
        partner = self.talk.partner
        if partner is not None and partner.npc in (a, b):
            return "talks to the hero"  # held or free: no chatter of theirs over it
        if self.dialogue.speaking(a) or self.dialogue.speaking(b):
            return "speaks"  # one of them answers someone
        # the words tier: the hero talks to anyone (a scene starts only without a partner, so his words
        # never go to the wrong one), or the floor is taken (another scene, his voice)
        if near and partner is not None:
            return "the hero talks to another"
        if near and self.floor_busy():
            return "the floor is taken"
        return None

    def _may_walk(self, npc: str, state: str) -> bool:
        """It may walk up to the other: not a mob user (a walk needs it held), not the plot's, not on duty."""
        return not self._mob(npc) and self.guard.body(npc, state) is None

    def _mob(self, npc: str) -> bool:
        """It uses a mob (a stove, a bench): it speaks free, never held."""
        view = self.perception.view(npc)
        if view is not None:
            return view.body == "mob"
        return any(o.id.upper() == npc and o.body == "mob" for v in self.perception.views() for o in v.sees)

    def _gap(self, a: str, b: str, ea: RosterEntry, eb: RosterEntry) -> float | None:
        """How far apart they are (cm; inf: surely apart), None if not known yet. Sight is the head's field
        of view, so two with their backs to each other see nothing: then the probe, distance only."""
        for viewer, other in ((a, b), (b, a)):
            view = self.perception.view(viewer)
            if view is not None:
                for o in view.sees:
                    if o.id.upper() == other:
                        return o.distance
        probed = self._gaps.get(_pair(a, b))
        if probed is not None and time.monotonic() - probed[0] <= PROBE_FRESH_S:
            return probed[1]
        if abs(ea.distance - eb.distance) > WALK_CM:
            return math.inf
        return None

    def _probe(self, a: str, b: str) -> None:
        pair = _pair(a, b)
        if pair in self._probing:
            return
        self._probing.add(pair)
        world = self._world

        async def probe() -> None:
            try:
                near = await self.link.story_nearby(a, WALK_CM)
            finally:
                self._probing.discard(pair)
            if world == self._world:
                d = next((float(p["distance"]) for p in near if str(p.get("npc", "")).upper() == b), math.inf)
                self._gaps[pair] = (time.monotonic(), d)

        self._spawn(probe())

    def _where(self, a: str, b: str) -> str | None:
        view = self.perception.view(a) or self.perception.view(b)
        place = self.perception.place(view) if view is not None else None
        return place.id if place is not None else None

    def _seen_by(self, a: str, b: str) -> tuple[str, ...]:
        """The hero (he hears or sees them) and whoever else sees one of them."""
        others = sorted(
            v.npc.upper()
            for v in self.perception.views()
            if v.npc.upper() not in (a, b) and any(o.id.upper() in (a, b) for o in v.sees)
        )
        return ("PLAYER", *others)

    def on_staged(self, r: Rumor, a: str, b: str, staging: Staging, fact: Fact) -> None:
        """A pass shown near the hero, its fact written: the speakers are busy from now on (a pass of theirs
        in the same spread waits), the scene goes on in the background."""
        a, b = a.upper(), b.upper()
        s = Scene(
            f"{a}>{b}@{self.rumors.clock or 0}",
            a,
            b,
            self._name(a),
            self._name(b),
            r.key,
            staging.tier,
            world=self._world,
            fact_at=time.monotonic(),
            news=_news(r),
        )
        people = self.talk.people
        s.hero_cm = min((people[n].distance for n in (a, b) if n in people), default=0.0)
        if staging.tier == "words":
            self.scene = s
            self.scenes += 1
            self.journal.add("scene", npc=a, name=s.a_name, to=b, to_name=s.b_name, status="start", id=s.id)
            log.info("scene %s: %s tells %s (walker %s)", s.id, s.a_name, s.b_name, staging.walker or "-")
            self._spawn(self._words(s, r, staging))
        elif staging.tier == "sight":
            self._sight_busy |= {a, b}
            self._spawn(self._sight(s))

    # --- the words tier -----------------------------------------------------------------------------

    async def _words(self, s: Scene, r: Rumor, staging: Staging) -> None:
        """Holds, a walk, the write, the play. Never cancelled: cut() sets the state and `stop`, and every
        step looks at them after its await (a hold that lands after a cut or a load is still recorded and
        freed)."""
        self.model.warm()
        try:
            for n in (s.a, s.b):
                if self._stopped(s):
                    return  # cut before it began, or while the other was being held: nobody more is held
                if self._mob(n):
                    continue  # it speaks free: a hold could leave it stuck in its mob's pose
                ok = await self.talk.hold(n, HOLD)
                if ok:
                    s.held.add(n)
                elif not self._stopped(s):
                    return self._drop(s, "hold")
            if self._stopped(s):
                return  # the last hold, or nothing awaited at all (mob users): no walk, no budget, no write
            if staging.walker is not None and not await self._walk(s, staging.walker):
                return
            if not self._hearable(s):
                return self._drop(s, "moved")
            if self.budget is not None and not self.budget.allow_all((s.a, s.b)):
                self.journal.add("scene_skipped", npc=s.a, name=s.a_name, to=s.b, why="budget")
                return self._drop(s, "budget")
            s.brief = self._brief(s, r)
            if not await self._write(s):
                return
            if not self._hearable(s):
                return self._drop(s, "moved")
            s.state = "playing"
            names = {s.a: s.a_name, s.b: s.b_name}
            other = {s.a: s.b, s.b: s.a}
            s.play = self.dialogue.play_scene(
                [(n, names[n], other[n], text, mood) for n, text, mood in s.lines], quiet=s.quiet
            )
            await asyncio.wait({s.play.task})  # does not raise when the play was cut
            if s.state == "playing":
                s.state, s.why = "done", s.play.why
        finally:
            self._end(s)

    async def _walk(self, s: Scene, walker: str) -> bool:
        """One walks up to the other (held: a controller walk); False if the scene is over."""
        other = s.b if walker == s.a else s.a
        walk = asyncio.ensure_future(
            self.link.act(walker, "go_to_npc", npc=other, radius=WALK_RADIUS_CM, timeout=WALK_TIMEOUT_S)
        )
        walk.add_done_callback(_retrieve)  # a cut leaves it to the detach, which ends it
        await self._or_stop(s, walk)
        if self._stopped(s):
            return False
        try:
            arrived = walk.result().ok
        except (GameError, TimeoutError) as err:
            log.info("scene %s: %s did not walk up: %s", s.id, walker, err or type(err).__name__)
            arrived = False
        if not arrived:
            self._drop(s, "walk")
        return arrived

    async def _write(self, s: Scene) -> bool:
        """The lines, one model call (a retry with the errors costs another); False if the scene is over."""
        labels = labels_of(s.a_name, s.b_name)
        previous: dict[str, Any] | None = None
        errors: list[str] = []
        for attempt in range(ATTEMPTS):
            if attempt:  # a retry is another call: only while the hero still hears them, and on budget
                if not self._hearable(s):
                    self._drop(s, "moved")
                    return False
                if self.budget is not None and not self.budget.allow_all((s.a, s.b)):
                    break
            started = time.monotonic()
            s.writing = asyncio.ensure_future(self.model.write(s.brief, labels, previous, errors))
            await self._or_stop(s, s.writing)
            s.write_ms += (time.monotonic() - started) * 1000
            if self._stopped(s):
                return False
            args = s.writing.result()
            if args is None:
                break  # the model failed: not asked again
            lines, quiet, errors = check(args, labels, (s.a_name, s.b_name))
            if not errors:
                s.lines = [((s.a, s.b)[who], text, mood) for who, text, mood in lines]
                s.quiet, s.reason = quiet, str(args.get("reason") or "")
                return True
            log.info("scene %s: lines rejected: %s", s.id, "; ".join(errors))
            s.errors += errors
            previous = args
        self._drop(s, "model")
        return False

    def _hearable(self, s: Scene) -> bool:
        """Both still close to the hero, he talks to nobody, no dialog of the scripts."""
        people = self.talk.people
        ea, eb = people.get(s.a), people.get(s.b)
        if ea is None or eb is None or "down" in (ea.activity, eb.activity):
            return False
        if max(ea.distance, eb.distance) > WORDS_CM or self.talk.partner is not None:
            return False
        return not any(e.activity == "dialog" for e in people.values())

    def _brief(self, s: Scene, r: Rumor) -> str:
        """The scene as the model sees it (Russian): where, when, who the two are, the news, the hero."""
        a, b, an, bn = s.a, s.b, s.a_name, s.b_name
        labels = labels_of(an, bn)
        clock = self.rumors.clock
        view = self.perception.view(a) or self.perception.view(b)
        head = []
        if view is not None:
            place = self.perception.place(view)
            where = place.where_ru if place is not None else (view.waypoint or view.nearest_waypoint)
            roof = "под крышей" if view.indoors else "под открытым небом"
            head.append(f"Место: {where} ({roof})." if where else f"Место: {roof}.")
        social = self.rumors.social
        if clock is not None and social is not None and social.smalltalk(a, b, clock) is not None:
            head.append("Здесь вы обычно болтаете.")
        if clock is not None:
            head.append(f"Время: день {clock // DAY}, {clock % DAY // 60:02d}:{clock % 60:02d}.")
        personas = self.dialogue.personas
        parts = [" ".join(head)] if head else []
        parts += [
            f"Рассказчик — {an}." + (f" В lines: «{labels[0]}»." if labels[0] != an else ""),
            personas.brief(a, an),
            f"Слушатель — {bn}." + (f" В lines: «{labels[1]}»." if labels[1] != bn else ""),
            personas.brief(b, bn),
            f"Новость (знает {an}, не знает {bn}): {s.news}",
            f"{an} {self._source(r, a, an)}.",
        ]
        honesty = self.trust.situation(a) if self.trust is not None else None
        if honesty is not None and "\n" in honesty:
            honesty = honesty.split("\n", 1)[1]  # its own header is in the second person
        parts.append(f"Что {an} знает о честности героя («ты» — {an}):\n{honesty or 'ничего особенного'}")
        sees = {n: "видит" if self._sees_hero(n) else "не видит" for n in (a, b)}
        parts.append(f"Герой стоит в {s.hero_cm / 100:.0f} м от них. {an} {sees[a]} его; {bn} {sees[b]} его.")
        return "\n".join(parts)

    def _source(self, r: Rumor, a: str, name: str) -> str:
        """How the teller knows it, and since when."""
        she = self._she(a, name)
        if r.known.get(a, 0) == 0 and not r.is_player:
            how = "сама раскрыла это" if she else "сам раскрыл это"
        elif r.known.get(a, 0) == 0 and r.kind == "broken":
            how = "сама не дождалась от него долга" if she else "сам не дождался от него долга"
        elif r.known.get(a, 0) == 0:
            how = "сама уличила героя" if she else "сам уличил героя"
        else:
            told = next(
                (
                    f.get("from_name") or f.get("from")
                    for f in self.chronicle.facts("rumor")
                    if f.get("to") == a and f.get("quest") == r.quest
                    and f.get("kind", r.kind) == r.kind and rumor_subject(f) == r.subject
                ),
                None,
            )
            how = ("слышала это" if she else "слышал это") + (f" от {told}" if told else "")
        clock, since = self.rumors.clock, r.since.get(a)
        if clock is None or since is None:
            return how
        days = clock // DAY - since
        return f"{how} {'сегодня' if days <= 0 else 'вчера' if days == 1 else f'{days} дн. назад'}"

    def _sees_hero(self, npc: str) -> bool:
        view = self.perception.view(npc)
        return view is not None and view.hero is not None and view.hero.seen

    def _she(self, npc: str, name: str) -> bool:
        """A woman, by her voice (a hand-written one says so, else the original actor's: persona.py)."""
        return "woman" in self.dialogue.personas.get(npc, name).voice.lower()

    # --- the sight tier -----------------------------------------------------------------------------

    async def _sight(self, s: Scene) -> None:
        """Seen, not heard: they turn to each other and chatter in the game's own voices (no model)."""
        try:
            ea, eb = self.talk.people.get(s.a), self.talk.people.get(s.b)
            if ea is not None and eb is not None and ea.state.upper() == eb.state.upper() == SMALLTALK:
                return  # the game's own chatter is the scene
            first = asyncio.ensure_future(self.bodies.act(s.a, "say_svm", svm=_svm(s, s.a), npc=s.b))
            await asyncio.sleep(SVM_GAP_S)
            if s.world == self._world:
                await self.bodies.act(s.b, "say_svm", svm=_svm(s, s.b), npc=s.a)
            await first  # one act per npc: no tracking is dropped
        finally:
            self._sight_busy.discard(s.a)
            self._sight_busy.discard(s.b)

    # --- the end --------------------------------------------------------------------------------------

    def cut(self, why: str) -> bool:
        """Stop the running scene at once (the hero's voice, a threat, he left; loading: nothing more goes
        to the game). Its notes are written now: they are in the memories before his words are answered.
        True if there was one."""
        s = self.scene
        if s is None or s.state not in RUNNING:
            return False
        s.state, s.why, s.cut_at = "cut", why, time.monotonic()
        s.stop.set()
        if s.writing is not None:
            s.writing.cancel()
        said = s.play.cut(hush=why != "loading") if s.play is not None else []
        if why != "loading":
            self._notes(s, said)
        log.info("scene %s cut (%s) after %d line(s)", s.id, why, len(said))
        self.journal.add(
            "scene_cut", npc=s.a, name=s.a_name, to=s.b, to_name=s.b_name, why=why, said=len(said)
        )
        return True

    def _drop(self, s: Scene, why: str) -> None:
        log.info("scene %s dropped: %s", s.id, why)
        s.state, s.why = "dropped", why

    def _stopped(self, s: Scene) -> bool:
        return s.state != "writing" or s.world != self._world

    def _end(self, s: Scene) -> None:
        """Notes, witnesses, the trace; the holds go now, or after the hero's grace. Once."""
        if s.ended:
            return
        s.ended = True
        if s.state in RUNNING:  # a step failed (logged by _spawn)
            s.state, s.why = "dropped", s.why or "error"
        gone = s.world != self._world or s.why == "loading"
        said = s.play.said() if s.play is not None else []
        if not gone:
            if not s.noted:
                self._notes(s, said)
            if said:
                self._spawn(self._witnesses(s, said))
        self._trace(s, said)
        self.journal.add(
            "scene", npc=s.a, name=s.a_name, to=s.b, to_name=s.b_name, status=s.state, why=s.why, id=s.id
        )
        if not gone and s.why == "hero" and said:
            self._spawn(self._release(s))  # his next words may still be for one of them
        else:
            # after a load too: Loading let the world's holds go (free is a no-op for them), but one that
            # landed after it is still ours
            self._free(s)
        if self.scene is s:
            self.scene = None
        self.last = s

    def _notes(self, s: Scene, said: list[SceneLine]) -> None:
        """What both remember: the lines said, never as their own replies (D10)."""
        s.noted = True
        an, bn = s.a_name, s.b_name
        ending = "а" if self._she(s.a, an) else ""
        if not said:
            text = f"Ты рассказал{ending} человеку по имени {bn} то, что знаешь о герое: {s.news}"
            self.dialogue.remember(s.a, an, text)
            return
        cut = ". Вы замолчали: заговорил герой." if s.why == "hero" else ""
        told = " — ".join(f"{'Ты' if x.npc == s.a else bn}: «{x.text}»" for x in said)
        heard = " — ".join(f"{'Ты' if x.npc == s.b else an}: «{x.text}»" for x in said)
        where = f" (герой стоял в {s.hero_cm / 100:.0f} м)"
        self.dialogue.remember(
            s.a, an, f"Ты рассказал{ending} о герое человеку по имени {bn}{where}: {told}{cut}"
        )
        self.dialogue.remember(s.b, bn, f"{an} рассказал{ending} тебе о герое: {heard}{cut}")

    async def _witnesses(self, s: Scene, said: list[SceneLine]) -> None:
        """Those standing by heard it (a note, no rumor fact: the code decides who passes it)."""
        lines = [(x.name, x.text) for x in said]
        heard: set[str] = set()
        for speaker in (s.a, s.b):
            try:
                near = await self.link.story_nearby(speaker, int(OVERHEAR_CM))
            except (GameError, TimeoutError) as err:
                log.info("scene %s: who stood by %s: %s", s.id, speaker, err or type(err).__name__)
                continue
            for p in near:
                n = str(p.get("npc", "")).upper()
                if not n or n in (s.a, s.b) or n in heard or p.get("dead") or s.world != self._world:
                    continue
                heard.add(n)
                self.dialogue.overheard_talk(n, p.get("name") or n, (s.a_name, s.b_name), lines)

    def _trace(self, s: Scene, said: list[SceneLine]) -> None:
        if self.trace is None:
            return
        self.trace.write(
            {
                "type": "scene",
                "t": round(time.time(), 3),
                "id": s.id,
                "npcs": [s.a, s.b],
                "rumor": s.rumor,
                "tier": s.tier,
                "brief": s.brief,
                "lines": [{"speaker": n, "text": text, "mood": mood} for n, text, mood in s.lines],
                "quiet": s.quiet,
                "said": [{"speaker": x.npc, "text": x.text} for x in said],
                "status": s.state,
                "why": s.why,
                "errors": s.errors,
                "write_ms": round(s.write_ms, 1),
                "reason": s.reason,
            }
        )

    async def _release(self, s: Scene) -> None:
        """Hold them GRACE_S after the hero's voice ends (not while the key is down: his words are still
        being recognized), then let them go; one he spoke to keeps listening (talk.py)."""
        try:
            while s.world == self._world:
                left = max(s.cut_at or 0.0, self._voice_end) + GRACE_S - time.monotonic()
                if left <= 0 and not self._voice_down:
                    self._free(s)
                    return
                await asyncio.sleep(max(left, RELEASE_POLL_S))
        finally:
            s.released = True

    def _free(self, s: Scene) -> None:
        s.released = True
        for n in s.held:
            self.talk.free(n, HOLD)

    # --- the game -----------------------------------------------------------------------------------

    def _on_message(self, msg: GameMessage) -> None:
        s = self.scene
        running = s is not None and s.state in RUNNING
        match msg:
            case Loading() | Session():
                self._world += 1
                self.cut("loading")
                self._sight_busy.clear()
                self._gaps.clear()
                self._probing.clear()
                self._checked.clear()
                self._asked.clear()
            case PttStart():
                self._voice_down = True  # the cut itself comes through talk.on_hero_voice
            case PttStop() | PttCancel():
                self._voice_down = False
                self._voice_end = time.monotonic()
            case PlayerText():
                self._voice_end = time.monotonic()
            case LineStarted() if s is not None and s.play is not None:
                line = next((x for x in s.play.lines if x.line == msg.line), None)
                if line is not None:
                    self.journal.add(
                        "npc", npc=line.npc, name=line.name, text=line.text, to=line.to, scene=s.id
                    )
                    if line.to in s.held and s.state == "playing":
                        # it turns its body to the speaker (not awaited: its queue is empty now)
                        self._spawn(self._turn(line.to, line.npc))
            case Perception() if running and s is not None and msg.npc.upper() in s.held:
                if classify(msg, {}, time.monotonic()) is not None or msg.perc in PERC_ACTS:
                    self.cut("threat")
            case NpcEvent() if running and s is not None and msg.npc.upper() in (s.a, s.b):
                kind, _ = normalize(msg.kind, msg.other, msg.other_name, msg.detail)
                if policy(kind).threat or policy(kind).scripted:
                    self.cut("threat")
            case Detached() if running and s is not None and msg.npc.upper() in s.held:
                if msg.reason != "server":
                    self.cut("gone")
            case NpcRoster() if running and s is not None:
                people = self.talk.people  # merged already: talk.py listens first
                ea, eb = people.get(s.a), people.get(s.b)
                if ea is None or eb is None or "down" in (ea.activity, eb.activity):
                    self.cut("gone")
                elif any(e.activity == "dialog" for e in people.values()):
                    self.cut("dialog")
                elif "body" in (ea.activity, eb.activity):
                    self.cut("body")
                elif min(ea.distance, eb.distance) > LEAVE_CM:
                    self.cut("hero_left")

    async def _turn(self, npc: str, to: str) -> None:
        try:
            await self.link.act(npc, "turn_to", npc=to, timeout=TURN_TIMEOUT_S)
        except (GameError, TimeoutError) as err:
            log.info("%s did not turn to %s: %s", npc, to, err or type(err).__name__)

    # --- the second occasion: two acquaintances see each other near the hero ------------------------

    def _on_view(self, view: NpcView) -> None:
        hello = self.link.hello
        if hello is None or hello.protocol < PROTOCOL:
            return  # an older bridge: no new occasion, passes are the schedule's facts as before (D12)
        rumors, social, clock = self.rumors, self.rumors.social, self.rumors.clock
        a = view.npc.upper()
        if clock is None or social is None or self.busy(a) or self.floor_busy():
            return
        day = clock // DAY
        for o in view.sees:
            b = o.id.upper()
            # the tie as it is: Social.tie() would make one
            if o.kind != "human" or o.distance > WALK_CM or social.ties.get(a, {}).get(b) is None:
                continue
            for r in list(rumors.rumors.values()):
                if a not in r.known or b in r.known or not r.tells(a, day):
                    continue
                key = (r.key, a, b)
                if (again := self._checked.get(key)) is not None and clock < again:
                    continue
                self._checked[key] = clock + PAIR_GAP_MIN
                self._spawn(self._offer(r, a, b, o.distance, o.name))

    async def _offer(self, r: Rumor, a: str, b: str, gap_cm: float, b_name: str) -> None:
        """Asked in shadow (traced, never acted on; once per PAIR_GAP_MIN), then the lot decides as for a
        meeting."""
        world = self._world
        planner = self.planner
        key, clock = (r.key, a, b), self.rumors.clock or 0
        asked = None
        if planner is not None and planner.decider is not None:
            if (at := self._asked.get(key)) is None or clock - at >= PAIR_GAP_MIN:
                self._asked[key] = clock
                asked = await self._shadow(planner, r, a, b, gap_cm, b_name)
        if world != self._world:
            return
        self._staged = None
        told = self.rumors.offer(r, a, b)
        waited = self._staged is not None and self._staged[:4] == (r.key, a, b, "wait")
        code = "told" if told else "wait" if waited else "held"
        if code == "wait":
            # busy now: the view offers it again a game minute later, without asking again
            self._checked[key] = (self.rumors.clock or clock) + RETRY_MIN
        if asked is not None and self.trace is not None:
            self.trace.write({**asked, "code": code})

    async def _shadow(
        self, planner: NpcPlanner, r: Rumor, a: str, b: str, gap_cm: float, b_name: str
    ) -> dict[str, Any] | None:
        """The behavioral model's `talk_to` for a, as a `brain` row (planner.py's shape), or None."""
        rec, entry = planner.npcs.get(a), self.talk.people.get(a)
        ctx = NpcContext(
            a,
            rec.name if rec is not None else self._name(a),
            rec.guild if rec is not None else entry.guild if entry is not None else 0,
            rec.distance_cm if rec is not None else entry.distance if entry is not None else 0.0,
            rec.in_view if rec is not None else entry.in_view if entry is not None else False,
            view=planner.view_of(a) if planner.view_of is not None else (),
        )
        clock = self.rumors.clock
        when = clock_text(clock % DAY // 60, clock % 60) if clock is not None else None
        news = (news_text(r.kind, r.about_name, r.title, about_you=r.about == a)
                if r.is_player else r.description())
        sit = planner.builder.talk_to(ctx, when, b_name, gap_cm / 100, news)
        rid = f"{a}#talk_to{next(self._ids)}"
        sent = time.monotonic()
        outcome = await planner.decider.decide(rid, "background", 0.2, SHADOW_MS, sit.state, sit.questions)
        answer = (outcome.answers or {}).get("talk_to") if outcome.kind == "answers" else None
        if not answer:
            return None
        criteria = sit.questions["talk_to"]["criteria"]
        return {
            "type": "brain",
            "t": round(time.time(), 3),
            "npc": a,
            "rid": rid,
            "question": "talk_to",
            "lane": "background",
            "tier": rec.tier.name if rec is not None else None,
            "state": sit.state,
            "options": dict(criteria),
            "stimuli": [],
            "choice": answer.get("choice"),
            "confidence": round(float(answer.get("confidence", 0.0)), 3),
            "probabilities": answer.get("probabilities", {}),
            "applied": False,
            "why": "shadow",
            "latency_ms": round((time.monotonic() - sent) * 1000, 1),
            "other": b,
        }

    # --- helpers ------------------------------------------------------------------------------------

    def _name(self, npc: str) -> str:
        entry = self.talk.people.get(npc)
        if entry is not None:
            return entry.name
        social = self.rumors.social
        return social.names.get(npc, npc) if social is not None else npc

    async def _or_stop(self, s: Scene, fut: asyncio.Future[Any]) -> None:
        """Until `fut` is done or the scene is cut."""
        stop = asyncio.ensure_future(s.stop.wait())
        try:
            await asyncio.wait({fut, stop}, return_when=asyncio.FIRST_COMPLETED)
        finally:
            stop.cancel()

    def _spawn(self, coro: Coroutine[Any, Any, Any]) -> None:
        async def run() -> None:
            try:
                await coro
            except (GameError, TimeoutError) as err:
                log.info("gossip: %s", err or type(err).__name__)
            except Exception:
                log.exception("gossip: a task failed")

        task = asyncio.get_running_loop().create_task(run())
        self._background.add(task)
        task.add_done_callback(self._background.discard)


def _news(r: Rumor) -> str:
    """What the teller knows, in the rumor's own wording (without whose words it is)."""
    return r.description()


def _svm(s: Scene, npc: str) -> str:
    """One of the game's smalltalk lines, the same for the same scene."""
    return f"SMALLTALK{1 + int(_chance(s.id, npc) * SVMS):02d}"


def _pair(a: str, b: str) -> tuple[str, str]:
    return (a, b) if a <= b else (b, a)


def _retrieve(fut: asyncio.Future[Any]) -> None:
    if not fut.cancelled():
        fut.exception()  # nobody else may look at it
