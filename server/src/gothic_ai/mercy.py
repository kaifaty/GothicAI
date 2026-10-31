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
from .contracts import GOLD, SYMBOL, Give
from .executor import Intent
from .game import GameError
from .llm import Item, Tool, ToolCall
from .perception import BEASTS
from .protocol import (
    CombatHit,
    GameMessage,
    LineStarted,
    Loading,
    NpcRoster,
    NpcViews,
    Session,
    Snapshot,
    TriggerFired,
)
from .turn import ToolOutcome

if TYPE_CHECKING:
    from .combat_barks import CombatBarks
    from .crime_law import CrimeLaw
    from .dialogue import Conversation, DialogueManager
    from .orders import Orders
    from .parley import Parley
    from .places import PlaceBook

log = logging.getLogger(__name__)

PEE_DISTANCE_CM = 100  # judged on bench stills: room for the arc between them
PEE_HOLD_MS = 4000
PEE_APPROACH_TIMEOUT_MS = 15000
HUMILIATE_FORBIDDEN_GUILDS = {1, 4, 5, 14}  # PAL, KDF, NOV, KDW
HERO_HOLD_S = 30
# after accepting, the winner and its allies start no native talk (a gate guard's FIRSTWARN) for this long:
# the engine would refuse their words and the humiliation scene while it runs
ACCEPT_QUIET_S = 30

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
VICTOR = (
    "Недавно он сдался тебе в бою, и ты его отпустил. Держишься с ним как победитель — "
    "насколько позволяет твой характер."
)
ACCEPT_SURRENDER = Tool(
    "accept_surrender",
    "Принять сдачу героя: прекратить бой и сказать words на ходу. Решай по характеру и положению, "
    "принимать ли и что брать: можно меньше или больше предложенного, либо отказаться и драться дальше. "
    "take: gold — всё золото героя; weapons — надетое ближнее и дальнее оружие; fine — только для "
    "представителя закона, точная сумма долга crimes, если золота хватает; также до пяти id предметов "
    "из инвентаря героя, по одной штуке. Пустой take — отпустить без выкупа. gold включает fine. "
    "humiliate — обязательный bool: false для обычной сдачи, "
    "true — унизить сдавшегося героя, обмочив его на коленях. "
    "Это обычный исход для грубого, злого или мстительного характера, "
    "особенно если герой сам об этом просит. "
    "Решай по характеру; законникам, магам, паладинам и послушникам запрещено.",
    {"type": "object", "properties": {
        "take": {"type": "array", "items": {"type": "string"}},
        "humiliate": {"type": "boolean", "default": False},
        "words": {"type": "string"},
    }, "required": ["take", "humiliate", "words"], "additionalProperties": False},
)


def fresh_surrender(chronicle: Chronicle, npc: str, now: int) -> Fact | None:
    return next((f for f in reversed(chronicle.facts("surrender"))
                 if f["npc"] == npc.upper() and now < f["until"]), None)


def fresh_yield(chronicle: Chronicle, npc: str, ticks: int) -> Fact | None:
    return next((f for f in reversed(chronicle.facts("hero_yield"))
                 if f["npc"] == npc.upper() and f["t"] <= ticks < f["deadline"]), None)


@dataclass
class Battle:
    last: float = float("-inf")
    hits: deque[float] = field(default_factory=deque)
    offered: bool = False
    hp: int = 1
    opponent: str = "PLAYER"


@dataclass
class HeroHold:
    members: list[str] = field(default_factory=list)
    timer: asyncio.TimerHandle | None = None
    resumed: bool = False


