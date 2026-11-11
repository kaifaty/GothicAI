"""Dispatch e2e scenarios using the running server's services."""

from __future__ import annotations

import argparse
import asyncio
import logging
import os
from collections.abc import Awaitable, Callable
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING, Any

from .. import places, scenarios
from ..bodypilot import BodyPilot
from ..game import GameLink
from ..journal import Journal
from ..llm import LLMBackend
from ..llm.off import OffBackend
from ..trace import TraceWriter
from . import occasions as scenario_occasions
from . import questlog as scenario_questlog
from . import showrunner as scenario_showrunner
from . import vanilla_line as scenario_vanilla_line
from .crime_director import crime_director as crime_director_scenario
from .crime_law import crime_law as crime_law_scenario
from .crime_talk import crime_talk as crime_talk_scenario
from .crimes import crimes as crimes_scenario

if TYPE_CHECKING:
    from ..session import ServerState

log = logging.getLogger("gothic_ai")


@dataclass
class ScenarioContext:
    args: argparse.Namespace
    state: ServerState
    place_book: places.PlaceBook | None
    journal: Journal
    trace: TraceWriter | None
    llm: LLMBackend
    done: asyncio.Event
    native_combat_available: Callable[[GameLink, BodyPilot | None, str], Awaitable[bool]]


