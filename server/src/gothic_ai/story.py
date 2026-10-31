"""Changes to the story graph in the game (docs/story-graph.md, stage 3).

Dialogs can be switched off, get a condition on top of their script one, or be added: the director will
do it (stage 5), for now the console does. Each change is one operation (see protocol.GraphPatchCmd). It
goes to the game and into the memory log of the save branch, so a load brings back exactly the changes
made before that save, like the rest of the world; the game gets them again whenever a session starts.

Some dialogs are off for good, on every branch (`static`): the scripted ambushes of the bandits below Xardas's
tower and Alvares and Engardo at Akil's farm (`FREED`). Their important dialogs ended in an attack the scripts
decided; without them the bandits stand at their posts and the behavioral and conversational models decide
what they do — whether they talk, threaten or attack (dialogue.py: an npc that means to fight the hero
attacks him through the scripts).
They still fight back when struck, and the gang joins a fight it hears, as in the game.
"""

from __future__ import annotations

import asyncio
import json
import logging
from collections.abc import Callable, Coroutine
from typing import Any

from .game import GameError, GameLink
from .memory import MemoryStore
from .protocol import DialogChosen, GameMessage, MemoryCmd, NpcSpec, Session

log = logging.getLogger(__name__)

Op = dict[str, Any]

# a character of the director's in the branch's log, {"op": "npc", "spec": NpcSpec} (docs/director.md,
# stage 2): not a change of the graph, the game makes it once (story_npc)
NPC = "npc"
# a quest of the director's, {"op": "quest", "quest": questbook.Quest} (docs/director.md, stage 3): its
# description, next to the changes of the graph built of it; the game gets the changes only
QUEST = "quest"
RECORDS = (NPC, QUEST)  # in the log, not changes of the graph

# the bandits below the steps to Xardas's tower (user, 2026-10-13): the lure's "Where are you going?" that
# ends in his attack or leads the hero into the cave, the cave's ambush, Brago's and his mate's attack, and
# Cavalorn's "let's deal with them", which sets them on the hero at sight
FREED = (
    "DIA_BDT_1013_BANDIT_WHERE",
    "DIA_1013_BANDIT_AMBUSH",
    "DIA_BDT_1014_BANDIT_KILLER",
    "DIA_1015_BANDIT_AMBUSH",
    "DIA_ADDON_CAVALORN_LETSKILLBANDITS",
    # Akil's farm: the warnings, Alvares's choices and the follow-up attacks. The farm's post-fight
    # dialogs need only both bandits dead, not AKILS_SLDSTILLTHERE or its journal topic.
    "DIA_ALVARES_HAUAB",
    "DIA_ALVARES_ATTACK",
    "DIA_ALVARES_ATTACK_KERLE",
    "DIA_ALVARES_ATTACK_SOELDNER",
    "DIA_ALVARES_ATTACK_WITZ",
    "DIA_ALVARES_ATTACK_AERGER",
    "DIA_ALVARES_ATTACK_LIEFERUNG",
    "DIA_ALVARES_SCHLUSS",
    "DIA_ENGARDO_HALLO",
)


def freed(known: Callable[[str], bool] = lambda node: True) -> list[Op]:
    """The ops that switch the freed npc's scripted dialogs off; only dialogs the game has (`known`)."""
    return [{"op": "disable", "node": node} for node in FREED if known(node)]


