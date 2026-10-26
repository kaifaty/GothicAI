"""The scripts' quest states, and their changes on the current save branch. No model calls."""

from __future__ import annotations

import asyncio
import logging
from collections.abc import Coroutine
from typing import Any

from .chronicle import Chronicle, Fact
from .game import GameError, GameLink
from .gamedata.storygraph import QUEST_STATES, StoryGraph
from .protocol import DialogChosen, GameMessage, Loading, Session

log = logging.getLogger(__name__)
STATES = {0: "open", **QUEST_STATES}


class QuestLog:
    def __init__(
        self,
        link: GameLink,
        graph: StoryGraph,
        chronicle: Chronicle,
        *,
        poll_s: float = 30.0,
        settle_s: float = 0.5,
    ) -> None:
        self.link, self.graph, self.chronicle = link, graph, chronicle
        self.poll_s, self.settle_s = poll_s, settle_s
        self.states: dict[str, str] = {}
        self._writers: dict[str, set[str]] = {}
        self._givers: dict[str, str] = {}
        for var, quest in graph.quests.items():
            writers = quest.movers() | (graph.vars[var].writers if var in graph.vars else set())
            writers |= {node.id for node in graph.nodes.values() if var in node.writes}
            for writer in writers:
                self._writers.setdefault(writer, set()).add(var)
            givers = {graph.nodes[n].npc for n in quest.running if n in graph.nodes and graph.nodes[n].npc}
            if len(givers) == 1:
                self._givers[var] = givers.pop()
        # A parent can start the information that offers a quest-changing reply. The reply itself
        # also reports dialog_chosen, with its own function as node.
        for node in graph.nodes.values():
            for choice in node.choices():
                self._writers.setdefault(node.id, set()).update(self._writers.get(choice, ()))
        self._pending: dict[str, tuple[DialogChosen, float]] = {}
        self._generation = 0
        self._active = False
        self._lock = asyncio.Lock()
        self._background: set[asyncio.Task[None]] = set()
        link.listeners.append(self._on_message)
        if link.session is not None:
            self._on_session()

    @property
    def recent(self) -> list[Fact]:
        return self.chronicle.facts("vanilla_quest")[-10:]

    async def poll(self) -> None:
        """Read all quest variables, retaining the selected writer as the cause until effects settle."""
        generation = self._generation
        async with self._lock:
            if not self._active or generation != self._generation:
                return
            while self._pending:
                delay = max(t for _, t in self._pending.values()) - asyncio.get_running_loop().time()
                if delay <= 0:
                    break
                await asyncio.sleep(delay)
                if not self._active or generation != self._generation:
                    return
            causes = dict(self._pending)
            state = await self.link.story_state(vars=self.graph.quests)
            if not self._active or generation != self._generation:
                return  # a response from before a load belongs to the old world
            for var, value in state.get("vars", {}).items():
                if var not in self.graph.quests or value not in STATES:
                    continue
                # Another dialog was chosen while the read was in flight: let its settled read win.
                if self._pending.get(var) != causes.get(var):
                    continue
                after = STATES[value]
                before = self.states.get(var)
                self.states[var] = after
                if before is None or before == after:
                    continue  # first observation on this branch is a baseline, including state 0
                fact: Fact = {
                    "fact": "vanilla_quest",
                    "quest": var,
                    "title": self.graph.quests[var].title,
                    "from": before,
                    "to": after,
                    **{k: state[k] for k in ("day", "hour", "minute")},
                }
                if var in causes:
                    chosen = causes[var][0]
                    fact.update(node=chosen.node, npc=chosen.npc)
                if var in self._givers:
                    fact["giver"] = self._givers[var]
                await self.chronicle.record(fact)
            for var, cause in causes.items():
                if self._pending.get(var) == cause:
                    del self._pending[var]

    async def run(self) -> None:
        """Script functions outside dialogs can move quests too."""
        while True:
            await asyncio.sleep(self.poll_s)
            try:
                await self.poll()
            except (GameError, TimeoutError) as err:
                log.warning("questlog: %s", err)

    def _on_message(self, message: GameMessage) -> None:
        if isinstance(message, Loading):
            self._reset()
        elif isinstance(message, Session):
            self._on_session()
        elif isinstance(message, DialogChosen) and not message.made and self._active:
            quests = self._writers.get(message.node.upper(), ())
            if quests:
                ready = asyncio.get_running_loop().time() + self.settle_s
                for var in quests:
                    self._pending[var] = (message, ready)
                self._spawn(self.poll())

    def _reset(self) -> None:
        self._generation += 1
        self._active = False
        self.states.clear()
        self._pending.clear()
        for task in self._background:
            task.cancel()

    def _on_session(self) -> None:
        self._reset()
        for fact in self.chronicle.facts("vanilla_quest"):
            if fact.get("quest") in self.graph.quests and fact.get("to") in STATES.values():
                self.states[fact["quest"]] = fact["to"]
        self._active = True
        self._spawn(self.poll())

    def _spawn(self, coro: Coroutine[Any, Any, None]) -> None:
        task = asyncio.create_task(coro)
        self._background.add(task)

        def finished(task: asyncio.Task[None]) -> None:
            self._background.discard(task)
            if not task.cancelled() and (err := task.exception()) is not None:
                log.warning("questlog: %s", err, exc_info=err)

        task.add_done_callback(finished)

    async def close(self) -> None:
        self._reset()
        self.link.listeners.remove(self._on_message)
        await asyncio.gather(*self._background, return_exceptions=True)
