"""The bandits' bounty: tiered knowledge, disclosure leverage and vanilla consequences."""

from __future__ import annotations

import json
from collections.abc import Iterable
from pathlib import Path
from typing import TYPE_CHECKING

from .chronicle import Chronicle, Fact
from .contracts import GOLD, Give
from .game import GameError, GameLink
from .llm import Tool, ToolCall
from .protocol import (
    Detached,
    GameMessage,
    HeroDeed,
    Loading,
    NpcEvent,
    NpcRoster,
    Perception,
    Session,
    Snapshot,
)
from .turn import ToolOutcome

if TYPE_CHECKING:
    from .dialogue import Conversation, DialogueManager
    from .perception import PerceptionDesk

LURE = "BDT_1013_BANDIT_L"
BRAGO = "BDT_1014_BANDIT_L"
SECRETS = ("client_name", "client_place")
TELL_SECRET = Tool(
    "tell_secret",
    "Раскрыть герою свою тайну об охоте: client_name — имя заказчика, client_place — его логово. "
    "Только если знаешь это и есть настоящий рычаг: принятая сделка secret с платой не ниже минимума, "
    "проигранная герою и прекращённая тобой драка (calm) или дружба BDT13_FRIEND у зазывалы. "
    "Без разрешения инструмента тайну словами не выдавай. Общие угрозы о награде и листовках — свободно.",
    {
        "type": "object",
        "properties": {"secret": {"type": "string", "enum": list(SECRETS)}},
        "required": ["secret"],
        "additionalProperties": False,
    },
)


