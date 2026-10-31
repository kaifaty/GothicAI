"""The two loops together, for the npc's we control (held for a conversation with the hero, or following him).

Something happens to the npc → an incident (brain/incident.py). At once, without waiting:
- the **behavioral model** picks the body's reaction (brain/reflex.py) — in ~0.1 s;
- with the planner, a listening npc threatened by another npc is freed before the first reaction, so its
  native fight or flight has its body; without it, the **safety rule** hands a beaten npc back to the
  game's scripts: a second hit from the same attacker within `SECOND_HIT_S`, or health below half.
The hero's companions are the exception: in a fight orders.py has their bodies (they stand up for him),
so neither the behavioral model nor the safety rule moves them; only their words come from here.

In parallel, if the incident is worth words and someone hears them, the **conversational model** gets
an event turn (dialogue.py): words and maybe a new intent, 3–5 s later. Its answer is dropped if the
incident is over by then. Calls of its own come out of the budget (budget.py).

The hero's phrase is an event too: the body may gesture in the pause before the answer.
"""

from __future__ import annotations

import asyncio
import logging
import time
from collections.abc import Callable, Coroutine
from typing import Any

from .brain.apply import Decision
from .brain.incident import INCIDENT_GAP_S, IncidentEvent, IncidentTracker, escalation_reason
from .brain.planner import Flight, NpcPlanner
from .brain.reflex import REACT_OPTIONS, Reaction, Reflexes
from .brain.situation import NpcContext, clock_text
from .brain.stimuli import Stimulus, policy
from .budget import Budget
from .dialogue import DialogueManager, FirstReaction
from .executor import Intent
from .game import GameError, GameLink
from .protocol import (
    Attached,
    Detached,
    FollowEnded,
    GameMessage,
    Loading,
    NpcEvent,
    Perception,
    Session,
)
from .trace import TraceWriter

log = logging.getLogger(__name__)

SECOND_HIT_S = 10.0
FREE_HEARING_CM = 2000.0  # a free npc's words are worth the call if the hero is this close (or sees it)
FREE_SEEING_CM = 3000.0
FREE_COOLDOWN_S = 30.0  # one call per free npc at most this often about the same thing
FREE_NEW_REASON_S = 5.0  # a new reason (the wolf it shouted about is dead) may follow sooner
FREE_TOOLS = ("set_intent", "recall")  # an npc nobody controls cannot hand over items or end a chat
# the danger a free npc spoke about ends within this (the monster is dead, whoever killed it): one more short
# line about that, though the moment alone would not call for words ("the wolf was not so scary after all")
FREE_SETTLE_S = 60.0
FREE_TURN_WAIT_S = 12.0  # a turn of a free npc still running this long is stuck (the LLM hangs): dropped
ENDS_DANGER = frozenset({"monster_killed"})
# a free npc's goals of the moment, carried out by its body through the planner: goal -> the planner's choice.
# Seeking help is running from the danger, shouting (its words ask for help): an npc that comes up to the hero
# starts its original dialog with him (an important info), wolf or no wolf
INTENT_BODY = {"flee": "flee", "fight": "fight", "seek_help": "flee"}

# a free npc's stimuli, for the conversational model (Russian); the rest are told in English
EVENTS_RU = {
    "monster_near": "{other} идёт на тебя",
    "npc_attack": "{other} напал на тебя",
    "npc_threat": "{other} враждебен и бежит на тебя с обнажённым оружием",
    "fight_nearby": "рядом драка: {detail}",
    "cry_for_help": "{other} зовёт на помощь",
    "player_near": "герой подошёл к тебе",
    "player_speech": "герой сказал рядом: «{detail}»",
    "enter_room": "герой вошёл в твой дом",
    "item_given": "герой отдал тебе {detail}",
    "item_dropped": "герой бросил рядом {detail}",
    "monster_killed": "{other} расправился с монстром ({detail}), опасность миновала",
}
# what the body already did, by the behavioral model's choice
REACTIONS_RU = {
    "fight": "ты вступил в бой",
    "flee": "ты убегаешь",
    "call_help": "ты зовёшь на помощь",
    "call_guards": "ты зовёшь стражу",
    "comment": "ты вскрикнул",
    "look_at": "ты смотришь на героя",
    "greet": "ты кивнул герою",
    "approach_talk": "ты идёшь к герою",
    "step_away": "ты отвернулся",
    "tell_to_leave": "ты велел герою убираться",
    "thank": "ты кивнул в знак благодарности",
}


