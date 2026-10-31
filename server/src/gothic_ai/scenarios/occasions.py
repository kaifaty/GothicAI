"""The director's occasions, on a save and without a model (stage 4, layer 1)."""

from __future__ import annotations

import asyncio
import logging
import math
from typing import TYPE_CHECKING, Any

from ..game import GameLink
from ..protocol import Session
from .common import _logged, _save_or_fail, _until, wait_message

if TYPE_CHECKING:
    from ..occasions import Occasions

log = logging.getLogger(__name__)


async def occasions(link: GameLink, service: Occasions, slot: int = 7) -> dict[str, str]:
    """A new inhabited place, return and reload, chapter, human death, then a quiet day.

    Uses the running service's real five-second sampling and ten-second settling. The save is copied
    by e2e.sh; the scenario's own save stays in its log directory.
    """
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    def facts(kind: str, place: str | None = None) -> list[dict[str, Any]]:
        return [
            f
            for f in service.chronicle.facts("occasion")
            if f.get("kind") == kind and (place is None or f.get("place") == place)
        ]

    await link.wait_world()
    if link.session is None:
        await wait_message(link, Session, timeout=10)

    # This is the production reader, including its read-only whitelist, rather than debug eval.
    people = await link.story_people()
    context = people[0]["npc"] if people else "PC_HERO"
    nearest = await link.story_eval(context, [["call", "NPC_GETNEARESTWP", ["inst", "HERO"]]])
    if not nearest or not isinstance(nearest[0], str) or not nearest[0]:
        check("nearest_waypoint", False, "story_eval has no nearest waypoint; protocol left unchanged")
        return _logged(results)
    start = service.places.of(nearest[0])
    if start is None:
        check("start", False, f"no place for {nearest[0]}")
        return _logged(results)

    # Pick a real resident's waypoint in a new, safe place near the loaded hero. Avoid the bandits'
    # routines here: the human death below is separate, and nobody should attack during settling.
    points = {p["name"].upper(): p for p in await link.waypoints() if not p.get("fp")}
    homes: dict[str, tuple[str, list[str]]] = {}
    for person in people:
        if not str(person["npc"]).startswith(("BAU_", "VLK_", "MIL_")):
            continue
        for entry in person.get("routine", []):
            waypoint = str(entry.get("waypoint", "")).upper()
            place = service.places.of(waypoint)
            if place is None or waypoint not in points:
                continue
            if place.id not in homes:
                homes[place.id] = (waypoint, [])
            homes[place.id][1].append(person["npc"])
    candidates = [
        p
        for p in service.places.places
        if p.id in homes and p.id != start.id and p.id not in service.visited and p.center is not None
    ]
    if not candidates:
        check("start", False, f"no unvisited inhabited place near {start.id}")
        return _logged(results)
    target = min(candidates, key=lambda p: math.dist(start.center or (0, 0, 0), p.center))
    waypoint, residents = homes[target.id]
    for npc in dict.fromkeys(residents):
        await link.debug("skip_important", near=npc)
    await link.debug("player_hp", hp=5000)

    async def go(point: str, place: str) -> bool:
        await link.debug("player_goto", waypoint=point)
        return await _until(lambda: service.current is not None and service.current.id == place, 22)

    before = len(facts("arrived", target.id))
    settled = await go(waypoint, target.id)
    await asyncio.sleep(6)  # another production sample must not repeat it
    arrived = facts("arrived", target.id)[before:]
    check(
        "arrived",
        settled and target.id in service.visited and len(arrived) == 1,
        f"{target.id}, waypoint {waypoint}, {len(arrived)} arrived, visited {target.id in service.visited}",
    )
    if not settled or len(arrived) != 1:
        return _logged(results)

    away = await go(nearest[0], start.id)
    returned = await go(waypoint, target.id)
    await asyncio.sleep(6)
    after_return = len(facts("arrived", target.id)) - before
    check("returned", away and returned and after_return == 1, f"{target.id}: {after_return} arrived")

    visited = set(service.visited)
    arrivals = len(facts("arrived"))
    await asyncio.sleep(0.5)  # the game's save must contain the chronicle's latest memory head
    await _save_or_fail(link, slot)
    loaded = asyncio.create_task(wait_message(link, Session, lambda m: m.reason == "load"))
    await asyncio.sleep(0)
    await link.debug("load", slot)
    await loaded
    await link.wait_world()
    queue_cleared = not service.pending
    settled = await _until(lambda: service.current is not None and service.current.id == target.id, 22)
    await asyncio.sleep(6)
    check(
        "after_load",
        queue_cleared
        and settled
        and service.visited == visited
        and len(facts("arrived")) == arrivals
        and not service.pending,
        f"visited {len(visited)} -> {len(service.visited)}, arrived {arrivals} -> {len(facts('arrived'))}, "
        f"queue {len(service.pending)}",
    )

    state = await link.story_state(vars=["KAPITEL"])
    chapter = int(state.get("vars", {}).get("KAPITEL", 0))
    chapters = len(facts("chapter"))
    await link.story_effect(context, [{"do": "set", "var": "KAPITEL", "value": chapter + 1}])
    raised = await _until(lambda: len(facts("chapter")) > chapters, 37)
    new_chapters = facts("chapter")[chapters:]
    actual = int((await link.story_state(vars=["KAPITEL"]))["vars"]["KAPITEL"])
    check("chapter", raised and actual == chapter + 1 and len(new_chapters) == 1, f"{chapter} -> {actual}")

    # Kill an existing human as an actual deadly hit by the hero. `npc=` would merely remove him
    # without a killer; `instance=` also exercises HeroKill and the chronicle's human/monster check.
    living = {p["npc"] for p in await link.story_people()}
    victim = next(
        (npc for npc in ("BDT_1015_BANDIT_L", "BDT_1014_BANDIT_L", "BDT_1013_BANDIT_L") if npc in living),
        next((npc for npc in sorted(living) if npc.startswith("BDT_")), None),
    )
    if victim is None:
        check("deed", False, "no living bandit for the human kill")
    else:
        deeds = len(facts("deed"))
        await link.debug("kill", instance=victim, near=victim, by="PLAYER")
        heard = await _until(lambda: len(facts("deed")) > deeds, 15)
        death = (await link.story_state(npcs=[victim])).get("npcs", {}).get(victim, {})
        caused = []
        for fact in facts("deed")[deeds:]:
            deed = fact.get("deed", {})
            if (
                deed.get("fact") == "crime"
                and deed.get("kind") == "murder"
                and deed.get("victim") == victim
                and fact.get("crime") == deed.get("id")
            ) or (
                deed.get("fact") == "died"
                and deed.get("npc") == victim
                and deed.get("killer") == "PLAYER"
            ):
                caused.append(fact)
        check("deed", heard and death.get("state") == "dead" and len(caused) == 1, f"{victim}: {caused}")

    state = await link.story_state()
    hour, minute = int(state["hour"]), int(state["minute"])
    quiet = len(facts("quiet"))
    # Two half-days always pass a full day, including when the starting hour is near midnight.
    await link.debug("time", hour=(hour + 12) % 24, minute=minute)
    await link.debug("time", hour=hour, minute=minute)
    passed = await _until(lambda: len(facts("quiet")) > quiet, 15)
    end = await link.story_state()
    elapsed = (
        (int(end["day"]) - int(state["day"])) * 24 * 60
        + int(end["hour"]) * 60
        + int(end["minute"])
        - hour * 60
        - minute
    )
    check(
        "quiet",
        passed and elapsed >= 24 * 60 and len(facts("quiet")) == quiet + 1,
        f"{elapsed} game minutes",
    )
    return _logged(results)
