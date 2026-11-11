"""Build and close a game session and its subsystems."""

from __future__ import annotations

import argparse
import asyncio
import logging
import os
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path
from tempfile import TemporaryDirectory
from typing import Any

from . import lore, places
from .amends import Amends
from .authoring import Author
from .beasts import BeastDesk
from .blame import Blame
from .bodies import Bodies
from .bodypilot import BodyPilot
from .bounty import Bounty
from .brain import DeciderClient, GameApplier, NpcPlanner, PlannerConfig
from .brain.reflex import Reflexes
from .brain.situation import SituationBuilder
from .budget import Budget
from .cast import Cast
from .casting import Casting
from .chains import ChainAuthor
from .challenges import Challenges
from .characters import Cards, Traits
from .chests import ChestDesk
from .chronicle import Chronicle
from .combat_barks import CombatBarks
from .concerns import Concerns
from .consequences import LieConsequences
from .contracts import symbol_by_name
from .crime_law_graph import init_ops as crime_law_ops
from .crime_law_runtime import CrimeLawRuntime
from .crime_meet import CrimeMeet
from .crime_memory import CrimeMemoryRuntime
from .crime_react import CrimeReact
from .crimes import Crimes
from .daylife import DayDesk
from .deals import Deals
from .dialogue import DialogueManager
from .director import Director
from .errands import Errands
from .executor import Intent
from .fatecard import FateCards
from .fates import Fates
from .feat_art import DEFAULT_ROOT, Artist
from .feats import Feats
from .found_traces import FoundTraces
from .game import GameLink
from .gamedata import storygraph
from .gates import Gates
from .gossip import GossipDesk
from .group import GroupTalk
from .guide import Guide
from .incidents import IncidentDesk
from .initiative import ImportantApproach, InitiativeDesk
from .interject import Interjections
from .journal import Journal
from .knowledge import Knowledge
from .llm import LLMBackend
from .meetings import Meetings
from .memory import MemoryStore
from .mercy import Mercy
from .npc_crime_law import NpcCrimeLaw
from .npc_crimes import NpcCrimes
from .npc_will import NpcWill
from .occasions import Occasions
from .orders import Orders
from .parley import Parley
from .people_knowledge import PeopleKnowledge
from .perception import PerceptionDesk
from .persona import PersonaProvider, habits_of
from .protocol import (
    Attached,
    Detached,
    GameMessage,
    Perception,
    PlayerText,
    Saved,
    Session,
    ThreatControlCmd,
)
from .questbook import QuestBook
from .questlog import QuestLog
from .quests import QuestBoard
from .scenarios.dispatch import ScenarioContext, run_scenario
from .scenarios.feats import ScenarioArtist
from .showrunner import Showrunner
from .sightings import Sightings
from .social import Rumors
from .story import StoryPatches, freed
from .storyguard import StoryGuard
from .talk import TalkDesk, fights
from .trace import TraceWriter
from .triggers import Triggers
from .trust import Trust
from .tts import TtsClient
from .vanilla import VanillaQuests
from .voice import AsrClient, AudioSource, VoiceInput
from .whereabouts import Whereabouts
from .witnesses import Witnesses
from .wrath import Wrath

log = logging.getLogger("gothic_ai")


class ServerState:
    def __init__(self) -> None:
        self.dialogue: DialogueManager | None = None
        self.combat_barks: CombatBarks | None = None
        self.mercy: Mercy | None = None
        self.voice: VoiceInput | None = None
        self.brain: NpcPlanner | None = None
        self.incidents: IncidentDesk | None = None
        self.talk: TalkDesk | None = None
        self.story: StoryPatches | None = None
        self.chronicle: Chronicle | None = None
        self.crimes: Crimes | None = None
        self.npc_crimes: NpcCrimes | None = None
        self.blame: Blame | None = None
        self.npc_will: NpcWill | None = None
        self.npc_crime_law: NpcCrimeLaw | None = None
        self.crime_law: CrimeLawRuntime | None = None
        self.crime_memory: CrimeMemoryRuntime | None = None
        self.crime_react: CrimeReact | None = None
        self.crime_meet: CrimeMeet | None = None
        self.quests: QuestBoard | None = None
        self.director: Director | None = None
        self.occasions: Occasions | None = None
        self.questlog: QuestLog | None = None
        self.showrunner: Showrunner | None = None
        self.feats: Feats | None = None
        self.fates: Fates | None = None
        self.fate_cards: FateCards | None = None
        self.feat_challenges: Challenges | None = None
        self.vanilla: VanillaQuests | None = None
        self.gates: Gates | None = None
        self.trust: Trust | None = None
        self.people_knowledge: PeopleKnowledge | None = None
        self.rumors: Rumors | None = None
        self.amends: Amends | None = None
        self.author: Author | None = None
        self.chains: ChainAuthor | None = None
        self.cast: Cast | None = None
        self.casting: Casting | None = None
        self.triggers: Triggers | None = None
        self.book: QuestBook | None = None
        self.consequences: LieConsequences | None = None
        self.knowledge: Knowledge | None = None
        self.deals: Deals | None = None
        self.parley: Parley | None = None
        self.bounty: Bounty | None = None
        self.chests: ChestDesk | None = None
        self.interject: Interjections | None = None
        self.bodies: Bodies | None = None
        self.concerns: Concerns | None = None
        self.guard: StoryGuard | None = None
        self.initiative: InitiativeDesk | None = None
        self.gossip: GossipDesk | None = None
        self.day: DayDesk | None = None
        self.perception: PerceptionDesk | None = None
        self.sightings: Sightings | None = None
        self.found_traces: FoundTraces | None = None
        self.whereabouts: Whereabouts | None = None
        self.lore: lore.Lore | None = None
        self.guide: Guide | None = None
        self.meetings: Meetings | None = None
        self.errands: Errands | None = None
        self.witnesses: Witnesses | None = None
        self.wrath: Wrath | None = None
        self.beasts: BeastDesk | None = None


