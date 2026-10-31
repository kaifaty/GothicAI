"""One denial, two kinds of evidence: native fence debris and Hilda's theft testimony."""

from __future__ import annotations

import asyncio
import logging
import math
import re
from collections.abc import AsyncIterator
from dataclasses import asdict
from pathlib import Path
from typing import TYPE_CHECKING, Any

from ..brain.reflex import Reflexes
from ..game import GameError, GameLink
from ..llm import LLMEvent, Message, ToolCall
from ..llm.fake import FakeBackend, FakeStep
from ..protocol import Session
from ..trace import read as read_trace
from .common import (
    HILDA,
    LOBART,
    ClaimScript,
    FixedDecider,
    WitnessReaction,
    _logged,
    _save,
    _trace_evidence_line,
    _until,
    _until_async,
    attach_when_free,
    prepare_theft_witness,
    wait_message,
)

if TYPE_CHECKING:
    from ..crimes import Crimes
    from ..dialogue import DialogueManager
    from ..sightings import Sightings
    from ..social import Rumors
    from ..talk import TalkDesk
    from ..trust import Trust
    from ..witnesses import Witnesses

log = logging.getLogger(__name__)
HOUSE = "NW_FARM1_INHOUSE_01"
ASK = "я заходил в ваш дом"
AGE_LIE = "забор этот давно сломан, еще с весны"
THEFT_LIE = "у вас я ничего не брал"
DENIAL = f"{AGE_LIE}, а {THEFT_LIE}"


