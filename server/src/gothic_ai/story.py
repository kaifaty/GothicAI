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
        self._generation = 0
        self._prepared_session: Session | None = None
        self._background: set[asyncio.Task[Any]] = set()
        link.listeners.append(self._on_message)

    async def initialize(self) -> None:
        async def prepare_current() -> None:
            session = self.link.session
            if session is not None and session is not self._prepared_session:
                await self._on_session(session)

        await self.memory.complete(prepare_current)

    async def ops(self) -> list[Op]:
        """The changes of the graph on the current branch, oldest first."""
        return [op for op in await self._log() if op.get("op") not in RECORDS]

    async def records(self, kind: str) -> list[Op]:
        """The records of one kind on the current branch (NPC, QUEST), oldest first."""
        return [op for op in await self._log() if op.get("op") == kind]

    async def characters(self) -> list[NpcSpec]:
        """The director's characters on the current branch, in the order they came."""
        return [NpcSpec.model_validate(op["spec"]) for op in await self.cast()]

    async def cast(self) -> list[Op]:
        """The records of the director's characters on the current branch: {"op", "spec", "person"?}."""
        return await self.records(NPC)

    async def introduce(self, spec: NpcSpec, person: dict[str, Any] | None = None) -> dict[str, Any]:
        """A character of the director's: the game makes it (or GameError says why not), then the branch
        remembers it, with who it is as a person (cast.Person) if given. The game's answer: {npc, state,
        index}."""
        if self.branch is None:
            raise GameError("the game has not reported a session yet")
        branch, generation, session = self.branch, self._generation, self.link.session
        record: Op = {"op": NPC, "spec": spec.model_dump(exclude_none=True)}
        if person is not None:
            record["person"] = person
        content = json.dumps(record, ensure_ascii=False)
        made = await self.link.story_npc(spec)
        await self._record(content, branch, generation, session)
        return made

    async def _log(self) -> list[Op]:
        branch, generation, session = self.branch, self._generation, self.link.session
        records = await self.memory.patches(branch) if branch is not None else []
        if generation != self._generation or branch != self.branch or session is not self.link.session:
            raise GameError("session changed while reading story patches")
        return [json.loads(op) for op in records]

    async def apply(self, op: Op) -> None:
        """One change: the game takes it (or GameError says why not), then the branch remembers it."""
        if self.branch is None:
            raise GameError("the game has not reported a session yet")
        branch, generation, session = self.branch, self._generation, self.link.session
        content = json.dumps(op, ensure_ascii=False)
        await self.link.patch_graph([op])
        await self._record(content, branch, generation, session)

    async def make(self, record: Op | list[Op], ops: list[Op]) -> None:
        """Something of the director's built of changes (one quest or a chain): the game takes all of them,
        then the branch remembers the records and the changes. If the game refuses one (GameError), the
        others are taken back: it gets the branch's changes anew, as they were."""
        if self.branch is None:
            raise GameError("the game has not reported a session yet")
        branch, generation, session = self.branch, self._generation, self.link.session
        entries = [
            json.dumps(entry, ensure_ascii=False)
            for entry in [*([record] if isinstance(record, dict) else record), *ops]
        ]
        try:
            await self.link.patch_graph(ops)
        except GameError:
            if generation == self._generation and session is self.link.session:
                await self._replay(await self.characters(), await self.ops(), generation, session)
            raise
        async def persist() -> None:
            for content in entries:
                await self._record(content, branch, generation, session)

        await self.memory.complete(persist)

    async def _on_message(self, msg: GameMessage) -> None:
        match msg:
            case Session():
                await self._on_session(msg)
            case DialogChosen(made=True):
                # the hero chose an added dialog: it is not offered again, also after a load
                await self._record(
                    json.dumps({"op": "told", "node": msg.node}, ensure_ascii=False),
                    self.branch, self._generation, self.link.session,
                )
            case DialogChosen():
                log.info("dialog chosen: %s %s", msg.npc, msg.node)

    async def _on_session(self, session: Session) -> None:
        self._prepared_session = None
        self.branch = session.branch
        self.version += 1
        self._generation += 1
        generation = self._generation

        async def load() -> tuple[list[Op], list[NpcSpec]]:
            await self.memory.ensure_branch(session.branch, session.parent, session.parent_seq)
            records = [json.loads(op) for op in await self.memory.patches(session.branch)]
            return (
                [op for op in records if op.get("op") not in RECORDS],
                [NpcSpec.model_validate(op["spec"]) for op in records if op.get("op") == NPC],
            )

        ops, cast = await self.memory.complete(load)
        if generation != self._generation or session is not self.link.session:
            return
        self._prepared_session = session
        if ops or cast:
            log.info(
                "story graph: %d change(s), %d character(s) on branch %s", len(ops), len(cast), session.branch
            )
        self._spawn(self._replay(cast, ops, generation, session))

    async def _replay(
        self, cast: list[NpcSpec], ops: list[Op], generation: int, session: Session | None,
    ) -> None:
        # the characters first, the graph's dialogs name them; one the save has stays as it is
        for spec in cast:
            if generation != self._generation or session is not self.link.session:
                return
            try:
                await self.link.story_npc(spec)
            except (GameError, TimeoutError) as err:
                log.warning("story: %s: %s", spec.npc, err)
        # also when there are none: whatever the game had belongs to another line of play
        if generation != self._generation or session is not self.link.session:
            return
        await self.link.patch_graph([*self.static, *ops], replace=True)

    async def _record(
        self, record: str, branch: str | None, generation: int, session: Session | None,
    ) -> None:
        if branch is None:
            return

        async def persist() -> None:
            if generation != self._generation or branch != self.branch or session is not self.link.session:
                raise GameError("session changed while recording a story patch")
            seq = await self.memory.add_patch(branch, record)
            if generation != self._generation or branch != self.branch or session is not self.link.session:
                raise GameError("session changed while recording a story patch")
            self.version += 1
            # the head goes into the next save: a load must bring this change back
            try:
                await self.link.send(MemoryCmd(branch=branch, seq=seq))
            except (GameError, TimeoutError) as err:
                log.warning("story graph: %s", err)

        await self.memory.complete(persist)

    def _spawn(self, coro: Coroutine[Any, Any, Any]) -> None:
        async def run() -> None:
            try:
                await coro
            except (GameError, TimeoutError) as err:
                log.warning("story graph: %s", err)

        task = asyncio.create_task(run())
        self._background.add(task)
        task.add_done_callback(self._background.discard)