class StoryPatches:
    def __init__(self, link: GameLink, memory: MemoryStore, *, static: list[Op] | None = None) -> None:
        self.link = link
        self.memory = memory
        self.static = static or []  # changes on every branch, never recorded
        self.branch: str | None = None
        self.version = 0  # grows with every change of the log or of the branch: for readers that cache it
        self._background: set[asyncio.Task[Any]] = set()
        link.listeners.append(self._on_message)
        if link.session is not None:
            self._on_session(link.session)

    def ops(self) -> list[Op]:
        """The changes of the graph on the current branch, oldest first."""
        return [op for op in self._log() if op.get("op") not in RECORDS]

    def records(self, kind: str) -> list[Op]:
        """The records of one kind on the current branch (NPC, QUEST), oldest first."""
        return [op for op in self._log() if op.get("op") == kind]

    def characters(self) -> list[NpcSpec]:
        """The director's characters on the current branch, in the order they came."""
        return [NpcSpec.model_validate(op["spec"]) for op in self.cast()]

    def cast(self) -> list[Op]:
        """The records of the director's characters on the current branch: {"op", "spec", "person"?}."""
        return self.records(NPC)

    async def introduce(self, spec: NpcSpec, person: dict[str, Any] | None = None) -> dict[str, Any]:
        """A character of the director's: the game makes it (or GameError says why not), then the branch
        remembers it, with who it is as a person (cast.Person) if given. The game's answer: {npc, state,
        index}."""
        if self.branch is None:
            raise GameError("the game has not reported a session yet")
        made = await self.link.story_npc(spec)
        record: Op = {"op": NPC, "spec": spec.model_dump(exclude_none=True)}
        if person is not None:
            record["person"] = person
        self._record(record)
        return made

    def _log(self) -> list[Op]:
        return [json.loads(op) for op in self.memory.patches(self.branch)] if self.branch else []

    async def apply(self, op: Op) -> None:
        """One change: the game takes it (or GameError says why not), then the branch remembers it."""
        if self.branch is None:
            raise GameError("the game has not reported a session yet")
        await self.link.patch_graph([op])
        self._record(op)

    async def make(self, record: Op | list[Op], ops: list[Op]) -> None:
        """Something of the director's built of changes (one quest or a chain): the game takes all of them,
        then the branch remembers the records and the changes. If the game refuses one (GameError), the
        others are taken back: it gets the branch's changes anew, as they were."""
        if self.branch is None:
            raise GameError("the game has not reported a session yet")
        try:
            await self.link.patch_graph(ops)
        except GameError:
            await self._replay(self.characters(), self.ops())
            raise
        for entry in [record] if isinstance(record, dict) else record:
            self._record(entry)
        for op in ops:
            self._record(op)

    def _on_message(self, msg: GameMessage) -> None:
        match msg:
            case Session():
                self._on_session(msg)
            case DialogChosen(made=True):
                # the hero chose an added dialog: it is not offered again, also after a load
                self._record({"op": "told", "node": msg.node})
            case DialogChosen():
                log.info("dialog chosen: %s %s", msg.npc, msg.node)

    def _on_session(self, session: Session) -> None:
        self.branch = session.branch
        self.version += 1
        self.memory.ensure_branch(session.branch, session.parent, session.parent_seq)
        ops, cast = self.ops(), self.characters()
        if ops or cast:
            log.info(
                "story graph: %d change(s), %d character(s) on branch %s", len(ops), len(cast), session.branch
            )
        self._spawn(self._replay(cast, ops))

    async def _replay(self, cast: list[NpcSpec], ops: list[Op]) -> None:
        # the characters first, the graph's dialogs name them; one the save has stays as it is
        for spec in cast:
            try:
                await self.link.story_npc(spec)
            except (GameError, TimeoutError) as err:
                log.warning("story: %s: %s", spec.npc, err)
        # also when there are none: whatever the game had belongs to another line of play
        await self.link.patch_graph([*self.static, *ops], replace=True)

    def _record(self, op: Op) -> None:
        if self.branch is None:
            return
        seq = self.memory.add_patch(self.branch, json.dumps(op, ensure_ascii=False))
        self.version += 1
        # the head goes into the next save: a load must bring this change back
        self._spawn(self.link.send(MemoryCmd(branch=self.branch, seq=seq)))

    def _spawn(self, coro: Coroutine[Any, Any, Any]) -> None:
        async def run() -> None:
            try:
                await coro
            except (GameError, TimeoutError) as err:
                log.warning("story graph: %s", err)

        task = asyncio.create_task(run())
        self._background.add(task)
        task.add_done_callback(self._background.discard)