async def trace_crime(
    link: GameLink,
    service: Crimes,
    dialogue: DialogueManager,
    talk: TalkDesk,
    rumors: Rumors,
    witnesses: Witnesses,
    sightings: Sightings,
    trust: Trust,
    log_dir: str,
    slot: int = 7,
) -> dict[str, str]:
    """Keep native theft perceptions enabled; only positions and fake model choices are scripted.

    Lobart asks Hilda about an earlier visit before the combined denial. Her real testimony supplies
    a С row; later the actual sword break supplies a В row. Both catches must finish in the same
    conversation turn, with separate clauses as their duplicate-detection keys. No trace identifies
    a culprit or supplies proof of the theft. The final load checks the saved facts and knowledge.
    """
    results = dict.fromkeys(
        (
            "crime_heard",
            "debris_seen",
            "both_rows",
            "caught_debris",
            "caught_crime",
            "both_accepted",
            "trust",
            "no_proof_from_debris",
            "branch",
        ),
        "failed (scene did not reach this check)",
    )
    directory, desk, chronicle = Path(log_dir), sightings.desk, service.chronicle
    original_llm, original_gate, original_stage = dialogue.llm, rumors.gate, rumors.stage
    original_reflexes = talk.reflexes
    facts = chronicle.facts

    def check(name: str, ok: bool, detail: object) -> bool:
        results[name] = f"{'done' if ok else 'failed'} ({detail})"
        log.info("trace_crime %-22s %s", name, results[name])
        return ok

    def events() -> list[dict[str, Any]]:
        path = directory / "events.jsonl"
        return read_trace(path) if path.exists() else []

    async def mark(name: str) -> None:
        if not await _until(
            lambda: any(r.get("kind") == "mark" and r.get("text") == name for r in events()), 120
        ):
            raise RuntimeError(f"trace_crime bench did not reach {name}")

    async def release(name: str) -> None:
        await link.debug("destruction_traces", text=f"signal {name}")

    async def stand(npc: str, point: str, *, face: str | None = None) -> None:
        await link.debug("skip_important", near=npc)
        await link.npc_act(
            npc, "start_activity", state="ZS_STAND_WP", waypoint=point, minutes=120, place=True
        )
        if face is not None:
            await link.npc_act(npc, "turn_to", npc=face)

    async def move(*, waypoint: str | None = None, near: str | None = None, distance: int = 0) -> None:
        try:
            await link.detach(LOBART)
        except GameError as err:
            if "npc is not attached" not in str(err):
                raise
        await link.debug(
            "npc_goto", npc=LOBART, waypoint=waypoint, to=near, distance=distance, face="PC_HERO"
        )
        await attach_when_free(link, LOBART)

    async def drop() -> bool:
        try:
            await link.debug("npc_drop", npc=LOBART, item="ITMI_SILVERCUP")
        except GameError as err:
            if "could not drop it now" not in str(err):
                raise
            return False
        return True

    def turns() -> list[dict[str, Any]]:
        return [
            row
            for path in sorted((directory / "traces").glob("*.jsonl"))
            for row in read_trace(path)
            if row.get("type") == "turn" and row.get("npc") == LOBART
        ]

    def knowledge() -> list[dict[str, Any]]:
        return [asdict(k) for k in service.known(LOBART)]

    def caught() -> list[dict[str, Any]]:
        return [f for f in facts("caught") if f.get("npc") == LOBART]

    async def settled() -> bool:
        return await _until(
            lambda: (
                LOBART not in witnesses._asking
                and all(
                    not dialogue.speaking(npc)
                    and not (dialogue.conversations.get(npc) and dialogue.conversations[npc].actions)
                    for npc in (LOBART, HILDA)
                )
            ),
            45,
        )

    try:
        if not check("backend", isinstance(original_llm, FakeBackend), original_llm.name):
            return _logged(results)
        await link.wait_world()
        if link.session is None:
            await wait_message(link, Session, timeout=10)
        await mark("trace_crime_ready")
        talk.reflexes = Reflexes(FixedDecider({"posture": "listen"}))
        # This scene uses actual ask_witness testimony. Keep unrelated random meetings out of it.
        rumors.gate, rumors.stage = lambda *_: False, None
        await link.debug("player_hp", hp=5000)
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
        house = points[HOUSE]
        center = (house["x"], house["y"], house["z"])
        outside = [p for p in points.values() if p["name"].startswith("NW_FARM1_") and not p.get("room")]
        far = [p for p in outside if math.dist((p["x"], p["y"], p["z"]), center) > 8000]
        fields = [p for p in outside if "FIELD" in p["name"]]
        if not check("positions", bool(far) and bool(fields), f"{len(far)} parking, {len(fields)} field"):
            return _logged(results)
        parked = min(far, key=lambda p: math.dist((p["x"], p["y"], p["z"]), center))["name"]
        field = min(fields, key=lambda p: math.dist((p["x"], p["y"], p["z"]), center))["name"]

        await link.debug("player_goto", waypoint=HOUSE)
        for npc in farm:
            await stand(npc, parked)
        await stand(LOBART, HOUSE)
        await asyncio.sleep(0.5)
        if not check("owned_cup", await _until_async(drop, bool, 10), "native silver cup in Lobart's house"):
            return _logged(results)
        await asyncio.sleep(1)
        await stand(LOBART, parked)
        prepared, preparation = await prepare_theft_witness(link, HILDA)
        if not check("native_witness_ready", prepared, preparation):
            return _logged(results)
        before = {f["id"] for f in facts("crime")}
        with WitnessReaction(link, service.chronicle, HILDA) as reaction:
            await link.debug("player_take", item="ITMI_SILVERCUP", distance=1200)
            await _until(lambda: any(f["id"] not in before for f in facts("crime")), 10)
            crime = next((f for f in facts("crime") if f["id"] not in before), None)
            if crime is not None and not check(
                "native_reaction", await reaction.wait(str(crime["id"])), reaction.detail
            ):
                return _logged(results)
        # Keep the witness in place until the held rule and its native voice have finished.
        await stand(HILDA, field)
        if not check(
            "native_theft",
            crime is not None
            and crime.get("kind") == "theft"
            and crime.get("owner") == LOBART
            and set(crime.get("witnesses", ())) == {HILDA}
            and any(k.crime["id"] == crime["id"] and k.degree == "seen" for k in service.known(HILDA))
            and not service.known(LOBART),
            crime,
        ):
            return _logged(results)
        assert crime is not None
        cid = str(crime["id"])

        await stand(LOBART, field, face="PLAYER")
        await link.debug("player_goto", to=LOBART, distance=300)
        if not check(
            "talk_ready",
            await _until(lambda: LOBART in talk.people and HILDA in talk.people, 15),
            "Lobart and Hilda within hearing",
        ):
            return _logged(results)
        await witnesses.prepare(LOBART)
        dialogue.llm = ClaimScript(
            [
                (
                    ASK,
                    FakeStep(
                        "Хильда, что ты видела в доме?",
                        [ToolCall("ask_witness", {"who": "Хильда", "said": ASK})],
                    ),
                )
            ],
            [],
            "Я запомню, что сказала Хильда.",
            only="Лобарт",
            others=FakeBackend("Я сама видела, как он взял серебряный кубок в нашем доме.", delay=0),
        )
        await link.debug("type", text=f"Лобарт, {ASK}.")
        await _until(lambda: any(cid in f.get("crime_told", []) for f in facts("testimony")), 60)
        await settled()
        testimony = [
            f for f in facts("testimony") if f.get("npc") == LOBART and cid in f.get("crime_told", [])
        ]
        known = service.known(LOBART)
        if not check(
            "crime_heard",
            len(known) == 1
            and known[0].crime["id"] == cid
            and known[0].degree == "heard"
            and known[0].source == HILDA
            and len(testimony) == 1
            and testimony[0].get("witness") == HILDA
            and not caught(),
            {"knowledge": knowledge(), "testimony": testimony, "caught": caught()},
        ):
            return _logged(results)
        talk.end("gone")
        await settled()

        await release("setup")
        await mark("fence_ready")
        setup = next(r for r in reversed(events()) if r.get("kind") == "destruction2_farm_setup")
        await move(near="PC_HERO", distance=300)
        await link.act(LOBART, "turn_to", npc="PLAYER")
        view = await desk.fresh(LOBART)
        trail = sightings.hero_trail(LOBART)
        if not check(
            "hero_on_farm",
            view is not None
            and view.hero is not None
            and view.hero.seen
            and bool(trail)
            and any("ферм" in p for p in trail[0].places),
            [s.record() for s in trail],
        ):
            return _logged(results)
        await move(waypoint=parked)
        hidden = await desk.fresh(LOBART)
        if not check(
            "break_unseen",
            hidden is not None
            and hidden.hero is not None
            and not hidden.hero.seen
            and hidden.hero.distance > 2000,
            hidden.hero if hidden else "no view",
        ):
            return _logged(results)
        baseline = set(sightings.mind(LOBART).traces)
        await release("hero_attack")
        await mark("hero_broken")
        completed = [r for r in events() if r.get("kind") == "destruction2_melee_complete"]
        await desk.fresh(LOBART)
        if not check(
            "native_break",
            setup.get("actor") == "PC_HERO"
            and setup.get("from_map")
            and bool(completed)
            and completed[-1].get("case") == "farm-fence-1h"
            and set(sightings.mind(LOBART).traces) == baseline,
            completed,
        ):
            return _logged(results)
        await move(near="PC_HERO", distance=400)
        await link.act(LOBART, "turn_to", npc="PLAYER")
        view = await desk.fresh(LOBART)
        debris = (
            next(
                (t for t in view.traces if t.kind == "broken" and t.id.endswith(f":vob:{setup['zen_id']}")),
                None,
            )
            if view is not None
            else None
        )
        observed = sightings.mind(LOBART).traces.get(f"TRACE:{debris.id}:broken") if debris else None
        if not check(
            "debris_seen",
            debris is not None
            and observed is not None
            and observed.trace_kind == "broken"
            and observed.trace_age is not None
            and 0 <= observed.trace_age <= 5
            and observed.name == "сломанный забор"
            and "NW_FARM1" in (debris.waypoint or ""),
            observed.record() if observed else "no remembered native debris",
        ):
            return _logged(results)
        assert observed is not None

        class EvidenceScript:
            """Choose evidence ids from this request: fresh sightings can renumber the В rows."""

            name = "trace-crime-script"

            async def stream(self, **kw: Any) -> AsyncIterator[LLMEvent]:
                prompt = "\n".join(
                    m.content for m in kw["messages"] if isinstance(m, Message) and m.role == "developer"
                )
                last = kw["messages"][-1]
                step = FakeStep("Я запомню.")
                if kw.get("cache_key") == LOBART and isinstance(last, Message) and DENIAL in last.content:
                    sid, _ = _trace_evidence_line(prompt, observed)
                    told_ids = re.findall(r"^- (С\d+):", prompt, re.MULTILINE)
                    told = next(
                        (s for s in told_ids if (t := witnesses.told(LOBART, s)) and t.crime == cid), ""
                    )
                    step = FakeStep(
                        "Не с весны: обломки свежие! И Хильда сама видела, как ты взял наш кубок.",
                        [
                            ToolCall("catch_lie", {"said": AGE_LIE, "seen": sid}),
                            ToolCall("catch_lie", {"said": THEFT_LIE, "seen": told}),
                        ],
                    )
                async for event in FakeBackend(script=[step], delay=0).stream(**kw):
                    yield event

        dialogue.llm = EvidenceScript()  # type: ignore[assignment]
        await link.debug("teleport", near=LOBART, distance=250)
        await _until(lambda: LOBART in talk.people and talk.people[LOBART].distance < 800, 10)
        previous = {r["id"] for r in turns()}
        await link.debug("type", text=f"Лобарт, {DENIAL}.")
        await _until(
            lambda: any(
                r["id"] not in previous and DENIAL in r.get("trigger", {}).get("text", "") for r in turns()
            ),
            60,
        )
        await settled()
        rows = [
            r for r in turns() if r["id"] not in previous and DENIAL in r.get("trigger", {}).get("text", "")
        ]
        row = rows[-1] if rows else {}
        prompt = "\n".join(
            str(item.get("content", ""))
            for step in row.get("steps", [])
            for item in step.get("added", [])
            if item.get("role") == "developer"
        )
        sid, debris_line = _trace_evidence_line(prompt, observed)
        told_lines = [
            line
            for line in prompt.splitlines()
            if re.match(r"- С\d+:", line) and "Хильд" in line and service.describe(crime) in line
        ]
        calls = [
            c for step in row.get("steps", []) for c in step.get("calls", []) if c.get("name") == "catch_lie"
        ]
        check(
            "both_rows",
            len(rows) == 1
            and row.get("status") == "completed"
            and bool(sid)
            and bool(re.search(r"давность при наблюдении [0-5] мин", debris_line))
            and bool(told_lines),
            {"turn": row.get("id"), "debris": debris_line, "testimony": told_lines},
        )
        check(
            "both_accepted",
            len(calls) == 2
            and all(c.get("outcome") == "accepted" for c in calls)
            and calls[0].get("arguments") == {"said": AGE_LIE, "seen": sid}
            and any(line.startswith(f"- {calls[1].get('arguments', {}).get('seen')}:") for line in told_lines)
            and calls[1].get("arguments", {}).get("said") == THEFT_LIE,
            calls,
        )
        debris_facts = [f for f in caught() if f.get("title") == AGE_LIE]
        crime_facts = [f for f in caught() if f.get("title") == THEFT_LIE]
        check(
            "caught_debris",
            len(debris_facts) == 1
            and "crime" not in debris_facts[0]
            and "сам видел:" in debris_facts[0].get("how", "")
            and bool(_trace_evidence_line(f"- В1: {debris_facts[0].get('how', '')}", observed)[0]),
            debris_facts,
        )
        check(
            "caught_crime",
            len(crime_facts) == 1
            and crime_facts[0].get("crime") == cid
            and crime_facts[0].get("witness") == HILDA
            and crime_facts[0].get("testimony") == testimony[0]["id"]
            and "со слов:" in crime_facts[0].get("how", ""),
            crime_facts,
        )
        record = trust.of(LOBART)
        check(
            "trust",
            len(record.lied) == 2
            and len(record.crimes) == 1
            and record.crimes.count(service.describe(crime)) == 1
            and record.broken,
            {"record": asdict(record), "summary": trust.situation(LOBART)},
        )
        known = service.known(LOBART)
        check(
            "no_proof_from_debris",
            len(known) == 1
            and known[0].crime["id"] == cid
            and known[0].degree == "heard"
            and known[0].source == HILDA
            and not service.seen(LOBART),
            knowledge(),
        )

        talk.end("gone")
        await settled()
        snapshot_facts = [
            f for f in facts() if f.get("fact") in {"crime", "crime_seen", "testimony", "caught"}
        ]
        snapshot_knowledge, snapshot_trust = knowledge(), asdict(trust.of(LOBART))
        snapshot_debris = observed.record()
        head = sightings.memory.head(chronicle.branch) if chronicle.branch else 0
        await asyncio.sleep(0.5)  # the bridge must receive the memory head before the save
        saved = await _save(link, slot)
        if saved is None:
            check("branch", False, "save was refused")
            return _logged(results)
        loading = asyncio.create_task(wait_message(link, Session, lambda m: m.reason == "load", 60))
        await asyncio.sleep(0)
        await link.debug("load", slot)
        session = await loading
        await link.wait_world()
        loaded_debris = sightings.mind(LOBART).traces.get(observed.who)
        loaded_facts = [f for f in facts() if f.get("fact") in {"crime", "crime_seen", "testimony", "caught"}]
        check(
            "branch",
            saved.seq >= head
            and session.branch != saved.branch
            and session.parent == saved.branch
            and session.parent_seq == saved.seq
            and loaded_facts == snapshot_facts
            and knowledge() == snapshot_knowledge
            and asdict(trust.of(LOBART)) == snapshot_trust
            and loaded_debris is not None
            and loaded_debris.record() == snapshot_debris,
            {
                "saved_seq": saved.seq,
                "head": head,
                "facts_before": len(snapshot_facts),
                "facts_after": len(loaded_facts),
                "knowledge": knowledge(),
                "debris": loaded_debris.record() if loaded_debris else None,
            },
        )
        return _logged(results)
    finally:
        dialogue.llm, talk.reflexes = original_llm, original_reflexes
        rumors.gate, rumors.stage = original_gate, original_stage
