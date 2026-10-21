"""Bandits bargain before attacking; the save branch owns threats and truces, never the model."""

from __future__ import annotations

import asyncio
import math
import uuid
from collections.abc import Callable
from typing import TYPE_CHECKING, Any

from .chronicle import Chronicle, Fact
from .dialogue import KILLERS
from .game import GameLink
from .llm import Tool, ToolCall
from .protocol import (
    GameMessage,
    HeroDeed,
    Loading,
    NpcDeed,
    NpcEvent,
    NpcRoster,
    NpcViews,
    Perception,
    Session,
    Snapshot,
    WorldLoaded,
)
from .turn import ToolOutcome

if TYPE_CHECKING:
    from .dialogue import Conversation, DialogueManager
    from .perception import PerceptionDesk
    from .places import PlaceBook

NEAR_CM = 1500
ULTIMATUM = "ultimatum"
ULTIMATUM_TOOL = Tool(
    ULTIMATUM,
    "Поставить герою ультиматум: потребовать плату, убрать оружие, уйти или пойти с вами. "
    "Срок 10–60 секунд. Если он отойдёт дальше 15 м, это требование уже не повод для драки.",
    {
        "type": "object",
        "properties": {
            "demand": {"type": "string", "description": "Что ты требуешь от героя."},
            "seconds": {"type": "integer", "minimum": 10, "maximum": 60},
        },
        "required": ["demand", "seconds"],
        "additionalProperties": False,
    },
)
NO_REASON = "Без повода сразу не нападаешь: сначала потребуй — ultimatum."
AGREEMENT = "У вас уговор: героя не трогать без нового повода."
EXPIRY_CUE = "[Срок вышел: герой не подчинился]"
DRAWN = frozenset({"melee", "ranged", "magic"})


