"""Dialogue intents and body control."""

from __future__ import annotations

import logging
from typing import Any

from . import executor
from .dialogue_constants import CALM_ATTITUDES, KILLERS
from .dialogue_state import Conversation, TurnState
from .dialogue_text import follow_agreement, game_time
from .executor import Intent
from .game import GameError
from .llm import ToolCall
from .protocol import Snapshot
from .turn import ToolOutcome

log = logging.getLogger("gothic_ai.dialogue")


class DialogueBodyMixin:
    def _controlled(self, npc: str) -> bool:
        return self.controls is None or self.controls(npc)

    async def _hold(self, conv: Conversation, why: str) -> bool:
        """The npc's body for a commitment: a free npc is held (attached) for it, if it can be."""
        if self.take_body is None:
            return self._controlled(conv.npc)
        return await self.take_body(conv.npc, why)

    def _free(self, conv: Conversation, why: str) -> None:
        if self.free_body is not None:
            self.free_body(conv.npc, why)

    def _set_intent(
        self, conv: Conversation, call: ToolCall, snap: Snapshot | None, *, state: TurnState | None = None
    ) -> ToolOutcome:
        sight = self.sight(conv.npc, snap) if self.sight is not None else None
        seen = sight.targets if sight is not None else None  # whom it perceives, not all within 15 m
        intent, reason = executor.check_intent(
            executor.parse_intent(call.arguments, conv.intent), snap, seen,
            fight_reason=(
                (lambda target: self.fight_reason(conv.npc, snap, target)) if self.fight_reason else None
            ),
        )
        if intent is None:
            return ToolOutcome(f"Отклонено: {reason}", "rejected")
        if (
            intent.goal == "fight" and self.companion_fight_reason is not None
            and not (state is not None and conv.trigger is not None and conv.trigger.role == "user")
        ):
            if reason := self.companion_fight_reason(conv.npc, intent.target):
                return ToolOutcome(f"Отклонено: {reason}", "rejected")
        if executor.same(conv.intent, intent):
            if (
                intent.goal not in executor.COMPANION_GOALS
                or snap is None
                or snap.npc.following
                or self.fighting(conv.npc)
                or (state is not None and state.follow_set)
            ):
                return ToolOutcome("Намерение не изменилось.", "accepted")

            async def follow() -> tuple[str, bool]:
                if not await self._hold(conv, "follow"):
                    return "Тело не смогло выполнить намерение: ты занят", False
                return await self._body(conv, [("follow", {"npc": "PLAYER"})])

            self._journal_call(conv, call.name, call.arguments)
            self._after_speech(conv, follow)
            return ToolOutcome(f"Принято: {intent.describe()}.", "accepted")
        self._journal_call(conv, call.name, call.arguments)
        body = not (state is not None and state.follow_set and intent.goal in executor.COMPANION_GOALS)
        self._change_intent(conv, intent, snap, body=body)
        return ToolOutcome(f"Принято: {intent.describe()}.", "accepted")

    def _repair_follow_agreement(self, conv: Conversation, heard: str, reply: str) -> None:
        """Carry out a spoken commitment when the conversational model forgot its tool call."""
        if not follow_agreement(heard, reply, conv.persona.name):
            return
        snap = self.link.snapshots.get(conv.npc)
        if conv.intent.goal == "follow_hero" and (
            snap is None or snap.npc.following or self.fighting(conv.npc)
        ):
            return
        outcome = self._set_intent(
            conv,
            ToolCall(
                "set_intent",
                {
                    "goal": "follow_hero",
                    "target": "PLAYER",
                    "attitude": conv.intent.attitude,
                    "agenda": None,
                },
            ),
            snap,
        )
        if outcome.status == "accepted":
            self._note(conv, f"Герой позвал тебя: «{heard}» — идти за ним. Ты согласился и идёшь за героем.")

    def _change_intent(
        self, conv: Conversation, intent: Intent, snap: Snapshot | None, *, body: bool = True
    ) -> None:
        old = conv.intent
        if intent.goal in executor.INCIDENT_GOALS and old.goal not in executor.INCIDENT_GOALS:
            conv.calm = old  # what to go back to when the incident is over
        conv.intent = intent
        with_hero = old.goal in executor.COMPANION_GOALS or (
            old.goal in executor.INCIDENT_GOALS
            and conv.calm is not None
            and conv.calm.goal in executor.COMPANION_GOALS
        )
        seq = self.memory.set_intent(
            conv.branch, conv.npc, intent.to_json(), game_time(self.link.snapshots.get(conv.npc))
        )
        self._send_head(conv.branch, seq)
        if self.on_intent is not None:
            self.on_intent(conv.npc, intent)
        following = snap.npc.following if snap else False
        acts = executor.intent_actions(old, intent, following)
        if body and intent.goal == "fight" and with_hero and self.companion_fight is not None:
            if self.companion_fight(conv.npc, intent.target):
                return  # a companion's fight: it stays with the hero (orders.py)
        if old.goal in executor.COMPANION_GOALS and intent.goal not in executor.COMPANION_GOALS:
            self._free(conv, "follow")  # after `stop`, if it is still following (below)
        if not body:
            return
        fighting = (old.goal, old.target) == ("fight", "PLAYER") or self.fights_hero(conv.npc)
        if fighting and intent.goal != "fight" and intent.attitude != "hostile":
            self._after_speech(conv, lambda: self._calm(conv, intent))  # talked down: it stops
            return
        if (
            intent.goal == "fight"
            and intent.target == "PLAYER"
            and (old.goal, old.target) != ("fight", "PLAYER")
        ):
            self._after_speech(conv, lambda: self._attack_hero(conv, intent))
            return
        if self.fighting(conv.npc) and intent.goal in executor.COMPANION_GOALS:
            return  # it is fighting for the hero: it follows him again once the fight is over (orders.py)
        if intent.goal in executor.INCIDENT_GOALS and not self._controlled(conv.npc):
            # a free npc's goal of the moment (flee, fight, seek help) is carried out by whoever runs its body
            # (the planner): held for it, the npc would be out of the scripts in the middle of the danger
            if self.on_free_intent is not None:
                self.on_free_intent(conv.npc, old, intent)
            return
        if not acts:
            return
        if self.take_body is None and not self._controlled(conv.npc):
            return  # nobody can lend us a free npc's body
        why = "follow" if intent.goal == "follow_hero" else "intent"

        async def job() -> tuple[str, bool]:
            if not await self._hold(conv, why):
                return "Тело не смогло выполнить намерение: ты занят", False
            try:
                return await self._body(conv, acts)
            finally:
                if why != "follow":
                    self._free(conv, why)

        self._after_speech(conv, job)

    def incident_over(self, npc: str, *, body: bool = True) -> None:
        """Goals that only hold while something is going on (flee, fight...) end with it: the npc goes
        back to what it wanted before, keeping its attitude and agenda. `body` False: the caller moves
        the body (orders.py)."""
        conv = self.conversations.get(npc.upper())
        if conv is None or conv.intent.goal not in executor.INCIDENT_GOALS:
            return
        calm = conv.calm or Intent()
        intent = Intent(calm.goal, calm.target, conv.intent.attitude, conv.intent.agenda)
        log.info("%s: the incident is over, intent back to %s", conv.npc, intent.goal)
        self._change_intent(conv, intent, self.link.snapshots.get(conv.npc), body=body)

    async def _attack_hero(
        self, conv: Conversation, intent: Intent, *, resume: bool = False,
    ) -> tuple[str, bool]:
        """The npc turns on the hero, after its words: the scripts' own attack (B_Attack), held or free, so
        the fight goes as the game's do. A bandit means to kill and its gang joins it; anyone else gives him
        a beating until he is down."""
        if conv.intent is not intent:
            return "", True  # it changed its mind before it was done talking
        if self.hero_fight_held(conv.npc):
            return "Ты опустил оружие и слушаешь героя; решение о сдаче ещё не принято.", True
        if not resume and self.fight_reason is not None:
            if why := self.fight_reason(conv.npc, self.link.snapshots.get(conv.npc), "PLAYER"):
                self._change_intent(
                    conv, conv.calm or Intent(), self.link.snapshots.get(conv.npc), body=False
                )
                return f"Ты не напал: {why}", False
        reason = "AR_SUDDENENEMYINFERNO" if self.guild_of(conv.npc) in KILLERS else "AR_NONE"
        try:
            await self.link.story_effect(conv.npc, [{"do": "attack", "reason": reason}])
        except (GameError, TimeoutError) as err:
            return f"Тело не напало на героя ({err})", False
        log.info("%s attacks the hero (%s)", conv.npc, reason)
        self.journal.add("attack", npc=conv.npc, name=conv.persona.name, target="PLAYER", reason=reason)
        for listener in self.on_attack:
            listener(conv.npc)
        return "Ты напал на героя.", True

    async def _calm(
        self, conv: Conversation, intent: Intent, *, effects: list[dict[str, Any]] | None = None,
        native_attitude: str | None = None,
    ) -> tuple[str, bool]:
        """The npc calls off its fight with the hero, after its words, and keeps to it: its attitude to him
        is set as the scripts set it (they would fall on him again otherwise), the fight ends now."""
        if conv.intent is not intent:
            return "", True  # it changed its mind before it was done talking
        attitude = native_attitude
        if attitude is None:
            attitude = CALM_ATTITUDES.get(intent.attitude, "ATT_NEUTRAL")
        try:
            await self.link.story_effect(
                conv.npc, [*(effects or []), {"do": "calm", "attitude": attitude}], check=bool(effects),
            )
        except (GameError, TimeoutError) as err:
            return f"Тело не прекратило драку ({err})", False
        log.info("%s calls off its fight with the hero (%s)", conv.npc, attitude)
        self.journal.add("calm", npc=conv.npc, name=conv.persona.name, attitude=attitude)
        for listener in self.on_calm:
            listener(conv.npc)
        return "Ты прекратил драку с героем.", True

    async def _body(self, conv: Conversation, acts: list[executor.Action]) -> tuple[str, bool]:
        """Game actions for an intent; only what did not work is noted."""
        failed = []
        for action, args in acts:
            if action == "fight" and self.on_fight is not None:
                self.on_fight(conv.npc, args["npc"])
            try:
                res = await self.link.act(conv.npc, action, **args)  # type: ignore[arg-type]
            except (GameError, TimeoutError) as err:
                failed.append(f"{action}: {err}")
                continue
            if res.status != "done":
                failed.append(f"{action}: {res.detail or res.status}")
        if failed:
            return f"Тело не выполнило намерение ({'; '.join(failed)})", False
        return "", True
