"""NPC-chosen surrender; the current save branch owns surrender and pardon."""

from __future__ import annotations

import asyncio
import logging
import time
import uuid
from collections import deque
from dataclasses import dataclass, field
from typing import TYPE_CHECKING

from . import triggers
from .chronicle import Chronicle, Fact
from .executor import Intent
from .game import GameError
from .llm import Tool, ToolCall
from .perception import BEASTS
from .protocol import (
    CombatHit,
    GameMessage,
    Loading,
    NpcRoster,
    NpcViews,
    Session,
    Snapshot,
    TriggerFired,
)
from .turn import ToolOutcome

if TYPE_CHECKING:
    from .dialogue import Conversation, DialogueManager
    from .orders import Orders
    from .parley import Parley

log = logging.getLogger(__name__)

PARDON = "Ты проиграл бой герою и сдался; он тебя пощадил"
YIELDING = (
    "Ты недавно сдался ему в бою, и он тебя пощадил. Ты помнишь, чем это могло кончиться: "
    "держишься с ним осторожнее и уступчивее, насколько позволяет твой характер."
)
SURRENDER = Tool(
    "surrender",
    "Сдаться герою и его спутникам: прекратить бой, убрать оружие и произнести words на ходу. "
    "Решай сам по характеру, ранам и раскладу сил; можно продолжать бой, бежать или звать на помощь.",
    {"type": "object", "properties": {"words": {"type": "string"}},
     "required": ["words"], "additionalProperties": False},
)


def fresh_surrender(chronicle: Chronicle, npc: str, now: int) -> Fact | None:
    return next((f for f in reversed(chronicle.facts("surrender"))
                 if f["npc"] == npc.upper() and now < f["until"]), None)


@dataclass
class Battle:
    last: float = float("-inf")
    hits: deque[float] = field(default_factory=deque)
    offered: bool = False
    hp: int = 1
    opponent: str = "PLAYER"


