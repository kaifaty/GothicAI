"""Native sword damage at Lobart's farm, followed by a lie about the debris' age."""

from __future__ import annotations

import asyncio
import logging
import math
import re
from pathlib import Path
from typing import Any

from ..brain.reflex import Reflexes
from ..dialogue import DialogueManager
from ..game import GameError, GameLink
from ..llm import ToolCall
from ..llm.fake import FakeBackend, FakeStep
from ..perception import trace_name
from ..protocol import DebugCmd
from ..sightings import Sighting, Sightings
from ..talk import TalkDesk
from ..trace import read as read_trace
from .common import (
    LOBART,
    ClaimScript,
    FixedDecider,
    _logged,
    _trace_evidence_line,
    _until,
    attach_when_free,
)

log = logging.getLogger(__name__)


async def broken_memory(
    link: GameLink,
    dialogue: DialogueManager,
    talk: TalkDesk,
    sightings: Sightings,
    log_dir: str,
) -> dict[str, str]:
    """A stranger breaks one native fence before the hero arrives. Lobart has no grounds to accuse
    the hero. Then he sees the hero on the farm, leaves, and comes back to a freshly broken fence;
    its evidence line contradicts the hero's claim that it has been broken since spring.

    Only the fake backend receives scripted replies. Live models get the ordinary conversation
    prompt and tools; every reply and tool outcome remains in the normal turn traces.
    """
    results: dict[str, str] = {}
    directory, desk, chronicle = Path(log_dir), sightings.desk, sightings.chronicle
    original = dialogue.llm
    age_lie = "этот забор давно сломан, еще с весны"
    control_denial = "этот забор сломал не я, я только пришел и ничего здесь не трогал"

    def check(name: str, ok: bool, detail: object = "") -> bool:
        results[name] = f"{'done' if ok else 'failed'} ({detail})"
        log.info("broken %-22s %s", name, results[name])
        return ok

    def events() -> list[dict[str, Any]]:
        path = directory / "events.jsonl"
        return read_trace(path) if path.exists() else []

    async def mark(name: str) -> None:
        if not await _until(
            lambda: any(r.get("kind") == "mark" and r.get("text") == name for r in events()),
            120,
        ):
            raise RuntimeError(f"broken bench did not reach {name}")

    async def release(name: str) -> None:
        await link.debug("destruction_traces", text=f"signal {name}")

    async def move(
        who: str, *, waypoint: str | None = None, near: str | None = None, distance: int = 0
    ) -> None:
        try:
            await link.detach(who)
        except GameError as err:
            if "npc is not attached" not in str(err):
                raise
        await link.debug("npc_goto", npc=who, waypoint=waypoint, to=near, distance=distance, face="PC_HERO")
        if who == LOBART:
            await attach_when_free(link, who)

    def setup() -> dict[str, Any]:
        return [r for r in events() if r.get("kind") == "destruction2_farm_setup"][-1]

    def turn_rows() -> list[dict[str, Any]]:
        return [
            r
            for path in sorted((directory / "traces").glob("*.jsonl"))
            for r in read_trace(path)
            if r.get("type") == "turn" and r.get("npc") == LOBART
        ]

    def request(row: dict[str, Any]) -> str:
        return "\n".join(
            str(item.get("content", ""))
            for step in row.get("steps", [])
            for item in step.get("added", [])
            if item.get("role") == "developer"
        )

    def catches(row: dict[str, Any]) -> list[dict[str, Any]]:
        return [
            call
            for step in row.get("steps", [])
            for call in step.get("calls", [])
            if call.get("name") == "catch_lie"
        ]

    async def deny(observation: Sighting | None = None) -> dict[str, Any]:
        said = age_lie if observation is not None else control_denial
        before = {row["id"] for row in turn_rows()}
        await link.debug("teleport", near=LOBART, distance=250)
        await _until(lambda: LOBART in talk.people and talk.people[LOBART].distance < 800, 10)
        await desk.fresh(LOBART)
        if isinstance(original, FakeBackend):
            step = FakeStep("Не видел, кто ломал. Одних обломков мало, чтобы обвинять тебя.")
            if observation is not None:
                sid, _ = _trace_evidence_line(await sightings.situation(LOBART) or "", observation)
                step = FakeStep(
                    "Не с весны! Я только что видел свежие обломки этого забора.",
                    [ToolCall("catch_lie", {"said": age_lie, "seen": sid})],
                )
            dialogue.llm = ClaimScript([(said, step)], [], "Я запомню.", only="Лобарт")
        await link.debug("type", text=f"Лобарт, {said}.")
        completed = await _until(
            lambda: any(
                row["id"] not in before and said in row.get("trigger", {}).get("text", "")
                for row in turn_rows()
            ),
            90,
        )
        await _until(lambda: not dialogue.speaking(LOBART), 30)
        rows = [
            row
            for row in turn_rows()
            if row["id"] not in before and said in row.get("trigger", {}).get("text", "")
        ]
        row = rows[-1] if rows else {}
        label = "positive" if observation is not None else "control"
        replies = [step.get("text") for step in row.get("steps", [])]
        log.info("broken %s reply: %s", label, replies)
        check(f"{label}_turn", completed and row.get("status") == "completed", replies)
        return row

    async def broken_view(zen: int):
        view = await desk.fresh(LOBART)
        trace = (
            next(
                (t for t in view.traces if t.kind == "broken" and t.id.endswith(f":vob:{zen}")),
                None,
            )
            if view is not None
            else None
        )
        return view, trace

    try:
        await link.wait_world()
        await mark("broken_ready")
        talk.reflexes = Reflexes(FixedDecider({"posture": "listen"}))
        for who in (LOBART, "BAU_951_HILDA", "BAU_952_VINO", "BAU_954_MALETH"):
            await link.debug("skip_important", near=who)
        await link.debug("player_hp", hp=5000)
        await link.debug("time", hour=8, minute=0)
        points = await link._request(DebugCmd(req=link._next_id("broken-"), cmd="waypoints"), 10)
        house = next(p for p in points.value if p["name"] == "NW_FARM1_INHOUSE_01")
        center = (house["x"], house["y"], house["z"])
        distant = sorted(
            [
                p
                for p in points.value
                if p["name"].startswith("NW_FARM1_")
                and not p["fp"]
                and math.dist((p["x"], p["y"], p["z"]), center) > 6000
            ],
            key=lambda p: math.dist((p["x"], p["y"], p["z"]), center),
        )
        if not check("preparation", bool(distant), "farm waypoint beyond 60 m"):
            return _logged(results)
        parked = distant[0]["name"]
        await move(LOBART, waypoint=parked)
        await release("control_setup")
        await mark("control_ready")
        control_setup = setup()
        control_zen, actor = control_setup["zen_id"], control_setup["actor"]
        view = await desk.fresh(LOBART)
        if not check(
            "control_unseen",
            view is not None
            and view.hero is not None
            and not view.hero.seen
            and view.hero.distance > 2000
            and not sightings.hero_trail(LOBART),
            view.hero if view else "no view",
        ):
            return _logged(results)
        await release("control_attack")
        await mark("control_broken")
        completed = [r for r in events() if r.get("kind") == "destruction2_melee_complete"]
        check(
            "npc_sword_break",
            actor != "PC_HERO" and bool(completed) and completed[-1].get("case") == "farm-fence-1h-npc",
            completed[-1] if completed else "no native animation completion",
        )
        await link.debug("player_goto", waypoint="NW_XARDAS_START")
        await move(LOBART, near=actor, distance=350)
        await link.act(LOBART, "turn_to", npc=actor)
        view, control = await broken_view(control_zen)
        if not check(
            "control_trace",
            control is not None
            and view is not None
            and view.hero is not None
            and not view.hero.seen
            and not sightings.hero_trail(LOBART),
            view.model_dump() if view else "no view",
        ):
            return _logged(results)
        control_observed = sightings.mind(LOBART).traces[f"TRACE:{control.id}:broken"]
        await asyncio.sleep(5)  # the debris predates the hero's first arrival by a game minute
        row = await deny()
        sid, line = _trace_evidence_line(request(row), control_observed)
        check("control_in_request", bool(sid) and "Следы, которые ты видел" in request(row), line)
        check("control_no_accusation", not catches(row) and not chronicle.facts("caught"), catches(row))
        await talk.end("gone")

        await link.debug("time", hour=9, minute=0)
        await release("positive_setup")
        await mark("positive_ready")
        positive_setup = setup()
        zen = positive_setup["zen_id"]
        check("another_native_fence", zen != control_zen and positive_setup["from_map"], positive_setup)
        await move(LOBART, near="PC_HERO", distance=300)
        await link.act(LOBART, "turn_to", npc="PLAYER")
        await desk.fresh(LOBART)
        trail = sightings.hero_trail(LOBART)
        hero_start = trail[0].start if trail else -1
        if not check(
            "hero_on_farm",
            bool(trail) and trail[0].end % 1440 >= 9 * 60 and any("ферм" in p for p in trail[0].places),
            sightings.words(trail[0]) if trail else "no sighting",
        ):
            return _logged(results)
        await move(LOBART, waypoint=parked)
        view = await desk.fresh(LOBART)
        if not check(
            "break_unseen",
            view is not None and view.hero is not None and not view.hero.seen and view.hero.distance > 2000,
            view.hero if view else "no view",
        ):
            return _logged(results)
        baseline = set(sightings.mind(LOBART).traces)
        heard = len(sightings.mind(LOBART).heard)
        await release("hero_attack")
        await mark("hero_broken")
        completed = [r for r in events() if r.get("kind") == "destruction2_melee_complete"]
        check(
            "hero_sword_break",
            bool(completed) and completed[-1].get("case") == "farm-fence-1h",
            completed[-1:],
        )
        hidden = await desk.fresh(LOBART)
        check(
            "act_not_seen",
            hidden is not None
            and hidden.hero is not None
            and not hidden.hero.seen
            and hidden.hero.distance > 2000
            and set(sightings.mind(LOBART).traces) == baseline,
            hidden.hero if hidden else "no view",
        )
        check("act_not_heard", len(sightings.mind(LOBART).heard) == heard, f"heard {heard}")
        await move(LOBART, near="PC_HERO", distance=400)
        await link.act(LOBART, "turn_to", npc="PLAYER")
        view, broken = await broken_view(zen)
        if not check(
            "new_broken_trace",
            broken is not None
            and broken.name == "сломанный забор"
            and broken.age is not None
            and 0 <= broken.age <= 5
            and "NW_FARM1" in (broken.waypoint or ""),
            view.model_dump() if view else "no view",
        ):
            return _logged(results)
        observed = sightings.mind(LOBART).traces[f"TRACE:{broken.id}:broken"]
        sid, line = _trace_evidence_line(await sightings.situation(LOBART) or "", observed)
        check("trace_memory", bool(sid) and bool(line), line)
        row = await deny(observed)
        prompt = request(row)
        sid, line = _trace_evidence_line(prompt, observed)
        fresh = re.search(
            rf"{re.escape(trace_name(observed.trace_kind, observed.name))} "
            rf"\([^)]*{re.escape(observed.places[0])}[^)]*давность [0-5] мин\)",
            prompt,
        )
        check("trace_in_request", bool(sid) and "Следы, которые ты видел" in prompt and bool(fresh), line)
        hero_ids = [
            key
            for key, s in sightings._shown.get(LOBART, {}).items()
            if s.who == "PLAYER" and s.start == hero_start
        ]
        hero_lines = [
            line for line in prompt.splitlines() if any(line.startswith(f"- {key}:") for key in hero_ids)
        ]
        check("hero_in_request", any("ферм" in line for line in hero_lines), hero_lines)
        called = catches(row)
        check(
            "catch_uses_broken_line",
            any(c.get("arguments", {}).get("seen") == sid and c.get("outcome") == "accepted" for c in called),
            called,
        )
        await _until(lambda: bool(chronicle.facts("caught")), 15)
        caught = chronicle.facts("caught")
        check(
            "caught",
            len(caught) == 1
            and bool(_trace_evidence_line(f"- В1: {caught[0].get('how', '')}", observed)[0])
            and "сам видел:" in caught[0].get("how", ""),
            caught,
        )
        await release("save")
        await mark("broken_saved")
        saves = [
            r
            for r in events()
            if r.get("kind") == "movable_session_save" and r.get("archive") == "session-step3-broken.sav"
        ]
        check("saved_artifact", bool(saves) and saves[-1].get("bytes", 0) > 0, saves[-1:] or "no save event")
        await release("done")
        return _logged(results)
    finally:
        dialogue.llm = original
