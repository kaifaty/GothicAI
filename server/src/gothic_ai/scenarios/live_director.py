"""Manual live director decisions, reusing the model-off scenarios' game fixtures."""

from __future__ import annotations

import asyncio
from typing import TYPE_CHECKING

from ..game import GameError, GameLink
from ..protocol import Session
from .common import _save_or_fail, _until, wait_message
from .crime_director import crime_director
from .feats import feats
from .lies import amends_storylet
from .live import LiveSuite
from .npc_director import npc_director
from .showrunner import _LiveBuilderTrace, trace_live_builders
from .story import author_chain, authored_quest, director_casts, repair_after_death

SCENARIOS = {
    "live_director": ("LIVE", None),
}

if TYPE_CHECKING:
    from ..cli import ServerState
    from ..journal import Journal
    from ..places import PlaceBook


async def live_director(
    link: GameLink, state: ServerState, places: PlaceBook, journal: Journal, log_dir: str,
) -> dict[str, str]:
    dialogue, talk = state.dialogue, state.talk
    director, story = state.director, state.story
    author, casting, chains, showrunner = state.author, state.casting, state.chains, state.showrunner
    chronicler, amends = state.feats, state.amends
    assert dialogue and talk and director and story and author and casting and chains and showrunner
    assert chronicler and amends and state.chronicle
    if dialogue.llm.name in {"off", "fake"}:
        raise GameError("scenario live_director requires a live LLM")
    suite = LiveSuite("live_director", dialogue.trace)
    try:
        await link.wait_world()
        await _save_or_fail(link, 9)
    except Exception as error:
        suite.setup_error(error)
        return suite.results()

    original_reflexes = talk.reflexes
    state.chronicle.on_entry.remove(chronicler._on_entry)
    try:
        for name in suite.selected:
            with suite.case(name) as case:
                loading = asyncio.create_task(wait_message(link, Session, lambda m: m.reason == "load", 60))
                await asyncio.sleep(0)
                await link.debug("load", 9)
                await loading
                await link.wait_world()
                models = [director.model, amends.model, chronicler.model]
                original = [model.llm for model in models]
                repair = None
                try:
                    for model, backend in zip(models, original, strict=True):
                        model.llm = _LiveBuilderTrace(backend, suite.trace, state.chronicle.branch)
                    with trace_live_builders(showrunner, suite.trace):
                        if name == "repair_handover":
                            repair = asyncio.create_task(director.run())
                            results = await repair_after_death(link, director, story, live=case)
                        elif name == "authored_quest":
                            results = await authored_quest(link, author, story, live=case)
                        elif name == "character_cast":
                            results = await director_casts(link, casting, author, story, places, live=case)
                        elif name == "chain_built":
                            results = await author_chain(link, chains, places, live=case)
                        elif name == "crime_line":
                            assert state.npc_will and state.chests and state.witnesses
                            results = await npc_director(
                                link, state.npc_will, showrunner, state.chests, dialogue,
                                talk, state.witnesses, log_dir, live=case,
                            )
                        elif name == "crime_return":
                            assert state.crimes and state.rumors and state.sightings and state.deals
                            assert state.chests and state.concerns and state.guard and state.bodies
                            assert state.perception
                            results = await crime_director(
                                link, state.crimes, showrunner, dialogue, talk, state.rumors,
                                state.sightings, state.deals, state.chests, state.concerns,
                                amends, guard=state.guard, bodies=state.bodies,
                                perception=state.perception, initiative=state.initiative, live=case,
                            )
                        elif name == "amends_storylet":
                            results = await amends_storylet(link, dialogue, talk, amends, live=case)
                        elif name == "feat_award":
                            state.chronicle.on_entry.append(chronicler._on_entry)
                            results = await feats(link, chronicler, journal, dialogue, live=case)
                        else:
                            raise ValueError(f"unknown live_director case {name!r}")
                    failed = {key: value for key, value in results.items() if not value.startswith("done")}
                    if failed:
                        raise GameError(f"{name} preparation failed: {failed}")
                finally:
                    if repair is not None:
                        repair.cancel()
                        await asyncio.gather(repair, return_exceptions=True)
                    if chronicler._on_entry in state.chronicle.on_entry:
                        state.chronicle.on_entry.remove(chronicler._on_entry)
                    for model, backend in zip(models, original, strict=True):
                        model.llm = backend
                    talk.reflexes = original_reflexes
                    talk.end("gone")
                    await _until(lambda: not talk.attached, 30)
    finally:
        state.chronicle.on_entry.append(chronicler._on_entry)
    return suite.results()
