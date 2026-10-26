"""How much of the hero's insolence an npc takes (docs/xardas-wrath.md): on by default, off with
`serve --no-wrath`.

The conversational model judges each insult with `take_offense` (how bad, and whether it is the one thing
the npc never forgives); the code keeps the scales: what the npc took, how much patience it has left, and the
npc sees it in its situation. When the scales tip, the npc says one last line and answers with force: Xardas
kills the hero with a spell, raises him where he fell as a skeleton, and the game ends on a picture
(the game's `raise_dead`, protocol v13).
"""

from __future__ import annotations

import asyncio
import logging
from dataclasses import dataclass, field
from pathlib import Path
from typing import TYPE_CHECKING

from .game import GameError, GameLink
from .journal import Journal
from .llm import Tool, ToolCall
from .protocol import EndCard, Snapshot
from .turn import ToolOutcome

if TYPE_CHECKING:
    from .dialogue import Conversation, DialogueManager

log = logging.getLogger(__name__)

TAKE_OFFENSE = "take_offense"
ASSETS = Path(__file__).parent / "assets"


@dataclass(frozen=True)
class Temper:
    patience: int  # points of insult it takes; the next that reaches it tips the scales
    unforgivable: str  # what tips them at once, as the npc sees it
    spell: str  # what it kills with
    card: EndCard  # the picture the game ends on


TEMPERS: dict[str, Temper] = {
    "NONE_100_XARDAS": Temper(
        patience=5,
        unforgivable="оскорбление твоей матери",
        spell="SPL_SKULL",
        card=EndCard(
            image=str(ASSETS / "xardas-end.jpg"),
            title="Конец",
            text="Ксардас не терпит хамства. Теперь ты служишь ему вечно.",
        ),
    ),
}

TOOL = Tool(
    TAKE_OFFENSE,
    "Герой тебя оскорбил, унизил или нагло тебе дерзит — отметь это. severity: 1 — дерзость, грубость; "
    "2 — оскорбление; 3 — тяжкое оскорбление, издёвка над тем, что тебе дорого. unforgivable: true, если он "
    "задел то, чего ты не прощаешь никому (это сказано в обстановке). Не отмечай спор, несогласие, шутку "
    "без злобы или грубость, на которую ты сам его вызвал. Терпение твоё не бесконечно: сколько его осталось "
    "— в обстановке. Когда оно кончится, ты ответишь не словами — это сделает игра; не обещай этого заранее.",
    {
        "type": "object",
        "properties": {
            "severity": {"type": "integer", "description": "1, 2 или 3"},
            "unforgivable": {"type": "boolean", "description": "задел то, чего ты не прощаешь"},
            "what": {"type": "string", "description": "чем он тебя задел, коротко"},
        },
        "required": ["severity", "unforgivable", "what"],
        "additionalProperties": False,
    },
)

LAST_WORD = (
    "Чаша твоего терпения переполнена: сейчас ты убьёшь его своей магией и поднимешь из мёртвых — пусть "
    "служит тебе скелетом. Скажи одну короткую фразу, последнюю, что он услышит, и больше ничего."
)


@dataclass
class Scales:
    """What one npc took from the hero on a save branch."""

    took: int = 0
    insults: list[str] = field(default_factory=list)
    tipped: bool = False


