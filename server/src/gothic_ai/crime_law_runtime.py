"""Keep native law dialogs in step with this save branch's community law."""

from __future__ import annotations

import asyncio
import logging
from typing import Any

from .chronicle import Fact
from .crime_law import CrimeLaw, game_minutes
from .crime_law_graph import (
    AMBIENT,
    LAWFUL,
    absolution_program,
    ambient_ops,
    city_entry_nodes,
    city_entry_program,
    counter_values,
    gate_nodes,
    law_var,
    project_program,
    projection_values,
    validate_ops,
)
from .crime_memory import memory_value
from .crimes import COMMUNITIES, Crimes
from .game import GameError, GameLink
from .gamedata.storygraph import StoryGraph
from .people_knowledge import rumor_subject
from .protocol import DialogChosen, GameMessage, Loading, Session
from .story import StoryPatches
from .validator import Story, WorldState, names_in

log = logging.getLogger(__name__)
POLL_SECONDS = 1.0


class CrimeLawRuntime:
    def __init__(self, link: GameLink, crimes: Crimes, graph: StoryGraph, patches: StoryPatches) -> None:
        self.link, self.crimes, self.graph, self.patches = link, crimes, graph, patches
        self.chronicle = crimes.chronicle
        self.law = CrimeLaw(crimes)
        self._lock = asyncio.Lock()
        self._wake = asyncio.Event()
        self._generation = 0
        self._installed = False
        self._programs: list[dict[str, Any]] | None = None
        self._ambient: list[dict[str, Any]] = []
        self._levels: dict[str, int] | None = None
        self._projection: dict[str, tuple[str, ...]] = {}
        self._events: list[tuple[DialogChosen, list[str]]] = []
        self._paynow = {
            f"{node}_PAYNOW": community
            for node, community in gate_nodes(graph).items()
            if node.endswith("_PETZMASTER")
        }
        self.chronicle.on_fact.append(self._on_fact)
        self.link.listeners.append(self._on_message)

    def _on_fact(self, fact: Fact) -> None:
        if fact.get("fact") == "rumor" and rumor_subject(fact) != "PLAYER":
            return
        if fact.get("fact") in {
            "crime", "crime_seen", "rumor", "testimony", "died", "forgiven", "silenced", "absolved"
        }:
            self._wake.set()

    def _on_message(self, message: GameMessage) -> None:
        if isinstance(message, (Session, Loading)):
            self._generation += 1
            self._installed = False
            self._levels = None
            self._projection.clear()
            self._events.clear()
            self._wake.set()
        elif isinstance(message, DialogChosen) and not message.made:
            # `made` means a server-added dialog, not whether the native function ran.
            community = self._paynow.get(message.node)
            if community is not None:
                self._events.append((message, [f["id"] for f in self.law.unpaid(community)]))
            elif message.node == "DIA_PARLAN_WELCOME":
                self._events.append((message, [f["id"] for f in self.crimes.facts()]))
            self._wake.set()

    async def run(self) -> None:
        while not self.link.closed.is_set():
            self._wake.clear()
            await self.link.wait_world()
            try:
                await self.tick()
            except (GameError, TimeoutError) as error:
                log.warning("crime law: %s", error or type(error).__name__)
            try:
                await asyncio.wait_for(self._wake.wait(), POLL_SECONDS)
            except TimeoutError:
                pass

    async def close(self) -> None:
        self.link.listeners.remove(self._on_message)
        self.chronicle.on_fact.remove(self._on_fact)

    async def _install(self) -> None:
        generation = self._generation
        # StoryPatches owns branch replay. Finish it before adding derived, unrecorded programs.
        if self.patches._background:
            await asyncio.gather(*tuple(self.patches._background))
        if self._programs is None:
            entries = city_entry_nodes(self.graph)
            keys = [(node, "condition") for node in sorted(set(gate_nodes(self.graph)) | entries)]
            keys += [(node, "information") for node in gate_nodes(self.graph) if node.endswith("_PETZMASTER")]
            keys += [(node, "information") for node in sorted(entries)]
            keys += [(node, "information") for node in self._paynow]
            if "DIA_PARLAN_WELCOME" in self.graph.nodes:
                keys.append(("DIA_PARLAN_WELCOME", "information"))
            if AMBIENT in self.graph.nodes:
                keys.append((AMBIENT, "information"))
            own = await asyncio.gather(
                *(self.link.story_program(node, part, own=True) for node, part in keys)
            )
            ops = []
            communities = gate_nodes(self.graph)
            for (node, part), program in zip(keys, own, strict=True):
                steps = program.get("steps")
                if not isinstance(steps, list) or not steps:
                    raise GameError(f"crime law: missing native {node} {part} program")
                if node == AMBIENT:
                    self._ambient = steps
                    continue
                if node in self._paynow or node == "DIA_PARLAN_WELCOME":
                    steps = absolution_program(steps)
                elif node in entries:
                    steps = city_entry_program(node, part, steps)
                else:
                    steps = project_program(communities[node], steps)
                ops.append({"op": "program", "node": node, "part": part, "steps": steps})
            ops += ambient_ops(self.graph, self._ambient, {})
            state = await self.link.story_state(symbols=names_in(ops))
            story = Story(self.graph, WorldState.from_game(state)).then(self.patches.static)
            errors = validate_ops(ops, story)
            if errors:
                raise GameError("crime law patches: " + "; ".join(errors))
            self._programs = ops
            # Replays install safe ambient knowledge (none); the next tick derives this branch's view.
            self.patches.static.extend(ops)
        await self.link.patch_graph(self._programs)
        if generation == self._generation:
            self._installed = True

    async def _people(self) -> None:
        generation = self._generation
        # The deed carries eyewitness guild/NPCTYPE. Rumor recipients and legacy crime_seen facts
        # need the same existing VM fields, never an inferred guild from an NPC's symbol.
        evidence = [*self.chronicle.facts("crime_seen"), *self.chronicle.facts("rumor"),
                    *self.chronicle.facts("testimony")]
        for fact in evidence:
            if fact.get("fact") == "rumor" and (
                not fact.get("crime") or rumor_subject(fact) != "PLAYER"
            ):
                continue
            npc = str(fact.get("to") or fact.get("npc") or "").upper()
            if not npc:
                continue
            if "guild" in fact and "npctype" in fact:
                self.crimes.people[npc] = {
                    **self.crimes.people.get(npc, {}), "guild": fact["guild"], "npctype": fact["npctype"]
                }
                continue
            if "npctype" in self.crimes.people.get(npc, {}):
                continue
            try:
                values = await self.link.story_eval(
                    npc, [["field", ["inst", "SELF"], "GUILD"], ["field", ["inst", "SELF"], "NPCTYPE"]]
                )
            except (GameError, TimeoutError):
                # A legacy eyewitness can be in another world. Its missing identity must not
                # prevent other communities' news, payments or already classified reports.
                continue
            if generation != self._generation:
                return
            if len(values) == 2 and all(isinstance(value, int) for value in values):
                self.crimes.people[npc] = {
                    **self.crimes.people.get(npc, {}), "guild": values[0], "npctype": values[1]
                }

    def _news_levels(self) -> dict[str, int]:
        recipients = {
            str(f.get("to") or f.get("npc"))
            for f in self.chronicle.facts()
            if f.get("fact") in {"crime_seen", "rumor", "testimony"} and (f.get("to") or f.get("npc"))
            and (f.get("fact") != "rumor" or rumor_subject(f) == "PLAYER")
        }
        return {
            npc: level
            for npc in recipients
            if (level := memory_value(self.crimes, npc)[0])
        }

    async def tick(self) -> None:
        async with self._lock:
            generation, branch = self._generation, self.chronicle.branch
            if branch is None:
                return
            if not self._installed:
                await self._install()
            if generation != self._generation or branch != self.chronicle.branch:
                return
            await self._people()
            debt_vars = [
                f"{node.removeprefix('DIA_').removesuffix('_PETZMASTER')}_SCHULDEN"
                for node in gate_nodes(self.graph) if node.endswith("_PETZMASTER")
            ]
            state = await self.link.story_state(vars=debt_vars)
            if generation != self._generation or branch != self.chronicle.branch:
                return
            now = game_minutes(state)
            if now is None:
                raise GameError("crime law: game clock missing")
            events, self._events = self._events, []
            self.law.tick(now)
            # Payments cover exactly the cases known when the native choice ran. A report that
            # arrives during this server tick belongs to the next bill.
            for message, cases in events:
                if message.node == "DIA_PARLAN_WELCOME":
                    self.law.absolve(None, crimes=cases, how="LOC_ALL")
                else:
                    prefix = message.node.removeprefix("DIA_").removesuffix("_PETZMASTER_PAYNOW")
                    if state.get("vars", {}).get(f"{prefix}_SCHULDEN") == 0:
                        self.law.absolve(self._paynow[message.node], crimes=cases)
            for community in LAWFUL:
                unpaid = self.law.unpaid(community)
                cases = tuple(sorted(f["id"] for f in unpaid))
                values = projection_values(community, unpaid)
                # Native perceptions may still fill PETZ behind a closed lock. Before opening it,
                # restore known cases; programs repeat that store whenever an open law is read.
                if cases and self._projection.get(community) != cases:
                    values.update(counter_values(community, unpaid))
                if not self._projection.get(community) and cases:
                    for npc in COMMUNITIES[community].lawkeepers:
                        prefix = npc.split("_", 2)[-1]
                        if not state.get("vars", {}).get(f"{prefix}_SCHULDEN", 0):
                            values.update({f"{prefix}_LASTPETZCOUNTER": 0, f"{prefix}_LASTPETZCRIME": 0})
                values[law_var(community)] = int(bool(cases))
                if self._projection.get(community) != cases:
                    await self.link.story_effect(
                        "PC_HERO",  # pure global stores also work when this keeper is in another world
                        [{"do": "set", "var": var, "value": value} for var, value in values.items()],
                    )
                    if generation != self._generation or branch != self.chronicle.branch:
                        return
                    self._projection[community] = cases
            levels = self._news_levels()
            if levels != self._levels:
                await self.link.patch_graph(ambient_ops(self.graph, self._ambient, levels))
                if generation == self._generation and branch == self.chronicle.branch:
                    self._levels = levels
