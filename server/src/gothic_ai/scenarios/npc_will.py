"""NPC motives on save 1: the scene supplies places and cards, never a harmful command."""

from __future__ import annotations

import asyncio
import logging
import math
from dataclasses import replace
from typing import TYPE_CHECKING, Any

from .. import triggers
from ..game import GameLink
from ..llm.fake import FakeBackend
from ..npc_will import OWNER_NEAR_CM, ROUTINE_CM, SIMULATION_CM
from ..protocol import GameMessage, NpcActCmd, NpcActResult, NpcDeed, NpcIdle, Session
from .common import (
    HILDA,
    LOBART,
    VINO,
    _logged,
    _save,
    _until,
    _until_async,
    prepare_npc_pose,
    wait_message,
    wait_npc_visibility,
)

if TYPE_CHECKING:
    from ..chests import ChestDesk
    from ..dialogue import DialogueManager
    from ..npc_will import NpcWill
    from ..protocol import Chest

log = logging.getLogger("gothic_ai.scenarios")
HOUSE, ROAD, RETURN_ROAD = "NW_FARM1_INHOUSE_01", "NW_FARM1_PATH_04", "NW_FARM1_PATH_03"
CHEST = "CHEST_LOBART"
# debug.npc_goto resolves native instance symbols; its `to`/`face` do not resolve the roster alias PLAYER.
HERO_INSTANCE = "PC_HERO"


