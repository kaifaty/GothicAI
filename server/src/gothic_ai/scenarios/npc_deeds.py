"""Marked NPC theft and assault, witnesses, restitution and save branches on save 1."""

from __future__ import annotations

import asyncio
import logging
import math
import time
from dataclasses import replace
from typing import TYPE_CHECKING, Any

from .. import triggers
from ..game import GameError, GameLink
from ..llm.fake import FakeBackend
from ..protocol import GameMessage, NpcActResult, NpcDeed, NpcIdle, Session
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
    from ..dialogue import DialogueManager
    from ..npc_crimes import NpcCrimes
    from ..people_knowledge import PeopleKnowledge
    from ..social import Rumors

log = logging.getLogger("gothic_ai.scenarios")
MALETH = "BAU_954_MALETH"
HOUSE, WITNESS = "NW_FARM1_INHOUSE_01", "NW_FARM1_INHOUSE_02"
ROAD, RETURN_ROAD = "NW_FARM1_PATH_04", "NW_FARM1_PATH_03"
CUP, CHEST = "ITMI_SILVERCUP", "CHEST_LOBART"
ATTACK_WITNESS = LOBART


async def npc_deeds(
    link: GameLink,
    service: NpcCrimes,
    people: PeopleKnowledge,
    rumors: Rumors,
    dialogue: DialogueManager,
    log_dir: str,
    slot: int = 7,
) -> dict[str, str]:
    """Vino steals Lobart's chest item before Hilda, knocks Maleth out, then loses stolen goods.

    The final theft is a native world-item pickup in front of its brave owner. Only the scenario's
    courage card and the initial health change: ownership, walking, chest use, hits, witnesses and
    return use production paths. Farm guards are covered by NpcCrimes' module tests.
    """
    results: dict[str, str] = {}
    facts = service.chronicle.facts
    original_cards, original_llm = service.cards, dialogue.llm
    original_gate, original_stage = rumors.gate, rumors.stage
    received: list[NpcDeed | NpcActResult | NpcIdle] = []
    transfer_sight: dict[str, asyncio.Task[dict[str, Any]]] = {}
    body_states: dict[str, list[dict[str, Any]]] = {VINO: [], MALETH: [], LOBART: []}
    symbols: dict[int, str] = {}
    body_latest: dict[str, dict[str, Any]] = {}
    entities: dict[int, str] = {}
    defense_started: float | None = None
    defense_game_start = 0
    defense_task: asyncio.Task[None] | None = None
    defense_pending: set[asyncio.Task[None]] = set()
    defense_stop = asyncio.Event()
    defense_lock = asyncio.Lock()
    defense_samples: list[dict[str, Any]] = []
    defense_hp: dict[str, int] = {}
    defense_hp_changes: list[dict[str, Any]] = []
    defense_contacts: list[dict[str, Any]] = []
    defense_inventory: dict[str, dict[str, int]] = {}
    defense_event_ids: set[int] = set()
    defense_counts = {
        npc: {"applied_contacts": 0, "positive_hits_received": 0, "no_loss_hits_received": 0,
              "blocked": 0, "evaded": 0, "unconscious_events": 0, "death_events": 0}
        for npc in (VINO, LOBART)
    }

    def check(name: str, ok: bool, detail: object = "") -> bool:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"
        log.info("npc_deeds %-24s %s", name, results[name])
        return ok

    def record(message: GameMessage) -> None:
        if isinstance(message, NpcDeed | NpcActResult | NpcIdle):
            received.append(message)
        if isinstance(message, NpcDeed):
            transfer_sight[message.id] = asyncio.create_task(at_transfer(message))

    async def at_transfer(deed: NpcDeed) -> dict[str, Any]:
        # Read the real ray and poses on receipt, without turning anyone or delivering a perception.
        # This distinguishes the opening location from the actor's earlier waypoint and heading.
        witness = ATTACK_WITNESS if deed.kind == "attack" else (HILDA if deed.chest else LOBART)
        native = await link.story_eval(witness, [
            ["call", "NPC_CANSEENPC", ["inst", "SELF"], ["inst", VINO]],
            ["call", "NPC_CANSEENPCFREELOS", ["inst", "SELF"], ["inst", VINO]],
            ["call", "NPC_GETDISTTONPC", ["inst", "SELF"], ["inst", VINO]],
        ])
        views = await link.npc_view([witness, VINO])
        poses = {}
        for npc in (witness, VINO):
            snapshot = await link.snapshot(npc, fresh=True)
            poses[npc] = {"pos": snapshot.npc.pos, "rot": snapshot.npc.rot}
        detail = {
            "witness": witness, "deed_position": deed.position.model_dump(),
            "native_seen_free_los_distance": native,
            "views": [view.model_dump() for view in views], "poses": poses,
        }
        scene = "attack" if deed.kind == "attack" else ("chest" if deed.chest else "world")
        log.info("npc_deeds %s transfer sight: %s", scene, detail)
        return detail

    def observe(packet: dict[str, Any]) -> None:
        for row in packet["observations"]:
            if (npc := symbols.get(row["script_symbol"])) in body_states:
                body_states[npc].append({"t_game_ms": row["t_game_ms"], **row["self"]})
                body_latest[npc] = row
                entities[row["entity_id"]] = npc
            if defense_started is None or npc not in (VINO, LOBART):
                continue
            hp = row["self"]["hp"]
            before, defense_hp[npc] = defense_hp.get(npc, hp), hp
            if before != hp:
                change = {
                    "npc": npc, "game_ms": row["t_game_ms"], "hp": [before, hp],
                    "body_state": row["self"]["body_state_base"], "goal": row["goal"],
                    "target": row["target"],
                }
                defense_hp_changes.append(change)
                log.info("npc_deeds defense hp: %s", change)
                if hp > before:
                    pending = asyncio.create_task(sample_defense("hp_increase", change))
                    defense_pending.add(pending)
                    pending.add_done_callback(defense_pending.discard)
            for event in row["events"]:
                if (
                    event["perspective"] != "self" or event["event_id"] in defense_event_ids
                    or event["source_game_ms"] < defense_game_start
                ):
                    continue
                defense_event_ids.add(event["event_id"])
                counter = {
                    3: "applied_contacts", 7: "blocked", 8: "evaded",
                    9: "positive_hits_received", 10: "no_loss_hits_received",
                    11: "death_events", 12: "unconscious_events",
                }.get(event["code"])
                if counter is None:
                    continue
                defense_counts[npc][counter] += 1
                contact = {
                    "npc": npc, "event": event, "meaning": counter, "hp": hp,
                    "native_target": entities.get(row["goal"]["target_id"]),
                    "target_id": row["goal"]["target_id"], "counts": dict(defense_counts[npc]),
                }
                defense_contacts.append(contact)
                log.info("npc_deeds defense contact: %s", contact)

    async def sample_defense(label: str, cause: dict[str, Any] | None = None) -> None:
        """Read both native bodies and inventories; never heal, equip or steer either fighter."""
        async with defense_lock:
            try:
                views = {view.npc: view for view in await link.npc_view([VINO, LOBART])}
                actors = {}
                positions = {}
                for npc, opponent in ((VINO, LOBART), (LOBART, VINO)):
                    snap = await link.snapshot(npc, fresh=True)
                    positions[npc] = snap.npc.pos
                    view = views.get(npc)
                    native = await link.story_eval(npc, [
                        ["field", ["inst", "SELF"], "ATTRIBUTE", 0],
                        ["field", ["inst", "SELF"], "ATTRIBUTE", 1],
                        ["field", ["inst", "SELF"], "ATTRIBUTE", 6],
                        ["call", "NPC_GETACTIVESPELL", ["inst", "SELF"]],
                        ["call", "NPC_GETLASTHITSPELLID", ["inst", "SELF"]],
                    ])
                    inventory = {item.item: item.count for item in snap.npc.inventory}
                    previous = defense_inventory.get(npc)
                    changed = {
                        item: inventory.get(item, 0) - previous.get(item, 0)
                        for item in inventory.keys() | previous.keys()
                        if inventory.get(item, 0) != previous.get(item, 0)
                    } if previous is not None else {}
                    defense_inventory[npc] = inventory
                    row = body_latest.get(npc, {})
                    target = row.get("target", {})
                    actors[npc] = {
                        "pos": snap.npc.pos, "hp": snap.npc.hp, "hp_max": snap.npc.hp_max,
                        "state": view.state if view is not None else None,
                        "body": view.body if view is not None else None,
                        "weapon": view.weapon if view is not None else None,
                        "in_hands": (
                            view.in_hands.model_dump() if view is not None and view.in_hands else None
                        ),
                        "native_target": entities.get(row.get("goal", {}).get("target_id", 0)),
                        "target_id": row.get("goal", {}).get("target_id"),
                        "target_is_opponent": (
                            entities.get(row.get("goal", {}).get("target_id", 0)) == opponent
                        ),
                        "native_hp_max_regen": native[:3], "active_last_hit_spell": native[3:],
                        "counts": dict(defense_counts[npc]),
                        "attack_anim": row.get("self", {}).get("attack_anim"),
                        "target_visible": target.get("visible"),
                        "fight_distance_cm": target.get("fight_distance_cm"),
                        "melee_line_clear": target.get("melee_line_clear"),
                        "inventory": inventory, "inventory_delta": changed,
                    }
                detail = {
                    "label": label, "elapsed_s": round(time.monotonic() - (defense_started or 0), 3),
                    "distance_cm": math.dist(positions[VINO], positions[LOBART]), "actors": actors,
                    "defense": facts("npc_crime_defense"), **({"cause": cause} if cause else {}),
                }
                defense_samples.append(detail)
                log.info("npc_deeds defense sample: %s", detail)
            except (GameError, TimeoutError, ConnectionError) as err:
                log.info("npc_deeds defense sample refused: %s", err)

    async def monitor_defense() -> None:
        while not defense_stop.is_set():
            started = time.monotonic()
            await sample_defense("periodic")
            try:
                await asyncio.wait_for(defense_stop.wait(), max(0, 1.5 - (time.monotonic() - started)))
            except TimeoutError:
                pass

    def deed_crime(kind: str, after: set[str]) -> dict[str, Any] | None:
        return next((
            f for f in facts("npc_crime")
            if f.get("id") not in after and f.get("kind") == kind and f.get("culprit") == VINO
        ), None)

    def seen(cid: str) -> set[str]:
        return {str(f["npc"]) for f in facts("npc_crime_seen") if f.get("crime") == cid}

    def receipts(action: str) -> list[dict[str, Any]]:
        return [
            m.model_dump() for m in received
            if isinstance(m, NpcActResult) and m.action == action
        ]

    async def completed(action: str, *, timeout: float = 60) -> bool:
        def finished() -> bool:
            accepted = any(
                isinstance(m, NpcActResult) and m.id == action and m.status == "queued" for m in received
            )
            return accepted and any(
                isinstance(m, NpcIdle) and m.act == action and m.reason == "done" for m in received
            )

        return await _until(finished, timeout)

    async def voiced(npc: str, after: int) -> bool:
        def finished() -> bool:
            queued = {
                m.id for m in received[after:]
                if isinstance(m, NpcActResult) and m.npc == npc and m.action == "say_svm"
                and m.status == "queued"
            }
            return any(
                isinstance(m, NpcIdle) and m.act in queued and m.reason == "done"
                for m in received[after:]
            )

        return await _until(finished, 30)

    async def place(npc: str, waypoint: str, **kw: Any) -> bool:
        # Parked bodies are beyond native perception processing; only the active scene requires
        # its callback. Their native state, posture, weapon and health are still checked.
        if waypoint == parked:
            kw.setdefault("require_perception_ready", False)
        ok, detail = await prepare_npc_pose(link, npc, waypoint, **kw)
        if not ok:
            check("pose", False, detail)
        return ok

    async def visible(label: str, witness: str, target: str, **kw: Any) -> bool:
        ok, detail = await wait_npc_visibility(link, witness, target, **kw)
        return check(label, ok, detail)

    async def count(npc: str, item: str) -> int:
        return int(await link.evaluate(["call", "NPC_HASITEMS", ["inst", npc], ["sym", item]]))

    async def health(label: str, values: dict[str, int]) -> bool:
        # A one-shot preparation trigger sets initial health; it is removed before any fight.
        maximum = {npc: (await link.snapshot(npc, fresh=True)).npc.hp_max for npc in values}
        fields = [["field", ["inst", npc], "ATTRIBUTE", slot] for npc in values for slot in (1, 0)]
        wanted = [amount for npc, hp in values.items() for amount in (max(maximum[npc], hp), hp)]
        trigger = f"DIR_NPC_DEEDS_{label.upper()}_HEALTH"
        await link.patch_graph([triggers.trigger(trigger, triggers.STATE, [
            {"do": "set", "target": field, "value": ["int", hp]}
            for field, hp in zip(fields, wanted, strict=True)
        ])])
        try:
            applied = await _until_async(
                lambda: link.story_eval(VINO, fields), lambda got: got == wanted, 5,
            )
        finally:
            await link.patch_graph([triggers.removal(trigger)])
        snapshots = {npc: (await link.snapshot(npc, fresh=True)).npc.hp for npc in values}
        return check(f"{label}_health", applied and snapshots == values, snapshots)

    async def native_drop() -> bool:
        try:
            await link.debug("npc_drop", npc=LOBART, item=CUP)
        except GameError as err:
            if "could not drop it now" not in str(err):
                raise
            return False
        return True

    link.listeners.append(record)
    link.raw_body_listeners.append(observe)
    try:
        await link.wait_world()
        if link.session is None:
            await wait_message(link, Session, timeout=10)
        if not check(
            "enabled", service.enabled and isinstance(original_llm, FakeBackend),
            "npc crimes enabled, LLM=fake; logs: " + log_dir,
        ):
            return _logged(results)
        dialogue.llm = FakeBackend("Ну, как знаешь.", delay=0)
        rumors.gate, rumors.stage = lambda *_: False, None
        await link.debug("player_hp", hp=5000)
        await link.debug("time", hour=8, minute=0)
        population = await link.story_people()
        humans = {str(p["npc"]) for p in population}
        participants = (LOBART, HILDA, VINO, MALETH)
        points = {p["name"]: p for p in await link.waypoints() if not p.get("fp")}
        if not check(
            "farm", set(participants) <= humans and {HOUSE, WITNESS, ROAD, RETURN_ROAD} <= points.keys()
            and not service.guard.body(VINO) and not service.guard.body(MALETH),
            {"thief": VINO, "victim": MALETH, "roads": [ROAD, RETURN_ROAD]},
        ):
            return _logged(results)
        symbols.update(zip(
            await link.story_eval(VINO, [["sym", VINO], ["sym", MALETH], ["sym", LOBART]]),
            (VINO, MALETH, LOBART), strict=True,
        ))
        origin = (points[HOUSE]["x"], points[HOUSE]["y"], points[HOUSE]["z"])
        far = [
            p for p in points.values() if p["name"].startswith("NW_FARM1_") and not p.get("room")
            and math.dist((p["x"], p["y"], p["z"]), origin) > 8000
        ]
        if not check("parking", bool(far), len(far)):
            return _logged(results)
        parked = min(far, key=lambda p: math.dist((p["x"], p["y"], p["z"]), origin))["name"]
        farm = list(dict.fromkeys([
            *participants,
            *[p["npc"] for p in await link.story_nearby(LOBART, 7000) if p["npc"] in humans],
        ]))
        await link.story_effect(LOBART, [{"do": "end"}])
        await link.debug("player_goto", waypoint=HOUSE)
        for npc in farm:
            if not await place(npc, parked):
                return _logged(results)
        hero_facts = [f for f in facts() if f.get("fact") in {"crime", "crime_seen"}]

        # Hilda alone witnesses the actual transfer out of Lobart's chest. The command replaces
        # Vino's standing activity with native walking, opening, taking and closing.
        chests = await link.npc_chests(LOBART)
        chest = next((c for c in chests if c.id == CHEST and c.items), None)
        if not check("chest", chest is not None, [(c.id, c.why) for c in chests]):
            return _logged(results)
        assert chest is not None
        item = next((i for i in chest.items if i.item == "ITMI_GOLD"), chest.items[0]).item
        if not check("chest_endpoint", chest.waypoint in points, chest.waypoint):
            return _logged(results)
        assert chest.waypoint is not None
        # The beds occlude the ray from INHOUSE_02 to this chest. Prepare Hilda in the same
        # part of the room, first behind the future thief, keeping her clear of the opening.
        # Prove the native ray there, then move only him back to the door. Her initialized
        # state and heading stay intact while he walks and opens it natively.
        if not await place(VINO, chest.waypoint):
            return _logged(results)
        witness_ready, attempts = False, []
        for offset in (-300, 300, -450, 450):
            posed, pose = await prepare_npc_pose(
                link, HILDA, chest.waypoint, to=VINO, distance=offset, face=VINO,
            )
            line_of_sight: dict[str, Any] = {}
            if posed:
                witness_ready, line_of_sight = await wait_npc_visibility(link, HILDA, VINO)
            attempt = {"offset": offset, "pose": pose, "visibility": line_of_sight}
            attempts.append(attempt)
            log.info("npc_deeds chest witness placement: %s", attempt)
            if witness_ready:
                break
        if not check("chest_witness_ready", witness_ready, attempts):
            return _logged(results)
        if not await place(VINO, HOUSE):
            return _logged(results)
        owned_before = await count(VINO, item)
        prior = {str(f["id"]) for f in facts("npc_crime")}
        voice_after = len(received)
        take = await link.npc_act(VINO, "steal", chest=chest.id, what=item, count=1, deed=True)
        await _until(lambda: deed_crime("theft", prior), 60)
        crime = deed_crime("theft", prior)
        cid = str(crime["id"]) if crime is not None else ""
        deeds = [m for m in received if isinstance(m, NpcDeed) and m.id == cid]
        sight = await transfer_sight[cid] if cid in transfer_sight else None
        if not check(
            "chest_theft", crime is not None and crime.get("owner") == LOBART
            and crime.get("chest") == CHEST and crime.get("item") == item and crime.get("count") == 1
            and HILDA in crime.get("witnesses", []) and seen(cid) == set(crime.get("witnesses", []))
            and len(deeds) == 1 and deeds[0].culprit == VINO and await completed(take)
            and await count(VINO, item) == owned_before + 1,
            {"crime": crime, "deeds": [d.model_dump() for d in deeds], "actions": receipts("steal"),
             "transfer_sight": sight},
        ):
            return _logged(results)
        knowledge = people.context(HILDA) or ""
        discoveries = [e for e in people.of(HILDA) if e.subject == VINO and e.direct]
        await rumors.tick()
        rumor = next((
            r for r in rumors.rumors.values()
            if r.subject == VINO and r.known.get(HILDA) == 0 and cid in r.quest
        ), None)
        if not check(
            "hilda_knowledge", bool(discoveries) and "Что ты знаешь о людях вокруг" in knowledge
            and any(e.subject_name in knowledge for e in discoveries) and rumor is not None,
            {"context": knowledge, "source": rumor.key if rumor else None},
        ):
            return _logged(results)
        if not check("hilda_shout", await voiced(HILDA, voice_after), receipts("say_svm")):
            return _logged(results)
        assert rumor is not None
        rumors.gate = lambda r, a, b: r.key == rumor.key and (a, b) == (HILDA, LOBART)
        passed = rumors.offer_status(rumor, HILDA, LOBART)
        if not check(
            "npc_rumor", passed == "told" and rumor.known.get(LOBART) == 1
            and any(e.subject == VINO and not e.direct and e.source == HILDA for e in people.of(LOBART)),
            {"status": passed, "context": people.context(LOBART)},
        ):
            return _logged(results)
        rumors.gate = lambda *_: False

        # Both fighters stand on the same connected stretch of road. Native AI_GotoNpc has no
        # waynet detour, so the relative placement is short and its ray is checked before combat.
        await link.debug("player_goto", waypoint=ROAD)
        if not await place(HILDA, parked) or not await place(VINO, ROAD):
            return _logged(results)
        if not await place(MALETH, ROAD, to=VINO, distance=250, face=VINO):
            return _logged(results)
        # Leave room for WATCHFIGHT's native 300 cm dodge while keeping a 550 cm margin
        # to the victim-centred damage perception range when the victim retreats.
        if not await place(ATTACK_WITNESS, ROAD, to=MALETH, distance=-450, face=VINO):
            return _logged(results)
        if not await visible("fight_lane_ready", MALETH, VINO):
            return _logged(results)
        if not await health("attack", {VINO: 1000, MALETH: 100}):
            return _logged(results)
        # Keep the passive hero off both fighting capsules before their native movement begins.
        await link.debug("player_goto", waypoint=RETURN_ROAD)
        attack_poses = {
            npc: (await link.snapshot(npc, fresh=True)).npc for npc in (VINO, MALETH, ATTACK_WITNESS)
        }
        player = (await link.snapshot(VINO, fresh=True)).player
        clearance = {
            npc: math.dist(player.pos, pose.pos) for npc, pose in attack_poses.items()
        } if player is not None else {}
        if not check(
            "attack_lane_clear", bool(clearance) and min(clearance[VINO], clearance[MALETH]) >= 700
            and max(clearance.values()) <= 2000,
            {"hero": player.pos if player is not None else None, "clearance": clearance,
             "npcs": {npc: pose.pos for npc, pose in attack_poses.items()}},
        ):
            return _logged(results)
        await link.body_watch([VINO, MALETH], rate_hz=20)
        body_states[VINO].clear()
        body_states[MALETH].clear()
        prior = {str(f["id"]) for f in facts("npc_crime")}
        deeds_before = {message.id for message in received if isinstance(message, NpcDeed)}
        # STAND_WP does not register ASSESSOTHERSDAMAGE. A real, unmarked fight sound makes the
        # native observer enter WATCHFIGHT with its own OTHER/VICTIM and B_CheerFight callback.
        # Keep the initialized observer and the fighters' native states and positions through warmup.
        warmup = await link.npc_act(
            VINO, "fight", npc=MALETH, reason="AR_NONE", nonlethal=True, deed=False,
        )
        await _until(
            lambda: any(isinstance(m, NpcActResult) and m.id == warmup for m in received), 5,
        )
        if not check(
            "warmup_queued", any(
                isinstance(m, NpcActResult) and m.id == warmup and m.status == "queued" for m in received
            ), [m.model_dump() for m in received if isinstance(m, NpcActResult) and m.id == warmup],
        ):
            return _logged(results)
        ready, preparation = await wait_npc_visibility(
            link, ATTACK_WITNESS, VINO, perception="others_damage", state="ZS_WATCHFIGHT",
            bodies=("stand", "walk", "run"), range_target=MALETH, timeout=120,
        )
        victim = (await link.snapshot(MALETH, fresh=True)).npc
        victim_views = await link.npc_view([MALETH])
        standing = (
            victim.hp > 1 and bool(victim_views) and victim_views[0].body not in {"dead", "down"}
            and not any(s["down"] or s["dead"] or s["unconscious"] for s in body_states[MALETH])
        )
        if not check(
            "warmup_unmarked", {str(f["id"]) for f in facts("npc_crime")} == prior
            and {message.id for message in received if isinstance(message, NpcDeed)} == deeds_before,
            {"action": warmup, "crimes_before": len(prior), "crimes_after": len(facts("npc_crime"))},
        ):
            return _logged(results)
        if not check(
            "attack_witness_ready", ready and standing,
            {"witness": preparation, "victim_hp": victim.hp, "victim_still_up": standing,
             "victim_views": [view.model_dump() for view in victim_views]},
        ):
            return _logged(results)
        # Prepare the weak target once, before the marked command. This changes no native state,
        # position, weapon or queued action, and there are no health writes during the marked fight.
        if not await health("marked_attack", {MALETH: 10}):
            return _logged(results)
        ready, preparation = await wait_npc_visibility(
            link, ATTACK_WITNESS, VINO, perception="others_damage", state="ZS_WATCHFIGHT",
            bodies=("stand", "walk", "run"), range_target=MALETH, timeout=10,
        )
        victim = (await link.snapshot(MALETH, fresh=True)).npc
        victim_views = await link.npc_view([MALETH])
        if not check(
            "marked_attack_ready", ready and victim.hp > 1 and bool(victim_views)
            and victim_views[0].body not in {"down", "dead"},
            {"witness": preparation, "victim_hp": victim.hp,
             "victim_views": [view.model_dump() for view in victim_views]},
        ):
            return _logged(results)
        fight = await link.npc_act(VINO, "fight", npc=MALETH, reason="AR_NONE", deed=True)

        async def attack_summary() -> dict[str, Any]:
            return {
                "hp_min_max": {
                    npc: [min((s["hp"] for s in body_states[npc]), default=0),
                          max((s["hp"] for s in body_states[npc]), default=0)]
                    for npc in (VINO, MALETH)
                },
                "views": [view.model_dump() for view in await link.npc_view([VINO, MALETH, ATTACK_WITNESS])],
                "native_targets": {
                    npc: {"goal": body_latest.get(npc, {}).get("goal"),
                          "target": body_latest.get(npc, {}).get("target")}
                    for npc in (VINO, MALETH)
                },
                "events": [
                    message.model_dump() for message in received
                    if (isinstance(message, NpcActResult) and message.id in {warmup, fight})
                    or (isinstance(message, NpcIdle) and message.act in {warmup, fight})
                ],
            }

        await _until(lambda: deed_crime("attack", prior), 120)
        attack = deed_crime("attack", prior)
        aid = str(attack["id"]) if attack is not None else ""
        attack_sight = await transfer_sight[aid] if aid in transfer_sight else None
        if not check(
            "attack", attack is not None and attack.get("victim") == MALETH
            and ATTACK_WITNESS in attack.get("witnesses", []) and VINO not in attack.get("witnesses", [])
            and MALETH not in attack.get("witnesses", []) and seen(aid) == set(attack.get("witnesses", [])),
            {"crime": attack, "actions": receipts("fight"), "actual_sight": attack_sight,
             "summary": await attack_summary()},
        ):
            return _logged(results)
        down = await _until(lambda: any(s["unconscious"] and not s["dead"] for s in body_states[MALETH]), 120)
        down_at = time.monotonic()
        ended = await completed(fight, timeout=20) if down else False
        # Observe beyond the state exit too: AR_NONE must never turn the defeated target into a
        # finishing animation or a death after the first unconscious sample.
        observed = await _until(lambda: time.monotonic() - down_at >= 5, 6) if ended else False
        maleth = (await link.snapshot(MALETH, fresh=True)).npc
        if not check(
            "nonlethal", down and ended and observed and maleth.hp > 0
            and not any(s["dead"] for s in body_states[MALETH]),
            {"hp": maleth.hp, "last": body_states[MALETH][-3:], "idle": ended,
             "summary": await attack_summary()},
        ):
            return _logged(results)
        await link.body_watch([])

        # A second physical theft covers a world item's native owner. Lobart's courage alone is
        # raised to 3; he sees the pickup himself, defeats the thief and reclaims through the service.
        service.cards = {**original_cards, LOBART: replace(original_cards[LOBART], courage=3)}
        await link.debug("player_goto", waypoint=RETURN_ROAD)
        if not await place(HILDA, parked) or not await place(VINO, RETURN_ROAD):
            return _logged(results)
        if not await place(LOBART, RETURN_ROAD, to=VINO, distance=300, face=VINO):
            return _logged(results)
        if not check("owned_cup", await _until_async(native_drop, bool, 10), "native Lobart drop"):
            return _logged(results)
        # The passive hero must not share the thief's capsule or obstruct the item approach.
        # The adjacent road point keeps both active bodies near the hero without occupying their lane.
        await link.debug("player_goto", waypoint=ROAD)
        lane = await link.snapshot(VINO, fresh=True)
        owner_pose = (await link.snapshot(LOBART, fresh=True)).npc
        hero_clearance = {
            npc: math.dist(lane.player.pos, position)
            for npc, position in ((VINO, lane.npc.pos), (LOBART, owner_pose.pos))
        } if lane.player is not None else {}
        if not check(
            "reclaim_lane_clear", bool(hero_clearance) and min(hero_clearance.values()) >= 700,
            {"hero": lane.player.pos if lane.player is not None else None,
             "thief": lane.npc.pos, "owner": owner_pose.pos, "hero_clearance": hero_clearance},
        ):
            return _logged(results)
        if not await visible("owner_witness_ready", LOBART, VINO):
            return _logged(results)
        if not await health("reclaim", {VINO: 10, LOBART: 1000}):
            return _logged(results)
        thief_cups, owner_cups = await count(VINO, CUP), await count(LOBART, CUP)
        prior = {str(f["id"]) for f in facts("npc_crime")}
        body_latest.pop(VINO, None)
        body_latest.pop(LOBART, None)
        await link.body_watch([VINO, LOBART], rate_hz=20)
        body_states[VINO].clear()
        body_states[LOBART].clear()
        await _until(lambda: {VINO, LOBART} <= body_latest.keys(), 5)
        defense_game_start = max((row["t_game_ms"] for row in body_latest.values()), default=0)
        defense_started = time.monotonic()
        await sample_defense("before_theft")
        defense_task = asyncio.create_task(monitor_defense())
        target = (await link.story_state(deed_target={
            "npc": VINO, "item": CUP, "what": CUP, "count": 1, "deed": True,
        })).get("deed_target", {})
        if not check(
            "world_target", target.get("owner") == LOBART and target.get("item") == CUP
            and target.get("count") == 1 and not target.get("mission") and not target.get("error"), target,
        ):
            return _logged(results)
        [hero_cups] = await link.story_eval(VINO, [
            ["call", "NPC_HASITEMS", ["inst", "OTHER"], ["sym", CUP]],
        ])
        if not check("hero_has_no_cup", hero_cups == 0, {"hero_cups": hero_cups}):
            return _logged(results)
        world_take = await link.npc_act(VINO, "steal", item=CUP, what=CUP, count=1, deed=True)
        await _until(lambda: deed_crime("theft", prior), 30)
        theft = deed_crime("theft", prior)
        tid = str(theft["id"]) if theft is not None else ""
        sight = await transfer_sight[tid] if tid in transfer_sight else None
        after_take = await link.snapshot(VINO, fresh=True)
        world_detail = {
            "crime": theft, "action": world_take, "preflight": target, "transfer_sight": sight,
            "events": [
                message.model_dump() for message in received
                if (isinstance(message, NpcActResult) and message.id == world_take)
                or (isinstance(message, NpcIdle) and message.act == world_take)
            ],
            "views": [view.model_dump() for view in await link.npc_view([VINO, LOBART])],
            "npc_snapshot": after_take.npc.model_dump(),
            "hero_snapshot": after_take.player.model_dump() if after_take.player is not None else None,
            "thief_cups": [thief_cups, await count(VINO, CUP)],
            "owner_cups": [owner_cups, await count(LOBART, CUP)],
        }
        log.info("npc_deeds world transfer: %s", world_detail)
        if not check(
            "world_theft", theft is not None and theft.get("owner") == LOBART
            and theft.get("item") == CUP and LOBART in theft.get("witnesses", [])
            and seen(tid) == set(theft.get("witnesses", [])), world_detail,
        ):
            return _logged(results)
        await _until(
            lambda: any(f.get("crime") == tid for f in facts("npc_crime_restitution"))
            or any(f.get("crime") == tid for f in facts("npc_crime_defense")), 120,
        )
        returned = [f for f in facts("npc_crime_restitution") if f.get("crime") == tid]
        reclaimed = bool(returned)
        decisions = [f for f in facts("npc_crime_reaction") if f.get("crime") == tid]
        await sample_defense("restitution_check")
        owner_won = (
            reclaimed and len(returned) == 1
            and returned[0].get("npc") == LOBART and returned[0].get("culprit") == VINO
            and returned[0].get("item") == CUP and returned[0].get("count") == 1
            and any(f.get("npc") == LOBART and f.get("reaction") == "fight" for f in decisions)
            and any(s["unconscious"] and not s["dead"] for s in body_states[VINO])
            and not any(s["dead"] for s in body_states[VINO])
            and await count(VINO, CUP) == thief_cups and await count(LOBART, CUP) == owner_cups + 1
        )
        recovery_observed = False
        if owner_won:
            def healed_after_down() -> bool:
                down = next((s for s in body_states[VINO] if s["unconscious"] and s["hp"] == 1), None)
                return down is not None and any(
                    s["t_game_ms"] > down["t_game_ms"] and s["hp"] >= 51 for s in body_states[VINO]
                )

            # Observe the native get-up and healing path without changing either body or HP.
            # Its HP and inventory evidence explain recovery only when it actually happens.
            recovery_observed = await _until(healed_after_down, 40)
            await sample_defense("after_recovery", {"healing_observed": recovery_observed})
            log.info("npc_deeds native recovery: observed=%s", recovery_observed)
        defense_stop.set()
        if defense_task is not None:
            await defense_task
            defense_task = None
        if defense_pending:
            await asyncio.gather(*defense_pending, return_exceptions=True)
        summary = {}
        for npc in (VINO, LOBART):
            rows = body_states[npc]
            down = next((s for s in rows if s["down"] or s["unconscious"]), None)
            up = next((
                s for s in rows if down is not None and s["t_game_ms"] > down["t_game_ms"]
                and not s["down"] and not s["unconscious"]
            ), None)
            damage = next((
                c for c in defense_contacts if c["npc"] == npc and c["meaning"] == "positive_hits_received"
            ), None)
            summary[npc] = {
                "counts": defense_counts[npc], "samples": len(rows),
                "first_damage_game_ms": damage["event"]["source_game_ms"] if damage else None,
                "first_down_game_ms": down["t_game_ms"] if down else None,
                "first_up_game_ms": up["t_game_ms"] if up else None,
                "hp_min_max": [min((s["hp"] for s in rows), default=0),
                               max((s["hp"] for s in rows), default=0)],
                "attack_animation_samples": sum(bool(s["attack_anim"]) for s in rows),
                "hp_changes": [change for change in defense_hp_changes if change["npc"] == npc],
            }
        log.info("npc_deeds defense summary: %s", summary)
        defense_started = None
        if not check(
            "owner_reclaim", owner_won
            and not any(s["dead"] for s in body_states[VINO])
            and await count(VINO, CUP) == thief_cups and await count(LOBART, CUP) == owner_cups + 1,
            {"reaction": decisions, "returned": returned, "last": body_states[VINO][-3:],
             "contact_counts": defense_counts, "hp_changes": defense_hp_changes,
             "healing_observed": recovery_observed,
             "summary": summary, "telemetry": defense_samples[-3:],
             "defense": facts("npc_crime_defense")},
        ):
            return _logged(results)
        await link.body_watch([])
        check(
            "hero_unchanged", [f for f in facts() if f.get("fact") in {"crime", "crime_seen"}] == hero_facts,
        )
        check("guard_coverage", True, "farm scene uses brave owner; community guard covered by module tests")

        kinds = {
            "npc_crime", "npc_crime_seen", "npc_crime_reaction", "npc_crime_restitution",
            "npc_crime_defense", "rumor",
        }
        kept = [f for f in facts() if f.get("fact") in kinds]
        knowledge_before = {npc: people.of(npc) for npc in (HILDA, LOBART)}
        inventory_before = {npc: await count(npc, CUP) for npc in (VINO, LOBART)}
        saved = await _save(link, slot)
        if not check("saved", saved is not None, len(kept)):
            return _logged(results)
        assert saved is not None
        loading = asyncio.create_task(wait_message(link, Session, lambda m: m.reason == "load", 60))
        await asyncio.sleep(0)
        await link.debug("load", slot)
        loaded = await loading
        await link.wait_world()
        check(
            "loaded", loaded.parent == saved.branch and loaded.parent_seq == saved.seq
            and [f for f in facts() if f.get("fact") in kinds] == kept
            and {npc: people.of(npc) for npc in (HILDA, LOBART)} == knowledge_before
            and {npc: await count(npc, CUP) for npc in (VINO, LOBART)} == inventory_before,
            {"crimes": len(facts("npc_crime")), "witnesses": len(facts("npc_crime_seen")),
             "restitution": len(facts("npc_crime_restitution"))},
        )
        return _logged(results)
    finally:
        defense_stop.set()
        if defense_task is not None:
            await defense_task
        if defense_pending:
            await asyncio.gather(*defense_pending, return_exceptions=True)
        for pending in transfer_sight.values():
            if not pending.done():
                pending.cancel()
        if transfer_sight:
            await asyncio.gather(*transfer_sight.values(), return_exceptions=True)
        service.cards, dialogue.llm = original_cards, original_llm
        rumors.gate, rumors.stage = original_gate, original_stage
        try:
            if not link.closed.is_set():
                await link.body_watch([])
        finally:
            link.listeners.remove(record)
            link.raw_body_listeners.remove(observe)
