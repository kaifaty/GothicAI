"""A body as a trace (v58): Lobart finds two dead farmhands; only the evidence he saw makes a suspect."""

from __future__ import annotations

import asyncio
import logging
import math
from typing import TYPE_CHECKING, Any

from ..game import GameError, GameLink
from ..protocol import Session, Trace
from .common import LOBART, VINO, _logged, _save, _until_async, attach_when_free, wait_message

SCENARIOS = {
    "body_found": ("OFF", {"save": True, "timeout": 600}),
}

if TYPE_CHECKING:
    from ..crimes import Crimes
    from ..found_traces import Found, FoundTraces
    from ..sightings import Sightings

log = logging.getLogger("gothic_ai.scenarios")
MALETH = "BAU_954_MALETH"
HOUSE = "NW_FARM1_INHOUSE_01"
AWAY = "NW_XARDAS_START"  # the hero far from the farm: nobody there sees him
DAY = 24 * 60


async def body_found(
    link: GameLink,
    sightings: Sightings,
    found: FoundTraces,
    crimes: Crimes,
    slot: int = 7,
) -> dict[str, str]:
    """(а) Lobart sees the hero by Maleth; Maleth dies with no witness (`kill npc=`); Lobart, back from
    the house, finds the body: one `trace_found` and suspicion of the hero by the chest's rule.
    (б) Two hours later the hero stands by Vino unseen; Vino dies; Lobart finds him: a find, no suspicion.
    Then a save and a load: the finds and the suspicion stay, and the same bodies are not found anew.

    No model: nothing is decided, only seen. The engine owns who killed (nobody here) and never tells it.
    """
    results: dict[str, str] = {}
    finds: list[Found] = []
    found.on_found.append(finds.append)
    chronicle = sightings.chronicle

    def check(name: str, ok: bool, detail: object = "") -> bool:
        results[name] = f"{'done' if ok else 'failed'} ({detail})"
        log.info("body_found %-20s %s", name, results[name])
        return ok

    def suspected(body: str) -> list[dict[str, Any]]:
        return [
            f
            for kind in ("crime_suspected", "npc_crime_suspected")
            for f in chronicle.facts(kind)
            if f.get("npc") == LOBART and f.get("body") == body
        ]

    def mine(body: str) -> list[Found]:
        return [f for f in finds if f.npc == LOBART and f.body == body]

    async def move(
        *,
        waypoint: str | None = None,
        near: str | None = None,
        distance: int = 0,
        face: str | None = "PC_HERO",
    ) -> None:
        """Lobart stands there, held by the attachment so his routine does not walk him off."""
        try:
            await link.detach(LOBART)
        except GameError as err:
            if "npc is not attached" not in str(err):
                raise
        await link.debug("npc_goto", npc=LOBART, waypoint=waypoint, to=near, distance=distance, face=face)
        await attach_when_free(link, LOBART)

    async def body_trace(victim: str, timeout: float = 15) -> Trace | None:
        got: dict[str, Trace] = {}

        async def look() -> bool:
            view = await sightings.desk.fresh(LOBART)
            trace = (
                next((t for t in view.traces if t.kind == "body" and (t.npc or "").upper() == victim), None)
                if view is not None
                else None
            )
            if trace is not None:
                got["trace"] = trace
            return trace is not None

        await _until_async(look, bool, timeout)
        return got.get("trace")

    async def clock() -> int | None:
        view = await sightings.desk.fresh(LOBART)
        return view.time if view is not None else None

    try:
        await link.wait_world()
        if link.session is None:
            await wait_message(link, Session, timeout=10)
        for who in (LOBART, MALETH, VINO):
            await link.debug("skip_important", near=who)
        await link.debug("player_hp", hp=5000)
        await link.debug("time", hour=8, minute=0)
        points = {p["name"]: p for p in await link.waypoints() if not p.get("fp")}
        book = crimes.places
        house = points[HOUSE]
        center = (house["x"], house["y"], house["z"])
        outside = [
            p
            for p in points.values()
            if p["name"].startswith("NW_FARM1_")
            and not p.get("room")
            and book is not None
            and book.of(p["name"]) is not None
        ]
        if not check("positions", len(outside) >= 2, f"{len(outside)} outside farm points with a place"):
            return _logged(results)

        def pos(p: dict[str, Any]) -> tuple[float, float, float]:
            return (p["x"], p["y"], p["z"])

        spot_a = max(outside, key=lambda p: math.dist(pos(p), center))
        spot_b = max(outside, key=lambda p: math.dist(pos(p), pos(spot_a)))
        assert book is not None  # every point in `outside` has a place
        place_a, place_b = book.of(spot_a["name"]), book.of(spot_b["name"])
        assert place_a is not None and place_b is not None
        if not check(
            "places",
            place_a.id != place_b.id,
            f"{spot_a['name']}: {place_a.id}, {spot_b['name']}: {place_b.id}",
        ):
            return _logged(results)

        # (а) Lobart sees the hero by Maleth, then goes indoors; Maleth dies unseen
        await link.debug("npc_goto", npc=MALETH, waypoint=spot_a["name"])
        await link.npc_act(
            MALETH, "start_activity", state="ZS_STAND_WP", waypoint=spot_a["name"], minutes=120, place=True
        )
        await link.debug("teleport", near=MALETH, distance=250)
        await move(near="PC_HERO", distance=400)
        await link.act(LOBART, "turn_to", npc="PLAYER")
        await _until_async(
            lambda: sightings.desk.fresh(LOBART), lambda v: bool(v and v.hero and v.hero.seen), 10
        )
        trail = sightings.hero_trail(LOBART)
        if not check(
            "hero_seen_by_maleth",
            bool(trail) and place_a is not None and place_a.id in trail[0].place_ids,
            sightings.words(trail[0]) if trail else "no sighting",
        ):
            return _logged(results)
        await move(waypoint=HOUSE, face=None)
        await link.debug("player_goto", waypoint=AWAY)
        await link.debug("kill", npc=MALETH)
        killed = await clock()
        await move(near=MALETH, distance=250, face=MALETH)
        trace = await body_trace(MALETH)
        if not check(
            "maleth_trace",
            trace is not None and trace.age is not None and trace.id.count(":body:") == 1,
            trace.model_dump() if trace else "no body trace",
        ):
            return _logged(results)
        assert trace is not None
        first = mine(MALETH)
        # the time of death is the view's clock less the body's age: the kill, within a few game minutes
        died = (
            bool(first)
            and first[0].died is not None
            and killed is not None
            and abs(first[0].died - killed) <= 5
        )
        check("maleth_found_once", len(first) == 1 and died, {"finds": first, "killed": killed})
        hero = [f for f in suspected(MALETH) if f["fact"] == "crime_suspected"]
        check(
            "hero_suspected",
            len(hero) == 1
            and hero[0]["suspect"] == "PLAYER"
            and hero[0]["crime"] == "murder"
            and hero[0]["trace"] == trace.id,
            hero,
        )
        line = next(
            (
                row
                for row in (sightings.situation(LOBART) or "").splitlines()
                if "тело" in row and "здесь кого-то убили" in row and "герой" in row
            ),
            "",
        )
        check("situation_a", bool(line), line or sightings.situation(LOBART))
        await body_trace(MALETH, 5)  # shown again
        check("maleth_not_found_again", len(mine(MALETH)) == 1, len(mine(MALETH)))

        # (б) two hours later the hero stands by Vino, but nobody sees him; Vino dies
        now = await clock()
        await move(waypoint=HOUSE, face=None)
        await link.debug("time", hour=((now or 8 * 60) % DAY // 60 + 2) % 24, minute=0)
        await link.debug("npc_goto", npc=VINO, waypoint=spot_b["name"])
        await link.npc_act(
            VINO, "start_activity", state="ZS_STAND_WP", waypoint=spot_b["name"], minutes=120, place=True
        )
        await link.debug("teleport", near=VINO, distance=250)
        hidden = await sightings.desk.fresh(LOBART)
        check(
            "hero_unseen",
            hidden is not None and hidden.hero is not None and not hidden.hero.seen,
            hidden.hero if hidden else "no view",
        )
        await link.debug("player_goto", waypoint=AWAY)
        await link.debug("kill", npc=VINO)
        await move(near=VINO, distance=250, face=VINO)
        trace_b = await body_trace(VINO)
        if not check("vino_trace", trace_b is not None, trace_b.model_dump() if trace_b else "no body trace"):
            return _logged(results)
        check("vino_found", len(mine(VINO)) == 1, mine(VINO))
        check("vino_no_suspicion", suspected(VINO) == [], suspected(VINO))
        line_b = next(
            (
                row
                for row in (sightings.situation(LOBART) or "").splitlines()
                if "кто это сделал — не знаешь" in row
            ),
            "",
        )
        check("situation_b", bool(line_b), line_b or sightings.situation(LOBART))

        # save and load: the finds and the suspicion are in the save branch; the bodies are not found anew
        kept = {"a": suspected(MALETH), "b": suspected(VINO)}
        traces = set(sightings.mind(LOBART).traces)
        count = len(finds)
        saved = await _save(link, slot)
        if not check("saved", saved is not None, slot):
            return _logged(results)
        assert saved is not None
        loading = asyncio.create_task(wait_message(link, Session, lambda m: m.reason == "load", 90))
        await asyncio.sleep(0)
        await link.debug("load", slot)
        loaded = await loading
        await link.wait_world()
        await move(near=VINO, distance=250, face=VINO)
        again = await body_trace(VINO)
        check(
            "loaded",
            loaded.parent == saved.branch
            and again is not None
            and again.id == trace_b.id
            and {"a": suspected(MALETH), "b": suspected(VINO)} == kept
            and traces <= set(sightings.mind(LOBART).traces)
            and len(finds) == count,
            {"trace": again.id if again else None, "finds": len(finds) - count},
        )
        return _logged(results)
    finally:
        found.on_found.remove(finds.append)
