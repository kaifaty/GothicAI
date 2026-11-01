"""Live NPC choices, using the model-off scenarios' existing game fixtures."""

from __future__ import annotations

import asyncio
import os
from typing import TYPE_CHECKING

from ..game import GameError, GameLink
from ..protocol import Session
from .common import _save_or_fail, _until, wait_message
from .companions import calm_scenario, rescue
from .feats import feat_fame
from .lies import vanilla_spare, xardas_wrath
from .live import LiveSuite
from .npc_life import errand_scenario, initiative_scenario
from .perception import cave_scenario, knows_scenario

if TYPE_CHECKING:
    from ..cli import ServerState


async def live_npc(link: GameLink, state: ServerState) -> dict[str, str]:
    """Reload the baseline between choices; absent optional deciders consume no attempt."""
    dialogue = state.dialogue
    assert dialogue is not None
    if dialogue.llm.name in {"off", "fake"}:
        raise GameError("scenario live_npc requires a live LLM")
    suite = LiveSuite("live_npc", dialogue.trace)
    runnable = [name for name in suite.selected
                if name not in {"initiative_debt", "rescue_call"} or os.environ.get("DECIDER_URL")]
    for name in suite.selected:
        if name not in runnable:
            with suite.case(name) as case:
                case.skip("DECIDER_URL is not set; a live behavioral model is required")
    if not runnable:
        return suite.results()
    talk = state.talk
    assert talk is not None
    original_reflexes = talk.reflexes
    try:
        await link.wait_world()
        await _save_or_fail(link, 9)
    except Exception as error:
        suite.setup_error(error)
        return suite.results()

    for name in runnable:
        with suite.case(name) as case:
            loading = asyncio.create_task(wait_message(link, Session, lambda m: m.reason == "load", 60))
            await asyncio.sleep(0)
            await link.debug("load", 9)
            await loading
            await link.wait_world()
            original_sink = dialogue.journal.sink
            try:
                if name == "wrath_tip":
                    assert state.wrath is not None
                    results = await xardas_wrath(link, dialogue, talk, state.wrath, live=case)
                elif name == "calm_bandit":
                    assert state.parley is not None
                    results = await calm_scenario(link, dialogue, talk, state.parley, live=case)
                elif name == "cave_unnamed":
                    assert state.whereabouts is not None
                    results = await cave_scenario(link, dialogue, talk, state.whereabouts, live=case)
                elif name == "errand_tell":
                    assert state.errands is not None and state.rumors is not None
                    results = await errand_scenario(
                        link, dialogue, talk, state.errands, state.rumors, state.gossip, live=case,
                    )
                elif name == "spare_bandit":
                    assert state.vanilla is not None
                    results = await vanilla_spare(link, dialogue, talk, state.vanilla, live=case)
                elif name == "initiative_debt":
                    assert state.deals is not None and state.concerns is not None
                    assert state.guard is not None and state.bodies is not None
                    assert state.perception is not None
                    results = await initiative_scenario(
                        link, dialogue, talk, state.deals, state.concerns, state.guard,
                        state.bodies, state.perception, desk=state.initiative, live=case,
                    )
                elif name == "knows_danger":
                    assert state.knowledge is not None and state.lore is not None
                    assert state.whereabouts is not None
                    results = await knows_scenario(
                        link, dialogue, talk, state.knowledge, state.lore, state.whereabouts, live=case,
                    )
                elif name == "fame_recognized":
                    assert state.rumors is not None
                    results = await feat_fame(link, dialogue, talk, state.rumors, live=case)
                elif name == "rescue_call":
                    results = await rescue(link, planner=state.brain, dialogue=dialogue, live=case)
                else:
                    raise ValueError(f"unknown live_npc case {name!r}")
                failed = {key: value for key, value in results.items() if not value.startswith("done")}
                if failed:
                    raise GameError(f"{name} preparation failed: {failed}")
            finally:
                dialogue.journal.sink = original_sink
                talk.reflexes = original_reflexes
                talk.end("gone")
                await _until(lambda: not talk.attached, 30)
    return suite.results()