def hears(distance_cm: float, in_view: bool) -> bool:
    """The hero hears a free npc's words: it is close, or he sees it a little farther off."""
    return distance_cm <= FREE_HEARING_CM or (in_view and distance_cm <= FREE_SEEING_CM)


def describe_ru(stim: Stimulus) -> str:
    template = EVENTS_RU.get(stim.kind)
    if template is None:
        return stim.describe(stim.at)
    if stim.other == "PLAYER":
        other = "герой"
    elif stim.other_name == "you":
        other = "ты сам"  # e.g. a scared shepherd who killed the wolf: news to him too
    else:
        other = stim.other_name or stim.other
    return template.format(other=other, detail=stim.detail or "")


def reasons(stimuli: list[Stimulus]) -> frozenset[tuple[str, str]]:
    """What a free npc's words would be about: its salient events and who caused them."""
    return frozenset((s.kind, s.other) for s in stimuli if s.policy.salient)


LOW_HEALTH = 0.5
SETTLE_S = 0.5  # events this close together go to the conversational model as one

# how a perception of a controlled npc reads as an incident event (the rest are ignored)
FOLLOW_REASONS = {
    "target_gone": "герой пропал",
    "target_too_far": "герой ушёл слишком далеко",
    "timeout": "надоело",
}


def classify(p: Perception, names: dict[str, str], now: float) -> IncidentEvent | None:
    other_name = names.get(p.other)
    match p.perc:
        case "ASSESSDAMAGE":
            return IncidentEvent("attacked", now, p.other, other_name)
        case "ASSESSOTHERSDAMAGE":
            kind = "hero_attacked" if p.victim == "PLAYER" else "other_attacked"
            return IncidentEvent(kind, now, p.other, other_name, names.get(p.victim or "", p.victim))
        case "ASSESSMURDER":
            return IncidentEvent("murder", now, p.other, other_name, names.get(p.victim or "", p.victim))
        case "ASSESSMAGIC":
            return IncidentEvent("magic", now, p.other, other_name)
    return None


