"""Off-screen NPC harm on save 1: natural routines, cards and clocks start every deed."""

from __future__ import annotations

import asyncio
import logging
import math
from dataclasses import replace
from typing import TYPE_CHECKING, Any

from ..game import GameLink
from ..llm.fake import FakeBackend
from ..npc_far import FarWorld
from ..npc_will import OWNER_NEAR_CM, ROUTINE_CM
from ..protocol import NpcActCmd, Session, StoryEffectCmd
from .common import (
    HILDA,
    LOBART,
    _logged,
    _save,
    _until,
    _until_async,
    prepare_npc_pose,
    wait_message,
)

if TYPE_CHECKING:
    from ..chests import ChestDesk
    from ..dialogue import DialogueManager
    from ..npc_will import NpcWill

log = logging.getLogger("gothic_ai.scenarios")
CHEST, HOUSE = "CHEST_LOBART", "NW_FARM1_INHOUSE_01"
HERO_INSTANCE = "PC_HERO"
DAY = 24 * 60


async def npc_far(
    link: GameLink,
    will: NpcWill,
    chests: ChestDesk,
    dialogue: DialogueManager,
    log_dir: str,
    slot: int = 7,
) -> dict[str, str]:
    """Hilda steals by an hourly motive; Lobart discovers, suspects and accuses off screen.

    Before the theft only the hero, clock and character cards change. The farm's native daytime
    routines place Hilda in the house and Lobart outside. After the transfer Lobart really opens
    his chest, as in npc_will; an actual view supplies his suspicion, never the crime's hidden truth.
    Lobart belongs to GIL_OUT, so community guard enforcement is deliberately module coverage.
    """
    results: dict[str, str] = {}
    crimes, rumors, people = will.npc_crimes, will.rumors, will.people
    facts = will.chronicle.facts
    world = FarWorld(will)
    original_cards, original_crime_cards = will.cards, crimes.cards
    original_llm, send = dialogue.llm, link.send
    assert rumors is not None
    original_gate, original_stage = rumors.gate, rumors.stage
    neutral = {npc: replace(card, honesty=5, greed=1) for npc, card in original_cards.items()}
    sent: list[NpcActCmd | StoryEffectCmd] = []

    def check(name: str, ok: bool, detail: object = "") -> bool:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"
        log.info("npc_far %-25s %s", name, results[name])
        return ok

    async def sending(command: Any) -> None:
        if isinstance(command, NpcActCmd | StoryEffectCmd):
            sent.append(command)
        await send(command)

    def cards(*, greed: bool = False) -> None:
        prepared = {
            **neutral,
            HILDA: replace(neutral[HILDA], honesty=1 if greed else 5, greed=5 if greed else 1),
            LOBART: replace(neutral[LOBART], courage=2),
        }
        will.cards = crimes.cards = prepared
        # A card edit within an hour is setup; the normal hourly rule must reconsider that hour.
        will._hours.clear()

    async def count(npc: str, item: str) -> int:
        return int(await link.evaluate(["call", "NPC_HASITEMS", ["inst", npc], ["sym", item]]))

    async def clock_at(hour: int) -> int | None:
        await link.debug("time", hour=hour, minute=0)
        reading: dict[str, int] = {}

        async def read() -> bool:
            views = await link.npc_view([HILDA])
            if not views or views[0].time is None or views[0].time % DAY // 60 != hour:
                return False
            reading["now"] = views[0].time
            return True

        if not await _until_async(read, bool, 10):
            return None
        await rumors.tick()
        await will.tick(now=reading["now"])
        return reading["now"]

    async def geometry() -> dict[str, Any]:
        generation = will._generation
        thief = await world.position(HILDA, generation)
        owner = await world.position(LOBART, generation)
        thief_routine = world.routine(HILDA)
        hero = (await link.snapshot(HILDA, fresh=True)).player
        return {
            "now": will._now,
            "thief": None if thief is None else {
                "pos": thief.pos, "state": thief.state, "source": thief.source,
                "waypoint": thief.waypoint,
            },
            "owner": None if owner is None else {
                "pos": owner.pos, "state": owner.state, "source": owner.source,
                "waypoint": owner.waypoint,
            },
            "thief_routine": None if thief_routine is None else {
                "pos": thief_routine.pos, "state": thief_routine.state,
                "waypoint": thief_routine.waypoint,
            },
            "hero_pos": hero.pos if hero is not None else None,
            "both_far": await world.far(HILDA, generation) and await world.far(LOBART, generation),
            "alone": await world.alone(HILDA, generation),
        }

    def hero_facts() -> list[dict[str, Any]]:
        return [f for f in facts() if str(f.get("fact") or "").startswith("crime")]

    link.send = sending  # type: ignore[method-assign]
    cards()
    rumors.gate, rumors.stage = lambda *_: False, None
    try:
        await link.wait_world()
        if link.session is None:
            await wait_message(link, Session, timeout=10)
        if not check(
            "enabled", will.enabled and crimes.enabled and isinstance(original_llm, FakeBackend), log_dir,
        ):
            return _logged(results)
        dialogue.llm = FakeBackend("Ну.", delay=0)
        await link.debug("player_hp", hp=5000)
        population = {str(p["npc"]): p for p in await link.story_people()}
        points = {p["name"]: p for p in await link.waypoints() if not p.get("fp")}
        if not check("farm", {HILDA, LOBART} <= population.keys() and HOUSE in points):
            return _logged(results)
        found = await chests.chests(LOBART, fresh=True)
        chest = next((c for c in found if c.id == CHEST and c.why == "owner" and c.items), None)
        if not check("chest", chest is not None, [(c.id, c.why) for c in found]):
            return _logged(results)
        assert chest is not None
        far_points = [
            p for p in points.values() if not p.get("room")
            and 20000 <= math.dist(tuple(p[a] for a in ("x", "y", "z")), chest.pos) <= 40000
        ]
        if not check("hero_destination", bool(far_points), len(far_points)):
            return _logged(results)
        far_point = min(far_points, key=lambda p: math.dist(tuple(p[a] for a in ("x", "y", "z")), chest.pos))
        far_waypoint = str(far_point["name"])
        await link.debug("player_goto", waypoint=far_waypoint)
        before_hero = hero_facts()
        if not check("clean_branch", not facts("npc_crime") and not facts("npc_will")):
            return _logged(results)

        # Choose an actual natural opportunity. Neither an NPC's destination nor a routine is edited.
        opportunity: dict[str, Any] | None = None
        preparations: list[dict[str, Any]] = []
        for hour in range(8, 22):
            if await clock_at(hour) is None:
                preparations.append({"hour": hour, "clock": "unavailable"})
                continue
            prepared = await geometry()
            thief, owner, routine = prepared["thief"], prepared["owner"], prepared["thief_routine"]
            prepared.update(
                hour=hour,
                thief_chest=math.dist(thief["pos"], chest.pos) if thief else None,
                routine_chest=math.dist(routine["pos"], chest.pos) if routine and routine["pos"] else None,
                owner_chest=math.dist(owner["pos"], chest.pos) if owner else None,
            )
            preparations.append(prepared)
            if (prepared["both_far"] and prepared["alone"] and thief and owner and routine
                    and routine["pos"] is not None and prepared["routine_chest"] <= ROUTINE_CM
                    and prepared["thief_chest"] <= ROUTINE_CM and prepared["owner_chest"] > OWNER_NEAR_CM):
                opportunity = prepared
                break
        if not check("natural_opportunity", opportunity is not None, preparations):
            return _logged(results)
        assert opportunity is not None
        expected_witnesses = set(await world.witnesses(HILDA, will._generation))
        inventories = {item.item: await count(HILDA, item.item) for item in chest.items}
        chest_counts = {item.item: item.count for item in chest.items}
        sent_at = len(sent)
        cards(greed=True)
        await will.tick(now=int(opportunity["now"]))
        await _until(lambda: bool(facts("npc_crime")), 45)
        cards()
        theft = next((f for f in facts("npc_crime") if f.get("culprit") == HILDA), None)
        if not check(
            "autonomous_far_theft", theft is not None and theft.get("far") is True
            and theft.get("kind") == "theft" and theft.get("owner") == LOBART
            and theft.get("chest") == CHEST and theft.get("count") == 1,
            {"crime": theft, "preparation": opportunity},
        ):
            return _logged(results)
        assert theft is not None
        cid, item = str(theft["id"]), str(theft["item"])
        effects = [c for c in sent[sent_at:] if isinstance(c, StoryEffectCmd)
                   and c.npc == HILDA and any(e.get("do") == "chest_take" for e in c.effects)]
        physical = [c.model_dump() for c in sent[sent_at:] if isinstance(c, NpcActCmd)
                    and c.action in {"steal", "fight"}]
        current_chest = next(c for c in await link.npc_chests(LOBART) if c.id == CHEST)
        after_chest = sum(i.count for i in current_chest.items if i.item == item)
        if not check(
            "real_transfer", item in inventories and await count(HILDA, item) == inventories[item] + 1
            and after_chest == chest_counts.get(item, 0) - 1 and len(effects) == 1 and not physical,
            {"thief": [inventories.get(item), await count(HILDA, item)],
             "chest": [chest_counts.get(item), after_chest], "physical": physical,
             "effects": [c.model_dump() for c in effects]},
        ):
            return _logged(results)
        seen = {str(f["npc"]) for f in facts("npc_crime_seen") if f.get("crime") == cid}
        if not check(
            "far_witnesses", set(theft.get("witnesses", [])) == expected_witnesses == seen,
            {"expected": sorted(expected_witnesses), "recorded": theft.get("witnesses"),
             "seen": sorted(seen)},
        ):
            return _logged(results)
        attempts = [a for a in will.attempts() if a.get("npc") == HILDA and a.get("kind") == "greed"]
        if not check(
            "far_committed", len(attempts) == 1 and attempts[0].get("far") is True
            and attempts[0].get("outcome") == "committed" and attempts[0].get("crime") == cid,
            attempts,
        ):
            return _logged(results)

        # The owner genuinely opens his chest. A fresh real view at the scene supplies the remembered
        # presence; only then does looked() discover the loss and derive its suspicion.
        # Use npc_will's proven short chest approach after the autonomous transfer. Starting from
        # Lobart's outdoor guard routine can leave the native path blocked at the farmhouse doorway.
        await link.debug("player_goto", pos=(chest.pos[0], chest.pos[1], chest.pos[2] + 200))
        await link.debug("npc_goto", npc=LOBART, to=HERO_INSTANCE, distance=0)
        await link.debug("player_goto", waypoint=HOUSE)
        conv = dialogue.open(LOBART, "Лобарт")
        held = await _until_async(
            lambda: dialogue.borrow_body(conv, "scenario-npc-far-chest"), bool, 15,
        )
        if not check("owner_held", held):
            return _logged(results)
        try:
            opened = await link.act(LOBART, "use_chest", chest=CHEST, take=[], put=[], timeout=90)
            if not check("owner_opened", opened.status == "done", opened.model_dump()):
                return _logged(results)
            sightings = crimes.crimes.sightings if crimes.crimes is not None else None
            assert sightings is not None

            async def recalled() -> bool:
                await sightings.desk.fresh(LOBART)
                return any(s.who == HILDA and s.end >= will._at(theft) - 30
                           and s.start <= will._at(theft) + 30 for s in sightings.mind(LOBART).sightings)

            remembered = await _until_async(recalled, bool, 5)
            if not remembered:
                assert chest.waypoint is not None
                posed, detail = await prepare_npc_pose(
                    link, LOBART, chest.waypoint, to=HILDA, distance=350, face=HILDA,
                )
                log.info("npc_far owner memory preparation: %s", detail)
                remembered = posed and await _until_async(recalled, bool, 5)
            if not check("owner_remembers", remembered, [
                s.record() for s in sightings.mind(LOBART).sightings if s.who == HILDA
            ]):
                return _logged(results)
            note = await chests.looked(LOBART, CHEST, opened.detail)
            suspected = [f for f in facts("npc_crime_suspected")
                         if f.get("crime") == cid and f.get("suspect") == HILDA]
            if not check("owner_suspects", "пропало" in note and len(suspected) == 1,
                         {"note": note, "suspected": suspected}):
                return _logged(results)
            # Keep the borrowed body busy until the hero has left; accusation belongs to the far rule.
            await link.debug("player_goto", waypoint=far_waypoint)
        finally:
            if held:
                dialogue.return_body(conv, "scenario-npc-far-chest")
        sent_at = len(sent)
        await rumors.tick()
        await will.tick()
        await _until(lambda: any(f.get("crime") == cid for f in facts("npc_accused")), 30)
        accusations = [f for f in facts("npc_accused") if f.get("crime") == cid]
        if not check(
            "far_accusation", len(accusations) == 1 and accusations[0].get("npc") == LOBART
            and accusations[0].get("target") == HILDA and accusations[0].get("kind") == "suspect"
            and accusations[0].get("far") is True and not any(
                isinstance(c, NpcActCmd) and c.npc == LOBART and c.action in {"say_svm", "fight"}
                for c in sent[sent_at:]
            ), accusations,
        ):
            return _logged(results)
        await rumors.tick()
        check("accusation_rumor", any(r.subject == HILDA and r.kind == "npc_suspected"
                                     for r in rumors.rumors.values()))
        check("guard_coverage", theft.get("community") is None,
              "Lobart's GIL_OUT farm has no community law; guard enforcement is covered by module tests")
        check("hero_unchanged", hero_facts() == before_hero)

        kept_kinds = {
            "npc_motive", "npc_will", "npc_will_outcome", "npc_crime", "npc_crime_seen",
            "npc_crime_suspected", "npc_accused", "npc_crime_known", "npc_crime_settled", "rumor",
        }
        kept = [f for f in facts() if f.get("fact") in kept_kinds]
        inventory = {npc: await count(npc, item) for npc in (HILDA, LOBART)}
        knowledge = people.of(LOBART)
        cards(greed=True)
        saved = await _save(link, slot)
        if not check("saved", saved is not None, len(kept)):
            return _logged(results)
        assert saved is not None
        loading = asyncio.create_task(wait_message(link, Session, lambda m: m.reason == "load", 90))
        await asyncio.sleep(0)
        await link.debug("load", slot)
        loaded = await loading
        await link.wait_world()
        if not check(
            "loaded", loaded.parent == saved.branch and loaded.parent_seq == saved.seq
            and [f for f in facts() if f.get("fact") in kept_kinds] == kept
            and {npc: await count(npc, item) for npc in (HILDA, LOBART)} == inventory
            and people.of(LOBART) == knowledge,
            {"facts": len(kept), "inventory": inventory},
        ):
            return _logged(results)
        before_thefts, before_attempts = list(facts("npc_crime")), list(facts("npc_will"))
        checked_hours = []
        for hour in (int(opportunity["hour"]) + 1, int(opportunity["hour"]) + 2):
            cards()
            now = await clock_at(hour % 24)
            if not check(f"cooldown_{hour}_clock", now is not None, now):
                return _logged(results)
            cards(greed=True)
            await will.tick(now=now)
            checked_hours.append(now)
        check("loaded_cooldown", facts("npc_crime") == before_thefts
              and facts("npc_will") == before_attempts, checked_hours)
        return _logged(results)
    finally:
        will.cards, crimes.cards = original_cards, original_crime_cards
        rumors.gate, rumors.stage = original_gate, original_stage
        dialogue.llm = original_llm
        link.send = send  # type: ignore[method-assign]