class Mercy:
    all_tools = (SURRENDER, ACCEPT_SURRENDER)

    def __init__(
        self, dialogue: DialogueManager, chronicle: Chronicle, orders: Orders,
        parley: Parley | None, *, law: CrimeLaw | None = None,
        barks: CombatBarks | None = None, places: PlaceBook | None = None,
    ) -> None:
        self.dialogue, self.link, self.chronicle = dialogue, dialogue.link, chronicle
        self.orders, self.parley = orders, parley
        self.law, self.barks, self.places = law, barks, places
        self._scene: asyncio.Task | None = None
        self.accepting: set[str] = set()
        self.battles: dict[str, Battle] = {}
        self.offering: set[str] = set()
        self.holds: dict[str, HeroHold] = {}
        self._hold_turns: dict[str, HeroHold] = {}
        self._resuming: dict[str, asyncio.Task[None]] = {}
        self._fight_words: dict[str, str] = {}
        # False until the resumed native attack appears in a roster; its later end starts a new fight.
        self._hold_used: dict[str, bool] = {}
        self._ticks = 0
        self._minute: int | None = None
        self._loading = False
        self._dead: set[str] = set()
        self._armed = False
        self.link.listeners.append(self._on_message)
        dialogue.on_player_said.append(self.hero_said)
        dialogue.surrender_hold = self.hold_surrender
        dialogue.hero_fight_held = self.held
        dialogue.surrender_context = self._fight_words.get

    def held(self, npc: str) -> bool:
        return any(npc in hold.members for hold in self.holds.values())

    def _allies(self, npc: str) -> list[str]:
        entry = self.orders.talk.people[npc]
        band = set(self.parley.members(npc)) if self.parley and self.parley.bandit(npc) else set()
        snap = self.link.snapshots.get(npc)
        return [
            other.id for other in (snap.nearby_npcs if snap else [])
            if other.id != npc and other.id in self.orders.talk.people and other.distance <= 1000
            and (self.orders.talk.people[other.id].guild == entry.guild or other.id in band)
        ]

    def _release_hold(self, npc: str, hold: HeroHold) -> bool:
        if self.holds.get(npc) is not hold:
            return False
        del self.holds[npc]
        if hold.timer is not None:
            hold.timer.cancel()
        return True

    def _end_hold(self, npc: str, hold: HeroHold, *, force: bool = False) -> None:
        if self._release_hold(npc, hold) or force:
            hold.resumed = True
            task = self._resuming[npc] = asyncio.create_task(self._resume_hold(hold))
            self.dialogue._background.add(task)

            def finished(done: asyncio.Task[None]) -> None:
                self.dialogue._background.discard(done)
                if self._resuming.get(npc) is done:
                    del self._resuming[npc]

            task.add_done_callback(finished)

    async def _resume_hold(self, hold: HeroHold) -> None:
        for member in hold.members:
            if self._loading or not self._alive(member):
                continue
            conv = self.dialogue.conversations[member]
            intent = Intent(goal="fight", target="PLAYER", attitude="hostile")
            self.dialogue._change_intent(conv, intent, self.link.snapshots.get(member), body=False)
            output, ok = await self.dialogue._attack_hero(conv, intent, resume=True)
            if not ok:
                self._hold_used.pop(member, None)
                log.warning("%s: surrender hold could not resume combat: %s", member, output)

    async def hold_surrender(self, conv: Conversation, request: list[Item]) -> None:
        npc = conv.npc
        opponent = self._fight_words.get(npc, "")  # preserve the combat words preceding the plea
        if (self.dialogue.quick is None or npc in self._hold_used or self.held(npc)
                or not self._hero_turn(npc)):
            return
        self.dialogue._surrender_probes += 1
        try:
            async with asyncio.timeout_at(conv.talked_at + 2.0):
                if self.barks is not None and self._scene is None and (
                    task := self.barks.task
                ) is not None and not task.done():
                    task.cancel()
                    await asyncio.gather(task, return_exceptions=True)
                if not await self._hero_fight(npc):
                    return
        except (GameError, TimeoutError) as error:
            log.info("%s: surrender hold unavailable: %s", npc, error)
            return
        finally:
            self.dialogue._surrender_probes -= 1
        if not await self.dialogue._surrendering(conv, request, opponent=opponent):
            return
        if self._loading or not self._hero_turn(npc) or not self._alive(npc):
            return
        hold = self.holds[npc] = HeroHold()
        self._hold_turns[npc] = hold
        hold.timer = asyncio.get_running_loop().call_later(HERO_HOLD_S, self._end_hold, npc, hold)
        assert conv.reply is not None
        talked_at = conv.talked_at

        def finished_turn(done: asyncio.Task) -> None:
            nonlocal talked_at
            if (done.cancelled() and self.holds.get(npc) is hold and conv.reply is not done
                    and conv.talked_at > talked_at and self._hero_turn(npc)):
                talked_at = conv.talked_at
                assert conv.reply is not None
                conv.reply.add_done_callback(finished_turn)
                return
            self._end_hold(npc, hold)
            if self._hold_turns.get(npc) is hold:
                del self._hold_turns[npc]

        conv.reply.add_done_callback(finished_turn)
        self.accepting.add(npc)
        for member in [npc, *self._allies(npc)]:
            if self.holds.get(npc) is not hold:
                return
            try:
                if member != npc and (self.held(member) or not await self._hero_fight(member)):
                    continue
            except (GameError, TimeoutError) as error:
                log.info("%s: surrender hold unavailable: %s", member, error)
                self._end_hold(npc, hold)
                return
            if self.holds.get(npc) is not hold:
                return
            person = self.orders.talk.people[member]
            listener = self.dialogue.open(member, person.name, warm=False)
            intent = Intent(attitude="wary")
            self.dialogue._change_intent(listener, intent, self.link.snapshots.get(member), body=False)
            hold.members.append(member)
            self._hold_used[member] = False
            requested = time.monotonic()
            output, ok = await self.dialogue._calm(listener, intent, native_attitude="ATT_FRIENDLY")
            if self.dialogue.trace is not None:
                self.dialogue.trace.write({
                    "type": "surrender_hold", "t": round(time.time(), 3), "npc": npc, "member": member,
                    "received_at": time.time() - (time.monotonic() - conv.talked_at),
                    "request_ms": round((requested - conv.talked_at) * 1000, 1),
                    "applied_ms": round((time.monotonic() - conv.talked_at) * 1000, 1), "ok": ok,
                })
            if not ok:
                log.info("%s: surrender hold failed: %s", member, output)
                self._end_hold(npc, hold)
                return

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
        if attacker in {"PLAYER", "PC_HERO"}:
            for recipient, hold in list(self._hold_turns.items()):
                if npc in hold.members:
                    self.accepting.discard(recipient)
                    if self.holds.get(recipient) is hold:
                        if hit.dead or hit.down:
                            self._dead.add(npc)
                        self._end_hold(recipient, hold)
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
        if attacker in {"PLAYER", "PC_HERO"} and self._betray(npc):
            if not hit.dead and not hit.down:
                self._urgent(
                    npc, "[Он сдался тебе и ударил. Перемирие снято. Реши по характеру, что делать.]",
                )
            return
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
            if self._scene is not None:
                self._scene.cancel()
            self._loading = isinstance(msg, Loading)
            self.battles.clear()
            self.offering.clear()
            self.accepting.clear()
            for npc, hold in list(self.holds.items()):
                self._release_hold(npc, hold)
            self._hold_turns.clear()
            self._fight_words.clear()
            for task in self._resuming.values():
                task.cancel()
            self._resuming.clear()
            self._hold_used.clear()
            self._dead.clear()
            self._armed = False
            self._ticks, self._minute = 0, None
            return
        if self._loading:
            return
        if isinstance(msg, LineStarted):
            npc = msg.npc.upper()
            if (line := self.dialogue._lines.get(msg.line)) and (
                self.dialogue.fights_hero(npc) or self._fighting(npc) or self.held(npc)
            ):
                self._fight_words[npc] = line[0]
            return
        if isinstance(msg, CombatHit):
            self._hit(msg)
            return
        if isinstance(msg, (Snapshot, NpcViews, NpcRoster)):
            self._ticks = msg.t
            if isinstance(msg, NpcRoster):
                for person in msg.npcs:
                    if person.state != "ZS_ATTACK" and not self.held(person.id):
                        self._fight_words.pop(person.id, None)
                    if person.id in self._hold_used and not self.held(person.id):
                        if person.state == "ZS_ATTACK":
                            self._hold_used[person.id] = True
                        elif self._hold_used[person.id]:
                            del self._hold_used[person.id]
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
        self.accepting.discard(npc)
        if self._hero_turn(npc) and (npc in self.holds or await self._hero_fight(npc)):
            self.accepting.add(npc)

    def _hero_turn(self, npc: str) -> bool:
        conv = self.dialogue.conversations.get(npc)
        return bool(conv and conv.trigger and conv.trigger.role == "user"
                    and conv.reply and not conv.reply.done())

    async def _hero_fight(self, npc: str) -> bool:
        entry = self.orders.talk.people.get(npc)
        if (entry is None or entry.guild >= BEASTS or self.orders.companion(npc)
                or not self._alive(npc) or self.silent(npc)):
            return False
        [fighting] = await self.link.story_eval(npc, [[
            "&&", ["call", "NPC_ISINSTATE", ["inst", "SELF"], ["sym", "ZS_ATTACK"]],
            ["!", ["call", "NPC_ISDEAD", ["inst", "SELF"]]],
            ["call", "NPC_GETTARGET", ["inst", "SELF"]],
            ["==", ["call", "HLP_GETINSTANCEID", ["inst", "OTHER"]],
             ["call", "HLP_GETINSTANCEID", ["inst", "PC_HERO"]]],
        ]])
        return fighting == 1

    def situation(self, npc: str) -> str | None:
        now = self._now()
        fact = fresh_surrender(self.chronicle, npc, now) if now is not None else None
        lines = [YIELDING] if fact and fact["status"] == "spared" else []
        if now is not None and any(f["npc"] == npc and now < f["until"]
                                   for f in self.chronicle.facts("hero_yield")):
            lines.append(VICTOR)
        for memory in self.chronicle.facts("humiliated"):
            if memory["npc"] == npc:
                lines.append("Ты помнишь, как обмочил его, когда он сдался.")
            elif npc in memory["witnesses"]:
                lines.append(f"Ты помнишь, как {memory['name']} обмочил его, когда он сдался.")
        if npc in self.accepting and self._hero_turn(npc):
            posture = ("Ты опустил оружие и слушаешь героя. Он сказал тебе эту фразу. "
                       if npc in self.holds else
                       "Сейчас ты в бою с героем, твоя цель — герой. Он сказал тебе эту фразу. ")
            outcome = "без вызова бой продолжится. " if npc in self.holds else "без вызова бой продолжается. "
            lines.append(posture +
                         "Если решишь принять его сдачу, вызови accept_surrender{take, words, humiliate}; "
                         + outcome + "Выкуп и решение — по твоему характеру и положению.")
        return "\n".join(lines) or None

    def tools(self, npc: str) -> list[Tool]:
        tools = [SURRENDER] if npc.upper() in self.offering else []
        if npc.upper() in self.accepting and self._hero_turn(npc.upper()):
            tools.append(ACCEPT_SURRENDER)
        return tools

    async def _native_outcome(self, npc: str, *, won: bool = False) -> None:
        names = ["AIV_LASTFIGHTAGAINSTPLAYER", "AIV_LASTFIGHTCOMMENT"]
        values = [["var", "FIGHT_WON" if won else "FIGHT_LOST"], ["int", 0]]
        if not won:
            names.append("AIV_DEFEATEDBYPLAYER")
            values.append(["int", 1])
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
                for index, value in zip(indices, values, strict=True)
            ], npc=npc)])
            result = await asyncio.wait_for(done, 3)
            if result.error is not None:
                raise GameError(f"surrender outcome: {result.error}")
        finally:
            self.link.listeners.remove(fired)
            await self.link.patch_graph([triggers.removal(key)])

    def _betray(self, npc: str) -> bool:
        fact = fresh_yield(self.chronicle, npc, self._ticks)
        if fact is None or any(f["yield"] == fact["id"] for f in self.chronicle.facts("yield_broken")):
            return False
        self.chronicle.record({"fact": "yield_broken", "yield": fact["id"], "npc": npc, "t": self._ticks})
        for truce in self.chronicle.facts("truce"):
            if npc in truce["members"] and not any(
                f["truce"] == truce["id"] for f in self.chronicle.facts("truce_broken")
            ):
                self.chronicle.record({"fact": "truce_broken", "truce": truce["id"], "npc": npc,
                                       "members": truce["members"], "reason": "yield_broken"})
        return True

    async def _ransom(self, npc: str, raw: object) -> tuple[list[Give], str | None, list[str]]:
        if not isinstance(raw, list) or any(not isinstance(x, str) for x in raw):
            raise GameError("take must be a list of gold, weapons, fine or item ids")
        take = set(raw)
        ids = {x.upper() for x in take - {"gold", "weapons", "fine"}}
        if len(ids) > 5 or any(not SYMBOL.fullmatch(x) for x in ids):
            raise GameError("take accepts at most five item ids")
        community, fine = None, 0
        cases = []
        if "fine" in take:
            community = self.law.community_of(npc) if self.law else None
            if self.law is None or not self.law.is_guard(npc, community):
                raise GameError("fine is only available to community lawkeepers")
            fine = self.law.debt(community)
            if fine <= 0:
                raise GameError("the hero has no unpaid crimes known to your community law")
            cases = [f["id"] for f in self.law.unpaid(community)]
        if "weapons" in take:
            weapons = await self.link.story_eval(npc, [
                ["call", fn, ["inst", "HERO"]]
                for fn in ("NPC_GETEQUIPPEDMELEEWEAPON", "NPC_GETEQUIPPEDRANGEDWEAPON")
            ])
            equipped = {w.upper() for w in weapons if isinstance(w, str)}
            if not equipped:
                raise GameError("the hero has no equipped weapons")
            ids.update(equipped)
        if "gold" in take or fine:
            ids.add(GOLD)
        items = sorted(ids)
        state = await self.link.story_state(symbols=items, items=items) if items else {}
        if missing := [i for i in items if state.get("symbols", {}).get(i) != "item"]:
            raise GameError(f"unknown item ids: {', '.join(missing)}")
        counts = await self.link.story_eval(npc, [
            ["call", "NPC_HASITEMS", ["inst", "HERO"], ["sym", item]] for item in items
        ]) if items else []
        gives = []
        for item, held in zip(items, counts, strict=True):
            wanted = held if item == GOLD and "gold" in take else fine if item == GOLD and fine else 1
            if type(held) is not int or held <= 0 or held < wanted or (item == GOLD and held < fine):
                raise GameError(f"missing hero item: {item} (has {held}, needs {max(wanted or 0, fine)})")
            gives.append(Give(item, wanted, state.get("items", {}).get(item) or item))
        return gives, community, cases

    async def _humiliate(self, conv: Conversation) -> bool:
        npc, branch = conv.npc, self.chronicle.branch
        completed, entered, held = False, False, False
        self._scene = asyncio.current_task()
        try:
            held = await self.dialogue._hold(conv, "humiliate")
            if not held:
                raise GameError("NPC body is unavailable")
            await self.link.player_pose("kneel")
            for action, args in [
                ("go_to_npc", {"npc": "PLAYER", "radius": PEE_DISTANCE_CM, "close": True,
                               "timeout_ms": PEE_APPROACH_TIMEOUT_MS}),
                ("turn_to", {"npc": "PLAYER"}),
            ]:
                result = await self.link.act(npc, action, **args)
                if result.status != "done":
                    raise GameError(f"{action}: {result.status} {result.detail or ''}")
            snap = await self.link.snapshot(npc, fresh=True)
            if snap.player is None or not snap.player.kneeling:
                raise GameError("the hero left the kneeling pose")
            views = await self.link.npc_view(list(self.orders.talk.people))
            witnesses = {
                v.npc: v.name for v in views
                if v.npc != npc and v.hp > 0 and v.body not in {"dead", "down"}
                and v.hero is not None and v.hero.distance <= v.senses_range
                and self.orders.talk.people[v.npc].guild < BEASTS
            }
            result = await self.link.act(npc, "play_ani", name="T_STAND_2_PEE")
            if result.status != "done":
                raise GameError(f"pee transition: {result.status} {result.detail or ''}")
            entered = True
            point = snap.npc.nearest_waypoint or snap.npc.waypoint
            place = self.places.of(point) if self.places else None
            where = place.ru if place else point or snap.world.name
            self.chronicle.record({
                "fact": "humiliated", "npc": npc, "name": conv.persona.name, "hero": "PLAYER",
                "place": place.id if place else point or snap.world.name, "place_name": where,
                "time": self._now(), "t": self._ticks, "witnesses": list(witnesses),
            })
            self.dialogue.remember(npc, conv.persona.name, f"Ты обмочил сдавшегося героя: у {where}.")
            for witness, name in witnesses.items():
                self.dialogue.remember(
                    witness, name, f"{conv.persona.name} обмочил сдавшегося героя у {where}.",
                )
            hold = self.link.act(npc, "play_ani", name="S_PEE", hold_ms=PEE_HOLD_MS)
            if self.barks is not None:
                result, _ = await asyncio.gather(hold, self.barks.humiliate(conv, "герой"))
            else:
                result = await hold
            if result.status != "done":
                raise GameError(f"pee hold: {result.status} {result.detail or ''}")
            completed = True
        except (GameError, TimeoutError) as error:
            log.info("humiliation scene for %s cancelled: %s", npc, error)
        finally:
            if not self._loading and self.chronicle.branch == branch:
                try:
                    if not completed:
                        await self.link.player_pose("none")
                    if entered:
                        await self.link.act(npc, "play_ani", name="T_PEE_2_STAND")
                    if held:
                        await self.link.act(npc, "return_to_routine")
                except (GameError, TimeoutError) as error:
                    log.info("humiliation cleanup for %s: %s", npc, error)
            self._scene = None
        return completed

    async def _accept(self, conv: Conversation, call: ToolCall, snap: Snapshot | None) -> ToolOutcome:
        npc, now = conv.npc, self._now()
        hold = self._hold_turns.get(npc)
        try:
            eligible = (npc in self.accepting and self._hero_turn(npc)
                        and (npc in self.holds or await self._hero_fight(npc)))
            allies = ([member for member in self._allies(npc) if await self._hero_fight(member)]
                      if eligible else [])
        except (GameError, TimeoutError) as error:
            return ToolOutcome(f"Отклонено: не удалось проверить бой с героем ({error}).", "rejected")
        if not eligible:
            return ToolOutcome(
                "Отклонено: ты не в бою с героем, не слышал его фразу или уже сдался сам.", "rejected",
            )
        words = call.arguments.get("words")
        if not isinstance(words, str) or not words.strip() or now is None:
            return ToolOutcome("Отклонено: нужны слова в words и известное время мира.", "rejected")
        humiliate = call.arguments.get("humiliate", False)
        if type(humiliate) is not bool:
            return ToolOutcome("Отклонено: humiliate должен быть true или false.", "rejected")
        if humiliate:
            guild = self.orders.talk.people[npc].guild
            metadata = {"guild": guild}
            community = self.law.community_of(npc, metadata) if self.law else None
            if guild in HUMILIATE_FORBIDDEN_GUILDS or (
                self.law is not None and self.law.is_guard(npc, community, metadata)
            ):
                return ToolOutcome(
                    "Отклонено: законникам, магам (KDF, KDW), послушникам (NOV) и паладинам (PAL) "
                    "запрещено обмачивать сдавшегося героя. Прими сдачу с humiliate:false или откажись.",
                    "rejected",
                )
        try:
            gives, community, cases = await self._ransom(npc, call.arguments.get("take"))
            if npc not in self.accepting:
                return ToolOutcome("Отклонено: герой нарушил паузу боя.", "rejected")
            await self._native_outcome(npc, won=True)
        except (GameError, TimeoutError) as error:
            return ToolOutcome(f"Отклонено: не удалось принять сдачу ({error}).", "rejected")
        # A deadline or a hit may have resumed combat during validation. Let its native attack
        # finish before the final calm, so an already queued resume cannot undo accepted mercy.
        if resume := self._resuming.get(npc):
            await resume
        if npc not in self.accepting:
            return ToolOutcome("Отклонено: герой нарушил паузу боя.", "rejected")
        if hold is not None and hold.timer is not None:
            hold.timer.cancel()
        intent = Intent(attitude="wary")
        self.dialogue._change_intent(conv, intent, snap, body=False)
        quiet = {"do": "refuse_talk", "seconds": ACCEPT_QUIET_S}
        output, ok = await self.dialogue._calm(
            conv, intent, effects=[*(g.effect("self") for g in gives), quiet], native_attitude="ATT_FRIENDLY",
        )
        if not ok:
            if hold is not None:
                self._end_hold(npc, hold, force=hold.resumed)
                if resume := self._resuming.get(npc):
                    await resume
            return ToolOutcome(f"Отклонено: {output}", "rejected")
        if hold is not None and hold.resumed:
            allies = list(dict.fromkeys([*allies, *hold.members]))
        for member in allies:
            if member == npc or not self._alive(member):
                continue
            person = self.orders.talk.people[member]
            listener = self.dialogue.open(member, person.name, warm=False)
            listening = Intent(attitude="wary")
            self.dialogue._change_intent(
                listener, listening, self.link.snapshots.get(member), body=False,
            )
            output, ok = await self.dialogue._calm(
                listener, listening, effects=[quiet], native_attitude="ATT_FRIENDLY",
            )
            if not ok:
                if hold is not None:
                    self._end_hold(npc, hold, force=True)
                    if resume := self._resuming.get(npc):
                        await resume
                return ToolOutcome(f"Отклонено: {output}", "rejected")
        if npc not in self.accepting:
            if resume := self._resuming.get(npc):
                await resume
            if hold is not None:
                await self._resume_hold(hold)
            return ToolOutcome("Отклонено: герой нарушил паузу боя.", "rejected")
        if hold is not None:
            self._release_hold(npc, hold)
            self._hold_turns.pop(npc, None)
            for member in hold.members:
                self._hold_used.pop(member, None)
        self.accepting.discard(npc)
        if community is not None:
            self.law.absolve(community, crimes=cases)
        self.chronicle.record({
            "fact": "hero_yield", "id": uuid.uuid4().hex, "npc": npc, "name": conv.persona.name,
            "t": self._ticks, "deadline": self._ticks + 60000, "until": (now // 1440 + 2) * 1440,
            "take": [g.fact() for g in gives],
        })
        self.orders.truce(npc)
        if self.parley:
            await self.parley.accept_deal(npc, 1, now=now, native_attitude="ATT_FRIENDLY")
        taken = "; ".join(f"{g.name} ×{g.count} ({g.item})" for g in gives) or "ничего, без выкупа"
        self.dialogue.remember(npc, conv.persona.name, f"Герой сдался тебе в бою; ты взял: {taken}.")
        if conv.acting or not conv.last_lines:  # acting: only Gemma has spoken; the model's text is silent
            await self.dialogue._say(conv, words.strip(), None, overlay=True)
        scene = await self._humiliate(conv) if humiliate else None
        return ToolOutcome(f"Принято: бой прекращён; герой передал тебе: {taken}."
                           + (" Долг перед законом погашен." if community else "")
                           + (" Унижение завершено." if scene else
                              " Сцена унижения отменена; сдача остаётся в силе." if humiliate else ""),
                           "accepted")

    async def handle(
        self, dialogue: DialogueManager, conv: Conversation, call: ToolCall, snap: Snapshot | None,
    ) -> ToolOutcome | None:
        if call.name == "accept_surrender":
            return await self._accept(conv, call, snap)
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
            await self._native_outcome(conv.npc)
        except (GameError, TimeoutError) as error:
            return ToolOutcome(
                f"Отклонено: игра не подтвердила исход сдачи ({type(error).__name__}: {error}).", "rejected",
            )
        intent = Intent(attitude="wary")
        dialogue._change_intent(conv, intent, snap, body=False)
        # C_NpcIsBotheredByWeapon needs native ATT_FRIENDLY, not a friendly saved intent.
        _, ok = await dialogue._calm(conv, intent, native_attitude="ATT_FRIENDLY")
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
        if conv.acting or not conv.last_lines:
            await dialogue._say(conv, words.strip(), None, overlay=True)
        return ToolOutcome(
            "Принято: ты сдался, прекратил бой и убрал оружие; ждёшь решения героя.", "accepted",
        )
