"""Blood as a trace (v60): Lobart finds blood; only whom he saw there near its time is suspected."""

from __future__ import annotations

import logging
import math
from typing import TYPE_CHECKING, Any

from ..game import GameError, GameLink
from ..protocol import Session, Trace
from .common import LOBART, VINO, _logged, _until_async, attach_when_free, wait_message

SCENARIOS = {
    "blood_found": ("OFF", {"save": True, "timeout": 600}),
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


async def blood_found(
    link: GameLink,
    sightings: Sightings,
    found: FoundTraces,
    crimes: Crimes,
) -> dict[str, str]:
    """(а) Lobart sees Vino at a spot, two hours later the hero there; twelve minutes on, blood appears
    there with no body (`debug blood`). Lobart finds it: one `trace_found`, the hero suspected of an attack
    by the chest's rule, Vino (seen two hours before) not.
    (б) The hero stands at another spot unseen; blood appears there; Lobart finds it: no suspicion.
    (в) Maleth dies unseen at a third spot, his pool under him: Lobart finds the body, and no blood.

    No model: nothing is decided, only seen. The engine owns who bled (nobody here) and never tells it.
    """
    results: dict[str, str] = {}
    finds: list[Found] = []
    found.on_found.append(finds.append)
    chronicle = sightings.chronicle

    def check(name: str, ok: bool, detail: object = "") -> bool:
        results[name] = f"{'done' if ok else 'failed'} ({detail})"
        log.info("blood_found %-22s %s", name, results[name])
        return ok

    def suspected(trace: str) -> list[dict[str, Any]]:
        return [
            f
            for kind in ("crime_suspected", "npc_crime_suspected")
            for f in chronicle.facts(kind)
            if f.get("npc") == LOBART and f.get("trace") == trace
        ]

    def blood_finds() -> list[Found]:
        return [f for f in finds if f.npc == LOBART and f.kind == "blood"]

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

    async def view_traces(kind: str, timeout: float = 15, want: Any = None) -> list[Trace]:
        got: dict[str, list[Trace]] = {}

        async def look() -> bool:
            view = await sightings.desk.fresh(LOBART)
            traces = [t for t in view.traces if t.kind == kind] if view is not None else []
            got["traces"] = traces
            return bool(traces) if want is None else want(traces)

        await _until_async(look, bool, timeout)
        return got.get("traces", [])

    async def clock() -> int | None:
        view = await sightings.desk.fresh(LOBART)
        return view.time if view is not None else None

    async def stand(npc: str, waypoint: str) -> None:
        """The npc stands at the waypoint and stays there for two game hours."""
        await link.debug("npc_goto", npc=npc, waypoint=waypoint)
        await link.npc_act(
            npc, "start_activity", state="ZS_STAND_WP", waypoint=waypoint, minutes=120, place=True
        )

    async def skip(minutes: int) -> None:
        now = await clock()
        at = (now if now is not None else 8 * 60) + minutes
        await link.debug("time", hour=at % DAY // 60, minute=at % 60)

    async def judged(trace: str) -> bool:
        def done() -> bool:
            return any(
                f.get("npc") == LOBART and f.get("trace") == trace for f in chronicle.facts("trace_judged")
            )

        return await _until_async(lambda: sightings.desk.fresh(LOBART), lambda _: done(), 15)

    async def bleed_before_lobart(spot: str) -> tuple[Trace | None, int | None]:
        """Lobart at the spot, blood three metres in front of him: he finds it at once."""
        await move(waypoint=spot, face=None)
        known = {f.trace for f in blood_finds()}
        await link.debug("blood", near=LOBART, distance=300, kind="splash", radius=20)
        laid = await clock()
        traces = await view_traces("blood", want=lambda ts: any(t.id not in known for t in ts))
        trace = next((t for t in traces if t.id not in known), None)
        return trace, laid

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
        if not check("positions", len(outside) >= 3, f"{len(outside)} outside farm points with a place"):
            return _logged(results)

        def pos(p: dict[str, Any]) -> tuple[float, float, float]:
            return (p["x"], p["y"], p["z"])

        spot_a = max(outside, key=lambda p: math.dist(pos(p), center))
        spot_b = max(outside, key=lambda p: math.dist(pos(p), pos(spot_a)))
        spot_c = max(
            outside, key=lambda p: min(math.dist(pos(p), pos(spot_a)), math.dist(pos(p), pos(spot_b)))
        )
        assert book is not None
        place_a, place_b = book.of(spot_a["name"]), book.of(spot_b["name"])
        assert place_a is not None and place_b is not None
        if not check(
            "places",
            place_a.id != place_b.id,
            f"{spot_a['name']}: {place_a.id}, {spot_b['name']}: {place_b.id}, c: {spot_c['name']}",
        ):
            return _logged(results)

        # (а) Lobart sees Vino at spot A; two hours later the hero there; blood twelve minutes after that
        await link.debug("player_goto", waypoint=AWAY)
        await stand(VINO, spot_a["name"])
        await move(near=VINO, distance=400, face=VINO)
        seen_vino = await _until_async(
            lambda: sightings.desk.fresh(LOBART),
            lambda v: bool(v and any(o.id.upper() == VINO for o in v.sees)),
            10,
        )
        vino = [s for s in sightings.mind(LOBART).sightings if s.who == VINO and place_a.id in s.place_ids]
        if not check("vino_seen_at_a", seen_vino and bool(vino), [sightings.words(s) for s in vino]):
            return _logged(results)
        await move(waypoint=HOUSE, face=None)
        await skip(120)
        # after the leap: Lobart indoors again; Vino waits in the house, away from spot A from now on
        await move(waypoint=HOUSE, face=None)
        await stand(VINO, HOUSE)
        await link.debug("player_goto", waypoint=spot_a["name"])
        await move(near="PC_HERO", distance=400)
        await link.act(LOBART, "turn_to", npc="PLAYER")
        await _until_async(
            lambda: sightings.desk.fresh(LOBART), lambda v: bool(v and v.hero and v.hero.seen), 10
        )
        trail = sightings.hero_trail(LOBART)
        if not check(
            "hero_seen_at_a",
            bool(trail) and place_a.id in trail[0].place_ids,
            sightings.words(trail[0]) if trail else "no sighting",
        ):
            return _logged(results)
        await move(waypoint=HOUSE, face=None)
        await link.debug("player_goto", waypoint=AWAY)
        await skip(12)
        await stand(VINO, HOUSE)  # the leap sent him back to his routine
        trace_a, laid_a = await bleed_before_lobart(spot_a["name"])
        if not check(
            "blood_a_trace",
            trace_a is not None and trace_a.age is not None and ":blood:" in trace_a.id,
            trace_a.model_dump() if trace_a else "no blood trace",
        ):
            return _logged(results)
        assert trace_a is not None
        check("blood_a_judged", await judged(trace_a.id), chronicle.facts("trace_judged"))
        first = [f for f in blood_finds() if f.trace == trace_a.id]
        check(
            "blood_a_at_spot", bool(first) and first[0].place == place_a.id, first[0].place if first else ""
        )
        bled = (
            bool(first)
            and first[0].bled is not None
            and laid_a is not None
            and abs(first[0].bled - laid_a) <= 5
        )
        check("blood_a_found_once", len(first) == 1 and bled, {"finds": first, "laid": laid_a})
        facts = suspected(trace_a.id)
        hero = [f for f in facts if f["fact"] == "crime_suspected"]
        check(
            "hero_suspected_attack",
            len(hero) == 1 and hero[0]["suspect"] == "PLAYER" and hero[0]["crime"] == "attack",
            hero,
        )
        check("vino_not_suspected", all(f["suspect"] != VINO for f in facts), facts)
        line = next(
            (
                row
                for row in (sightings.situation(LOBART) or "").splitlines()
                if "кровь" in row and "здесь кого-то ранили" in row and "герой" in row
            ),
            "",
        )
        check("situation_a", bool(line), line or sightings.situation(LOBART))
        await view_traces("blood", 5)  # shown again
        check("blood_a_not_found_again", len([f for f in blood_finds() if f.trace == trace_a.id]) == 1, finds)

        # (б) the hero stands at spot B, but nobody sees him; blood appears there
        now = await clock()
        await move(waypoint=HOUSE, face=None)
        await link.debug("time", hour=((now or 8 * 60) % DAY // 60 + 2) % 24, minute=0)
        await move(waypoint=HOUSE, face=None)
        await link.debug("player_goto", waypoint=spot_b["name"])
        hidden = await sightings.desk.fresh(LOBART)
        check(
            "hero_unseen",
            hidden is not None and hidden.hero is not None and not hidden.hero.seen,
            hidden.hero if hidden else "no view",
        )
        await link.debug("player_goto", waypoint=AWAY)
        trace_b, _ = await bleed_before_lobart(spot_b["name"])
        if not check("blood_b_trace", trace_b is not None, trace_b.model_dump() if trace_b else "no trace"):
            return _logged(results)
        assert trace_b is not None
        check("blood_b_found", len([f for f in blood_finds() if f.trace == trace_b.id]) == 1, blood_finds())
        check("blood_b_judged", await judged(trace_b.id), chronicle.facts("trace_judged"))
        check("blood_b_no_suspicion", suspected(trace_b.id) == [], suspected(trace_b.id))
        line_b = next(
            (
                row
                for row in (sightings.situation(LOBART) or "").splitlines()
                if "кровь" in row and "кто и кого — не знаешь" in row
            ),
            "",
        )
        check("situation_b", bool(line_b), line_b or sightings.situation(LOBART))

        # (в) Maleth dies unseen at spot C, his pool under him: the body is the find, the pool is not
        blood_before = len(blood_finds())
        await move(waypoint=HOUSE, face=None)
        await stand(MALETH, spot_c["name"])
        await link.debug("kill", npc=MALETH)

        async def pool() -> bool:
            try:
                await link.debug("blood", npc=MALETH, kind="pool", radius=45)
                return True
            except GameError as err:
                log.info("blood_found pool: %s", err)
                return False

        pooled = await _until_async(pool, bool, 15)
        if not check("pool_laid", pooled, "debug blood npc=MALETH"):
            return _logged(results)
        await move(near=MALETH, distance=250, face=MALETH)
        bodies = await view_traces("body", want=lambda ts: any((t.npc or "").upper() == MALETH for t in ts))
        body = next((t for t in bodies if (t.npc or "").upper() == MALETH), None)
        if not check("body_trace", body is not None, [t.model_dump() for t in bodies]):
            return _logged(results)
        assert body is not None
        await view_traces("blood", 5)  # a few more views of the place
        view = await sightings.desk.fresh(LOBART)
        blood_here = [
            t.model_dump()
            for t in (view.traces if view is not None else [])
            if t.kind == "blood" and t.id not in (trace_a.id, trace_b.id)
        ]
        maleth = [f for f in finds if f.npc == LOBART and f.body == MALETH]
        check(
            "one_find_for_body_and_pool",
            len(maleth) == 1 and len(blood_finds()) == blood_before and blood_here == [],
            {"body_finds": maleth, "new_blood_finds": len(blood_finds()) - blood_before, "blood": blood_here},
        )
        return _logged(results)
    finally:
        found.on_found.remove(finds.append)
