"""Decisions and what happens to them: `GameApplier` turns them into soft actions in the game (npc_act,
queued like script actions; the routine resumes by itself), `LogApplier` only logs them."""

from __future__ import annotations

import asyncio
import logging
from collections import deque
from collections.abc import Awaitable, Callable
from dataclasses import dataclass, field
from typing import TYPE_CHECKING, Any, Protocol

from ..protocol import NOISE_PROTOCOL

if TYPE_CHECKING:
    from ..game import GameLink

log = logging.getLogger(__name__)


@dataclass(frozen=True)
class Decision:
    npc: str
    question: str  # "react" or "plan"
    choice: str
    confidence: float
    probabilities: dict[str, float]
    stimuli: tuple[str, ...] = field(default_factory=tuple)  # kinds that led to it
    attitude: float | None = None  # expected level 0..4 when it was asked
    latency_ms: float = 0.0  # from the request leaving the planner to the answer
    target: str | None = None  # whom a fight or flight is about (the newest threat's `other`)
    args: dict[str, Any] | None = None  # a plan's activity, or a reaction's destination (noise)
    own: bool = False  # the newest event is the npc's own doing (it killed the monster itself)


class Applier(Protocol):
    def apply(self, decision: Decision) -> None: ...

    def verdict(self, hold: str, to: str, *, npc: str | None = None) -> None:
        """A held threat: `server` (the model took the body over) or `scripts` (they get it now)."""
        ...


class LogApplier:
    def __init__(self, keep: int = 200) -> None:
        self.recent: deque[Decision] = deque(maxlen=keep)
        self.verdicts: deque[tuple[str, str]] = deque(maxlen=keep)

    def verdict(self, hold: str, to: str, *, npc: str | None = None) -> None:
        self.verdicts.append((hold, to))
        log.info("held threat %s -> %s", hold, to)

    def apply(self, decision: Decision) -> None:
        self.recent.append(decision)
        log.info(
            "decision %s %s=%s (%.2f) after %s in %.0f ms",
            decision.npc,
            decision.question,
            decision.choice,
            decision.confidence,
            ",".join(decision.stimuli) or "timer",
            decision.latency_ms,
        )


# choice -> soft actions; voiced SVM lines are the npc's own recordings ("$NAME" in the scripts)
ACTIONS: dict[str, list[tuple[str, dict[str, Any]]]] = {
    "look_at": [("look_at", {"ms": 3000})],
    "greet": [("play_ani", {"name": "T_GREETNOV"})],
    "approach_talk": [("go_to_npc", {})],
    "step_away": [("turn_away", {})],
    "tell_to_leave": [("say_svm", {"svm": "GETOUTOFHERE"})],
    "thank": [("play_ani", {"name": "T_YES"})],
    "call_guards": [("say_svm", {"svm": "GUARDS"})],
    # the game's own combat and fleeing, against the threat (protocol v6)
    "fight": [("fight", {"npc": "{target}"})],
    "flee": [("flee", {"npc": "{target}"})],
}
# "comment": the line fitting the newest event; without one the npc just looks at the player
COMMENT_SVM = {
    "enter_room": "GETOUTOFHERE",
    "weapon_drawn": "WEAPONDOWN",
    "theft": "DIRTYTHIEF",
    "magic": "STOPMAGIC",
    "item_given": "NOTBAD",
    "attack": "NEVERHITMEAGAIN",
    "monster_near": "HELP",
    "monster_attack": "HELP",
    "npc_attack": "HELP",
    "npc_threat": "WEAPONDOWN",
    "fight_nearby": "OHMYGODITSAFIGHT",
    "monster_killed": "GOODMONSTERKILL",
    "noise": "WHATWASTHAT",
}