@dataclass
class SessionEnv:
    args: argparse.Namespace
    state: ServerState
    llm: LLMBackend
    memory: MemoryStore
    asr: AsrClient | None
    tts: TtsClient | None
    decider: DeciderClient | None
    quick: LLMBackend | None
    graph: storygraph.StoryGraph | None
    place_book: places.PlaceBook | None
    journal: Journal
    trace: TraceWriter | None
    done: asyncio.Event
    personas: PersonaProvider
    budget: Budget
    tone_quick: LLMBackend | None
    source: AudioSource | None
    set_exit_code: Callable[[int], None]


def print_event(msg: GameMessage) -> None:
    match msg:
        case Attached():
            log.info("attached %s (%s)", msg.npc, msg.name)
        case Detached():
            log.info("detached %s: %s", msg.npc, msg.reason)
        case Perception():
            log.debug("perception %s: %s from %s at %.0fcm", msg.npc, msg.perc, msg.other, msg.distance)
        case PlayerText():
            log.info("player typed (focus %s): %s", msg.focus or "-", msg.text)
        case Saved():
            log.info("game saved at memory %s@%d", msg.branch, msg.seq)
        case Session():
            pass  # logged by the dialogue manager


def _start_showrunner(args: argparse.Namespace, showrunner: Showrunner) -> asyncio.Task[None] | None:
    """Ordinary play starts the loop; scenarios control their own model calls."""
    if args.no_showrunner or args.scenario is not None:
        return None
    return asyncio.create_task(showrunner.run())


def _start_bodypilot(args: argparse.Namespace, link: GameLink, talk: TalkDesk) -> BodyPilot | None:
    policy = getattr(args, "body_policy", "engine")
    if not args.neural_body or (policy == "service" and not args.body_url):
        return None
    return BodyPilot(
        link,
        args.body_url,
        body_policy=policy,
        max_fights=args.neural_body_max,
        companion=talk.orders.companion,
        unavailable=lambda npc: npc.upper() in talk.attached,
        talking=lambda npc: talk.partner is not None and talk.partner.npc == npc.upper(),
    )


def _perception_control(args: argparse.Namespace, *, planner: bool) -> ThreatControlCmd:
    """Deeds have rules even when no behavioral model is configured."""
    return ThreatControlCmd(
        hold=planner and not args.no_hold_threats,
        deeds=True if not args.no_crimes else None,
        bandits=not args.no_parley,
    )


def _start_crime_meet(state: ServerState, link: GameLink, cards: Any) -> asyncio.Task[None] | None:
    """The crime switch owns encounters too, independently of the behavioral planner."""
    if state.crimes is None:
        return None
    assert state.crime_react is not None and state.crime_memory is not None
    assert state.bodies is not None and state.talk is not None and state.chronicle is not None
    state.crime_meet = CrimeMeet(
        link, state.crimes, state.crime_react, state.crime_memory,
        amends=state.amends,
        approach=ImportantApproach(link, state.bodies, state.talk, state.guard, state.chronicle),
        cards=cards,
    )
    state.talk.refuses = state.crime_meet.refuses
    state.talk.refusal = state.crime_meet.refusal
    return asyncio.create_task(state.crime_meet.run())


async def _native_combat_available(link: GameLink, pilot: BodyPilot | None, npc: str) -> bool:
    """Let acquisition finish before sending native combat that could interrupt it. A refused acquisition
    leaves the native fallback available; an accepted fight stays with its combat provider."""
    npc = npc.upper()
    while True:
        if link.body_owned(npc):
            return False
        if pilot is None:
            return True
        entry = pilot.people.get(npc)
        if (entry is not None and entry.activity == "body") or npc in pilot._claimed_fights:
            return False
        if not pilot.handles(npc):
            return True
        # The dispatcher registers a fight before any acquire request or claim. Checking only the
        # lease or claimed set would let a native command reset that fight during its initial window.
        await asyncio.sleep(0.05)


