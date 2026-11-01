"""Live lie decisions using the same game fixtures as the model-off scenarios."""

from __future__ import annotations

import asyncio
from typing import TYPE_CHECKING

from ..game import GameError, GameLink
from ..protocol import Session
from .common import _save_or_fail, _until, wait_message
from .cover import cover_scenario
from .crime_talk import crime_talk
from .frame import frame
from .lies import vanilla_check, witness_check
from .live import LiveSuite
from .perception import seen_memory, trace_memory

if TYPE_CHECKING:
    from ..chests import ChestDesk
    from ..concerns import Concerns
    from ..crimes import Crimes
    from ..deals import Deals
    from ..dialogue import DialogueManager
    from ..npc_will import NpcWill
    from ..sightings import Sightings
    from ..social import Rumors
    from ..talk import TalkDesk
    from ..trust import Trust
    from ..vanilla import VanillaQuests
    from ..witnesses import Witnesses


async def live_lies(
    link: GameLink,
    dialogue: DialogueManager,
    talk: TalkDesk,
    sightings: Sightings,
    witnesses: Witnesses,
    vanilla: VanillaQuests,
    crimes: Crimes,
    deals: Deals,
    rumors: Rumors,
    chests: ChestDesk,
    concerns: Concerns,
    will: NpcWill,
    trust: Trust,
    log_dir: str,
) -> dict[str, str]:
    """Play selected decisions independently, reloading the original farm save each time."""
    if dialogue.llm.name in {"off", "fake"}:
        raise GameError("scenario live_lies requires a live LLM")
    suite = LiveSuite("live_lies", dialogue.trace)
    original_reflexes = talk.reflexes
    try:
        await link.wait_world()
        await _save_or_fail(link, 9)
    except Exception as error:
        suite.setup_error(error)
        return suite.results()

    for name in suite.selected:
        with suite.case(name) as case:
            loading = asyncio.create_task(wait_message(link, Session, lambda m: m.reason == "load", 60))
            await asyncio.sleep(0)
            await link.debug("load", 9)
            await loading
            await link.wait_world()
            try:
                if name == "seen_alibi":
                    results = await seen_memory(link, dialogue, talk, sightings, live=case)
                elif name in {"trace_age", "trace_control"}:
                    results = await trace_memory(link, dialogue, talk, sightings, log_dir, live=case)
                elif name == "witness_alibi":
                    results = await witness_check(link, dialogue, talk, sightings, witnesses, live=case)
                elif name == "vcheck_claim":
                    results = await vanilla_check(link, dialogue, talk, vanilla, live=case)
                elif name == "crime_testimony":
                    results = await crime_talk(
                        link, crimes, dialogue, talk, rumors, witnesses, sightings,
                        deals, chests, concerns, live=case,
                    )
                elif name == "frame_belief":
                    results = await frame(
                        link, will, chests, dialogue, talk, witnesses, trust, log_dir, live=case,
                    )
                elif name == "cover_offer":
                    results = await cover_scenario(
                        link, crimes, dialogue, talk, deals, rumors, witnesses, sightings,
                        trust, log_dir, live=case,
                    )
                else:
                    raise ValueError(f"unknown live_lies case {name!r}")
                failed = {key: value for key, value in results.items() if not value.startswith("done")}
                if failed:
                    raise GameError(f"{name} preparation failed: {failed}")
            finally:
                talk.reflexes = original_reflexes
                talk.end("gone")
                await _until(lambda: not talk.attached, 30)
    return suite.results()