class Mercy:
    all_tools = (SURRENDER,)

    def __init__(
        self, dialogue: DialogueManager, chronicle: Chronicle, orders: Orders,
        parley: Parley | None,
    ) -> None:
        self.dialogue, self.link, self.chronicle = dialogue, dialogue.link, chronicle
        self.orders, self.parley = orders, parley
        self.battles: dict[str, Battle] = {}
        self.offering: set[str] = set()
        self._ticks = 0
        self._minute: int | None = None
        self._loading = False
        self._dead: set[str] = set()
        self._armed = False
        self.link.listeners.append(self._on_message)
        dialogue.on_player_said.append(self.hero_said)

    def _ours(self, npc: str) -> bool:
        return npc in {"PLAYER", "PC_HERO"} or self.orders.companion(npc)

    def _latest(self, npc: str) -> Fact | None:
        return next((f for f in reversed(self.chronicle.facts("surrender"))
                     if f["npc"] == npc.upper()), None)

    def _now(self) -> int | None:
        if self._minute is not None:
            return self._minute
        snap = next(iter(self.link.snapshots.values()), None)
        return snap.world.day * 1440 + snap.world.hour * 60 + snap.world.minute if snap else None

    def silent(self, npc: str) -> bool:
        now = self._now()
        return now is not None and fresh_surrender(self.chronicle, npc, now) is not None

    def _alive(self, npc: str) -> bool:
        return npc not in self._dead and self.battles.get(npc, Battle()).hp > 0

    def _fighting(self, npc: str) -> bool:
        battle = self.battles.get(npc)
        return bool(battle and self._alive(npc) and self._ours(battle.opponent)
                    and (time.monotonic() - battle.last < 20 or self.dialogue.in_fight(npc)))

    def _urgent(self, npc: str, cue: str, *, offer: bool = False) -> asyncio.Task | None:
        entry = self.orders.talk.people.get(npc)
        conv = self.dialogue.open(npc, entry.name if entry else npc, warm=False)
        self.dialogue.interrupt(npc)
        self.dialogue._cancel(conv)
        if offer:
            self.offering.add(npc)
        turn = self.dialogue.event_turn(
            npc, cue, relevant=lambda: self._alive(npc), overlay=True,
            tools={"set_intent", "recall", *({"surrender"} if offer else set())}, kind="mercy",
        )
        if turn is not None:
            turn.add_done_callback(lambda _: self.offering.discard(npc))
        return turn

    def _hit(self, hit: CombatHit) -> None:
        npc, attacker = hit.npc.upper(), hit.by.upper()
        if self._ours(npc) or not self._ours(attacker):
            return
        entry = self.orders.talk.people.get(npc)
        if entry is None or entry.guild >= BEASTS:
            return
        now = time.monotonic()
        battle = self.battles.setdefault(npc, Battle())
        if now - battle.last >= 20 and not self.dialogue.in_fight(npc):
            battle.hits.clear()
            # A load must not grant a second opportunity in the saved fight.
            offers = self.chronicle.facts("mercy_offer")
            battle.offered = any(f["npc"] == npc and self._ticks - f["t"] < 20000 for f in offers)
        battle.last, battle.hp, battle.opponent = now, hit.hp, attacker
        if hit.dead or hit.down:
            self._dead.add(npc)
            return
        if (surrender := self._latest(npc)) and surrender["status"] == "pending":
            if attacker in {"PLAYER", "PC_HERO"}:
                self.chronicle.record({**surrender, "status": "attacked"})
                self._urgent(
                    npc, "[Ты сдался, а он бьёт. Реши сам: бежать, драться насмерть или молить ещё.]",
                )
            return
        if battle.offered:
            return
        battle.hits.append(now)
        while battle.hits and battle.hits[0] < now - 5:
            battle.hits.popleft()
        low = hit.hp_max > 0 and hit.hp < hit.hp_max * 0.3
        if low or (self.parley is not None and len(battle.hits) >= 2):
            self.dialogue._spawn(self._offer(npc, battle, hit, low))

    async def _offer(self, npc: str, battle: Battle, hit: CombatHit, low: bool) -> None:
        if not low:
            try:
                snap = await self.link.snapshot(npc, fresh=True)
            except (GameError, TimeoutError) as error:
                log.info("mercy balance for %s: no fresh snapshot: %s", npc, error)
                return
            if snap.npc.hp <= 0:
                return
        if self.battles.get(npc) is not battle or battle.offered or not self._alive(npc):
            return
        balance = self.parley.combat_balance(npc) if self.parley else None
        if not low and (balance is None or "он опасен" not in balance):
            return
        battle.offered = True
        self.chronicle.record({"fact": "mercy_offer", "npc": npc, "t": self._ticks})
        attacker = hit.by.upper()
        who = "герой" if attacker in {"PLAYER", "PC_HERO"} else f"спутник героя {attacker}"
        plight = "ты тяжело ранен, сил почти не осталось" if low else "бой идёт против тебя"
        self._urgent(
            npc, f"[Сейчас бой: {plight}; "
            f"бьёт {who}. {balance or ''} Реши по характеру и положению, сдаваться ли: surrender{{words}}. "
            "Можно продолжать драться, бежать или звать на помощь.]", offer=True,
        )

    def _spare(self, npc: str) -> None:
        fact = self._latest(npc)
        if fact is None or fact["status"] != "pending" or not self._alive(npc):
            return
        self.chronicle.record({**fact, "status": "spared"})
        self.dialogue.remember(npc, fact["name"], PARDON)

    def hero_said(self, npc: str) -> None:
        self._spare(npc.upper())

    def _on_message(self, msg: GameMessage) -> None:
        if isinstance(msg, (Loading, Session)):
            self._loading = isinstance(msg, Loading)
            self.battles.clear()
            self.offering.clear()
            self._dead.clear()
            self._armed = False
            self._ticks, self._minute = 0, None
            return
        if self._loading:
            return
        if isinstance(msg, CombatHit):
            self._hit(msg)
            return
        if isinstance(msg, (Snapshot, NpcViews, NpcRoster)):
            self._ticks = msg.t
            if isinstance(msg, Snapshot):
                self._minute = msg.world.day * 1440 + msg.world.hour * 60 + msg.world.minute
            if isinstance(msg, NpcViews):
                for view in msg.views:
                    if view.hero:
                        self._armed = view.hero.weapon != "none"
                    if view.time is not None:
                        self._minute = view.time
                    if view.hp <= 0 or view.body in {"dead", "down"}:
                        self._dead.add(view.npc.upper())
                    elif view.hero and view.hero.weapon == "none":
                        fact = self._latest(view.npc)
                        if fact and fact.get("armed"):
                            self._spare(view.npc.upper())
            latest = {f["npc"]: f for f in self.chronicle.facts("surrender")}
            for npc, fact in latest.items():
                if fact["status"] == "pending" and msg.t >= fact["deadline"]:
                    self._spare(npc)

    async def prepare(self, npc: str) -> None:
        pass

    def situation(self, npc: str) -> str | None:
        now = self._now()
        fact = fresh_surrender(self.chronicle, npc, now) if now is not None else None
        return YIELDING if fact and fact["status"] == "spared" else None

    def tools(self, npc: str) -> list[Tool]:
        return [SURRENDER] if npc.upper() in self.offering else []

    async def _native_loss(self, npc: str) -> None:
        names = ["AIV_LASTFIGHTAGAINSTPLAYER", "AIV_LASTFIGHTCOMMENT", "AIV_DEFEATEDBYPLAYER"]
        indices = await self.link.story_eval(npc, [["var", name] for name in names])
        key = f"DIR_MERCY_{uuid.uuid4().hex[:12].upper()}"
        done = asyncio.get_running_loop().create_future()

        def fired(msg: GameMessage) -> None:
            if isinstance(msg, TriggerFired) and msg.id == key and not done.done():
                done.set_result(msg)

        self.link.listeners.append(fired)
        try:
            await self.link.patch_graph([triggers.trigger(key, triggers.STATE, [
                {"do": "set", "target": ["field", ["inst", "SELF"], "AIVAR", index], "value": value}
                for index, value in zip(indices, [["var", "FIGHT_LOST"], ["int", 0], ["int", 1]], strict=True)
            ], npc=npc)])
            result = await asyncio.wait_for(done, 3)
            if result.error is not None:
                raise GameError(f"surrender outcome: {result.error}")
        finally:
            self.link.listeners.remove(fired)
            await self.link.patch_graph([triggers.removal(key)])

    async def handle(
        self, dialogue: DialogueManager, conv: Conversation, call: ToolCall, snap: Snapshot | None,
    ) -> ToolOutcome | None:
        if call.name != "surrender":
            return None
        if not self._alive(conv.npc):
            return ToolOutcome("Отклонено: ты мёртв или без сознания.", "rejected")
        if self.silent(conv.npc):
            return ToolOutcome("Отклонено: ты уже сдался.", "rejected")
        if conv.npc not in self.offering or not self._fighting(conv.npc):
            return ToolOutcome(
                "Отклонено: ты не в бою с героем или его спутником либо нет повода сдаться.", "rejected",
            )
        words, now = call.arguments.get("words"), self._now()
        if not isinstance(words, str) or not words.strip():
            return ToolOutcome("Отклонено: укажи слова сдачи в words.", "rejected")
        if now is None:
            return ToolOutcome("Отклонено: время мира ещё неизвестно.", "rejected")
        group = {"PC_HERO", *(n for n in self.orders.talk.people if self.orders.companion(n))}
        [fighting] = await self.link.story_eval(conv.npc, [[
            "&&", ["call", "NPC_GETTARGET", ["inst", "SELF"]],
            ["||", *(["==", ["call", "HLP_GETINSTANCEID", ["inst", "OTHER"]],
                       ["call", "HLP_GETINSTANCEID", ["inst", n]]] for n in sorted(group))],
        ]])
        if not fighting:
            return ToolOutcome("Отклонено: ты больше не в бою с героем или его спутниками.", "rejected")
        try:
            await self._native_loss(conv.npc)
        except (GameError, TimeoutError) as error:
            return ToolOutcome(
                f"Отклонено: игра не подтвердила исход сдачи ({type(error).__name__}: {error}).", "rejected",
            )
        intent = Intent(attitude="wary")
        dialogue._change_intent(conv, intent, snap, body=False)
        _, ok = await dialogue._calm(conv, intent)
        if not ok:
            return ToolOutcome("Отклонено: тело не смогло прекратить бой.", "rejected")
        self.chronicle.record({
            "fact": "surrender", "npc": conv.npc, "name": conv.persona.name,
            "until": (now // 1440 + 2) * 1440, "deadline": self._ticks + 20000,
            "status": "pending", "armed": self._armed,
        })
        self.orders.truce(conv.npc)
        if self.parley:
            await self.parley.accept_deal(conv.npc, 1, now=now)
        dialogue._drop_unvoiced(conv)
        await dialogue._say(conv, words.strip(), None, overlay=True)
        return ToolOutcome(
            "Принято: ты сдался, прекратил бой и убрал оружие; ждёшь решения героя.", "accepted",
        )