async def npc_will(
    link: GameLink,
    will: NpcWill,
    chests: ChestDesk,
    dialogue: DialogueManager,
    log_dir: str,
    slot: int = 7,
) -> dict[str, str]:
    """Hilda's greed steals; Lobart learns, reclaims or accuses, suspects and remembers a load.

    Hilda's native working and sleeping places include Lobart's house. His native daytime routine
    leaves the chest: neither ownership nor anyone's routine is invented by the fixture. Vino waits
    inside while Hilda decides behind a wall; her native walk can expose the theft. Branches use
    courage 3 and 2, then an unwitnessed theft in which the owner remembers the innocent Vino.
    Every steal and fight below
    must have been sent by NpcWill and have its persisted npc_will fact.
    """
    results: dict[str, str] = {}
    crimes, people, rumors = will.npc_crimes, will.people, will.rumors
    facts = will.chronicle.facts
    original_cards, original_crime_cards = will.cards, crimes.cards
    original_gate, original_stage = rumors.gate, rumors.stage
    original_llm, send = dialogue.llm, link.send
    original_busy = will.busy
    preparing: set[str] = set()
    neutral = {npc: replace(card, honesty=5, greed=1) for npc, card in original_cards.items()}
    sent: list[NpcActCmd] = []
    received: list[NpcActResult | NpcDeed | NpcIdle] = []
    bodies: dict[str, list[dict[str, Any]]] = {HILDA: [], LOBART: []}
    symbols: dict[int, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> bool:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"
        log.info("npc_will %-26s %s", name, results[name])
        return ok

    async def sending(command: Any) -> None:
        if isinstance(command, NpcActCmd):
            sent.append(command)
        await send(command)

    def record(message: GameMessage) -> None:
        if isinstance(message, NpcActResult | NpcDeed | NpcIdle):
            received.append(message)

    def body(packet: dict[str, Any]) -> None:
        for row in packet["observations"]:
            if (npc := symbols.get(row["script_symbol"])) in bodies:
                bodies[npc].append(row["self"])

    def cards(*, greed: bool = False, courage: int = 2) -> None:
        prepared = {
            **neutral,
            HILDA: replace(neutral[HILDA], honesty=1 if greed else 5, greed=5 if greed else 1),
            LOBART: replace(neutral[LOBART], courage=courage),
        }
        will.cards = crimes.cards = prepared
        # Scene cards change within an hour. The next normal roster check must reconsider that hour.
        will._hours.clear()

    async def place(npc: str, point: str, **kwargs: Any) -> bool:
        if point == parked:
            kwargs.setdefault("require_perception_ready", False)
        ok, detail = await prepare_npc_pose(link, npc, point, **kwargs)
        return ok or check("pose", False, detail)

    async def count(npc: str, item: str) -> int:
        return int(await link.evaluate(["call", "NPC_HASITEMS", ["inst", npc], ["sym", item]]))

    def actions(npc: str, action: str, after: int = 0) -> list[NpcActCmd]:
        return [c for c in sent[after:] if c.npc == npc and c.action == action]

    async def voiced(npc: str, after: int) -> bool:
        def done() -> bool:
            queued = {
                m.id for m in received[after:]
                if isinstance(m, NpcActResult) and m.npc == npc
                and m.action == "say_svm" and m.status == "queued"
            }
            return any(
                isinstance(m, NpcIdle) and m.act in queued and m.reason == "done"
                for m in received[after:]
            )

        return await _until(done, 30)

    async def load_clean(label: str) -> bool:
        cards()
        loading = asyncio.create_task(wait_message(link, Session, lambda m: m.reason == "load", 90))
        await asyncio.sleep(0)
        await link.debug("load", slot + 1)
        await loading
        await link.wait_world()
        sent.clear()
        received.clear()
        for rows in bodies.values():
            rows.clear()
        return check(f"{label}_branch", not facts("npc_crime") and not facts("npc_will"))

    async def owner_away() -> bool:
        # Reapply the existing script routine, then ground at its real current destination. This
        # resets a prior scene's standing activity without creating a new daily destination.
        await link.npc_act(LOBART, "set_routine", name="START")
        await link.debug("npc_goto", npc=LOBART, waypoint=owner_point)
        detail: dict[str, Any] = {"routine_waypoint": owner_point}

        async def ready() -> bool:
            found = next((c for c in await link.npc_chests(LOBART) if c.id == CHEST), None)
            views = await link.npc_view([LOBART])
            entry = will.roster.get(LOBART)
            detail.update(
                chest_distance=found.distance if found is not None else None,
                view=views[0].model_dump() if views else None,
                roster=entry.model_dump() if entry is not None else None,
            )
            return bool(
                found and found.distance > OWNER_NEAR_CM and views and views[0].theft_ready
                and views[0].hero and views[0].hero.distance < 3000
                and entry and entry.activity == "routine"
            )

        prepared = await _until_async(ready, bool, 15)
        log.info("npc_will owner routine preparation: %s", detail)
        return prepared

    async def alone() -> bool:
        views = await link.npc_view([HILDA])
        return bool(
            views and views[0].theft_ready and not views[0].sees
            and views[0].hero and not views[0].hero.seen and views[0].hero.distance < 3000
        )

    async def stage_witness(label: str, chest: Chest) -> bool:
        assert chest.waypoint is not None
        attempts: list[dict[str, Any]] = []
        # Prove the future sight line inside the house, before putting Hilda behind its wall.
        # Vino stays here with this initialized native state and heading through the actual theft.
        for offset in (-300, 300, -450, 450):
            posed, pose = await prepare_npc_pose(
                link, VINO, chest.waypoint, to=HILDA, distance=offset, face=HILDA,
            )
            visible, detail = False, {}
            if posed:
                visible, detail = await wait_npc_visibility(link, VINO, HILDA)
            poses = {}
            for npc in (HILDA, VINO):
                snapshot = await link.snapshot(npc, fresh=True)
                poses[npc] = {"pos": snapshot.npc.pos, "rot": snapshot.npc.rot}
            [free_los] = await link.story_eval(VINO, [
                ["call", "NPC_CANSEENPCFREELOS", ["inst", "SELF"], ["inst", HILDA]],
            ])
            attempt = {
                "offset": offset, "pose": pose, "visibility": detail,
                "positions": poses, "free_los": free_los,
            }
            attempts.append(attempt)
            log.info("npc_will %s witness preparation: %s", label, attempt)
            if posed and visible:
                return check(f"{label}_witness", True, attempt)
        return check(f"{label}_witness", False, attempts)

    async def stage_departure(label: str, chest: Chest, *, witnessed: bool) -> bool:
        owner = (await link.snapshot(LOBART, fresh=True)).npc
        witness = (await link.snapshot(VINO, fresh=True)).npc if witnessed else None

        def position(point: dict[str, Any]) -> tuple[float, float, float]:
            return tuple(float(point[axis]) for axis in ("x", "y", "z"))

        exterior = [p for p in points.values() if not p.get("room")]
        starts = [p for p in exterior if 500 <= math.dist(position(p), chest.pos) <= ROUTINE_CM]
        hero_spots = [p for p in exterior if 700 <= math.dist(position(p), chest.pos) <= 2500]
        pairs = [
            (start, hero) for start in starts for hero in hero_spots
            if math.dist(position(start), position(hero)) >= 700
            and all(math.dist(position(hero), target) < 2700 for target in (
                position(start), owner.pos, chest.pos, *((witness.pos,) if witness else ()),
            ))
        ]
        # The opposite side of the house puts a real wall between the greedy NPC and the owner.
        # The hero stays on the waynet, away from the chest's small interaction slot.
        pairs.sort(key=lambda pair: (
            -math.dist(position(pair[0]), owner.pos), math.dist(position(pair[1]), owner.pos),
        ))
        attempts: list[dict[str, Any]] = []
        for start, hero in pairs:
            await link.debug("player_goto", waypoint=hero["name"])
            posed, pose = await prepare_npc_pose(
                link, HILDA, start["name"], face=HERO_INSTANCE, timeout=5,
            )
            solitude = posed and await alone()
            attempt = {"start": start["name"], "hero": hero["name"], "pose": pose, "alone": solitude}
            attempts.append(attempt)
            log.info("npc_will %s departure preparation: %s", label, attempt)
            if solitude:
                return check(f"{label}_departure", True, attempt)
        return check(f"{label}_departure", False, attempts)

    async def stage_theft(label: str, *, witnessed: bool, remember: bool = False) -> Chest | None:
        cards()
        await link.debug("player_goto", waypoint=HOUSE)
        for npc in farm:
            if not await place(npc, parked):
                return None
        found = await chests.chests(LOBART, fresh=True)
        chest = next((c for c in found if c.id == CHEST and c.items), None)
        if not check(f"{label}_chest", chest is not None, [(c.id, c.why) for c in found]):
            return None
        assert chest is not None
        if not check(f"{label}_endpoint", chest.waypoint in points, chest.waypoint):
            return None
        assert chest.waypoint is not None
        routine = [r for r in population[HILDA]["routine"] if r.get("pos")]
        distance = min((math.dist(r["pos"], chest.pos) for r in routine), default=math.inf)
        if not check(
            f"{label}_ordinary_place", distance <= 2000, round(distance) if math.isfinite(distance) else None,
        ):
            return None

        if remember:
            # An actual look at an innocent acquaintance near the chest supplies the suspicion.
            await rumors.tick()
            sightings = crimes.crimes.sightings
            memory_detail: dict[str, Any] = {}

            async def recalled() -> bool:
                # A debug npc_view is only a read. PerceptionDesk.fresh stores that actual view
                # through the production on_view path which feeds the observer's Sightings.
                view = await sightings.desk.fresh(LOBART)
                look = next((o for o in view.sees if o.id == VINO), None) if view else None
                memory_detail.update(
                    seen=look is not None,
                    known=bool(look and sightings.desk.knows(LOBART, look)),
                    social_tie=bool(rumors.social and VINO in rumors.social.ties.get(LOBART, {})),
                    sightings=[s.record() for s in sightings.mind(LOBART).sightings if s.who == VINO],
                )
                return bool(memory_detail["sightings"])

            if not await place(VINO, chest.waypoint):
                return None
            remembered, attempts = False, []
            for offset in (-300, 300, -450, 450):
                posed, pose = await prepare_npc_pose(
                    link, LOBART, chest.waypoint, to=VINO, distance=offset, face=VINO,
                )
                seen, detail = False, {}
                if posed:
                    seen, detail = await wait_npc_visibility(link, LOBART, VINO)
                remembered = seen and await _until_async(recalled, bool, 5)
                attempt = {
                    "offset": offset, "pose": pose, "visibility": detail, "memory": dict(memory_detail),
                }
                attempts.append(attempt)
                log.info("npc_will %s remembered preparation: %s", label, attempt)
                if remembered:
                    break
            if not check(f"{label}_remembered", remembered, attempts):
                return None
            if not await place(VINO, parked):
                return None

        # Keep the native waypoint heading, exactly as npc_deeds' successful chest preparation.
        # The relative witness positions depend on that axis. Actual sight is checked below;
        # no perception or callback is disabled, and Hilda's decision starts outside afterwards.
        await link.debug("player_goto", pos=tuple(chest.pos))
        if not await place(HILDA, chest.waypoint):
            return None
        thief = (await link.snapshot(HILDA, fresh=True)).npc
        owner_destination = points[owner_point]
        dx = owner_destination["x"] - thief.pos[0]
        dz = owner_destination["z"] - thief.pos[2]
        length = math.hypot(dx, dz)
        if not check(f"{label}_owner_direction", length > 0, owner_point):
            return None
        await link.debug("player_goto", pos=(
            thief.pos[0] + 2200 * dx / length, thief.pos[1], thief.pos[2] + 2200 * dz / length,
        ))
        if not check(f"{label}_owner_routine", await owner_away(), owner_point):
            return None

        if witnessed and not await stage_witness(label, chest):
            return None
        if not await stage_departure(label, chest, witnessed=witnessed):
            return None
        if not check(
            f"{label}_perception", await alone(), [v.model_dump() for v in await link.npc_view([HILDA])],
        ):
            return None
        return chest

    async def local_geometry(label: str, chest: Chest) -> dict[str, Any] | None:
        thief = await link.snapshot(HILDA, fresh=True)
        owner = await link.snapshot(LOBART, fresh=True)
        geometry = {
            "thief_pos": thief.npc.pos,
            "owner_pos": owner.npc.pos,
            "chest_pos": chest.pos,
            "hero_pos": thief.player.pos if thief.player is not None else None,
            "thief_hero": math.dist(thief.npc.pos, thief.player.pos) if thief.player else None,
            "owner_hero": math.dist(owner.npc.pos, owner.player.pos) if owner.player else None,
            "chest_hero": math.dist(chest.pos, thief.player.pos) if thief.player else None,
            "thief_chest": math.dist(thief.npc.pos, chest.pos),
        }
        log.info("npc_will %s transfer preparation: %s", label, geometry)
        if not check(
            f"{label}_local", all(
                geometry[key] is not None and geometry[key] < SIMULATION_CM
                for key in ("thief_hero", "owner_hero", "chest_hero")
            ) and geometry["thief_chest"] <= ROUTINE_CM, geometry,
        ):
            return None
        return geometry

    async def steal(label: str, *, witnessed: bool, remember: bool = False) -> dict[str, Any] | None:
        chest = await stage_theft(label, witnessed=witnessed, remember=remember)
        if chest is None or (geometry := await local_geometry(label, chest)) is None:
            return None
        before = {str(f["id"]) for f in facts("npc_crime")}
        sent_at = len(sent)
        cards(greed=True)
        await _until(lambda: any(f.get("id") not in before for f in facts("npc_crime")), 90)
        crime = next((f for f in facts("npc_crime") if f.get("id") not in before), None)
        cards()
        cid = str(crime["id"]) if crime is not None else ""
        decisions = [f for f in facts("npc_will") if f.get("npc") == HILDA and f.get("kind") == "greed"]
        commands = actions(HILDA, "steal", sent_at)
        native = [m for m in received if isinstance(m, NpcDeed) and m.id == cid]
        witnesses = set(crime.get("witnesses", [])) if crime else set()
        completed = bool(commands) and await _until(
            lambda: any(
                isinstance(m, NpcIdle) and m.act == commands[0].id and m.reason == "done"
                for m in received
            ), 30,
        )
        if not check(
            f"{label}_autonomous_theft", crime is not None and crime.get("culprit") == HILDA
            and crime.get("owner") == LOBART and crime.get("chest") == CHEST and crime.get("count") == 1
            and len(decisions) == 1 and decisions[0].get("act") == "steal"
            and len(commands) == 1 and decisions[0].get("action") == commands[0].id
            and completed and len(native) == 1 and LOBART not in witnesses
            and ((VINO in witnesses) if witnessed else not witnesses),
            {"crime": crime, "will": decisions, "commands": [c.model_dump() for c in commands],
             "geometry": geometry},
        ):
            return None
        attempts = [a for a in will.attempts() if a.get("action") == commands[0].id]
        if not check(
            f"{label}_committed", len(attempts) == 1 and attempts[0].get("outcome") == "committed"
            and attempts[0].get("crime") == cid, attempts,
        ):
            return None
        return crime

    async def tell_owner(label: str, crime: dict[str, Any]) -> bool:
        cid = str(crime["id"])
        preparing.add(LOBART)
        # Learning creates a pending claim only after the owner has left honest simulation.
        # This near-body scenario holds its fixture until the final positions and health are ready;
        # otherwise the new distant-meeting rule can consume the claim during parking.
        if not await place(LOBART, parked):
            return False
        await rumors.tick()
        rumor = next((r for r in rumors.rumors.values() if r.subject == HILDA and cid in r.quest), None)
        if not check(f"{label}_rumor_source", rumor is not None and VINO in rumor.known):
            return False
        assert rumor is not None
        rumors.gate = lambda r, a, b: r.key == rumor.key and (a, b) == (VINO, LOBART)
        status = rumors.offer_status(rumor, VINO, LOBART)
        rumors.gate = lambda *_: False
        return check(
            f"{label}_owner_heard", status == "told"
            and any(e.subject == HILDA and not e.direct and e.source == VINO for e in people.of(LOBART)),
            {"status": status, "knowledge": people.context(LOBART)},
        )

    async def health() -> bool:
        # Set starting health once before the encounter; the actual fighting and knockout are native.
        trigger = "DIR_NPC_WILL_HEALTH"
        assignments = [(HILDA, 1, 1000), (HILDA, 0, 10), (LOBART, 1, 1000), (LOBART, 0, 1000)]
        await link.patch_graph([triggers.trigger(trigger, triggers.STATE, [
            {"do": "set", "target": ["field", ["inst", npc], "ATTRIBUTE", index], "value": ["int", value]}
            for npc, index, value in assignments
        ])])
        try:
            return await _until_async(
                lambda: link.story_eval(HILDA, [
                    ["field", ["inst", npc], "ATTRIBUTE", index] for npc, index, _ in assignments
                ]), lambda values: values == [value for _, _, value in assignments], 5,
            )
        finally:
            await link.patch_graph([triggers.removal(trigger)])

    async def encounter(label: str, crime: dict[str, Any], courage: int) -> bool:
        cid = str(crime["id"])
        voice_at, sent_at = len(received), len(sent)
        preparing.add(LOBART)
        cards(courage=courage)
        await link.debug("player_goto", waypoint=ROAD)
        if not await place(LOBART, parked) or not await place(VINO, parked) or not await place(HILDA, ROAD):
            return False
        if courage >= 3 and not check(f"{label}_health", await health()):
            return False
        before = {npc: await count(npc, str(crime["item"])) for npc in (HILDA, LOBART)}
        await link.body_watch([HILDA, LOBART], rate_hz=20)
        for rows in bodies.values():
            rows.clear()
        # Keep the hero off the direct AI_GotoNpc lane while leaving both bodies within 30 m.
        await link.debug("player_goto", waypoint=RETURN_ROAD)
        # Release the prepared character only after fresh engine positions prove the final scene.
        # A tick already in flight must not use its earlier far geometry after the fixture is free.
        await link.debug("npc_goto", npc=LOBART, to=HILDA, distance=300, face=HILDA)
        async with will._lock:
            await will.far_world.refresh(will._generation)
            cards(courage=courage)
            preparing.discard(LOBART)
        await _until(
            lambda: any(f.get("crime") == cid and f.get("npc") == LOBART for f in facts("npc_will")), 30,
        )
        decisions = [f for f in facts("npc_will") if f.get("crime") == cid and f.get("npc") == LOBART]
        expected = "fight" if courage >= 3 else "accuse"
        if not check(
            f"{label}_rule", len(decisions) == 1 and decisions[0].get("kind") == "reclaim"
            and decisions[0].get("act") == expected,
            decisions,
        ):
            return False
        if courage < 3:
            return check(
                f"{label}_accusation", await voiced(LOBART, voice_at)
                and not actions(LOBART, "fight", sent_at),
                [c.model_dump() for c in sent[sent_at:] if c.npc == LOBART],
            )
        returned = await _until(
            lambda: any(f.get("crime") == cid for f in facts("npc_crime_restitution")), 120,
        )
        item = str(crime["item"])
        return check(
            f"{label}_returned", returned and any(s["unconscious"] for s in bodies[HILDA])
            and not any(s["dead"] for s in bodies[HILDA])
            and await count(HILDA, item) == before[HILDA] - 1
            and await count(LOBART, item) == before[LOBART] + 1,
            {"restitution": facts("npc_crime_restitution"), "defense": facts("npc_crime_defense"),
             "body_samples": {npc: len(rows) for npc, rows in bodies.items()}},
        )

    link.send = sending  # type: ignore[method-assign]
    will.busy = lambda npc: npc in preparing or original_busy(npc)
    link.listeners.append(record)
    link.raw_body_listeners.append(body)
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
        await link.debug("time", hour=8, minute=0)
        population = {str(p["npc"]): p for p in await link.story_people()}
        points = {p["name"]: p for p in await link.waypoints() if not p.get("fp")}
        if not check(
            "farm", {HILDA, LOBART, VINO} <= population.keys()
            and {HOUSE, ROAD, RETURN_ROAD} <= points.keys(),
        ):
            return _logged(results)
        owner_entry = next((r for r in population[LOBART]["routine"] if r["start"] <= 480 < r["end"]), None)
        if not check("owner_day", owner_entry is not None and owner_entry["waypoint"] in points, owner_entry):
            return _logged(results)
        assert owner_entry is not None
        owner_point = str(owner_entry["waypoint"])
        origin = tuple(points[HOUSE][axis] for axis in ("x", "y", "z"))
        distant = [
            p for p in points.values() if p["name"].startswith("NW_FARM1_") and not p.get("room")
            and math.dist(tuple(p[axis] for axis in ("x", "y", "z")), origin) > 8000
        ]
        if not check("parking", bool(distant), len(distant)):
            return _logged(results)
        parked = min(distant, key=lambda p: math.dist(tuple(p[a] for a in ("x", "y", "z")), origin))["name"]
        farm = list(dict.fromkeys([
            HILDA, LOBART, VINO,
            *[p["npc"] for p in await link.story_nearby(LOBART, 7000) if p["npc"] in population],
        ]))
        symbols.update(zip(
            await link.story_eval(HILDA, [["sym", HILDA], ["sym", LOBART]]), (HILDA, LOBART), strict=True,
        ))
        await link.story_effect(LOBART, [{"do": "end"}])
        if not check(
            "baseline", not facts("npc_crime") and not facts("npc_will")
            and await _save(link, slot + 1) is not None,
        ):
            return _logged(results)

        brave = await steal("brave", witnessed=True)
        if brave is None or not await tell_owner("brave", brave) or not await encounter("brave", brave, 3):
            return _logged(results)
        await link.body_watch([])

        if not await load_clean("timid"):
            return _logged(results)
        timid = await steal("timid", witnessed=True)
        if timid is None or not await tell_owner("timid", timid) or not await encounter("timid", timid, 2):
            return _logged(results)
        await link.body_watch([])

        if not await load_clean("suspect"):
            return _logged(results)
        unknown = await steal("suspect", witnessed=False, remember=True)
        if unknown is None:
            return _logged(results)
        cid = str(unknown["id"])
        if not await place(HILDA, parked):
            return _logged(results)
        chest = next(c for c in await link.npc_chests(LOBART) if c.id == CHEST)
        await link.debug("player_goto", pos=(chest.pos[0], chest.pos[1], chest.pos[2] + 200))
        await link.debug("npc_goto", npc=LOBART, to=HERO_INSTANCE, distance=0)
        await link.debug("player_goto", waypoint=HOUSE)
        preparing.add(LOBART)
        conv = dialogue.open(LOBART, "Лобарт")
        held = await dialogue.borrow_body(conv, "scenario-npc-will-chest")
        try:
            opened = await link.act(LOBART, "use_chest", chest=CHEST, take=[], put=[], timeout=90)
            note = await chests.looked(LOBART, CHEST, opened.detail) if opened.status == "done" else ""
        finally:
            if held:
                dialogue.return_body(conv, "scenario-npc-will-chest")
        suspected = [
            f for f in facts("npc_crime_suspected") if f.get("crime") == cid and f.get("suspect") == VINO
        ]
        if not check(
            "suspect_discovery", opened.status == "done" and "пропало" in note and len(suspected) == 1,
            {"note": note, "facts": suspected},
        ):
            return _logged(results)
        sent_at, voice_at = len(sent), len(received)
        # Courage 3 cannot turn a suspicion into proof or a fight.
        cards(courage=3)
        await link.debug("player_goto", waypoint=ROAD)
        if not await place(LOBART, parked) or not await place(VINO, ROAD):
            return _logged(results)
        await link.debug("player_goto", waypoint=RETURN_ROAD)
        await link.debug("npc_goto", npc=LOBART, to=VINO, distance=300, face=VINO)
        async with will._lock:
            await will.far_world.refresh(will._generation)
            cards(courage=3)
            preparing.discard(LOBART)
        await _until(
            lambda: any(f.get("kind") == "suspect" and f.get("crime") == cid for f in facts("npc_will")), 30,
        )
        accusations = [
            f for f in facts("npc_will") if f.get("kind") == "suspect"
            and f.get("crime") == cid and f.get("target") == VINO
        ]
        if not check(
            "suspect_accusation", len(accusations) == 1 and accusations[0].get("act") == "accuse"
            and await voiced(LOBART, voice_at) and not actions(LOBART, "fight", sent_at), accusations,
        ):
            return _logged(results)

        # Restore the exact stealing opportunity less than two game days after the first theft.
        # A load must restore the cooldown and consumed pair even when both become eligible again.
        cards(greed=True, courage=3)
        kept_kinds = {
            "npc_motive", "npc_will", "npc_will_outcome", "npc_crime", "npc_crime_seen",
            "npc_crime_suspected",
        }
        kept = [f for f in facts() if f.get("fact") in kept_kinds]
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
            and [f for f in facts() if f.get("fact") in kept_kinds] == kept, len(kept),
        ):
            return _logged(results)
        before = [f for f in facts("npc_will") if f.get("npc") == HILDA and f.get("kind") == "greed"]
        deeds_before = list(facts("npc_crime"))
        checked_clock: list[int] = []
        for hour in (9, 10):
            cards()
            await link.debug("time", hour=hour, minute=0)
            # Native setDayTime resets every body to its routine. Restore the real opportunity
            # after that reset, then use a fresh game clock rather than an earlier pushed view.
            opportunity = await stage_theft(f"cooldown_{hour}", witnessed=False)
            if opportunity is None:
                return _logged(results)
            if await local_geometry(f"cooldown_{hour}", opportunity) is None:
                return _logged(results)
            clock: dict[str, int] = {}

            async def observed_hour(expected: int = hour, reading: dict[str, int] = clock) -> bool:
                views = await link.npc_view([HILDA])
                if not views or views[0].time is None:
                    return False
                reading["now"] = views[0].time
                return views[0].time % (24 * 60) // 60 == expected

            if not check(f"cooldown_{hour}_clock", await _until_async(observed_hour, bool, 5), clock):
                return _logged(results)
            cards(greed=True)
            await rumors.tick()
            await will.tick(now=clock["now"])
            checked_clock.append(clock["now"])
        check(
            "loaded_cooldown", [
                f for f in facts("npc_will") if f.get("npc") == HILDA and f.get("kind") == "greed"
            ] == before and facts("npc_crime") == deeds_before,
            {"thefts": len(deeds_before), "will": len(before), "game_minutes": checked_clock},
        )
        return _logged(results)
    finally:
        will.cards, crimes.cards = original_cards, original_crime_cards
        rumors.gate, rumors.stage = original_gate, original_stage
        dialogue.llm = original_llm
        will.busy = original_busy
        link.send = send  # type: ignore[method-assign]
        try:
            if not link.closed.is_set():
                await link.body_watch([])
        finally:
            link.listeners.remove(record)
            link.raw_body_listeners.remove(body)