async def run_session(env: SessionEnv, link: GameLink) -> None:
    args = env.args
    state = env.state
    llm = env.llm
    memory = env.memory
    asr = env.asr
    tts = env.tts
    decider = env.decider
    quick = env.quick
    graph = env.graph
    place_book = env.place_book
    journal = env.journal
    trace = env.trace
    done = env.done
    personas = env.personas
    budget = env.budget
    tone_quick = env.tone_quick
    source = env.source
    link.listeners.append(print_event)
    state.dialogue = DialogueManager(
        link,
        llm,
        personas,
        memory=memory,
        tts=tts,
        journal=journal,
        trace=trace,
        budget=budget,
        quick=quick,
        svm_lines=not args.no_svm_lines,
    )
    state.dialogue.debug_leak = getattr(args, "debug_leak", None)
    if not args.no_combat_barks or not args.no_mercy:
        state.combat_barks = CombatBarks(state.dialogue, hits=not args.no_combat_barks)
    # the bandits below Xardas's tower are freed of their scripted ambush: our models decide (story.py)
    static = freed(graph.nodes.__contains__) if graph is not None else freed()
    static = static if args.free_bandits else []
    if graph is not None and not args.no_crimes:
        static += crime_law_ops(graph)
    state.story = StoryPatches(link, memory, static=static)
    # npc's we control: the body's instant reactions (behavioral model) and words for incidents
    reflexes = Reflexes(decider, apply_threats=args.apply_threats) if decider is not None else None
    state.incidents = IncidentDesk(link, state.dialogue, reflexes, budget, trace)
    # the hero talks to the world: whom he speaks to, the conversation, the body meanwhile (M5e)
    state.talk = TalkDesk(
        link, state.dialogue, reflexes, journal=journal, trace=trace,
        known_names={npc: card.name for npc, card in personas.cards.items()},
        name_fix=not args.no_name_fix,
    )
    # orders to the hero's companions ("Стой!", "За мной!"): carried out at once, no model call
    state.talk.orders = Orders(link, state.talk, state.dialogue, journal=journal, places=place_book)
    state.dialogue.extensions.append(state.talk.orders)  # places around a companion, go_to
    # talk with several npc's: who takes part, and whose words the others heard (group.py)
    group = GroupTalk(link, state.dialogue, state.talk)
    state.dialogue.extensions.append(group)
    state.talk.lead = lambda npc: card.lead if (card := personas.cards.get(npc)) is not None else 0
    if reflexes is not None and args.interject:
        # who speaks up after the one the hero spoke to, by character (step B3, on by default)
        interject = Interjections(
            state.dialogue, state.talk, group, reflexes, budget, trace=trace, journal=journal
        )
        interject.view_of = lambda npc: state.talk.view_of(npc)
        state.interject = interject
        state.incidents.speaks_up_there = interject.covers
    state.incidents.companion = state.talk.orders.companion
    if state.combat_barks is not None:
        state.combat_barks.companion = state.talk.orders.companion
    if args.beasts or args.scenario in {"wolf", "wolf_load"}:
        # wolves hear the tone of his words: they let him pass, run or bite (docs/beasts.md)
        state.beasts = BeastDesk(link, quick=tone_quick, journal=journal, trace=trace)
        state.talk.beasts = state.beasts
    state.incidents.fighting_ru = state.talk.orders.fighting_ru
    state.incidents.foe = state.talk.orders.foe
    talk_desk = state.talk  # its roster is a new dict each time

    def guild_of(npc: str) -> int | None:
        e = talk_desk.people.get(npc.upper())
        return e.guild if e is not None else None

    state.dialogue.guild_of = guild_of
    orders = state.talk.orders
    # one in a fight near the hero that is not his companion fights him: talked to, it may call it off
    state.dialogue.fights_hero = lambda npc: (
        fights(talk_desk.people.get(npc.upper())) and not orders.companion(npc)
    )
    state.dialogue.on_calm.append(orders.truce)
    if asr is not None and source is not None:
        state.voice = VoiceInput(
            link,
            state.talk.hear,
            source,
            asr,
            journal=journal,
            files=args.voice_files,
            hints=state.talk.asr_hints,
        )
    if graph is not None and not args.no_knowledge:
        # what each npc knows of the world as far as the story has got: its dialogs, quests, talk (M6)
        state.knowledge = Knowledge(link, graph, state.story)
        state.dialogue.extensions.append(state.knowledge)

    def knows_hero(npc: str, dialogue: DialogueManager = state.dialogue) -> bool:
        """It has met the hero: it remembers talks with him, or he has heard its dialogs (knowledge)."""
        conv = dialogue.conversations.get(npc)
        if conv is not None and (conv.memory.entries or conv.memory.summary):
            return True
        if dialogue.branch is not None:
            mem = dialogue.memory.load(dialogue.branch, npc)
            if mem.entries or mem.summary:
                return True
        told = state.knowledge.views.get(npc) if state.knowledge is not None else None
        return told is not None and bool(told.told)

    # what each npc sees: names for those it knows, a look for strangers (wave 1 step 4)
    perception = state.perception = PerceptionDesk(
        link,
        social=lambda: state.rumors.social if state.rumors is not None else None,
        knows_hero=knows_hero,
        places=place_book,
    )
    state.dialogue.extensions.append(perception)
    state.talk.orders.view_of = perception.fresh  # a companion sent somewhere looks around there
    state.dialogue.sight = perception.sight
    state.talk.view_of = state.incidents.view_of = perception.english
    # what really happened, the director's quests and the hero's lies about them (story graph, stage 4)
    state.chronicle = Chronicle(link, memory)
    feat_image_root = None
    if not args.no_feats and (
        args.scenario in {None, "feats", "live_director"}
        or (args.scenario == "lie" and args.llm == "off")
        or (args.scenario == "vanilla_line_live" and os.environ.get("GOTHIC_FORCE_CHALLENGE") == "1")
    ):
        artist = None
        if not args.no_feat_images and args.scenario in {None, "feats", "live_director"}:
            if args.scenario in {"feats", "live_director"}:
                feat_image_root = TemporaryDirectory(prefix="feat-images-")
                artist = ScenarioArtist(Path(feat_image_root.name), codex="scenario")
            else:
                artist = Artist()
        state.feats = Feats(
            link, state.chronicle, llm, journal=journal, places=place_book,
            gather_s=1 if args.scenario in {"feats", "live_director"} else 20,
            artist=artist,
            fame=not args.no_feat_fame,
            hero_distance=lambda npc: (
                state.talk.people[npc].distance if npc in state.talk.people else None
            ),
            image_wait_s=30 if args.scenario in {"feats", "live_director"} else 150,
        )
        state.talk.on_phrase.append(lambda to, text: state.feats.hero_talk(to.id) if to else None)
    state.fates = Fates(
        link, state.chronicle, enabled=not args.no_feats,
        art_root=state.feats.artist.root if state.feats is not None
        and state.feats.artist is not None else DEFAULT_ROOT,
    )
    state.fate_cards = FateCards(
        link, state.chronicle, llm, enabled=state.feats is not None and not args.no_fate_card,
        art_root=state.fates.art_root,
    )
    if not args.no_crimes:
        state.crimes = Crimes(
            link,
            state.chronicle,
            graph=graph,
            dialogue=state.dialogue,
            places=place_book,
            names=lambda npc: personas.cards[npc].name if npc in personas.cards else npc,
            cover=not args.no_cover,
            mercy=not args.no_mercy,
        )
        state.dialogue.extensions.append(state.crimes)
        if graph is not None:
            state.crime_law = CrimeLawRuntime(link, state.crimes, graph, state.story)
        state.crime_memory = CrimeMemoryRuntime(link, state.crimes)
        state.crime_react = CrimeReact(
            link,
            state.crimes,
            state.crime_law.law if state.crime_law is not None else None,
            cards=personas.cards,
            project=state.crime_memory.project,
        )
    # the director's triggers fire in the game, with or without the server: the chronicle hears of them
    state.triggers = Triggers(link, state.chronicle)
    talk = state.talk
    # what each npc saw itself: where and how it saw the hero and the people it knows (wave 1 step 6)
    state.sightings = Sightings(
        link,
        memory,
        perception,
        state.chronicle,
        knows_hero=knows_hero,
        journal=journal,
        crimes=state.crimes,
    )
    if state.crimes is not None:
        state.crimes.sightings = state.sightings
    state.dialogue.extensions.append(state.sightings)
    # whom it saw lately is its intent's target too, and a companion goes where it saw him (step M2)
    perception.remembered = state.sightings.targets
    state.talk.orders.last_seen = state.sightings.where_seen
    # where a place or someone is, as the npc knows it: `where` (docs/npc-tools.md М3)
    # who and what is where, as each npc knows it from its dialogs (docs/npc-tools.md М4)
    state.lore = lore.Lore.load(place_book, state.knowledge, link=link, desk=perception)
    if state.lore is not None:
        state.talk.known_names = {**state.lore.names, **state.talk.known_names}
        state.dialogue.extensions.append(state.lore)
    log.info("lore: %s", f"{len(state.lore.facts)} npc's know" if state.lore else "no book of facts")
    state.whereabouts = Whereabouts(
        link, place_book, sightings=state.sightings, desk=perception, lore=state.lore
    )
    state.dialogue.extensions.append(state.whereabouts)
    # what each npc knows of the hero's word: its lies found out, rumors, claims that were true
    trust = state.trust = Trust(state.chronicle, crimes=state.crimes)
    state.people_knowledge = PeopleKnowledge(state.chronicle)
    state.dialogue.extensions.append(state.people_knowledge)
    state.dialogue.extensions.append(trust)
    state.bodies = Bodies(link)
    if not args.no_parley:
        state.parley = Parley(
            link, state.dialogue, state.chronicle,
            perception=perception, companion=orders.companion, places=place_book, lead=state.talk.lead,
            bodies=state.bodies, talk=state.talk, graph=graph, patches=state.story,
            hail=args.llm != "off" or args.scenario is None or args.scenario in {
                "bandit_parley", "bandit_bounty", "road_bandits", "companion",
            },
        )
        orders.parley_quiet = state.parley.quiet
        state.dialogue.extensions.append(state.parley)
    if not args.no_bounty:
        state.bounty = Bounty(link, state.dialogue, state.chronicle, perception=perception)
        state.dialogue.extensions.append(state.bounty)
    if not args.no_mercy:
        state.mercy = Mercy(
            state.dialogue, state.chronicle, orders, state.parley,
            law=state.crime_law.law if state.crime_law else None,
            barks=state.combat_barks, places=place_book,
        )
        state.dialogue.extensions.append(state.mercy)
        if state.combat_barks is not None:
            state.combat_barks.silenced = state.mercy.silent
    # deals the npc's offer the hero in conversation: a window, the player accepts (docs/npc-tools.md)
    if graph is not None and not args.no_gates:
        state.gates = Gates(link, state.dialogue, state.chronicle, graph, crimes=state.crimes)
    state.deals = Deals(
        link,
        state.dialogue,
        state.chronicle,
        journal=journal,
        names=lambda name: symbol_by_name(personas.cards, name),
        things=graph.things_of if graph is not None else None,
        crimes=state.crimes,
        cover=not args.no_cover and not args.no_crimes,
        cheat=not args.no_npc_cheat and not args.no_crimes,
        cards=personas.cards,
        gates=state.gates,
    )
    state.deals.roster = lambda: talk.people
    state.deals.parley = state.parley
    if state.parley is not None:
        state.parley.offer_pending = lambda npc: npc in state.deals.open
    state.deals.bounty = state.bounty
    state.deals.contracts.roster = lambda: talk.people
    state.dialogue.extensions.append(state.deals)
    # its tasks and their rewards, and what npc's keep in their stash (docs/contracts.md)
    state.dialogue.extensions.append(state.deals.contracts)
    if args.chests:
        # what lies in its chest, as it knows, and it goes to take it out (docs/npc-tools.md «Сундуки»)
        state.chests = ChestDesk(link, memory, places=place_book, journal=journal, crimes=state.crimes)
        state.dialogue.extensions.append(state.chests)
        state.deals.in_chests = state.chests.in_chests
    talk.on_talk_end.append(state.deals.end)
    if args.wrath or args.scenario == "wrath":
        # insult Xardas enough and he raises you as a skeleton (on by default, --no-wrath turns it off)
        state.wrath = Wrath(link, state.dialogue, journal=journal)
        state.dialogue.extensions.append(state.wrath)
    debts = asyncio.create_task(state.deals.run())  # a debt not paid back by its day is a broken word
    parley_clock = asyncio.create_task(state.parley.run()) if state.parley is not None else None
    # what each npc has on its mind about the hero, and what may come of it (wave 2 step 1)
    state.concerns = Concerns(link, state.chronicle, state.deals)
    if state.crimes is not None:
        state.concerns.extra.append(state.crimes.concerns)
    state.guard = StoryGuard(graph)
    state.npc_crimes = NpcCrimes(
        link, state.chronicle, crimes=state.crimes, cards=personas.cards, guard=state.guard,
        enabled=not args.no_crimes and not args.no_npc_crimes,
    )
    link.npc_crime_check = state.npc_crimes.check_command
    if not args.no_found_traces:
        # a body or blood first seen is a find: a stimulus, and suspicion by evidence (stage 8, 4б and 4в)
        assert state.sightings is not None
        state.found_traces = FoundTraces(
            state.sightings, crimes=state.crimes, npc_crimes=state.npc_crimes, journal=journal,
        )
    if state.crimes is not None:
        state.blame = Blame(
            link, state.chronicle, state.crimes, state.npc_crimes, state.dialogue, journal=journal,
        )
        state.dialogue.extensions.append(state.blame)
    if state.chests is not None:
        state.chests.npc_suspect = state.npc_crimes.suspect_chest
    if args.lead:
        # it leads the hero to a place, a term of a deal (docs/npc-tools.md, wave 2 step 4)
        state.guide = Guide(
            link,
            state.dialogue,
            talk,
            place_book,
            state.guard,
            state.chronicle,
            whereabouts=state.whereabouts,
            companion=state.talk.orders.companion,
            journal=journal,
        )
        state.deals.guide = state.guide
        state.dialogue.extensions.append(state.guide)
    meets: asyncio.Task[None] | None = None
    if args.meet and state.guide is not None:
        # a meeting with the hero, a term of a deal: it is there by the hour (docs/npc-tools.md, step 4)
        state.meetings = Meetings(
            link,
            state.dialogue,
            talk,
            state.guide,
            state.chronicle,
            state.bodies,
            budget=budget,
            journal=journal,
        )
        state.deals.meetings = state.meetings
        state.dialogue.extensions.append(state.meetings)
        state.concerns.extra.append(state.meetings.concerns)  # a meeting he did not come to
        meets = asyncio.create_task(state.meetings.run())  # the game's clock, while one is pending
    errand_clock: asyncio.Task[None] | None = None
    if args.errands and state.guide is not None:
        # an npc's goal: go and tell someone of the hero, or take it a thing (docs/npc-tools.md, step 5)
        state.errands = Errands(
            link,
            state.dialogue,
            talk,
            state.guide,
            state.chronicle,
            state.bodies,
            rumors=lambda: state.rumors,
            graph=graph,
            journal=journal,
        )
        state.dialogue.extensions.append(state.errands)
        if state.gates is not None:
            state.errands.gate_news = state.gates.news
            state.errands.gate_told = state.gates.told
        state.concerns.extra.append(state.errands.concerns)  # a thing it was to take and did not
        errand_clock = asyncio.create_task(state.errands.run())
    # the npc checks the hero's words by asking a witness it knows (wave 2 step 3)
    witnesses = state.witnesses = Witnesses(
        link,
        state.dialogue,
        state.sightings,
        perception,
        state.chronicle,
        state.guard,
        social=lambda: state.rumors.social if state.rumors is not None else None,
        budget=budget,
        hero_cm=lambda npc: talk.people[npc].distance if npc in talk.people else None,
        state_of=lambda npc: talk.people[npc].state if npc in talk.people else "",
        controlled=lambda npc: npc.upper() in talk.attached,
        partner=lambda: talk.partner.npc if talk.partner is not None else None,
        talking=lambda npc, on: talk.planner.talking(npc, on) if talk.planner is not None else None,
        journal=journal,
        cover=not args.no_cover and not args.no_crimes,
        cards=personas.cards,
        people_knowledge=state.people_knowledge,
        npc_crimes=not args.no_crimes and not args.no_npc_crimes,
    )
    # its lines right after what the npc saw itself
    if state.gates is not None:
        witnesses.gate_claim = state.gates.claim_heard
    state.dialogue.extensions.insert(state.dialogue.extensions.index(state.sightings) + 1, witnesses)
    group.asks = witnesses.asking
    group.witness = witnesses.called
    state.concerns.extra.append(witnesses.concerns)
    witnesses.on_change.append(state.concerns.changed)  # a far answer's time on its mind is over
    asks = asyncio.create_task(witnesses.run())  # far witnesses are asked at their meeting
    state.quests = QuestBoard(
        link,
        state.dialogue,
        state.chronicle,
        hero_distance=lambda npc: talk.people[npc].distance if npc in talk.people else None,
        journal=journal,
        trust=trust,
    )
    lies = asyncio.create_task(state.quests.run())
    directing: list[asyncio.Task[None]] = []
    if state.gates is not None:
        directing.append(asyncio.create_task(state.gates.run()))
    if state.crime_law is not None:
        directing.append(asyncio.create_task(state.crime_law.run()))
    if state.crime_memory is not None:
        directing.append(asyncio.create_task(state.crime_memory.run()))
    if state.crime_react is not None:
        directing.append(asyncio.create_task(state.crime_react.run()))
    if place_book is not None and not args.no_occasions:
        state.occasions = Occasions(
            link, state.chronicle, place_book, crimes=not args.no_crimes,
            npc_crimes=not args.no_crimes and not args.no_npc_crimes,
        )
        directing.append(asyncio.create_task(state.occasions.run()))
    if graph is not None and not args.no_questlog:
        state.questlog = QuestLog(link, graph, state.chronicle)
        directing.append(asyncio.create_task(state.questlog.run()))
    if graph is not None and not args.no_director:
        # quests the hero can no longer finish get repaired (story graph, stage 5)
        state.director = Director(
            link,
            graph,
            state.story,
            state.chronicle,
            llm,
            dialogue=state.dialogue,
            journal=journal,
            book=state.book,
        )
        # the scripts' quests in their givers' conversations, and the hero's lies about them
        state.vanilla = VanillaQuests(
            link,
            state.dialogue,
            state.chronicle,
            graph,
            hero_distance=lambda npc: talk.people[npc].distance if npc in talk.people else None,
            journal=journal,
            trust=trust,
        )
        # those who learn of a lie of the hero's ask him to make amends: the director's storylets
        state.amends = Amends(
            link,
            graph,
            state.story,
            state.chronicle,
            state.director,
            llm,
            dialogue=state.dialogue,
            journal=journal,
            crimes=state.crimes,
        )
        # the director's own characters (docs/director.md, stage 2): the branch keeps them, and whoever
        # reads npc's by id reads them as the scripts' own: their cards, persona and voice
        state.cast = Cast(link, state.story, state.chronicle)
        personas.cards = Cards(personas.cards, state.cast.cards)
        state.deals.cards = personas.cards
        witnesses.cards = personas.cards
        personas.people = state.cast.person
        # the director's model makes characters up for a place (layer 4): for now on the console's word
        if place_book is not None:
            state.casting = Casting(link, state.cast, llm, place_book, state.chronicle, journal=journal)
        # quests of the director's own as objects (docs/director.md, stage 3): the book makes and follows
        # them, the author makes them up (stage 1), for now on the console's word
        state.book = QuestBook(
            link,
            graph,
            state.story,
            state.chronicle,
            state.director,
            places=place_book,
            dialogue=state.dialogue,
            journal=journal,
        )
        state.director.book = state.book
        state.author = Author(
            link,
            graph,
            state.story,
            state.chronicle,
            state.director,
            llm,
            places=place_book,
            cards=personas.cards,
            dialogue=state.dialogue,
            journal=journal,
            book=state.book,
        )
        if state.casting is not None:
            state.chains = ChainAuthor(
                state.author, state.casting, crimes=state.crimes,
                npc_crimes=not args.no_crimes and not args.no_npc_crimes,
            )
            if state.occasions is not None:
                state.showrunner = Showrunner(
                    state.occasions,
                    state.author,
                    state.chains,
                    state.casting,
                    llm,
                    crimes=state.crimes,
                    law=state.crime_law.law if state.crime_law is not None else None,
                    feat_challenges=not args.no_feats and not args.no_feat_challenges,
                )
                if (task := _start_showrunner(args, state.showrunner)) is not None:
                    directing.append(task)
        # a lie found out changes the scripts' dialogs of the one lied to: the quest is not done there
        state.consequences = LieConsequences(
            link,
            graph,
            state.story,
            state.chronicle,
            state.director,
            dialogue=state.dialogue,
            journal=journal,
        )
        # Off scenarios own model calls, except those exercising these queues themselves.
        if args.scenario not in {"live_npc", "live_director"} and (
            args.llm != "off" or args.scenario is None or args.scenario in {"repair", "own_repair"}
        ):
            directing.append(asyncio.create_task(state.director.run()))
        directing.append(asyncio.create_task(state.vanilla.run()))
        if args.scenario not in {"live_npc", "live_director"} and (
            args.llm != "off" or args.scenario is None or args.scenario in {
                "amends", "amends_deal", "crime_director",
            }
        ):
            directing.append(asyncio.create_task(state.amends.run()))
    elif graph is not None and state.crimes is not None:
        # A victim's rule-driven demand remains available without background story authoring.
        # Neither repair nor optional Amends queues run in this mode; CrimeMeet calls demand.
        amends_director = Director(link, graph, state.story, state.chronicle, llm)
        state.amends = Amends(
            link, graph, state.story, state.chronicle, amends_director, llm,
            dialogue=state.dialogue, journal=journal, crimes=state.crimes,
        )
    # rumors of the hero's lies go round the people that know each other (story graph, stage 5)
    state.rumors = Rumors(
        link,
        state.dialogue,
        state.chronicle,
        graph,
        bonds=state.cast.bonds if state.cast else dict,
        crimes=state.crimes,
    )
    if state.book is not None and not args.no_feats and not args.no_feat_challenges:
        state.feat_challenges = Challenges(
            state.chronicle, state.book,
            social=state.rumors.prepare_social if state.rumors is not None else None,
        )
    if state.cast is not None:
        state.cast.on_change.append(state.rumors.people_changed)
    directing.append(asyncio.create_task(state.rumors.run()))
    # near the hero a retelling is said aloud, in the two's own voices, or seen (wave 2 step 2)
    state.gossip = GossipDesk(
        link,
        state.dialogue,
        talk,
        state.rumors,
        perception,
        budget,
        state.chronicle,
        trust,
        state.guard,
        state.bodies,
        trace=trace,
        journal=journal,
    )
    # what each npc does in its next hour: trips from its everyday concerns (wave 2 step 6); always
    # there, so the deaths the npc's see are written down without the planner too
    state.day = DayDesk(
        link,
        state.rumors,
        state.chronicle,
        state.bodies,
        state.guard,
        perception,
        talk,
        place_book,
        journal=journal,
    )
    state.npc_will = NpcWill(
        link, state.chronicle, state.npc_crimes, cards=personas.cards, guard=state.guard,
        people=state.people_knowledge, rumors=state.rumors, memory=memory,
        blame=state.blame,
        busy=lambda npc: npc in talk.attached or state.dialogue.speaking(npc),
    )
    state.npc_crime_law = NpcCrimeLaw(
        link, state.chronicle, state.npc_crimes, will=state.npc_will,
    )
    brain = None
    bodypilot: BodyPilot | None = None
    if decider is not None:
        # npc's outside conversations: the bridge reports them (npc_roster, npc_event, protocol v5)
        concerns = state.concerns

        def agenda_of(npc: str, dialogue: DialogueManager = state.dialogue) -> str | None:
            """What it wants of the hero: the conversation's agenda, else its most pressing concern."""
            saved = memory.intent(dialogue.branch, npc) if dialogue.branch else None
            agenda = Intent.from_json(saved).agenda if saved else None
            return concerns.current(npc, agenda) or concerns.agenda(npc)

        planner = state.brain = NpcPlanner(
            decider,
            GameApplier(link, native_body=lambda npc: _native_combat_available(link, bodypilot, npc)),
            agenda_of=agenda_of,
            routine_of=link.routine,
            habits_of=habits_of,
            view_of=perception.english,
            # each npc's character, from its card (characters.py)
            builder=SituationBuilder(traits=Traits(personas.cards), places=place_book),
            # the new day menu applied, or still asked in shadow beside the old one
            config=PlannerConfig(apply_day=args.apply_day),
            trace=trace,  # its decisions: data for fine-tuning (M5d)
        )
        planner.crime_rooms = state.crimes is not None
        state.incidents.follow(planner)  # free npc's speak when the moment calls for it
        if state.found_traces is not None:
            state.found_traces.follow(planner)  # a free npc decides what to do about a body it found
        state.talk.planner = planner  # npc's in a conversation are left to it; others overhear
        link.listeners.append(planner.on_message)
        # what the npc wants from the hero, from the conversation, steers it afterwards
        def on_intent(npc: str, intent: Intent) -> None:
            if intent.goal not in {"fight", "flee", "seek_help"}:
                planner.clear_body_intent(npc)
            planner.set_agenda(npc, concerns.current(npc, intent.agenda or None) or concerns.agenda(npc))

        state.dialogue.on_intent = on_intent
        concerns.on_change.append(lambda npc: planner.set_agenda(npc, agenda_of(npc)))
        # an npc with something on its mind comes up to the hero and speaks first (wave 2 step 1)
        state.initiative = InitiativeDesk(
            link,
            state.dialogue,
            state.talk,
            planner,
            concerns,
            state.guard,
            budget,
            state.chronicle,
            bodies=state.bodies,
            perception=perception,
            journal=journal,
        )
        # two talking near the hero: their veto before the initiative's, whose first words wait for them
        state.gossip.follow(planner, state.initiative)
        # the day's menus and trips: its veto before the two talking's
        state.day.follow(planner, state.gossip, state.initiative)
        if state.meetings is not None:  # an npc on its way to a meeting or waiting there: first veto
            state.meetings.attach_planner(planner)
        if state.errands is not None:  # an npc on an errand
            state.errands.attach_planner(planner)
        brain = asyncio.create_task(planner.run())
    if state.crimes is not None:
        sides = state.crimes.sides
        sides.orders, sides.book = state.talk.orders, state.book
        previous_intent = state.dialogue.on_intent

        def remember_fight(npc: str, intent: Intent) -> None:
            sides.note_intent(npc, intent)
            if previous_intent is not None:
                previous_intent(npc, intent)

        state.dialogue.on_intent = remember_fight
        state.dialogue.on_fight = sides.note_fight
        state.dialogue.on_attack.append(lambda npc: sides.note_fight(npc, "PLAYER"))
        assert state.talk.orders is not None
        state.talk.orders.on_fight = sides.note_fight
    if (crime_meet_task := _start_crime_meet(state, link, personas.cards)) is not None:
        directing.append(crime_meet_task)
    await link.send(_perception_control(args, planner=decider is not None))
    bodypilot = _start_bodypilot(args, link, state.talk)
    body_task = asyncio.create_task(bodypilot.run()) if bodypilot is not None else None
    try:
        ctx = ScenarioContext(
            args=args,
            state=state,
            place_book=place_book,
            journal=journal,
            trace=trace,
            llm=llm,
            done=done,
            native_combat_available=_native_combat_available,
        )
        scenario_exit_code = await run_scenario(ctx, link, bodypilot)
        if scenario_exit_code is not None:
            env.set_exit_code(scenario_exit_code)
        await link.closed.wait()  # the session lives while the game is connected
    finally:
        if bodypilot is not None:
            await bodypilot.aclose()
        if body_task is not None:
            await asyncio.gather(body_task, return_exceptions=True)
        lies.cancel()
        debts.cancel()
        if parley_clock is not None:
            parley_clock.cancel()
        asks.cancel()
        if meets is not None:
            meets.cancel()
        if errand_clock is not None:
            errand_clock.cancel()
        for task in directing:
            task.cancel()
        if state.feats is not None:
            await state.feats.close()
        if state.fates is not None:
            await state.fates.close()
        if state.fate_cards is not None:
            await state.fate_cards.close()
        if state.feat_challenges is not None:
            state.feat_challenges.close()
        if feat_image_root is not None:
            feat_image_root.cleanup()
        if state.questlog is not None:
            await state.questlog.close()
        if state.book is not None:
            await state.book.close()
        if state.crime_react is not None:
            await state.crime_react.close()
        if state.npc_crime_law is not None:
            await state.npc_crime_law.close()
        if state.npc_will is not None:
            await state.npc_will.close()
        if state.npc_crimes is not None:
            await state.npc_crimes.close()
        if state.blame is not None:
            await state.blame.close()
        if state.crime_meet is not None:
            await state.crime_meet.close()
        if state.crime_memory is not None:
            await state.crime_memory.close()
        if state.crimes is not None:
            await state.crimes.close()
        if state.crime_law is not None:
            await state.crime_law.close()
        if brain is not None:
            brain.cancel()
