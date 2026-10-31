"""Rule-based reactions to a witnessed theft on save 1, without a behavioral model."""

from __future__ import annotations

import asyncio
import logging
import math
from dataclasses import replace
from typing import TYPE_CHECKING, Any

from ..game import GameError, GameLink
from ..protocol import (
    HeroDeed,
    HoldVerdictCmd,
    NpcActCmd,
    NpcActResult,
    NpcIdle,
    PerceptionHeld,
    Session,
    ThreatControlCmd,
)
from .common import HILDA, LOBART, _logged, _save, _until, _until_async, wait_message
from .crimes import HOUSE

if TYPE_CHECKING:
    from ..crime_react import CrimeReact
    from ..crimes import Crimes

log = logging.getLogger("gothic_ai.scenarios")
CUP = "ITMI_SILVERCUP"
HERO_POINT = "NW_FARM1_INHOUSE_01"  # the house's walkable entrance, clear of the beds
WITNESS_POINT = "NW_FARM1_INHOUSE_02"


async def crime_react(
    link: GameLink,
    service: Crimes,
    reactions: CrimeReact,
    slot: int = 7,
    *,
    hold_threats: bool = False,
) -> dict[str, str]:
    """Exercise native ownership, retained witnesses, SVM, theft combat and the vanilla switch.

    Hilda's real card chooses a shout. Lobart's real courage is 2: first check his demand, then
    temporarily use courage 3 to exercise the owner's fighting branch. Only that test card changes;
    ownership, perceptions, damage, unconsciousness and restitution use the game's normal paths.
    Each scene reloads the same clean parent, including the scripts' memories and AI state.
    Community guards and their immediate law report are covered by the module's unit tests.
    """
    results: dict[str, str] = {}
    facts = service.chronicle.facts
    sent: list[Any] = []
    received: list[Any] = []
    hero_states: list[dict[str, Any]] = []
    original_cards, send = reactions.cards, link.send
    hero_cups_before = owner_cups_before = 0

    def check(name: str, ok: bool, detail: object = "") -> bool:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"
        log.info("crime_react %-24s %s", name, results[name])
        return ok

    async def sending(command: Any) -> None:
        sent.append(command)
        await send(command)

    def record(message: object) -> None:
        if isinstance(message, HeroDeed | PerceptionHeld | NpcActResult | NpcIdle):
            received.append(message)

    def body(packet: dict[str, Any]) -> None:
        # Only PLAYER is watched, without taking its body or choosing an action for it.
        hero_states.extend(row["self"] for row in packet["observations"])

    def holds(cid: str, npc: str) -> list[PerceptionHeld]:
        return [m for m in received if isinstance(m, PerceptionHeld) and m.deed == cid and m.npc == npc]

    def decisions(cid: str, npc: str) -> list[dict[str, Any]]:
        return [f for f in facts("crime_reaction") if f.get("crime") == cid and f.get("npc") == npc]

    def actions(npc: str, action: str) -> list[NpcActCmd]:
        return [c for c in sent if isinstance(c, NpcActCmd) and c.npc == npc and c.action == action]

    def verdicts(cid: str, npc: str) -> list[HoldVerdictCmd]:
        ids = {h.hold for h in holds(cid, npc)}
        return [c for c in sent if isinstance(c, HoldVerdictCmd) and c.hold in ids]

    def accepted(action: NpcActCmd) -> bool:
        return any(
            isinstance(m, NpcActResult) and m.id == action.id and m.status == "queued" for m in received
        )

    async def stand(npc: str, point: str, *, face: str | None = None, ready: bool = False) -> bool:
        await link.debug("skip_important", near=npc)
        # Test placement owns neither the body nor perceptions. npc_goto grounds the NPC,
        # removes an old interaction and clears its previous walk before the new activity starts.
        await link.debug("npc_goto", npc=npc, waypoint=point)
        action = await link.npc_act(
            npc,
            "start_activity",
            state="ZS_STAND_WP",
            waypoint=point,
            minutes=120,
            place=False,
        )
        await _until(
            lambda: any(isinstance(m, NpcActResult) and m.id == action for m in received),
            5,
        )
        queued = any(
            isinstance(m, NpcActResult) and m.id == action and m.status == "queued" for m in received
        )
        detail: dict[str, Any] = {"npc": npc, "point": point, "action": action}

        def receipts() -> list[dict[str, Any]]:
            return [
                m.model_dump() for m in received if isinstance(m, NpcActResult | NpcIdle) and m.npc == npc
            ]

        if not queued:
            log.info("crime_react placement not queued: %s", {**detail, "receipts": receipts()})
            return False

        async def at_point() -> bool:
            views = await link.npc_view([npc])
            snapshot = await link.snapshot(npc, fresh=True)
            expected = points[point]
            actual = snapshot.npc.pos
            horizontal = math.hypot(actual[0] - expected["x"], actual[2] - expected["z"])
            view = views[0] if views else None
            detail.update(
                {
                    "pos": actual,
                    "expected": (expected["x"], expected["y"], expected["z"]),
                    "horizontal": horizontal,
                    "view": view.model_dump() if view else None,
                }
            )
            return (
                horizontal <= 200
                and abs(actual[1] - expected["y"]) <= 300
                and view is not None
                and view.state == "ZS_STAND_WP"
                and view.theft_ready is True
            )

        if ready and not await _until_async(at_point, bool, 15):
            log.info("crime_react placement not ready: %s", {**detail, "receipts": receipts()})
            return False
        if face is not None:
            # Set the test pose after native initialization, whose AI_AlignToWP may face elsewhere.
            # Actual visibility is still checked through the engine immediately before taking.
            await link.debug("npc_goto", npc=npc, waypoint=point, face=face)
        return True

    async def drop() -> bool:
        try:
            await link.debug("npc_drop", npc=LOBART, item=CUP)
        except GameError as error:
            if "could not drop it now" not in str(error):
                raise
            return False
        return True

    async def count(npc: str) -> int:
        return int(await link.evaluate(["call", "NPC_HASITEMS", ["inst", npc], ["sym", CUP]]))

    async def perception_ready(label: str, witness: str) -> bool:
        last: dict[str, Any] = {}
        attempted: list[dict[str, Any]] = []

        async def read() -> bool:
            senses, senses_range, can_see, free_los, distance, state_time = await link.story_eval(
                witness,
                [
                    ["field", ["inst", "SELF"], "SENSES"],
                    ["field", ["inst", "SELF"], "SENSES_RANGE"],
                    ["call", "NPC_CANSEENPC", ["inst", "SELF"], ["inst", "HERO"]],
                    ["call", "NPC_CANSEENPCFREELOS", ["inst", "SELF"], ["inst", "HERO"]],
                    ["call", "NPC_GETDISTTONPC", ["inst", "SELF"], ["inst", "HERO"]],
                    ["call", "NPC_GETSTATETIME", ["inst", "SELF"]],
                ],
            )
            snapshot = await link.snapshot(witness, fresh=True)
            # Make the final read one coherent native sample of callback, posture and visibility.
            views = await link.npc_view([witness])
            if not views:
                return False
            view = views[0]
            last.update(
                {
                    "view": view.model_dump(),
                    "senses": senses,
                    "senses_range": senses_range,
                    "can_see": can_see,
                    "free_los": free_los,
                    "distance": distance,
                    "state_time": state_time,
                    "npc_pos": snapshot.npc.pos,
                    "npc_rot": snapshot.npc.rot,
                    "hero_pos": snapshot.player.pos if snapshot.player is not None else None,
                    "placements": attempted,
                    "receipts": [
                        m.model_dump()
                        for m in received
                        if isinstance(m, NpcActResult | NpcIdle) and m.npc == witness
                    ],
                }
            )
            return (
                view.theft_ready is True
                and view.state == "ZS_STAND_WP"
                and view.weapon == "none"
                and view.body == "stand"
                and bool(can_see)
                and view.hero is not None
                and view.hero.seen
                and distance <= 1500
            )

        ready = False
        for distance in (None, 180, 300):
            views = await link.npc_view([witness])
            if not views or views[0].state != "ZS_STAND_WP" or views[0].body != "stand":
                # A native routine can reclaim its NPC between test placements. Re-establish
                # the standing scene before computing a position relative to it, never relative
                # to the stove's interaction pose. No further npc_act replaces this activity.
                if not await stand(witness, WITNESS_POINT, ready=True):
                    return check(
                        f"{label}_perception_ready",
                        False,
                        {
                            **last,
                            "placements": attempted,
                            "reason": "standing placement lost",
                        },
                    )
            # BED_02 lies among furniture, so a grounded hero there may have no head-to-head ray
            # even after the NPC has turned. Start on the connected walk point; if it is occluded,
            # move only the hero a short way in front of the NPC's actual current heading.
            if distance is None:
                await link.debug("player_goto", waypoint=HERO_POINT)
                placement: dict[str, Any] = {"waypoint": HERO_POINT}
            else:
                await link.debug("teleport", near=witness, distance=distance)
                placement = {"in_front_of": witness, "distance": distance}
            # Keep the initialized activity's tracking intact. A second npc_act would replace it;
            # debug placement only clears pending movement and sets the pose towards the hero.
            await link.debug("npc_goto", npc=witness, waypoint=WITNESS_POINT, face="PLAYER")
            attempted.append(placement)
            if await _until_async(read, bool, 2):
                ready = True
                break
            placement["outcome"] = {
                "state": last.get("view", {}).get("state"),
                "body": last.get("view", {}).get("body"),
                "can_see": last.get("can_see"),
                "free_los": last.get("free_los"),
                "npc_pos": last.get("npc_pos"),
                "hero_pos": last.get("hero_pos"),
            }
        return check(f"{label}_perception_ready", bool(ready), last)

    async def household(label: str) -> bool:
        # Read the native sleep routines before moving anybody. The household must come from
        # production metadata; giving Hilda a test-only home would hide a broken role resolver.
        await reactions.prepare()
        people = {npc: service.people.get(npc, {}) for npc in (HILDA, LOBART)}
        detail = {
            npc: {
                "guild": person.get("guild"),
                "home_room": person.get("home_room"),
                "household": reactions.home(npc),
                "sleep": [
                    r for r in person.get("routine", ()) if r.get("state") in {"ZS_SLEEP", "ZS_GOTOBED"}
                ],
            }
            for npc, person in people.items()
        }
        # Ask the same resolver that handles the actual deed below. This read-only probe only
        # names the cup's intended native owner; it creates no deed or belief in the Chronicle.
        role = reactions.role(
            HILDA,
            {
                "id": "",
                "kind": "theft",
                "owner": LOBART,
                "victim": LOBART,
                "community": None,
            },
        )
        return check(
            f"{label}_household",
            role == "member" and all(person["household"] for person in detail.values()),
            {"role": role, "people": detail},
        )

    async def clean(label: str) -> bool:
        reactions.cards = original_cards
        loading = asyncio.create_task(wait_message(link, Session, lambda m: m.reason == "load", 60))
        await asyncio.sleep(0)
        await link.debug("load", slot + 1)
        await loading
        await link.wait_world()
        await asyncio.sleep(0.5)
        sent.clear()
        received.clear()
        hero_states.clear()
        return check(f"{label}_branch", not facts("crime"), f"{len(facts('crime'))} crimes")

    async def theft(
        label: str, witness: str, *, fighting: bool = False, held: bool = True
    ) -> dict[str, Any] | None:
        nonlocal hero_cups_before, owner_cups_before
        if not await household(label):
            return None
        await link.debug("player_goto", waypoint=HERO_POINT)
        for npc in farm:
            if npc == witness and npc != LOBART:
                # The eventual witness needs no parked activity: its actual native routine is
                # replaced only once, after the cup has landed, avoiding repeated same-state resets.
                await link.debug("npc_goto", npc=npc, waypoint=parked)
            elif npc != LOBART and not await stand(npc, parked):
                check(f"{label}_parked", False, npc)
                return None
        owner_point = WITNESS_POINT if witness == LOBART else HOUSE
        if not check(f"{label}_owner_arrived", await stand(LOBART, owner_point, ready=True), LOBART):
            return None
        if not check(f"{label}_drop", await _until_async(drop, bool, 10), "native Lobart silver cup"):
            return None
        if witness != LOBART and not await stand(LOBART, parked):
            check(f"{label}_owner_parked", False, LOBART)
            return None
        if witness == LOBART:
            # Already standing at the final point: retain its initialized state after the drop.
            await link.debug("npc_goto", npc=witness, waypoint=owner_point, face="PLAYER")
            positioned = True
        else:
            positioned = await stand(witness, WITNESS_POINT, face="PLAYER", ready=True)
        if not check(f"{label}_witness_arrived", positioned, witness):
            return None
        before = {f["id"] for f in facts("crime")}
        hero_cups_before, owner_cups_before = await count("HERO"), await count(LOBART)
        # A passive, unarmed hero falls from real melee damage; no debug knockdown or kill is used.
        await link.debug("player_hp", hp=10 if fighting else 5000)
        if fighting:
            await link.body_watch(["PLAYER"], rate_hz=20)
        if not await perception_ready(label, witness):
            return None
        sent.clear()
        received.clear()
        hero_states.clear()
        await link.debug("player_take", item=CUP, distance=1200)
        await _until(lambda: any(f["id"] not in before for f in facts("crime")), 10)
        crime = next((f for f in facts("crime") if f["id"] not in before), None)
        if crime is None:
            check(f"{label}_theft", False, "no crime from the native take")
            return None
        cid = str(crime["id"])
        if held:
            await _until(lambda: bool(decisions(cid, witness)) and bool(verdicts(cid, witness)), 10)
        seen = [f for f in facts("crime_seen") if f.get("crime") == cid and f.get("npc") == witness]
        deeds = [m for m in received if isinstance(m, HeroDeed) and m.id == cid]
        if not check(
            f"{label}_theft",
            crime.get("kind") == "theft"
            and crime.get("owner") == LOBART
            and set(crime.get("witnesses", ())) == {witness}
            and len(seen) == 1
            and len(deeds) == 1
            and deeds[0].witnesses == [witness]
            and any(k.crime["id"] == cid and k.degree == "seen" for k in service.known(witness)),
            {"crime": crime, "crime_seen": len(seen), "hero_deed": len(deeds)},
        ):
            return None
        return crime

    def server_reaction(label: str, cid: str, npc: str, reaction: str, role: str) -> bool:
        held, decided, answered = holds(cid, npc), decisions(cid, npc), verdicts(cid, npc)
        return check(
            label,
            len(held) == 1
            and len(decided) == 1
            and decided[0].get("reaction") == reaction
            and decided[0].get("role") == role
            and decided[0].get("verdict") == "server"
            and len(answered) == 1
            and answered[0].to == "server",
            {"held": [h.hold for h in held], "decided": decided, "verdicts": [v.to for v in answered]},
        )

    await link.wait_world()
    if link.session is None:
        await wait_message(link, Session, timeout=10)
    link.send = sending  # type: ignore[method-assign]
    link.listeners.append(record)
    link.raw_body_listeners.append(body)
    try:
        await link.debug("time", hour=8, minute=0)
        humans = {p["npc"] for p in await link.story_people()}
        farm = list(
            dict.fromkeys(
                [
                    LOBART,
                    HILDA,
                    *[p["npc"] for p in await link.story_nearby(LOBART, 7000) if p["npc"] in humans],
                ]
            )
        )
        await link.story_effect(LOBART, [{"do": "end"}])
        points = {p["name"]: p for p in await link.waypoints() if not p.get("fp")}
        if not check(
            "farm",
            {HOUSE, HERO_POINT, WITNESS_POINT} <= points.keys() and {LOBART, HILDA} <= humans,
            {"cup": HOUSE, "hero": HERO_POINT, "witness": WITNESS_POINT},
        ):
            return _logged(results)
        house = points[HOUSE]
        origin = (house["x"], house["y"], house["z"])
        far = [
            p
            for p in points.values()
            if p["name"].startswith("NW_FARM1_")
            and not p.get("room")
            and math.dist((p["x"], p["y"], p["z"]), origin) > 8000
        ]
        if not check("parking", bool(far), f"{len(far)} distant waypoints"):
            return _logged(results)
        parked = min(far, key=lambda p: math.dist((p["x"], p["y"], p["z"]), origin))["name"]
        for npc in farm:
            await link.debug("skip_important", near=npc)
        if not check("baseline", not facts("crime") and await _save(link, slot + 1) is not None):
            return _logged(results)

        crime = await theft("hilda", HILDA)
        if crime is None or not server_reaction("hilda_rule", str(crime["id"]), HILDA, "shout", "member"):
            return _logged(results)
        await _until(lambda: bool(actions(HILDA, "say_svm")), 10)
        voice = actions(HILDA, "say_svm")
        voiced = await _until(
            lambda: (
                bool(voice)
                and accepted(voice[0])
                and any(
                    isinstance(m, NpcIdle) and m.act == voice[0].id and m.reason == "done" for m in received
                )
            ),
            20,
        )
        if not check(
            "hilda_native_voice",
            voiced and voice[0].args.get("svm") == "DIRTYTHIEF",
            [v.model_dump() for v in voice],
        ):
            return _logged(results)
        [attacking] = await link.story_eval(
            HILDA,
            [
                ["call", "NPC_ISINSTATE", ["inst", "SELF"], ["sym", "ZS_ATTACK"]],
            ],
        )
        if not check(
            "hilda_scripts_suppressed",
            not attacking and not actions(HILDA, "fight"),
            f"ZS_ATTACK={attacking}; server verdict; shout completed",
        ):
            return _logged(results)

        if not await clean("timid_owner"):
            return _logged(results)
        crime = await theft("timid_owner", LOBART)
        if crime is None or not server_reaction(
            "timid_owner_rule", str(crime["id"]), LOBART, "demand", "owner"
        ):
            return _logged(results)
        if not check(
            "timid_owner_card",
            reactions.cards[LOBART].courage == 2 and not actions(LOBART, "fight"),
            "production courage=2: demand without a nearby guard",
        ):
            return _logged(results)

        if not await clean("brave_owner"):
            return _logged(results)
        reactions.cards = {**original_cards, LOBART: replace(original_cards[LOBART], courage=3)}
        check("brave_owner_fixture", True, "only the scenario card: Lobart courage 2 -> 3")
        crime = await theft("brave_owner", LOBART, fighting=True)
        if crime is None or not server_reaction(
            "brave_owner_rule", str(crime["id"]), LOBART, "fight", "owner"
        ):
            return _logged(results)
        await _until(lambda: any(accepted(a) for a in actions(LOBART, "fight")), 10)
        fight = actions(LOBART, "fight")
        reason = await link.evaluate(["var", "AR_THEFT"])
        if not check(
            "theft_attack_reason",
            len(fight) == 1 and accepted(fight[0]) and fight[0].args.get("reason") in {"AR_THEFT", reason},
            [a.model_dump() for a in fight],
        ):
            return _logged(results)
        down = await _until(lambda: any(s["unconscious"] and not s["dead"] for s in hero_states), 60)
        if not check(
            "hero_knocked_out",
            down and not any(s["dead"] for s in hero_states),
            {"samples": len(hero_states), "latest": hero_states[-1] if hero_states else None},
        ):
            return _logged(results)
        returned = await _until_async(lambda: count(LOBART), lambda n: n > owner_cups_before, 30)
        if not check(
            "cup_returned",
            bool(returned) and await count("HERO") == hero_cups_before,
            f"Lobart={await count(LOBART)}, hero={await count('HERO')}",
        ):
            return _logged(results)
        await link.body_watch([])

        if not await clean("vanilla"):
            return _logged(results)
        await link.send(ThreatControlCmd(hold=hold_threats, deeds=False))
        crime = await theft("vanilla", HILDA, held=False)
        if crime is None:
            return _logged(results)
        await asyncio.sleep(1)
        cid = str(crime["id"])
        check(
            "vanilla_no_holds",
            not holds(cid, HILDA) and not decisions(cid, HILDA) and not verdicts(cid, HILDA),
            "deeds:false; real crime_seen remains; no held perception",
        )
        return _logged(results)
    finally:
        reactions.cards = original_cards
        try:
            if not link.closed.is_set():
                await link.body_watch([])
                await send(ThreatControlCmd(hold=hold_threats, deeds=True))
        finally:
            link.send = send  # type: ignore[method-assign]
            link.listeners.remove(record)
            link.raw_body_listeners.remove(body)