async def run_scenario(ctx: ScenarioContext, link: GameLink, bodypilot: BodyPilot | None) -> int | None:
    args = ctx.args
    state = ctx.state
    place_book = ctx.place_book
    journal = ctx.journal
    trace = ctx.trace
    llm = ctx.llm
    done = ctx.done
    _native_combat_available = ctx.native_combat_available
    exit_code = None
    if args.scenario is None:
        return
    try:
        await link.wait_world()
        log.info("scenario %s: checks started", args.scenario)
        if args.scenario == "memprobe":
            where = Path(os.environ.get("LOG_DIR") or Path(args.trace_dir).parent)
            assert state.bodies is not None
            results = await scenarios.memprobe_scenario(
                link, bodies=state.bodies, log_dir=where,
                body_enabled=bodypilot is not None or bool(args.body_url),
            )
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario %s: %d checks, %d failed", args.scenario, len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "load_rewind":
            results = await scenarios.load_rewind(link)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario %s: %d checks, %d failed", args.scenario, len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "neural-soak":
            results = await scenarios.neural_soak_scenario(
                link, state.dialogue, bodypilot, log_dir=Path(args.memory_db).parent
            )
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario %s: %d checks, %d failed", args.scenario, len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "bandit_hold":
            results = await scenarios.bandit_hold(link, state.dialogue)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario %s: %d checks, %d failed", args.scenario, len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario in {"bandit_parley", "road_bandits"}:
            assert state.parley is not None and state.deals is not None and state.talk is not None
            scenario = {
                "road_bandits": scenarios.road_bandits, "bandit_parley": scenarios.bandit_parley,
            }[args.scenario]
            results = await scenario(
                link, state.dialogue, state.talk, state.deals, state.parley
            )
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario %s: %d checks, %d failed", args.scenario, len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "bandit_bounty":
            assert state.bounty is not None and state.deals is not None and state.talk is not None
            assert state.guide is not None
            results = await scenarios.bandit_bounty(
                link, state.dialogue, state.talk, state.deals, state.bounty, state.guide
            )
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario %s: %d checks, %d failed", args.scenario, len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "neural-still":
            results = await scenarios.neural_still_scenario(
                link, bodypilot, Path(args.memory_db).parent
            )
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario %s: %d checks, %d failed", args.scenario, len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "neural-talk":
            results = await scenarios.neural_talk_scenario(link, state.dialogue, bodypilot)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario %s: %d checks, %d failed", args.scenario, len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario in {"neural", "neural-companion", "neural-group", "neural-load"}:
            scenario = {
                "neural": scenarios.neural_scenario,
                "neural-companion": scenarios.neural_companion_scenario,
                "neural-group": scenarios.neural_group_scenario,
                "neural-load": scenarios.neural_load_scenario,
            }[args.scenario]
            results = await scenario(link, bodypilot)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario %s: %d checks, %d failed", args.scenario, len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "m0":
            steps = await scenarios.m0_smoke(link, args.npc)
            failed = [s for s in steps if s.result.status in ("failed", "timeout")]
            log.info("scenario m0: %d steps, %d failed", len(steps), len(failed))
            exit_code = 1 if failed else 0
        elif args.scenario == "m2":
            assert state.dialogue is not None
            results = await scenarios.m2_actions(link, state.dialogue, args.npc)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario m2: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "m1":
            lines = await scenarios.m1_chat(link, state.dialogue, args.npc)
            log.info("scenario m1: line_delivery: done (%d lines)", len(lines))
        elif args.scenario == "websocket":
            assert state.dialogue
            results = await scenarios.websocket(link, state.dialogue)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario websocket: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "m4":
            results = await scenarios.m4_planner_bridge(link, args.npc, state.brain)
            expected_rejects = ("unknown_npc", "attached_soft_act")
            bad = {
                k: v
                for k, v in results.items()
                if not v.startswith("rejected" if k in expected_rejects else "done")
            }
            log.info("scenario m4: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "m5":
            assert state.dialogue is not None
            results = await scenarios.m5_turn_loop(link, state.dialogue, args.npc)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario m5: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "m5b":
            results = await scenarios.m5b_bridge(link, args.npc)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario m5b: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "m5c":
            assert state.dialogue is not None
            results = await scenarios.m5c_free_npc(link, state.dialogue, args.npc)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario m5c: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario in ("rescue", "rescue_self"):
            killer = "hero" if args.scenario == "rescue" else "npc"
            results = await scenarios.rescue(
                link, planner=state.brain, killer=killer, dialogue=state.dialogue
            )
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario %s: %d checks, %d failed", args.scenario, len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "m5e":
            assert state.dialogue is not None and state.talk is not None
            results = await scenarios.m5e_talk(link, state.dialogue, state.talk, state.voice, args.npc)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario m5e: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "combat_barks":
            assert state.dialogue is not None and state.combat_barks is not None
            results = await scenarios.combat_barks(link, state.dialogue, state.combat_barks)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario %s: %d checks, %d failed", args.scenario, len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario in {"mercy", "hero_yield", "humiliate", "surrender_live"}:
            assert state.dialogue is not None and state.mercy is not None and state.talk is not None
            scenario = getattr(scenarios, args.scenario)
            results = await scenario(link, state.dialogue, state.talk, state.mercy)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario %s: %d checks, %d failed", args.scenario, len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "svm_lines":
            assert state.dialogue is not None
            results = await scenarios.svm_lines(
                link, state.dialogue, log_dir=Path(args.memory_db).parent
            )
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario %s: %d checks, %d failed", args.scenario, len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "orders":
            assert state.dialogue is not None and state.talk is not None
            results = await scenarios.orders_scenario(link, state.dialogue, state.talk, args.npc)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario orders: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario.startswith("fight_reactions_"):
            from ..scenario_fight_reactions import fight_reactions

            assert state.dialogue is not None and state.talk is not None and state.incidents is not None
            results = await fight_reactions(
                link, state.dialogue, state.talk, state.incidents, state.brain,
                args.scenario.removeprefix("fight_reactions_"),
                str(Path(args.memory_db).parent),
                native_body=lambda npc: _native_combat_available(link, bodypilot, npc),
            )
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario %s: %d checks, %d failed", args.scenario, len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "wolf":
            assert state.talk is not None and state.beasts is not None
            results = await scenarios.wolf_scenario(link, state.talk, state.beasts)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario wolf: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "wolf_load":
            assert state.talk is not None and state.beasts is not None
            results = await scenarios.wolf_load_scenario(link, state.talk, state.beasts)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario wolf_load: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "check_walk":
            assert state.dialogue is not None
            results = await scenarios.check_walk(link, state.dialogue)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario check_walk: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "follow_back":
            assert state.dialogue is not None
            results = await scenarios.follow_back(link, state.dialogue)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario follow_back: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "companion":
            assert state.dialogue is not None and state.talk is not None and state.parley is not None
            results = await scenarios.companion_scenario(
                link, state.dialogue, state.talk, parley=state.parley, perception=state.perception
            )
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario companion: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "crimes":
            assert state.crimes is not None
            results = await crimes_scenario(link, state.crimes)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario crimes: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "crime_ally":
            assert state.dialogue is not None and state.talk is not None and state.crimes is not None
            results = await scenarios.crime_ally(
                link, state.dialogue, state.talk, state.crimes, perception=state.perception
            )
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario crime_ally: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "crime_react":
            assert state.crimes is not None and state.crime_react is not None
            results = await scenarios.crime_react(
                link, state.crimes, state.crime_react,
                hold_threats=state.brain is not None and not args.no_hold_threats,
            )
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario crime_react: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "crime_meet":
            assert state.crimes is not None and state.crime_react is not None
            assert state.crime_meet is not None and state.amends is not None
            results = await scenarios.crime_meet(
                link, state.crimes, state.crime_react, state.crime_meet, state.amends,
            )
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario crime_meet: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "live_npc":
            results = await scenarios.live_npc(link, state)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario live_npc: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "live_director":
            assert place_book is not None and args.trace_dir is not None
            results = await scenarios.live_director(
                link, state, place_book, journal, str(Path(args.trace_dir).parent),
            )
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario live_director: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "live_deals":
            assert state.dialogue is not None and state.talk is not None and state.deals is not None
            assert state.parley is not None and state.bounty is not None and state.guide is not None
            assert state.meetings is not None and state.crimes is not None
            assert state.crime_react is not None and state.crime_meet is not None
            assert state.amends is not None and state.rumors is not None
            assert state.people_knowledge is not None and args.trace_dir is not None
            results = await scenarios.live_deals(
                link, state.dialogue, state.talk, state.deals, state.parley, state.bounty,
                state.guide, state.meetings, state.crimes, state.crime_react, state.crime_meet,
                state.amends, state.rumors, state.people_knowledge, str(Path(args.trace_dir).parent),
            )
            bad = {key: value for key, value in results.items() if not value.startswith("done")}
            log.info("scenario live_deals: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "live_lies":
            assert state.dialogue is not None and state.talk is not None
            assert state.sightings is not None and state.witnesses is not None
            assert state.vanilla is not None and state.crimes is not None
            assert state.deals is not None and state.rumors is not None
            assert state.chests is not None and state.concerns is not None
            assert state.npc_will is not None and state.trust is not None and args.trace_dir is not None
            results = await scenarios.live_lies(
                link, state.dialogue, state.talk, state.sightings, state.witnesses,
                state.vanilla, state.crimes, state.deals, state.rumors, state.chests,
                state.concerns, state.npc_will, state.trust, str(Path(args.trace_dir).parent),
            )
            bad = {key: value for key, value in results.items() if not value.startswith("done")}
            log.info("scenario live_lies: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "crime_talk":
            assert state.crimes is not None and state.dialogue is not None and state.talk is not None
            assert (
                state.rumors is not None and state.witnesses is not None and state.sightings is not None
            )
            assert state.deals is not None and state.chests is not None and state.concerns is not None
            results = await crime_talk_scenario(
                link,
                state.crimes,
                state.dialogue,
                state.talk,
                state.rumors,
                state.witnesses,
                state.sightings,
                state.deals,
                state.chests,
                state.concerns,
            )
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario crime_talk: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "npc_deeds":
            assert state.npc_crimes is not None and state.people_knowledge is not None
            assert state.rumors is not None and state.dialogue is not None and args.trace_dir is not None
            results = await scenarios.npc_deeds(
                link, state.npc_crimes, state.people_knowledge, state.rumors,
                state.dialogue, str(Path(args.trace_dir).parent),
            )
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario npc_deeds: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "npc_director":
            assert state.npc_will is not None and state.showrunner is not None
            assert state.chests is not None and state.dialogue is not None and state.talk is not None
            assert state.witnesses is not None and args.trace_dir is not None
            results = await scenarios.npc_director(
                link, state.npc_will, state.showrunner, state.chests, state.dialogue,
                state.talk, state.witnesses, str(Path(args.trace_dir).parent),
            )
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario npc_director: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "npc_will":
            assert state.npc_will is not None and state.chests is not None
            assert state.dialogue is not None and args.trace_dir is not None
            results = await scenarios.npc_will(
                link, state.npc_will, state.chests, state.dialogue,
                str(Path(args.trace_dir).parent),
            )
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario npc_will: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "npc_far":
            assert state.npc_will is not None and state.chests is not None
            assert state.dialogue is not None and args.trace_dir is not None
            results = await scenarios.npc_far(
                link, state.npc_will, state.chests, state.dialogue,
                str(Path(args.trace_dir).parent),
            )
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario npc_far: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "body_found":
            assert state.sightings is not None and state.found_traces is not None
            assert state.crimes is not None
            results = await scenarios.body_found(link, state.sightings, state.found_traces, state.crimes)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario body_found: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "blood_found":
            assert state.sightings is not None and state.found_traces is not None
            assert state.crimes is not None
            results = await scenarios.blood_found(link, state.sightings, state.found_traces, state.crimes)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario blood_found: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "frame":
            assert state.npc_will is not None and state.chests is not None
            assert state.dialogue is not None and state.talk is not None
            assert state.witnesses is not None and state.trust is not None and args.trace_dir is not None
            results = await scenarios.frame(
                link, state.npc_will, state.chests, state.dialogue, state.talk,
                state.witnesses, state.trust, str(Path(args.trace_dir).parent),
            )
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario frame: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "cheat":
            assert state.crimes is not None and state.dialogue is not None and state.talk is not None
            assert state.deals is not None and state.rumors is not None
            assert state.people_knowledge is not None and args.trace_dir is not None
            results = await scenarios.cheat_scenario(
                link, state.crimes, state.dialogue, state.talk, state.deals,
                state.rumors, state.people_knowledge, str(Path(args.trace_dir).parent),
            )
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario cheat: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario in {"cover", "cover_subject"}:
            assert state.crimes is not None and state.dialogue is not None and state.talk is not None
            assert (
                state.rumors is not None and state.witnesses is not None and state.sightings is not None
            )
            assert state.deals is not None and state.trust is not None and args.trace_dir is not None
            run_cover = scenarios.cover_scenario
            cover_options: dict[str, Any] = {}
            if args.scenario == "cover_subject":
                assert state.people_knowledge is not None
                run_cover = scenarios.cover_subject_scenario
                cover_options["people"] = state.people_knowledge
            results = await run_cover(
                link,
                state.crimes,
                state.dialogue,
                state.talk,
                state.deals,
                state.rumors,
                state.witnesses,
                state.sightings,
                state.trust,
                str(Path(args.trace_dir).parent),
                **cover_options,
            )
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario %s: %d checks, %d failed", args.scenario, len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "trace_crime":
            assert state.crimes is not None and state.dialogue is not None and state.talk is not None
            assert (
                state.rumors is not None and state.witnesses is not None and state.sightings is not None
            )
            assert state.trust is not None and args.trace_dir is not None
            results = await scenarios.trace_crime(
                link,
                state.crimes,
                state.dialogue,
                state.talk,
                state.rumors,
                state.witnesses,
                state.sightings,
                state.trust,
                str(Path(args.trace_dir).parent),
            )
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario trace_crime: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "crime_director":
            assert state.crimes is not None and state.showrunner is not None
            assert state.dialogue is not None and state.talk is not None and state.rumors is not None
            assert state.sightings is not None and state.deals is not None and state.chests is not None
            assert state.concerns is not None and state.amends is not None and state.bodies is not None
            assert state.guard is not None and state.perception is not None
            results = await crime_director_scenario(
                link,
                state.crimes,
                state.showrunner,
                state.dialogue,
                state.talk,
                state.rumors,
                state.sightings,
                state.deals,
                state.chests,
                state.concerns,
                state.amends,
                guard=state.guard,
                bodies=state.bodies,
                perception=state.perception,
                initiative=state.initiative,
            )
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario crime_director: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "crime_law":
            assert state.crimes is not None and state.crime_law is not None
            assert state.dialogue is not None and state.talk is not None
            assert state.rumors is not None and state.deals is not None
            results = await crime_law_scenario(
                link,
                state.crimes,
                state.crime_law,
                state.dialogue,
                state.talk,
                state.rumors,
                state.deals,
            )
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario crime_law: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "quest_law":
            assert state.crimes is not None and state.crime_law is not None and state.book is not None
            assert state.dialogue is not None and state.rumors is not None
            results = await scenarios.quest_law(
                link,
                state.crimes,
                state.crime_law,
                state.book,
                state.dialogue,
                state.rumors,
            )
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario quest_law: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "ground":
            assert state.dialogue is not None and state.talk is not None and state.perception is not None
            results = await scenarios.ground_scenario(link, state.dialogue, state.talk, state.perception)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario ground: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario in ("chests", "chests_lobart"):
            assert state.dialogue is not None and state.talk is not None
            lobart = {"npc": "BAU_950_LOBART", "name": "Лобарт", "item": "ITAR_BAU_L"}
            lobart["ask"] = "Лобарт, продай мне крестьянскую одежду."
            more = lobart if args.scenario == "chests_lobart" else {}
            results = await scenarios.chests_scenario(
                link, state.dialogue, state.talk, state.chests, **more
            )
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario chests: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "knows":
            assert state.dialogue is not None and state.talk is not None and state.whereabouts is not None
            assert state.knowledge is not None and state.lore is not None
            results = await scenarios.knows_scenario(
                link, state.dialogue, state.talk, state.knowledge, state.lore, state.whereabouts
            )
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario knows: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "lead":
            assert state.dialogue is not None and state.talk is not None and state.deals is not None
            assert state.guide is not None
            results = await scenarios.lead_scenario(
                link, state.dialogue, state.talk, state.deals, state.guide
            )
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario lead: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario in ("meet", "meet_missed"):
            assert state.dialogue is not None and state.talk is not None and state.deals is not None
            assert state.meetings is not None
            results = await scenarios.meet_scenario(
                link,
                state.dialogue,
                state.talk,
                state.deals,
                state.meetings,
                missed=args.scenario == "meet_missed",
            )
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario %s: %d checks, %d failed", args.scenario, len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario in ("errand", "errand_fork", "errand_far"):
            assert state.dialogue is not None and state.talk is not None and state.errands is not None
            assert state.rumors is not None
            results = await scenarios.errand_scenario(
                link,
                state.dialogue,
                state.talk,
                state.errands,
                state.rumors,
                state.gossip,
                variant={"errand": "near", "errand_fork": "fork", "errand_far": "far"}[args.scenario],
            )
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario %s: %d checks, %d failed", args.scenario, len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "errand_give":
            assert state.dialogue is not None and state.talk is not None and state.errands is not None
            results = await scenarios.errand_give_scenario(
                link, state.dialogue, state.talk, state.errands
            )
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario %s: %d checks, %d failed", args.scenario, len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "cave":
            assert state.dialogue is not None and state.talk is not None and state.whereabouts is not None
            results = await scenarios.cave_scenario(link, state.dialogue, state.talk, state.whereabouts)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario cave: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "hearing":
            assert state.talk is not None
            results = await scenarios.hearing_scenario(link, state.talk)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario hearing: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "group":
            assert state.dialogue is not None and state.talk is not None
            results = await scenarios.group_scenario(link, state.dialogue, state.talk)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario group: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "interject":
            assert state.dialogue is not None and state.talk is not None
            results = await scenarios.interject_scenario(
                link, state.dialogue, state.talk, state.interject
            )
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario interject: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "remembers":
            assert state.dialogue is not None and state.talk is not None
            assert state.sightings is not None and state.perception is not None
            results = await scenarios.remembers_scenario(
                link, state.dialogue, state.talk, state.sightings, state.perception
            )
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario remembers: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "calm":
            assert state.dialogue is not None and state.talk is not None and state.parley is not None
            results = await scenarios.calm_scenario(
                link, state.dialogue, state.talk, parley=state.parley
            )
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario calm: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "m6":
            dialogue, talk, knowledge = state.dialogue, state.talk, state.knowledge
            assert dialogue is not None and talk is not None and knowledge is not None
            results = await scenarios.m6_knows(link, dialogue, talk, knowledge, args.npc)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario m6: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "see":
            dialogue, talk, perception, rumors = (
                state.dialogue,
                state.talk,
                state.perception,
                state.rumors,
            )
            assert (
                dialogue is not None
                and talk is not None
                and perception is not None
                and rumors is not None
            )
            results = await scenarios.see_perception(link, dialogue, talk, perception, rumors, args.npc)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario see: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "seen":
            dialogue, talk, sightings = state.dialogue, state.talk, state.sightings
            assert dialogue is not None and talk is not None and sightings is not None
            results = await scenarios.seen_memory(link, dialogue, talk, sightings)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario seen: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario in ("trace", "broken"):
            dialogue, talk, sightings = state.dialogue, state.talk, state.sightings
            assert dialogue is not None and talk is not None and sightings is not None
            assert args.trace_dir is not None
            from ..scenarios.broken import broken_memory

            runner = broken_memory if args.scenario == "broken" else scenarios.trace_memory
            results = await runner(link, dialogue, talk, sightings, str(Path(args.trace_dir).parent))
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario %s: %d checks, %d failed", args.scenario, len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "witness":
            dialogue, talk, sightings = state.dialogue, state.talk, state.sightings
            assert dialogue is not None and talk is not None and sightings is not None
            assert state.witnesses is not None
            results = await scenarios.witness_check(link, dialogue, talk, sightings, state.witnesses)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario witness: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "places":
            # not a check: the world's points and people for the book of places (places.py)
            await link.wait_world()
            where = Path(args.trace_dir).parent if args.trace_dir else Path(".")
            data = await places.dump(link, where / "world_points.json")
            log.info(
                "places: %d points, %d people -> %s", len(data["points"]), len(data["people"]), where
            )
            exit_code = 0
        elif args.scenario == "wrath":
            dialogue, talk, wrath = state.dialogue, state.talk, state.wrath
            assert dialogue is not None and talk is not None and wrath is not None
            results = await scenarios.xardas_wrath(link, dialogue, talk, wrath, args.npc)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info(
                "scenario wrath: %d checks, %d failed",
                len(results),
                len(bad),
            )
            exit_code = 1 if bad else 0
        elif args.scenario == "lie":
            dialogue, talk, quests = state.dialogue, state.talk, state.quests
            assert dialogue is not None and talk is not None and quests is not None
            results = await scenarios.lie_found_out(
                link, dialogue, talk, quests, args.npc, chronicler=state.feats,
            )
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario lie: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "death_load":
            assert state.director is not None
            results = await scenarios.death_load(link, state.director)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario death_load: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "feats":
            assert state.feats is not None
            results = await scenarios.feats(link, state.feats, journal, state.dialogue)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario feats: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "feat_fame":
            assert state.dialogue is not None and state.talk is not None and state.rumors is not None
            results = await scenarios.feat_fame(link, state.dialogue, state.talk, state.rumors)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario feat_fame: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "repair":
            assert state.director is not None and state.story is not None
            results = await scenarios.repair_after_death(link, state.director, state.story)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario repair: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "authored":
            assert state.author is not None and state.story is not None
            results = await scenarios.authored_quest(link, state.author, state.story)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario authored: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "occasions":
            assert state.occasions is not None
            results = await scenario_occasions.occasions(link, state.occasions)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario occasions: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "questlog":
            assert state.questlog is not None and state.occasions is not None and state.story is not None
            results = await scenario_questlog.questlog(link, state.story, state.questlog, state.occasions)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario questlog: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "showrunner":
            assert state.showrunner is not None
            results = await scenario_showrunner.showrunner(link, state.showrunner, state.dialogue)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario showrunner: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "showrunner_live":
            assert state.showrunner is not None
            results = await scenario_showrunner.showrunner_live(link, state.showrunner, trace)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario showrunner_live: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario in {"vanilla_line", "vanilla_line_live"}:
            assert state.showrunner is not None and state.questlog is not None
            if args.scenario == "vanilla_line_live":
                results = await scenario_vanilla_line.vanilla_line_live(
                    link, state.showrunner, state.questlog, journal, trace
                )
            else:
                results = await scenario_vanilla_line.vanilla_line(
                    link, state.showrunner, state.questlog, state.dialogue
                )
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario %s: %d checks, %d failed", args.scenario, len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "character":
            casting, author, story = state.casting, state.author, state.story
            assert casting is not None and author is not None and story is not None and place_book
            results = await scenarios.director_casts(link, casting, author, story, place_book)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario character: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "person":
            assert state.cast is not None and state.dialogue is not None and state.rumors is not None
            results = await scenarios.cast_person(link, state.cast, state.dialogue, state.rumors)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario person: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "cast":
            assert state.cast is not None
            results = await scenarios.cast_character(link, state.cast)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario cast: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "chain":
            assert state.chains is not None and place_book is not None
            results = await scenarios.author_chain(link, state.chains, place_book)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario chain: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "stages":
            assert state.book is not None and place_book is not None
            results = await scenarios.book_stages(link, state.book, place_book)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario stages: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "own_amend":
            assert state.book is not None and place_book is not None and state.dialogue and state.talk
            results = await scenarios.own_amend(link, state.book, place_book, state.dialogue, state.talk)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario own_amend: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "requires":
            assert state.book is not None
            results = await scenarios.book_requires(link, state.book)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario requires: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "own_repair":
            assert state.book is not None and state.director is not None and place_book is not None
            results = await scenarios.own_repair(link, state.book, state.director, place_book)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario own_repair: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "trigger":
            assert state.story is not None
            results = await scenarios.story_triggers(link, state.story)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario trigger: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "own":
            results = await scenarios.own_symbols(link)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario own: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "gate":
            assert state.dialogue and state.talk and state.deals and state.gates
            results = await scenarios.gate_scenario(
                link, state.dialogue, state.talk, state.deals, state.gates
            )
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario gate: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "gate_vouch":
            assert state.dialogue and state.talk and state.gates
            results = await scenarios.gate_vouch_scenario(
                link, state.dialogue, state.talk, state.gates
            )
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario gate_vouch: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "vgive":
            dialogue, talk, vanilla = state.dialogue, state.talk, state.vanilla
            assert dialogue is not None and talk is not None and vanilla is not None
            results = await scenarios.vanilla_given(link, dialogue, talk, vanilla)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario vgive: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "vspare":
            dialogue, talk, vanilla = state.dialogue, state.talk, state.vanilla
            assert dialogue is not None and talk is not None and vanilla is not None
            results = await scenarios.vanilla_spare(link, dialogue, talk, vanilla)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario vspare: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "greg":
            dialogue, talk, vanilla = state.dialogue, state.talk, state.vanilla
            assert dialogue is not None and talk is not None and vanilla is not None
            results = await scenarios.greg(link, dialogue, talk, vanilla)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario greg: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario in ("amends", "amends_deal"):
            dialogue, talk, amends = state.dialogue, state.talk, state.amends
            assert dialogue is not None and talk is not None and amends is not None
            deals = state.deals if args.scenario == "amends_deal" else None
            results = await scenarios.amends_storylet(link, dialogue, talk, amends, deals=deals)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario %s: %d checks, %d failed", args.scenario, len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "deal":
            dialogue, talk, deals = state.dialogue, state.talk, state.deals
            assert dialogue is not None and talk is not None and deals is not None
            results = await scenarios.deal_window(link, dialogue, talk, deals)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario deal: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "contract":
            dialogue, talk, deals = state.dialogue, state.talk, state.deals
            assert dialogue is not None and talk is not None and deals is not None
            results = await scenarios.contract(link, dialogue, talk, deals)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario contract: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "haggle":
            dialogue, talk, deals = state.dialogue, state.talk, state.deals
            assert dialogue is not None and talk is not None and deals is not None
            results = await scenarios.haggle(link, dialogue, talk, deals)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario haggle: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "debt":
            dialogue, talk, deals = state.dialogue, state.talk, state.deals
            assert dialogue is not None and talk is not None and deals is not None
            results = await scenarios.deal_debt(link, dialogue, talk, deals)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario debt: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "xardas_money":
            dialogue, talk, deals = state.dialogue, state.talk, state.deals
            assert dialogue is not None and talk is not None and deals is not None
            results = await scenarios.xardas_money(link, dialogue, talk, deals)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario xardas_money: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "initiative":
            dialogue, talk, deals, concerns = state.dialogue, state.talk, state.deals, state.concerns
            guard, bodies, perception = state.guard, state.bodies, state.perception
            assert dialogue is not None and talk is not None and deals is not None
            assert concerns is not None and guard is not None and bodies is not None
            assert perception is not None
            # with DECIDER_URL the server's planner and its desk; else the scenario's fixed stand-in
            results = await scenarios.initiative_scenario(
                link, dialogue, talk, deals, concerns, guard, bodies, perception, desk=state.initiative
            )
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario initiative: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "deal_demo":
            assert state.deals is not None
            results = await scenarios.deal_demo(link, state.deals, args.npc)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario deal_demo: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario in ("face_cues_demo", "face_eyes_demo", "face_brows_demo"):
            assert state.dialogue is not None
            demo = {
                "face_cues_demo": scenarios.face_cues_demo,
                "face_eyes_demo": scenarios.face_eyes_demo,
                "face_brows_demo": scenarios.face_brows_demo,
            }[args.scenario]
            results = await demo(link, state.dialogue)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario %s: %d checks, %d failed", args.scenario, len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario in ("gossip", "gossip_demo"):
            dialogue, talk, rumors = state.dialogue, state.talk, state.rumors
            gossip, bodies, perception = getattr(state, "gossip", None), state.bodies, state.perception
            assert dialogue is not None and talk is not None and rumors is not None
            assert gossip is not None and bodies is not None and perception is not None
            if args.scenario == "gossip":
                where = str(Path(args.trace_dir).parent) if args.trace_dir else None
                results = await scenarios.gossip_scenario(
                    link, dialogue, talk, rumors, gossip, bodies, perception, log_dir=where
                )
            else:
                results = await scenarios.gossip_demo(link, dialogue, talk, rumors, gossip, bodies)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario %s: %d checks, %d failed", args.scenario, len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "day":
            dialogue, talk, rumors, gossip = state.dialogue, state.talk, state.rumors, state.gossip
            bodies, perception, day = state.bodies, state.perception, state.day
            assert dialogue is not None and talk is not None and rumors is not None
            assert gossip is not None and bodies is not None and perception is not None
            assert day is not None
            where = str(Path(args.trace_dir).parent) if args.trace_dir else None
            # with DECIDER_URL the server's planner (day settings for the run); else the scenario's own
            results = await scenarios.day_scenario(
                link, dialogue, talk, rumors, gossip, bodies, perception, day, state.brain, log_dir=where
            )
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario day: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "noise":
            assert state.sightings is not None
            results = await scenarios.noise_scenario(link, state.sightings, state.brain)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario noise: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "rumor":
            dialogue, talk, rumors = state.dialogue, state.talk, state.rumors
            assert dialogue is not None and talk is not None and rumors is not None
            where = str(Path(args.trace_dir).parent) if args.trace_dir else None
            results = await scenarios.rumor_spread(link, dialogue, talk, rumors, log_dir=where)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario rumor: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "vcheck":
            dialogue, talk, vanilla = state.dialogue, state.talk, state.vanilla
            assert dialogue is not None and talk is not None and vanilla is not None
            results = await scenarios.vanilla_check(link, dialogue, talk, vanilla)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario vcheck: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "vlie":
            dialogue, talk, vanilla = state.dialogue, state.talk, state.vanilla
            assert dialogue is not None and talk is not None and vanilla is not None
            results = await scenarios.vanilla_lie(link, dialogue, talk, vanilla)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario vlie: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "graph":
            assert state.story is not None
            results = await scenarios.graph_dialogs(link, state.story, args.npc)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario graph: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "programs":
            assert state.story is not None
            results = await scenarios.graph_programs(link, state.story, args.npc, dialogue=state.dialogue)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario programs: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "tts":
            results = await scenarios.tts_lines(link, state.dialogue, args.npc)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario tts: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
        elif args.scenario == "m3":
            results = await scenarios.m3_memory_voice(link, state.dialogue, state.voice, args.npc)
            bad = {k: v for k, v in results.items() if not v.startswith("done")}
            log.info("scenario m3: %d checks, %d failed", len(results), len(bad))
            exit_code = 1 if bad else 0
    except Exception:
        log.exception("scenario %s failed", args.scenario)
        exit_code = 1
    finally:
        if isinstance(llm, OffBackend):
            for reason in llm.used:
                log.info("scenario %s: given: %s", args.scenario, reason)
            unused = llm.unused()
            checks = {
                "model_off": "failed " + repr(llm.attempts[:5]) if llm.attempts else "done",
                "givens_used": "failed " + repr(unused) if unused else "done",
            }
            for name, result in checks.items():
                log.info("scenario %s: %s: %s", args.scenario, name, result)
            if llm.attempts or unused:
                exit_code = 1
    if args.exit_after:
        done.set()
    return exit_code