class Wrath:
    """A TurnExtension: the scales of the npc's patience, its tool, and its answer when they tip."""

    def __init__(self, link: GameLink, dialogue: DialogueManager, *, journal: Journal | None = None) -> None:
        self.link = link
        self.dialogue = dialogue
        self.journal = journal or dialogue.journal
        self.scales: dict[tuple[str | None, str], Scales] = {}

    def of(self, npc: str) -> Scales:
        return self.scales.setdefault((self.dialogue.branch, npc.upper()), Scales())

    # --- the conversation (TurnExtension) ------------------------------------------------------

    async def prepare(self, npc: str) -> None:
        pass

    def situation(self, npc: str) -> str | None:
        temper = TEMPERS.get(npc.upper())
        if temper is None:
            return None
        s = self.of(npc)
        if s.tipped:
            return None
        left = max(0, temper.patience - s.took)
        lines = [
            f"Твоё терпение к дерзостям героя: осталось {left} из {temper.patience}. Каждую его дерзость "
            f"отмечай вызовом {TAKE_OFFENSE}: иначе игра не узнает, что он тебя задел."
        ]
        if s.insults:
            lines.append(f"- он уже задел тебя: {'; '.join(s.insults)}")
        if 0 < left <= 2:
            lines.append("- ты на пределе: ещё одна дерзость — и ты ответишь не словами")
        if temper.unforgivable:
            lines.append(
                f"- {temper.unforgivable}: этого ты не прощаешь никому. Задел хоть словом — вызови "
                f"{TAKE_OFFENSE} с unforgivable=true, и разговор на этом кончится"
            )
        return "\n".join(lines)

    all_tools: tuple[Tool, ...] = (TOOL,)

    def tools(self, npc: str) -> list[Tool]:
        if npc.upper() not in TEMPERS or self.of(npc).tipped:
            return []
        return [TOOL]

    async def handle(
        self, dialogue: DialogueManager, conv: Conversation, call: ToolCall, snap: Snapshot | None
    ) -> ToolOutcome | None:
        if call.name != TAKE_OFFENSE:
            return None
        temper = TEMPERS.get(conv.npc.upper())
        if temper is None:
            return ToolOutcome("Тебе нечего отмечать", "rejected")
        s = self.of(conv.npc)
        if s.tipped:
            return ToolOutcome("Ты уже ответил ему", "ignored")
        severity = min(3, max(1, int(call.arguments.get("severity", 1))))
        unforgivable = bool(call.arguments.get("unforgivable")) and bool(temper.unforgivable)
        what = str(call.arguments.get("what", "")).strip()
        s.took += temper.patience if unforgivable else severity
        if what:
            s.insults.append(what)
        self.journal.add(
            "offense",
            npc=conv.npc,
            name=conv.persona.name,
            severity=severity,
            unforgivable=unforgivable,
            what=what,
            took=s.took,
            patience=temper.patience,
        )
        log.info(
            "%s takes offense (%d%s): %s; %d of %d",
            conv.npc,
            severity,
            ", unforgivable" if unforgivable else "",
            what,
            s.took,
            temper.patience,
        )
        if s.took < temper.patience:
            return ToolOutcome(
                f"Терпения осталось {temper.patience - s.took} из {temper.patience}", "accepted"
            )
        s.tipped = True
        await dialogue.change_intent(
            conv.npc, conv.persona.name, attitude="hostile", agenda="punish the hero for his insolence"
        )
        self._answer(conv, temper)
        return ToolOutcome(LAST_WORD, "read")  # the turn goes on: the last word, knowing it is the last

    def _answer(self, conv: Conversation, temper: Temper) -> None:
        """Once the npc has said all of this turn: the game's raise_dead."""
        reply = conv.reply

        async def run() -> None:
            if reply is not None and reply is not asyncio.current_task():
                await asyncio.wait({reply})
            if conv.voice is not None:
                await conv.voice.join()
            try:
                await self.link.raise_dead(conv.npc, temper.card, temper.spell)
            except (GameError, TimeoutError) as err:
                log.warning("%s: raise_dead: %s", conv.npc, err)
                return
            self.journal.add("raise_dead", npc=conv.npc, name=conv.persona.name, spell=temper.spell)
            await self.dialogue.remember(
                conv.npc,
                conv.persona.name,
                "Ты убил героя за его дерзость и поднял его из мёртвых скелетом — теперь он служит тебе",
            )

        task = asyncio.create_task(run())
        conv.actions.add(task)
        task.add_done_callback(conv.actions.discard)