class Parley:
    def __init__(
        self,
        link: GameLink,
        dialogue: DialogueManager,
        chronicle: Chronicle,
        *,
        perception: PerceptionDesk | None = None,
        companion: Callable[[str], bool] = lambda npc: False,
        places: PlaceBook | None = None,
        lead: Callable[[str], int] = lambda npc: 0,
    ) -> None:
        self.link, self.dialogue, self.chronicle = link, dialogue, chronicle
        self.perception, self.companion, self.places, self.lead = perception, companion, places, lead
        self._ticks = max((s.t for s in link.snapshots.values()), default=0)
        self._minute: int | None = None
        self._roster: dict[str, Any] = {}
        self._roster_complete = False
        self._guilds: dict[str, int] = {}
        self._loading = False
        dialogue.fight_reason = self.fight_reason
        dialogue.on_player_said.append(self.hero_said)
        link.listeners.append(self._on_message)

    def bandit(self, npc: str) -> bool:
        npc = npc.upper()
        guild = self.dialogue.guild_of(npc)
        if guild is not None:
            self._guilds[npc] = guild
        if npc in self._guilds:
            return self._guilds[npc] in KILLERS
        # Only bandits can create these facts; they identify them before the roster arrives.
        return any(
            (f.get("fact") == "truce" and npc in f.get("members", ()))
            or (f.get("fact") in {"parley_ultimatum", "parley_cause"} and f.get("npc") == npc)
            for f in self.chronicle.facts()
        )

    def _place(self, npc: str) -> str | None:
        view = self.perception.view(npc) if self.perception else None
        snap = self.link.snapshots.get(npc)
        points = (
            (view.waypoint, view.nearest_waypoint)
            if view
            else ((snap.npc.waypoint, snap.npc.nearest_waypoint) if snap else ())
        )
        place = self.places.of(*points) if self.places else None
        return place.id if place else None

    def members(self, npc: str) -> list[str]:
        """The band present at the deal: nearby bandits, or bandits at the same named place."""
        npc = npc.upper()
        members = {npc}
        snap = self.link.snapshots.get(npc)
        if snap:
            self._guilds.update((o.id.upper(), o.guild) for o in snap.nearby_npcs)
            members.update(
                o.id.upper()
                for o in snap.nearby_npcs
                if o.guild in KILLERS and not o.dead and o.distance <= NEAR_CM
            )
        view = self.perception.view(npc) if self.perception else None
        if view:
            members.update(
                o.id.upper()
                for o in view.sees
                if o.guild in KILLERS and o.body != "dead" and o.distance <= NEAR_CM
            )
        if place := self._place(npc):
            known = set(self.link.snapshots) | set(self._roster)
            if self.perception:
                known.update(v.npc.upper() for v in self.perception.views())
            members.update(who for who in known if self.bandit(who) and self._place(who) == place)
        return sorted(members)

    def _distance(self, npc: str) -> float | None:
        if (entry := self._roster.get(npc)) is not None:
            return entry.distance
        if self._roster_complete:
            return math.inf  # absent from the full roster: beyond its 60 m radius, or gone
        view = self.perception.view(npc) if self.perception else None
        if view and view.hero:
            return view.hero.distance
        snap = self.link.snapshots.get(npc)
        if snap and self.link.player:
            return math.dist(snap.npc.pos, self.link.player.pos)
        return snap.player.distance if snap and snap.player else None

    def now(self) -> int | None:
        if self._minute is not None:
            return self._minute
        snap = next(iter(self.link.snapshots.values()), None)
        if snap:
            return snap.world.day * 1440 + snap.world.hour * 60 + snap.world.minute
        return None

    def ultimatum(self, npc: str) -> Fact | None:
        latest = next(
            (f for f in reversed(self.chronicle.facts("parley_ultimatum")) if f.get("npc") == npc.upper()),
            None,
        )
        return latest if latest and latest.get("status") != "cleared" else None

    def truce(self, npc: str) -> Fact | None:
        now = self.now()
        broken = {f.get("truce") for f in self.chronicle.facts("truce_broken")}
        truce = next(
            (
                f
                for f in reversed(self.chronicle.facts("truce"))
                if npc.upper() in f.get("members", ())
                and f.get("id") not in broken
                and (now is None or now < f["until"])
            ),
            None,
        )
        if truce:
            fresh = [f for member in truce["members"] for f in self._causes(member, truce)]
            armed = any(self._armed(member) for member in truce["members"])
            if fresh or (armed and not truce.get("armed", False)):
                self.chronicle.record(
                    {
                        "fact": "truce_broken",
                        "truce": truce["id"],
                        "npc": npc.upper(),
                        "members": truce["members"],
                        "reason": fresh[-1].get("kind", "attack") if fresh else "weapon",
                    }
                )
                return None
        return truce

    def _armed(self, npc: str) -> bool:
        view = self.perception.view(npc) if self.perception else None
        return bool(view and view.hero and view.hero.seen and view.hero.weapon in DRAWN)

    def _causes(self, npc: str, after: Fact | None = None) -> list[Fact]:
        facts = self.chronicle.facts()
        start = next((i + 1 for i, f in enumerate(facts) if f is after), 0) if after else 0
        band = set(after["members"]) if after else set(self.members(npc))
        crimes = {f.get("id"): f for f in facts if f.get("fact") == "crime"}
        seen = {
            f.get("crime") for f in facts[start:] if f.get("fact") == "crime_seen" and f.get("npc") == npc
        }
        out = []
        for f in facts[start:]:
            if f.get("fact") == "parley_cause" and f.get("npc") == npc:
                # The witness and local victim were checked at the event; a truce has a fixed band.
                if not after or f.get("kind") in {"weapon", "theft"} or f.get("victim") in band:
                    out.append(f)
            if f.get("fact") == "crime" and (npc in f.get("witnesses", ()) or f.get("id") in seen):
                if f.get("kind") in {"attack", "murder"} and f.get("victim") in band:
                    out.append(f)
                elif after and f.get("kind") == "theft":
                    out.append(f)
            elif f.get("fact") == "crime_seen" and f.get("crime") in seen:
                crime = crimes.get(f.get("crime"), {})
                if crime.get("kind") in {"attack", "murder"} and crime.get("victim") in band:
                    out.append(f)
        return out

    def fight_reason(self, npc: str, snap: Snapshot | None = None) -> str | None:
        npc = npc.upper()
        if not self.bandit(npc):
            return None
        self.tick()
        truce = self.truce(npc)
        if truce:
            return AGREEMENT
        if self._armed(npc) or any(f.get("kind") in {"attack", "murder"} for f in self._causes(npc)):
            return None
        latest_truce = next(
            (f for f in reversed(self.chronicle.facts("truce")) if npc in f.get("members", ())),
            None,
        )
        now = self.now()
        if (
            latest_truce
            and now is not None
            and now < latest_truce["until"]
            and any(
                f.get("truce") == latest_truce["id"] for f in self.chronicle.facts("truce_broken")
            )
        ):
            return None
        if any(f.get("npc") == npc for f in self.chronicle.facts("caught")):
            return None
        own = self.ultimatum(npc)
        if own and (own.get("answered") or own.get("status") == "expired"):
            return None
        nearby = self.members(npc)
        leader = max(nearby, key=self.lead)
        threat = self.ultimatum(leader)
        if leader != npc and self.lead(leader) > self.lead(npc) and threat:
            if self._distance(leader) is not None and self._distance(leader) <= NEAR_CM:
                if threat.get("answered") or threat.get("status") == "expired":
                    return None
        return NO_REASON

    def hero_said(self, npc: str) -> None:
        threat = self.ultimatum(npc)
        if threat and threat.get("status") == "active" and not threat.get("answered"):
            self.chronicle.record({**threat, "answered": True})

    async def accept_deal(self, npc: str, hours: int | None = None) -> None:
        npc = npc.upper()
        if not self.bandit(npc):
            return
        if threat := self.ultimatum(npc):
            self.chronicle.record({**threat, "status": "cleared", "why": "deal"})
        if hours is None:
            return
        now = self.now()
        if now is None or not 1 <= hours <= 24:
            raise ValueError("truce needs the game clock and 1–24 hours")
        members = self.members(npc)
        self.chronicle.record(
            {
                "fact": "truce",
                "id": f"truce-{uuid.uuid4().hex[:8]}",
                "npc": npc,
                "members": members,
                "until": now + hours * 60,
                "hours": hours,
                "armed": any(self._armed(member) for member in members),
            }
        )
        calm = []
        snap = self.link.snapshots.get(npc)
        names = {o.id: o.name for o in snap.nearby_npcs} if snap else {}
        for member in members:
            conv = self.dialogue.conversations.get(member)
            if not (conv and (conv.intent.goal, conv.intent.target) == ("fight", "PLAYER")):
                if not self.dialogue.fights_hero(member):
                    continue
            name = conv.persona.name if conv else names.get(member, member)
            self.dialogue.change_intent(member, name, body=False, goal="none", target=None, attitude="wary")
            conv = self.dialogue.conversations[member]
            calm.append((conv, conv.intent))
        # The accepted truce cancels queued attacks and ends existing fights without waiting for speech.
        for conv, intent in calm:
            await self.dialogue._calm(conv, intent)

    def tick(self) -> None:
        if self._loading:
            return
        latest = {f["npc"]: f for f in self.chronicle.facts("parley_ultimatum")}
        for npc, threat in latest.items():
            if threat.get("status") not in {"active", "expired"}:
                continue
            distance = self._distance(npc)
            if distance is not None and distance > NEAR_CM:
                self.chronicle.record({**threat, "status": "cleared", "why": "left"})
                continue
            elif threat["status"] == "active" and self._ticks >= threat["deadline"]:
                threat = {**threat, "status": "expired"}
                self.chronicle.record(threat)
            if threat.get("status") == "expired" and not threat.get("notified"):
                conv = self.dialogue.conversations.get(npc)
                if conv is None:
                    entry = self._roster.get(npc)
                    snap = self.link.snapshots.get(npc)
                    name = entry.name if entry else (snap.npc.name if snap else threat["name"])
                    conv = self.dialogue.open(npc, name, warm=False)
                if conv.reply is not None and not conv.reply.done():
                    continue  # event_turn queues only a remark when busy; deliver an actual turn later
                turn = self.dialogue.event_turn(
                    npc,
                    EXPIRY_CUE,
                    relevant=lambda npc=npc, key=threat["id"]: self._relevant(npc, key),
                    tools={"set_intent", "recall", ULTIMATUM, "propose_deal"},
                    kind="ultimatum",
                )
                if turn is not None:
                    self.chronicle.record({**threat, "notified": True})

    def _relevant(self, npc: str, key: str) -> bool:
        threat = self.ultimatum(npc)
        distance = self._distance(npc)
        return bool(
            threat
            and threat.get("id") == key
            and threat.get("status") == "expired"
            and distance is not None
            and distance <= NEAR_CM
        )

    async def run(self) -> None:
        while True:
            self.tick()
            await asyncio.sleep(0.25)

    async def prepare(self, npc: str) -> None:
        self.tick()

    def _balance(self, npc: str) -> str | None:
        snap = self.link.snapshots.get(npc)
        if snap is None or snap.player is None:
            return None
        view = self.perception.view(npc) if self.perception else None
        band = {
            npc,
            *(o.id for o in snap.nearby_npcs if o.guild in KILLERS and not o.dead and o.distance <= NEAR_CM),
        }
        if view:
            band.update(
                o.id for o in view.sees if o.guild in KILLERS and o.body != "dead" and o.distance <= NEAR_CM
            )
        down = {o.id for o in view.sees if o.body in {"down", "dead"}} if view else set()
        down |= {who for who in band if who in self._roster and self._roster[who].activity == "down"}
        if snap.npc.hp <= 0:
            down.add(npc)
        count = len(band - down)
        companions = {
            o.id
            for o in snap.nearby_npcs
            if not o.dead and o.distance <= NEAR_CM and self.companion(o.id) and o.id not in down
        }
        hero = snap.player
        health = hero.hp / max(1, hero.hp_max) if hero.hp is not None and hero.hp_max else 1
        armor = (
            1.5
            if hero.armor and hero.armor.startswith(("ITAR_PAL", "ITAR_DJG"))
            else (0.5 if hero.armor else 0)
        )
        armed = 0.5 if hero.melee or hero.ranged else 0
        hero_strength = (0.75 + armor + armed) * max(0.2, health) + len(companions)
        own_health = snap.npc.hp / max(1, snap.npc.hp_max)
        band_strength = max(0, count - 1) + max(0, own_health)
        verdict = (
            "он опасен"
            if hero_strength > band_strength * 1.3
            else (
                "он слабее вас"
                if hero_strength < band_strength * 0.7
                else "силы равны"
            )
        )
        state = "ранен" if health < 1 else "цел"
        return (
            f"Силы: {verdict}; вас на ногах {count}, "
            f"спутников у героя {len(companions)}; "
            f"герой {state}, {'в доспехе' if hero.armor else 'без доспеха'}, "
            f"{'с оружием' if armed else 'без оружия'}."
        )

    def situation(self, npc: str) -> str | None:
        npc = npc.upper()
        if not self.bandit(npc):
            return None
        lines = [balance] if (balance := self._balance(npc)) else []
        reason = self.fight_reason(npc)
        if truce := self.truce(npc):
            until = truce["until"]
            lines.append(
                f"У вашей шайки уговор с героем до дня {until // 1440 + 1}, "
                f"{until % 1440 // 60:02d}:{until % 60:02d}: "
                "не трогать его без нового повода."
            )
        if threat := self.ultimatum(npc):
            left = max(0, math.ceil((threat["deadline"] - self._ticks) / 1000))
            lines.append(
                f"Твой ультиматум: {threat['demand']}; "
                + ("срок вышел." if threat["status"] == "expired" else f"осталось {left} с.")
            )
        if reason == NO_REASON:
            lines.append("Сначала давишь словами: требуешь, торгуешься, пугаешь; дерёшься, когда есть повод.")
        return "\n".join(lines) or None

    all_tools = (ULTIMATUM_TOOL,)

    def tools(self, npc: str) -> list[Tool]:
        return [ULTIMATUM_TOOL] if self.bandit(npc) else []

    async def handle(
        self,
        dialogue: DialogueManager,
        conv: Conversation,
        call: ToolCall,
        snap: Snapshot | None,
    ) -> ToolOutcome | None:
        if call.name != ULTIMATUM:
            return None
        demand, seconds = call.arguments.get("demand"), call.arguments.get("seconds")
        if not self.bandit(conv.npc):
            return ToolOutcome("Отклонено: ultimatum доступен только бандитам.", "rejected")
        if not isinstance(demand, str) or not demand.strip():
            return ToolOutcome("Отклонено: укажи своё требование.", "rejected")
        if type(seconds) is not int or not 10 <= seconds <= 60:
            return ToolOutcome("Отклонено: seconds — целое число от 10 до 60.", "rejected")
        if snap:
            self._ticks = max(self._ticks, snap.t)
        self.chronicle.record(
            {
                "fact": "parley_ultimatum",
                "id": f"ult-{uuid.uuid4().hex[:8]}",
                "npc": conv.npc,
                "name": conv.persona.name,
                "demand": demand.strip(),
                "seconds": seconds,
                "deadline": self._ticks + seconds * 1000,
                "status": "active",
                "answered": False,
            }
        )
        return ToolOutcome(
            "Принято: требование поставлено; дай герою ответить или дождись срока.", "accepted"
        )

    def _victim(self, npc: str, name: str | None) -> str:
        if not name or name == "you":
            return npc
        if self.bandit(name):
            return name.upper()
        snap = self.link.snapshots.get(npc)
        return next((o.id for o in snap.nearby_npcs if o.name == name), name) if snap else name

    def _cause(self, npc: str, kind: str, culprit: str, victim: str = "") -> None:
        npc, culprit, victim = npc.upper(), culprit.upper(), victim.upper()
        if not self.bandit(npc) or (culprit != "PLAYER" and not self.companion(culprit)):
            return
        if kind in {"attack", "murder"}:
            band = set(self.members(npc))
            snap = self.link.snapshots.get(npc)
            if snap:
                band.update(o.id.upper() for o in snap.nearby_npcs if o.guild in KILLERS)
            view = self.perception.view(npc) if self.perception else None
            if view:
                band.update(o.id.upper() for o in view.sees if o.guild in KILLERS)
            if victim not in band:
                return
        self.chronicle.record(
            {"fact": "parley_cause", "npc": npc, "kind": kind, "culprit": culprit, "victim": victim}
        )

    def _on_message(self, msg: GameMessage) -> None:
        match msg:
            case Loading():
                self._loading = True
            case Session() | WorldLoaded():
                if isinstance(msg, WorldLoaded) and self._loading:
                    # No Session means a portal: the hero left the old world's demands behind.
                    for npc in {f["npc"] for f in self.chronicle.facts("parley_ultimatum")}:
                        if threat := self.ultimatum(npc):
                            self.chronicle.record({**threat, "status": "cleared", "why": "left"})
                self._ticks, self._minute, self._roster = 0, None, {}
                self._roster_complete = False
                self._loading = False
            case Snapshot():
                self._ticks = max(self._ticks, msg.t)
                self._minute = msg.world.day * 1440 + msg.world.hour * 60 + msg.world.minute
            case NpcViews():
                self._ticks = max(self._ticks, msg.t)
                for view in msg.views:
                    if view.time is not None:
                        self._minute = view.time
            case NpcRoster():
                self._ticks = max(self._ticks, msg.t)
                if msg.full:
                    self._roster.clear()
                    self._roster_complete = True
                self._roster.update((p.id.upper(), p) for p in msg.npcs)
                self._guilds.update((p.id.upper(), p.guild) for p in msg.npcs)
            case Perception():
                kind = {
                    "ASSESSDAMAGE": "attack",
                    "ASSESSOTHERSDAMAGE": "attack",
                    "ASSESSMURDER": "murder",
                    "DRAWWEAPON": "weapon",
                    "ASSESSTHEFT": "theft",
                }.get(msg.perc)
                if kind:
                    self._cause(
                        msg.npc, kind, msg.other, msg.npc if msg.perc == "ASSESSDAMAGE" else msg.victim or ""
                    )
            case NpcEvent():
                kind = {
                    "attack": "attack",
                    "npc_attack": "attack",
                    "murder": "murder",
                    "theft": "theft",
                    "weapon_drawn": "weapon",
                }.get(msg.kind)
                if kind:
                    self._cause(msg.npc, kind, msg.other, msg.victim or self._victim(msg.npc, msg.detail))
            case HeroDeed() | NpcDeed():
                culprit = msg.culprit if isinstance(msg, NpcDeed) else "PLAYER"
                # Raw item pickup is not proof of theft (own property, dropped items, public rooms).
                # Theft uses the existing crime facts or native ASSESSTHEFT/NpcEvent witnesses.
                kind = {"attack": "attack", "murder": "murder"}.get(msg.kind)
                if kind:
                    for witness in msg.witnesses:
                        self._cause(witness, kind, culprit, msg.victim)