class IncidentDesk:
    def __init__(
        self,
        link: GameLink,
        dialogue: DialogueManager,
        reflexes: Reflexes | None,
        budget: Budget,
        trace: TraceWriter | None = None,
        *,
        clock: Callable[[], float] = time.monotonic,
        settle_s: float = SETTLE_S,
        gap_s: float = INCIDENT_GAP_S,
        planner: NpcPlanner | None = None,
    ) -> None:
        self.link = link
        self.dialogue = dialogue
        self.reflexes = reflexes
        self.budget = budget
        self.trace = trace
        self.clock = clock
        self.settle_s = settle_s
        self.tracker = IncidentTracker(gap_s)
        self.attached: set[str] = set()
        self.reactions: list[Reaction] = []  # recent ones, for the console and tests
        # what an npc perceives now, for the behavioral model (perception.py)
        self.view_of: Callable[[str], tuple[str, ...]] = lambda npc: ()
        self._tasks: set[asyncio.Task[Any]] = set()
        link.listeners.append(self.on_message)
        dialogue.first_reaction = self.first_reaction
        dialogue.on_body_failed = self.on_body_failed
        dialogue.controls = lambda npc: npc.upper() in self.attached
        dialogue.on_free_intent = self.on_free_intent
        self.planner: NpcPlanner | None = None
        # the hero's companions fight for him on their own (orders.py): whether the npc is one, and what its
        # body does in the fight now, for its words
        self.companion: Callable[[str], bool] = lambda npc: False
        self.fighting_ru: Callable[[str], str | None] = lambda npc: None
        # near the hero's conversation, whether a free npc speaks up at his words is asked there
        # (interject.py)
        self.speaks_up_there: Callable[[str], bool] = lambda npc: False
        # the npc struck the hero or a companion a moment ago (orders.py): it is in the fight too
        self.foe: Callable[[str], bool] = lambda npc: False
        dialogue.in_fight = self.in_fight
        # free npc -> when its words were last asked for, and what about
        self._spoke: dict[str, tuple[float, frozenset[tuple[str, str]]]] = {}
        self._calming: set[str] = set()  # free npc's waiting for their moment to pass
        self._releasing: set[str] = set()  # a threat is freeing its listening body before a reaction
        self.calm_poll_s = 1.0
        if planner is not None:
            self.follow(planner)

    # --- events ---------------------------------------------------------------------------

    def on_message(self, msg: GameMessage) -> None:
        match msg:
            case Attached():
                self.attached.add(msg.npc.upper())
            case Detached():
                npc = msg.npc.upper()
                self.attached.discard(npc)
                if msg.reason == "down":
                    self._event(npc, IncidentEvent("down", self.clock()))
                self.tracker.close(npc)
            case Perception():
                npc = msg.npc.upper()
                if npc in self.attached and npc in self.dialogue.conversations:
                    event = classify(msg, self._names(npc), self.clock())
                    if event is not None:
                        self._event(npc, event)
            case NpcEvent(kind="npc_threat"):
                npc = msg.npc.upper()
                if npc in self.attached and not self.companion(npc):
                    self._spawn(
                        self._release_for_threat(npc, msg.kind, msg.other, msg.other_name, msg.distance)
                    )
            case FollowEnded():
                npc = msg.npc.upper()
                if msg.reason in FOLLOW_REASONS and npc in self.dialogue.conversations:
                    self._event(
                        npc, IncidentEvent("follow_ended", self.clock(), detail=FOLLOW_REASONS[msg.reason])
                    )
            case Loading() | Session():
                for npc in list(self.tracker.current):
                    self.tracker.close(npc)

    def on_body_failed(self, npc: str, what: str) -> None:
        if npc in self.attached:
            self._event(npc, IncidentEvent("action_failed", self.clock(), detail=what.strip("[]")))

    async def first_reaction(self, npc: str, text: str) -> FirstReaction:
        """The hero spoke to the npc: if it is ours, the behavioral model's gesture while it thinks of an
        answer (made when the dialogue calls for it, after its first words)."""
        if self.reflexes is None or npc not in self.attached:
            return FirstReaction()
        ctx, clock = self._context(npc)
        conv = self.dialogue.conversations.get(npc)
        move = await self.reflexes.phrase(ctx, text, conv.intent if conv else None, clock)
        return FirstReaction((lambda: self._spawn(self._gesture(move))) if move is not None else None)

    def in_fight(self, npc: str) -> bool:
        """The npc is in a fight, its lines are quick ones (dialogue.py): a threat to its life or the hero's
        is going on (a controlled npc's incident, a free npc's stimuli), or it fights with or against him."""
        npc = npc.upper()
        inc = self.tracker.current.get(npc)
        if inc is not None and inc.urgent and inc.active(self.clock(), self.tracker.gap_s):
            return True
        if self.planner is not None and self.planner.stirred(npc):
            return True
        return self.fighting_ru(npc) is not None or self.foe(npc)

    def _event(self, npc: str, event: IncidentEvent) -> None:
        inc = self.tracker.add(npc, event)
        log.info("%s incident #%d: %s", npc, inc.id, event.describe_en())
        if inc.version == 1:
            self._spawn(self._watch(npc, inc.id))
        if event.spec.urgent:
            self.dialogue.prewarm(npc)  # its words come after `settle_s`: the quick model reads meanwhile
        if event.kind == "down":
            self.dialogue.note(npc, event.describe_ru())
            return
        if event.spec.urgent and self.companion(npc):
            self._spawn(self._escalate_later(npc, inc.id))  # its body fights already (orders.py)
            return
        if event.kind == "attacked" and event.other and event.other != "PLAYER" and self.planner is not None:
            self._spawn(self._release_for_threat(npc, "npc_attack", event.other, event.other_name))
            return
        if event.kind == "attacked" and self._beaten(npc, inc, event):
            self._spawn(self._release(npc, event))
            return
        if self.reflexes is not None and event.spec.urgent:
            self._spawn(self._react(npc, inc.id))
        self._spawn(self._escalate_later(npc, inc.id))

    def _beaten(self, npc: str, inc: Any, event: IncidentEvent) -> bool:
        if event.other and inc.hits_from(event.other, event.at - SECOND_HIT_S) >= 2:
            return True
        snap = self.link.snapshots.get(npc)
        return snap is not None and snap.npc.hp_max > 0 and snap.npc.hp < snap.npc.hp_max * LOW_HEALTH

    # --- the fast loop ----------------------------------------------------------------------

    def _context(self, npc: str) -> tuple[NpcContext, str | None]:
        conv = self.dialogue.conversations.get(npc)
        snap = self.link.snapshots.get(npc)
        name = conv.persona.name if conv else npc
        traits = conv.persona.traits if conv else None
        distance = snap.player.distance if snap is not None and snap.player is not None else 0.0
        clock = clock_text(snap.world.hour, snap.world.minute) if snap is not None else None
        return NpcContext(npc, name, -1, distance, True, traits=traits, view=self.view_of(npc)), clock

    async def _react(self, npc: str, incident_id: int) -> None:
        assert self.reflexes is not None
        inc = self.tracker.current.get(npc)
        if inc is None or inc.id != incident_id:
            return
        ctx, clock = self._context(npc)
        conv = self.dialogue.conversations.get(npc)
        reaction = await self.reflexes.react(ctx, inc, conv.intent if conv else None, clock)
        if reaction is not None:
            await self._apply(reaction, incident_id)

    async def _gesture(self, reaction: Reaction) -> None:
        npc = reaction.npc
        if reaction.applied and (self.dialogue.has_spoken(npc) or self.dialogue.body_busy(npc)):
            # too late (the answer started) or the body is busy with a commitment
            reaction.applied, reaction.why = (
                False,
                "too late" if self.dialogue.has_spoken(npc) else "body busy",
            )
        await self._apply(reaction, None)

    async def _apply(self, reaction: Reaction, incident_id: int | None) -> None:
        self.reactions = [*self.reactions[-49:], reaction]
        if self.trace is not None:
            self.trace.write({**reaction.record(), "incident": incident_id})
        log.info(
            "%s reflex %s=%s (%.2f, %.0f ms)%s",
            reaction.npc,
            reaction.question,
            reaction.choice,
            reaction.confidence,
            reaction.latency_ms,
            "" if reaction.applied else f" not applied: {reaction.why}",
        )
        if not reaction.applied or reaction.npc not in self.attached:
            return
        for action, args in reaction.actions:
            try:
                await self.link.act(reaction.npc, action, timeout=30, **args)  # type: ignore[arg-type]
            except (GameError, TimeoutError) as err:
                log.info("%s reflex action %s failed: %s", reaction.npc, action, err)

    async def _release(self, npc: str, event: IncidentEvent) -> None:
        """The safety rule: a beaten npc goes back to the game's scripts, which fight back or flee."""
        log.info("%s: beaten (%s), handing over to the game's scripts", npc, event.describe_en())
        self.tracker.close(npc)
        self.dialogue.note(npc, f"{event.describe_ru()}. Ты прервал разговор и защищаешься")
        try:
            await self.link.act(npc, "return_to_routine", timeout=10)
        except (GameError, TimeoutError) as err:
            log.info("%s: release failed: %s", npc, err)

    async def _release_for_threat(
        self,
        npc: str,
        kind: str,
        other: str,
        other_name: str | None,
        distance_cm: float | None = None,
    ) -> None:
        """Listening never owns a threatened NPC's body. Detach first so npc_act can start native combat
        against its actual attacker; Detached also clears talk.py's listening hold through its listener."""
        if self.planner is None or npc in self._releasing:
            return
        self._releasing.add(npc)
        try:
            if npc in self.attached:
                await self.link.detach(npc)
            self.planner.on_event(
                npc, kind, self.clock(), other=other, other_name=other_name, distance_cm=distance_cm
            )
            conv = self.dialogue.conversations.get(npc)
            if conv is not None and conv.calm is not None:
                intent = conv.intent
                choice = INTENT_BODY.get(intent.goal)
                same_threat = intent.target is not None and intent.target.upper() == other.upper()
                if choice and (
                    same_threat
                    or intent.goal == "seek_help"
                    or (intent.goal == "flee" and intent.target is None)
                ):
                    # An attached listener may have already chosen to run in its own words. Preserve
                    # that live choice after releasing its body, against this actual threat.
                    self.planner.take_body(npc, choice, target=other)
        except (GameError, TimeoutError) as err:
            log.info("%s: could not free its listening body for %s: %s", npc, kind, err)
        finally:
            self._releasing.discard(npc)

    async def _watch(self, npc: str, incident_id: int) -> None:
        """When the incident is over, so are the goals that only held during it."""
        while True:
            inc = self.tracker.current.get(npc)
            if inc is None or inc.id != incident_id or not inc.active(self.clock(), self.tracker.gap_s):
                break
            await asyncio.sleep(max(0.05, inc.last + self.tracker.gap_s - self.clock()))
        self.dialogue.incident_over(npc)

    # --- the slow loop ----------------------------------------------------------------------

    async def _escalate_later(self, npc: str, incident_id: int) -> None:
        await asyncio.sleep(self.settle_s)  # a burst of events becomes one call
        inc = self.tracker.current.get(npc)
        if inc is None or inc.id != incident_id or npc not in self.attached:
            return
        snap = self.link.snapshots.get(npc)
        hero = snap.player.distance if snap is not None and snap.player is not None else None
        reason = escalation_reason(inc, self.clock(), hero)
        if reason is None:
            return
        if not self.budget.allow(npc):
            log.info("%s incident #%d: no budget for words", npc, inc.id)
            return
        inc.escalated_version, inc.escalated_at = inc.version, self.clock()
        body = [r for r in self.reactions if r.npc == npc and r.applied and r.question == "react"]
        did = REACT_OPTIONS[body[-1].choice].ru if body and body[-1].choice in REACT_OPTIONS else ""
        did = self.fighting_ru(npc) or did
        done = f" Твоё тело уже среагировало: {did} — это уже случилось, не противоречь этому." if did else ""
        cue = f"[Событие: {inc.describe_ru()}.{done}]"
        log.info("%s incident #%d: calling the conversational model (%s)", npc, inc.id, reason)
        self.dialogue.event_turn(
            npc,
            cue,
            relevant=lambda: self.tracker.is_current(npc, incident_id, self.clock()),
            overlay=inc.urgent,  # shouted on the move: the body keeps running or fighting
        )

    # --- free npc's -------------------------------------------------------------------------

    def follow(self, planner: NpcPlanner) -> None:
        """Free npc's: the planner's fast loop reacts; when the moment calls for words, they come here."""
        self.planner = planner
        planner.on_escalate = self.on_planner_escalate

    def on_planner_escalate(self, npc: str, level: float, reaction: Decision | None, flight: Flight) -> None:
        """The behavioral model says how much a free npc's moment calls for its own words: from the planner's
        `escalate_level` on, or when the danger it spoke about is over, the conversational model speaks for
        it, on the move, if the hero hears it, not too often and within the budget."""
        assert self.planner is not None
        npc = npc.upper()
        rec = self.planner.npcs.get(npc)
        if rec is None or npc in self.attached:  # a controlled npc's incidents are handled above
            return
        now = self.clock()
        alive = [s for s in rec.stimuli if s.alive(self.planner.clock())]
        about = reasons(alive)
        spoke_at, spoke_about = self._spoke.get(npc, (float("-inf"), frozenset()))
        new = about - spoke_about
        settles = (
            now - spoke_at <= FREE_SETTLE_S
            and any(kind in ENDS_DANGER for kind, _ in new)
            and any(policy(kind).threat for kind, _ in spoke_about)
        )
        if level < self.planner.cfg.escalate_level and not settles:
            return
        if {kind for kind, _ in about} <= {"player_speech"} and self.speaks_up_there(npc):
            return  # only the hero's words: the conversation asks it whether it speaks up, by its character
        gap = FREE_NEW_REASON_S if new else FREE_COOLDOWN_S
        if not hears(rec.distance_cm, rec.in_view) or now - spoke_at < gap:
            return
        if not self.budget.allow(npc):
            log.info("%s: no budget for words (escalate %.1f)", npc, level)
            return
        self._spoke[npc] = (now, about)
        happened = "; ".join(describe_ru(s) for s in alive) or "что-то случилось"
        # its own kill: the scripts already ended the fight with a stock line, nothing of ours was done
        did = REACTIONS_RU.get(reaction.choice, "") if reaction is not None and not reaction.own else ""
        done = f" Твоё тело уже среагировало: {did} — это уже случилось, не противоречь этому." if did else ""
        after = (
            " Опасность, о которой ты только что говорил, позади: отзовись на это одной короткой фразой,"
            " помня свои прежние слова."
            if settles
            else ""
        )
        self.dialogue.open(npc, rec.name)
        log.info(
            "%s: escalate %.1f%s, the conversational model speaks for it",
            npc,
            level,
            ", the danger is over" if settles else "",
        )
        self._spawn(self._free_turn(npc, f"[Событие: {happened}.{done}{after}]"))
        if npc not in self._calming:
            self._spawn(self._calm_later(npc))

    async def _free_turn(self, npc: str, cue: str) -> None:
        """A free npc's words on the move. A turn of its still running is let finish first: an event that
        comes meanwhile would wait for the npc's next turn, and nobody else starts one for a free npc. One
        that does not finish in time is dropped for the newer event."""
        conv = self.dialogue.conversations.get(npc)
        if conv is not None and conv.reply is not None and not conv.reply.done():
            _, running = await asyncio.wait([conv.reply], timeout=FREE_TURN_WAIT_S)
            if running:
                log.info("%s: the turn before is stuck, dropped for the new event", npc)
                conv.reply.cancel()
                await asyncio.wait([conv.reply])
        self.dialogue.event_turn(
            npc,
            cue,
            relevant=lambda: self.planner is not None and self.planner.stirred(npc),
            overlay=True,
            tools=FREE_TOOLS,
        )

    def on_free_intent(self, npc: str, old: Intent, new: Intent) -> None:
        """A free npc's goal of the moment (flee, fight, seek help) is carried out by its body, through the
        planner, so that its words and deeds agree; the npc's routine is the planner's business otherwise."""
        choice = INTENT_BODY.get(new.goal)
        if self.planner is None:
            return
        if choice is None:
            self.planner.clear_body_intent(npc)
            return
        if (new.goal, new.target) == (old.goal, old.target):
            return
        decision = self.planner.take_body(npc, choice, target=new.target if new.goal != "seek_help" else None)
        log.info(
            "%s: intent %s moves the body: %s",
            npc,
            new.goal,
            decision.choice if decision else "nothing to do",
        )

    async def _calm_later(self, npc: str) -> None:
        """A free npc's goals of the moment end with it: once nothing threatens it any more, its intent goes
        back to what it was (as a controlled npc's does when its incident is over)."""
        self._calming.add(npc)
        try:
            while True:
                if self.planner is None or not self.planner.stirred(npc):
                    break
                await asyncio.sleep(self.calm_poll_s)
            self.dialogue.incident_over(npc)
        finally:
            self._calming.discard(npc)

    # --- helpers ----------------------------------------------------------------------------

    def _names(self, npc: str) -> dict[str, str]:
        snap = self.link.snapshots.get(npc)
        names = {"PLAYER": "герой"}
        if snap is not None:
            names.update({o.id: o.name for o in snap.nearby_npcs})
        return names

    def _spawn(self, coro: Coroutine[Any, Any, Any]) -> None:
        task = asyncio.get_running_loop().create_task(coro)
        self._tasks.add(task)
        task.add_done_callback(self._tasks.discard)