class Bounty:
    def __init__(
        self,
        link: GameLink,
        dialogue: DialogueManager,
        chronicle: Chronicle,
        *,
        perception: PerceptionDesk | None = None,
    ) -> None:
        self.link, self.dialogue, self.chronicle = link, dialogue, chronicle
        self.perception = perception
        self.data = json.loads((Path(__file__).parent / "gamedata/bandit_bounty.json").read_text())
        self._friend = False
        dialogue.on_calm.append(self._calmed)
        link.listeners.append(self._on_message)

    def tier(self, npc: str) -> dict | None:
        npc = npc.upper()
        # The client keeps his vanilla knowledge, including who hired him.
        if (self.link.world or "").upper() != "NEWWORLD.ZEN" or npc == self.data["client"]["npc"]:
            return None
        if npc in self.data["npcs"]:
            return self.data["npcs"][npc]
        return self.data["default"] if self.dialogue.guild_of(npc) == 9 else None

    def validate_sale(
        self, npc: str, secret: str, hero_gives: Iterable[Give], npc_gives: Iterable[Give]
    ) -> str | None:
        tier = self.tier(npc)
        if not tier or secret not in tier["secrets"]:
            return "ты не знаешь этой тайны об охоте"
        gold = sum(g.count for g in hero_gives if g.item == GOLD)
        gold -= sum(g.count for g in npc_gives if g.item == GOLD)
        if gold < tier["minimum_gold"]:
            return f"за эту тайну нужно не меньше {tier['minimum_gold']} золотых сейчас"
        return None

    def revealed(self, npc: str, secret: str) -> Fact | None:
        return next((f for f in self.chronicle.facts("bounty_secret")
                     if f.get("npc") == npc.upper() and f.get("secret") == secret), None)

    def _fight(self, npc: str) -> Fact | None:
        return next((f for f in reversed(self.chronicle.facts("bounty_fight"))
                     if f.get("npc") == npc.upper()), None)

    def _state(self, npc: str, state: str) -> None:
        previous = self._fight(npc)
        if previous is None or previous["state"] != state:
            self.chronicle.record({"fact": "bounty_fight", "npc": npc.upper(), "state": state})

    def _hurt(self, npc: str, *, down: bool = False) -> None:
        fight = self._fight(npc)
        snap = self.link.snapshots.get(npc.upper())
        if fight and fight["state"] == "hit" and (
            down or (snap and snap.npc.hp < snap.npc.hp_max / 2)
        ):
            self._state(npc, "lost")

    def _hit(self, npc: str) -> None:
        if self.tier(npc):
            if not (fight := self._fight(npc)) or fight["state"] != "lost":
                self._state(npc, "hit")
            self._hurt(npc)

    def _calmed(self, npc: str) -> None:
        self._hurt(npc)
        if (fight := self._fight(npc)) and fight["state"] == "lost":
            self._state(npc, "calm")

    def _on_message(self, msg: GameMessage) -> None:
        match msg:
            case Loading() | Session():
                self._friend = False
            case Perception(perc="ASSESSDAMAGE", other="PLAYER"):
                self._hit(msg.npc)
            case HeroDeed(kind="attack"):
                self._hit(msg.victim)
            case NpcEvent(kind="attack", other="PLAYER"):
                if msg.detail in (None, "you") or msg.victim == msg.npc:
                    self._hit(msg.npc)
            case Perception(perc="ASSESSDAMAGE") | NpcEvent(kind="attack" | "npc_attack"):
                if (fight := self._fight(msg.npc)) and fight["state"] in {"hit", "lost"}:
                    self._state(msg.npc, "ended")
            case Snapshot():
                if ((fight := self._fight(msg.npc.id)) and fight["state"] == "lost"
                        and msg.npc.hp == msg.npc.hp_max):
                    self._state(msg.npc.id, "ended")
                else:
                    self._hurt(msg.npc.id)
            case Detached(reason="down"):
                self._hurt(msg.npc, down=True)
            case NpcRoster():
                for npc in msg.npcs:
                    if npc.activity == "down":
                        self._hurt(npc.id, down=True)

    async def prepare(self, npc: str) -> None:
        if npc.upper() == LURE and self.tier(npc):
            self._friend = await self._bond()

    async def _bond(self) -> bool:
        state = await self.link.story_state(vars=["BDT13_FRIEND"])
        return state.get("vars", {}).get("BDT13_FRIEND") == 1

    def situation(self, npc: str) -> str | None:
        tier = self.tier(npc)
        if tier is None:
            return None
        lines = ["Охота на героя:", self.data["public"]]
        if not tier["secrets"]:
            lines.append("Кто платит, где его искать и сколько он даёт — ты не знаешь.")
            return "\n".join(lines)
        client = self.data["client"]
        lines.extend([
            f"Твоё знание (тайна client_name): заказчика зовут {client['name']}.",
            f"Твоё знание (тайна client_place): {client['place_text']} ({client['place']}).",
        ])
        if tier["seen_client"]:
            reward = self.data["reward"]
            lines.append(
                f"Ты сам видел Декстера. Он платит {reward['alive']} золотых за героя живым, "
                f"{reward['dead']} за мёртвого; пленника надо привести к нему. "
                "Это знаешь ты; можешь требовать от героя пойти с тобой и предложить lead к CASTLEMINE_HUT."
            )
        else:
            lines.append("Сам ты заказчика не видел; из вашей шайки его видел только Браго. Сумму не знаешь.")
        lines.append(
            f"Имя и место не выдавай без tell_secret: продаёшь тайну условием secret в сделке "
            f"не дешевле {tier['minimum_gold']} золотых; либо допрос после поражения герою и calm."
        )
        if npc.upper() == LURE and self._friend:
            lines.append("BDT13_FRIEND: герой тебе друг; можешь раскрыть тайну через tell_secret.")
        if (fight := self._fight(npc)) and fight["state"] == "calm":
            lines.append("Ты проиграл герою и прекратил драку: он может допросить тебя через tell_secret.")
        for secret in tier["secrets"]:
            if self.revealed(npc, secret):
                lines.append(f"{secret}: ты уже раскрыл герою эту тайну, можешь говорить о ней.")
            elif any(f.get("npc") == npc.upper() and f.get("secret") == secret
                     for f in self.chronicle.facts("deal")):
                lines.append(f"{secret}: герой оплатил тайну; теперь раскрой её через tell_secret.")
        return "\n".join(lines)

    all_tools = (TELL_SECRET,)

    def tools(self, npc: str) -> list[Tool]:
        tier = self.tier(npc)
        return [TELL_SECRET] if tier and tier["secrets"] else []

    def _witnesses(self, npc: str) -> dict[str, str]:
        snap = self.link.snapshots.get(npc)
        witnesses = {o.id: o.name for o in snap.nearby_npcs
                     if o.guild == 9 and not o.dead and o.distance <= 1500} if snap else {}
        if self.perception and (view := self.perception.view(npc)):
            witnesses.update({o.id: o.name for o in view.sees
                              if o.guild == 9 and o.body != "dead" and o.distance <= 1500})
        witnesses.pop(npc, None)
        return witnesses

    async def handle(
        self, dialogue: DialogueManager, conv: Conversation, call: ToolCall, snap: Snapshot | None
    ) -> ToolOutcome | None:
        if call.name != "tell_secret":
            return None
        npc, secret = conv.npc.upper(), call.arguments.get("secret")
        tier = self.tier(npc)
        if not tier or secret not in tier["secrets"]:
            return ToolOutcome("Отклонено: ты не знаешь этой тайны об охоте.", "rejected")
        client = self.data["client"]
        text = client["name"] if secret == "client_name" else client["place_text"]
        if self.revealed(npc, secret):
            return ToolOutcome(f"Уже раскрыто: {text}.", "accepted")
        paid = any(f.get("npc") == npc and f.get("secret") == secret
                   for f in self.chronicle.facts("deal"))
        fight = self._fight(npc)
        leverage = "paid" if paid else ("beaten" if fight and fight["state"] == "calm" else "")
        try:
            if not leverage and npc == LURE and await self._bond():
                leverage = "bond"
            if not leverage:
                return ToolOutcome(
                    "Отклонено: нет рычага — нужна оплаченная сделка secret, поражение герою и calm "
                    "или дружба BDT13_FRIEND у зазывалы.", "rejected",
                )
            if not self.chronicle.facts("bounty_secret"):
                # Read the native guard too: an earlier request may have applied but timed out.
                state = await self.link.story_state(vars=["BDT13_DEXTER_VERRATEN", "MIS_STECKBRIEFE"])
                variables = state.get("vars", {})
                if variables.get("BDT13_DEXTER_VERRATEN", 0) != 1:
                    effects = [{"do": "set", "var": "BDT13_DEXTER_VERRATEN", "value": 1}]
                    if variables.get("MIS_STECKBRIEFE", 0) <= 1:
                        effects.extend([
                            {"do": "set", "var": "MIS_STECKBRIEFE", "value": 1},
                            {"do": "journal", **self.data["journal"]},
                        ])
                    await self.link.story_effect(npc, effects)
        except (GameError, TimeoutError) as err:
            return ToolOutcome(
                f"Отклонено: игра не подтвердила раскрытие ({str(err) or type(err).__name__}).", "rejected",
            )
        self.chronicle.record({"fact": "bounty_secret", "npc": npc, "name": conv.persona.name,
                               "secret": secret, "leverage": leverage})
        if not any(f.get("npc") == npc for f in self.chronicle.facts("bounty_betrayal")):
            witnesses = self._witnesses(npc)
            self.chronicle.record({
                "fact": "bounty_betrayal", "id": f"bounty:{npc}", "npc": npc,
                "name": conv.persona.name, "witnesses": list(witnesses), "witness_names": witnesses,
                "day": snap.world.day if snap else 0,
            })
        return ToolOutcome(f"Раскрыто ({leverage}): {text}.", "accepted")
