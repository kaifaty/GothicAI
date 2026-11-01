"""Live bargaining decisions using the existing model-off game fixtures."""

from __future__ import annotations

import asyncio
from typing import TYPE_CHECKING

from ..game import GameError, GameLink
from ..protocol import Session
from .bandit_bounty import bandit_bounty
from .bandit_parley import bandit_parley
from .cheat import cheat_scenario
from .common import _save_or_fail, _until, wait_message
from .crime_meet import crime_meet
from .deals import xardas_money
from .lies import amends_storylet
from .live import LiveSuite
from .npc_life import lead_scenario, meet_scenario

if TYPE_CHECKING:
    from ..amends import Amends
    from ..bounty import Bounty
    from ..crime_meet import CrimeMeet
    from ..crime_react import CrimeReact
    from ..crimes import Crimes
    from ..deals import Deals
    from ..dialogue import DialogueManager
    from ..guide import Guide
    from ..meetings import Meetings
    from ..parley import Parley
    from ..people_knowledge import PeopleKnowledge
    from ..social import Rumors
    from ..talk import TalkDesk


async def live_deals(
    link: GameLink,
    dialogue: DialogueManager,
    talk: TalkDesk,
    deals: Deals,
    parley: Parley,
    bounty: Bounty,
    guide: Guide,
    meetings: Meetings,
    crimes: Crimes,
    reactions: CrimeReact,
    meets: CrimeMeet,
    amends: Amends,
    rumors: Rumors,
    people: PeopleKnowledge,
    log_dir: str,
) -> dict[str, str]:
    """Reload the original farm save for each selected decision and stop after its outcome."""
    if dialogue.llm.name in {"off", "fake"}:
        raise GameError("scenario live_deals requires a live LLM")
    suite = LiveSuite("live_deals", dialogue.trace)
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
                if name in {"money_unasked", "money_asked"}:
                    results = await xardas_money(link, dialogue, talk, deals, live=case)
                elif name == "parley_terms":
                    results = await bandit_parley(link, dialogue, talk, deals, parley, live=case)
                elif name == "bounty_leverage":
                    results = await bandit_bounty(link, dialogue, talk, deals, bounty, guide, live=case)
                elif name == "cheat_keeps":
                    results = await cheat_scenario(
                        link, crimes, dialogue, talk, deals, rumors, people, log_dir, live=case,
                    )
                elif name == "lead_offer":
                    results = await lead_scenario(link, dialogue, talk, deals, guide, live=case)
                elif name == "meet_agree":
                    results = await meet_scenario(link, dialogue, talk, deals, meetings, live=case)
                elif name == "amends_forgive":
                    results = await amends_storylet(
                        link, dialogue, talk, amends, deals=deals, live=case,
                    )
                elif name == "crime_demand":
                    results = await crime_meet(link, crimes, reactions, meets, amends, live=case)
                else:
                    raise ValueError(f"unknown live_deals case {name!r}")
                failed = {key: value for key, value in results.items() if not value.startswith("done")}
                if failed:
                    raise GameError(f"{name} preparation failed: {failed}")
            finally:
                talk.reflexes = original_reflexes
                talk.end("gone")
                await _until(lambda: not talk.attached, 30)
    return suite.results()