def actions_for(decision: Decision) -> list[tuple[str, dict[str, Any]]]:
    if decision.question == "plan":
        return [("start_activity", dict(decision.args))] if decision.args else []
    if decision.question != "react":
        return []  # attitudes and escalations have no game action
    if decision.choice == "investigate":
        return [("investigate", dict(decision.args))] if decision.args and "at" in decision.args else []
    if decision.choice == "look_at" and "noise" in decision.stimuli:
        return []  # the bridge has already turned it toward the sound, not toward the hero
    if decision.choice == "call_guards" and "noise" in decision.stimuli:
        return [("say_svm", {"svm": "GUARDS", "overlay": True})]
    if decision.choice == "comment":
        if decision.own:
            return []  # its own kill: the scripts end a fight with a line of their own (B_Say_AttackEnd)
        svm = next((COMMENT_SVM[k] for k in decision.stimuli if k in COMMENT_SVM), None)
        if svm == "WHATWASTHAT":
            return [("say_svm", {"svm": svm, "overlay": True})]
        return [("say_svm", {"svm": svm})] if svm else ACTIONS["look_at"]
    if decision.choice == "call_help":
        return call_help(decision)
    acts = ACTIONS.get(decision.choice, [])
    if any(v == "{target}" for _, args in acts for v in args.values()):
        if not decision.target:
            return []  # a fight or flight without someone to fight or flee from
        acts = [
            (a, {k: decision.target if v == "{target}" else v for k, v in args.items()}) for a, args in acts
        ]
    if "npc_threat" in decision.stimuli and decision.target:
        # The warning is shouted on the move. Do not wait for spoken dialogue before defending against
        # the armed attacker; the native fight state takes the body immediately afterwards.
        if decision.choice in {"fight", "flee"}:
            svm = "WEAPONDOWN" if decision.choice == "fight" else "HELP"
            return [(decision.choice, {"npc": decision.target, "svm": svm})]
    return acts


def call_help(decision: Decision) -> list[tuple[str, dict[str, Any]]]:
    """Shouted on the move while the body gets away from the threat (the game's own fleeing): busy with it,
    the npc is not led into a fight by its scripts. Not to the hero: an npc that comes up to him starts its
    original dialog with him (an important info, ZS_Talk), wolf or no wolf. The flight's own shout ($RUNAWAY)
    is dropped while this one sounds: the game plays one overlay line at a time."""
    if not decision.target:
        return [("say_svm", {"svm": "HELP"})]
    return [("flee", {"npc": decision.target, "svm": "HELP"})]


class GameApplier(LogApplier):
    """Sends decisions to the game as soft actions."""

    def __init__(
        self,
        link: GameLink,
        keep: int = 200,
        *,
        native_body: Callable[[str], Awaitable[bool]] | None = None,
    ) -> None:
        super().__init__(keep)
        self.link = link
        self.native_body = native_body
        self._tasks: set[asyncio.Task[None]] = set()
        self._combat_tasks: dict[str, asyncio.Task[None]] = {}

    def apply(self, decision: Decision) -> None:
        super().apply(decision)
        acts = actions_for(decision)
        if acts:
            task = asyncio.get_running_loop().create_task(self._send(decision.npc, acts))
            if any(action in {"fight", "flee"} for action, _ in acts):
                self._combat_tasks[decision.npc.upper()] = task
            self._tasks.add(task)
            task.add_done_callback(self._tasks.discard)

    def verdict(self, hold: str, to: str, *, npc: str | None = None) -> None:
        super().verdict(hold, to, npc=npc)
        from ..protocol import HoldVerdictCmd

        after = self._combat_tasks.get(npc.upper()) if npc is not None else None
        task = asyncio.get_running_loop().create_task(
            self._verdict(HoldVerdictCmd(hold=hold, to=to), after=after)  # type: ignore[arg-type]
        )
        self._tasks.add(task)
        task.add_done_callback(self._tasks.discard)

    async def _verdict(self, cmd: Any, *, after: asyncio.Task[None] | None = None) -> None:
        try:
            if after is not None:
                await after  # acquisition resolves and the native action is sent or skipped first
            await self.link.send(cmd)
        except Exception as err:  # the game went away: it hands held threats to the scripts itself
            log.debug("hold_verdict failed: %s", err)

    async def _send(self, npc: str, acts: list[tuple[str, dict[str, Any]]]) -> None:
        try:
            for action, args in acts:
                if action in {"fight", "flee"} and self.native_body is not None:
                    if not await self.native_body(npc):
                        continue  # a combat provider owns or has claimed this fight
                if action == "investigate":
                    hello = self.link.hello
                    if hello is None or hello.protocol < NOISE_PROTOCOL:
                        continue  # older bridges keep their routine; no unknown command on the wire
                await self.link.npc_act(npc, action, **args)  # type: ignore[arg-type]
        except Exception as err:  # the game went away: nothing to do
            log.debug("npc_act for %s failed: %s", npc, err)
