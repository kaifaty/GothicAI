"""A known theft follows the hero home, then a brave victim asks for paid amends."""

from __future__ import annotations

import asyncio
import logging
import math
import time
from collections.abc import AsyncIterator
from dataclasses import replace
from typing import TYPE_CHECKING, Any

from ..game import GameError, GameLink
from ..llm import Done, LLMEvent, TextDelta, ToolCall
from ..protocol import GameMessage, NpcActCmd, NpcActResult, NpcEvent, NpcIdle, NpcView, Session
from ..trace import CallTrace, TurnTrace
from .common import (
    HILDA,
    LOBART,
    SharedBodyWatch,
    WitnessReaction,
    _logged,
    _save,
    _until,
    _until_async,
    prepare_theft_witness,
    wait_message,
)
from .crimes import HOUSE

SCENARIOS = {
    "crime_meet": ("GIVEN", {"save": True, "timeout": 600}),
}

if TYPE_CHECKING:
    from ..amends import Amends, Proposal, Storylet
    from ..crime_meet import CrimeMeet
    from ..crime_react import CrimeReact
    from ..crimes import Crimes
    from .live import LiveCase

log = logging.getLogger(__name__)
CUP, GOLD = "ITMI_SILVERCUP", "ITMI_GOLD"
HERO_POINT, WITNESS_POINT = "NW_FARM1_INHOUSE_01", "NW_FARM1_INHOUSE_02"
MEET_POINT, MEET_PATH = "NW_FARM1_PATH_01", "NW_FARM1_ENTRANCE_03"
NEAR_REPEAT_S = 31.0  # bridge NearPeriodMs=30000; leaving alone does not reset the throttle
PUBLIC_PORTAL_GUILDS = {-1, 0, 15}  # unknown, GIL_NONE and the positive GIL_PUBLIC


async def crime_meet(
    link: GameLink,
    service: Crimes,
    reactions: CrimeReact,
    meets: CrimeMeet,
    amends: Amends,
    slot: int = 7,
    *,
    live: LiveCase | None = None,
) -> dict[str, str]:
    """Two branches of save 1 using native rules, including the required Amends demand.

    The native drop owns the cup, the native take supplies the witness, and common helpers prove
    sight and wait for the held rule's recorded SVM. No fixed pause stands in for scene readiness.
    Lobart keeps his production courage 2 during the theft; only the later meeting uses courage 3.
    Murder guards and city-gate dialogue paths are covered by the corresponding module tests.
    """
    results: dict[str, str] = {}
    facts = service.chronicle.facts
    original_cards, original_meet_cards = reactions.cards, meets.cards
    original_queue = amends._queue_crime
    original_model_llm, original_try, original_fallback = amends.model.llm, amends._try, amends._fallback
    model_traces: list[TurnTrace] = []
    demand_source: list[dict[str, Any]] = []
    fallback_used = False

    def close_model_traces(outcome: str) -> None:
        for trace in model_traces:
            if trace.closed:
                continue
            for step in trace.steps:
                for call in step.calls:
                    call.outcome = outcome if call.name == "propose_amends" else "ignored"
            trace.close("completed")

    class DemandTrace:
        # Amends has its own model loop. Record its actual requests in this live case as well.
        def __getattr__(self, name: str) -> Any:
            return getattr(original_model_llm, name)

        async def stream(self, **kwargs: Any) -> AsyncIterator[LLMEvent]:
            close_model_traces("rejected")
            assert live is not None
            trace = TurnTrace(
                live.trace, npc=LOBART, branch=None, trigger={"kind": "amends", "text": "victim demand"},
                instructions=kwargs.get("instructions", ""), tools=["propose_amends"], snapshot=None,
                llm=original_model_llm.name, model=getattr(original_model_llm, "model", None),
            )
            model_traces.append(trace)
            step = trace.step(kwargs.get("messages", []), kwargs.get("tool_choice"))
            started = time.monotonic()
            done = None
            try:
                async for event in original_model_llm.stream(**kwargs):
                    if isinstance(event, ToolCall):
                        step.calls.append(
                            CallTrace(event.name, event.arguments, event.call_id, "", "pending")
                        )
                    elif isinstance(event, TextDelta):
                        step.text += event.text
                    elif isinstance(event, Done):
                        done = event
                    yield event
            finally:
                trace.finish_step(step, done, None, time.monotonic() - started)
                if not step.calls:
                    trace.close("completed")

    async def tried(*args: Any, **kwargs: Any) -> tuple[Storylet | None, list[str]]:
        made, errors = await original_try(*args, **kwargs)
        if not fallback_used:
            close_model_traces("accepted" if made is not None else "rejected")
        if made is not None:
            demand_source.append({"source": "fallback" if fallback_used else "model", "storylet": made.id})
        return made, errors

    def fallback(*args: Any, **kwargs: Any) -> Proposal:
        nonlocal fallback_used
        close_model_traces("rejected")
        fallback_used = True
        return original_fallback(*args, **kwargs)
    sent: list[Any] = []
    received: list[tuple[float, GameMessage]] = []
    hero_states: list[dict[str, Any]] = []
    hero_symbol: set[int] = set()
    meeting_origin: tuple[float, ...] | None = None
    send = link.send

    def check(name: str, ok: bool, detail: object = "") -> bool:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"
        log.info("crime_meet %-24s %s", name, results[name])
        return ok

    async def sending(command: Any) -> None:
        sent.append(command)
        await send(command)

    def record(message: GameMessage) -> None:
        received.append((time.monotonic(), message))

    def observe(packet: dict[str, Any]) -> None:
        # The P30 pilot may add Lobart to the shared subscription: keep only the hero's rows.
        hero_states.extend(
            row["self"] for row in packet["observations"] if row["script_symbol"] in hero_symbol
        )

    def deferred_crime(_npc: str, _crime: str) -> None:
        # Preparation is not a meeting. Avoid an eager important Amends TALK consuming the scene
        # while the theft's HANDSOFF is still playing. The real meeting calls Amends.demand.
        pass

    def known(cid: str) -> bool:
        return any(k.crime.get("id") == cid and k.degree == "seen" for k in service.known(LOBART))

    def meet_rows(cid: str) -> list[dict[str, Any]]:
        return [f for f in facts("crime_meet") if f.get("npc") == LOBART and f.get("crime") == cid]

    def events(kind: str, since: float = 0) -> list[NpcEvent]:
        return [
            m
            for at, m in received
            if at >= since and isinstance(m, NpcEvent) and m.npc == LOBART and m.kind == kind
        ]

    async def count(item: str, npc: str = "HERO") -> int:
        return int(await link.evaluate(["call", "NPC_HASITEMS", ["inst", npc], ["sym", item]]) or 0)

    async def view() -> NpcView | None:
        views = await link.npc_view([LOBART])
        return views[0] if views else None

    async def placed(npc: str, point: str, *, ready: bool = False) -> bool:
        await link.debug("npc_goto", npc=npc, waypoint=point)
        act = await link.npc_act(
            npc, "start_activity", state="ZS_STAND_WP", waypoint=point, minutes=120, place=False
        )
        accepted = await _until(
            lambda: any(
                isinstance(m, NpcActResult) and m.id == act and m.status == "queued" for _, m in received
            ),
            5,
        )
        if not accepted:
            return False

        async def read_ready() -> bool:
            views = await link.npc_view([npc])
            if not views:
                return False
            snapshot = await link.snapshot(npc, fresh=True)
            expected = points[point]
            actual = snapshot.npc.pos
            return (
                views[0].state == "ZS_STAND_WP"
                and views[0].body == "stand"
                and (not ready or views[0].theft_ready is True)
                and math.hypot(actual[0] - expected["x"], actual[2] - expected["z"]) <= 200
            )

        return bool(await _until_async(read_ready, bool, 10))

    async def away() -> bool:
        snapshot = await link.snapshot(LOBART, fresh=True)
        origin = snapshot.npc.pos
        exits = [p for p in candidates if math.hypot(p["x"] - origin[0], p["z"] - origin[2]) > 1800]
        if not exits:
            return False
        point = min(exits, key=lambda p: math.dist((p["x"], p["y"], p["z"]), origin))
        await link.debug("player_goto", waypoint=point["name"])

        async def distant() -> bool:
            sample = await view()
            return sample is not None and sample.hero is not None and sample.hero.distance > 700

        return bool(await _until_async(distant, bool, 5))

    async def clean(label: str) -> bool:
        reactions.cards, meets.cards = original_cards, original_meet_cards
        amends._queue_crime = deferred_crime  # type: ignore[method-assign]
        loading = asyncio.create_task(wait_message(link, Session, lambda m: m.reason == "load", 60))
        await asyncio.sleep(0)
        await link.debug("load", slot + 1)
        await loading
        await link.wait_world()
        await watch.watch([])
        sent.clear()
        hero_states.clear()
        return check(f"{label}_branch", not facts("crime"), f"{len(facts('crime'))} crimes")

    async def theft(label: str) -> dict[str, Any] | None:
        amends._queue_crime = deferred_crime  # type: ignore[method-assign]
        # Native perceptions initialize only near the hero. Save 1 may put him at Xardas;
        # bring the scene into the active world before checking the owner's callbacks.
        await link.debug("player_goto", waypoint=HERO_POINT)
        for npc in farm:
            if npc != LOBART and not await placed(npc, parked):
                check(f"{label}_parking", False, npc)
                return None
        if not check(f"{label}_owner_ready", await placed(LOBART, WITNESS_POINT, ready=True), LOBART):
            return None

        async def drop() -> bool:
            try:
                await link.debug("npc_drop", npc=LOBART, item=CUP)
                return True
            except GameError as error:
                if "could not drop it now" not in str(error):
                    raise
                return False

        if not check(f"{label}_drop", await _until_async(drop, bool, 10), "native owner silver cup"):
            return None
        prepared, preparation = await prepare_theft_witness(link, LOBART)
        if not check(f"{label}_witness_ready", prepared, preparation):
            return None
        await link.debug("player_hp", hp=5000)
        before = {f["id"] for f in facts("crime")}
        with WitnessReaction(link, service.chronicle, LOBART) as reaction:
            await link.debug("player_take", item=CUP, distance=1200)
            await _until(lambda: any(f["id"] not in before for f in facts("crime")), 10)
            crime = next((f for f in facts("crime") if f["id"] not in before), None)
            if crime is None:
                check(f"{label}_theft", False, "no native crime")
                return None
            # A room assessment after the projection must belong to the later entry. Leave as soon
            # as the deed is accepted, before waiting out HANDSOFF's actual recorded voice.
            if not check(f"{label}_left", await away(), "hero outside, native speech remains intact"):
                return None
            if not check(
                f"{label}_native_reaction",
                await reaction.wait(str(crime["id"]), reaction="demand", svm="HANDSOFF"),
                reaction.detail,
            ):
                return None
        cid = str(crime["id"])
        if not check(
            f"{label}_theft",
            crime.get("kind") == "theft"
            and crime.get("owner") == LOBART
            and set(crime.get("witnesses", ())) == {LOBART}
            and known(cid),
            crime,
        ):
            return None
        return crime

    async def home_entry() -> tuple[bool, dict[str, Any]]:
        """Prove the actual household, portal and sight, independently of a waypoint's name.

        INIT_SUB_NEWWORLD_PART_FARM_01 marks Lobart's actual house GIL_PUBLIC. Its native portal,
        mapped house and real sleep routine still identify whose home it is. Keep the theft on the
        shared 01/02 pair; only the later room scene chooses a visible standing point and faces it.
        """
        detail: dict[str, Any] = {"attempts": []}
        for point in ("NW_FARM1_INHOUSE_03", "NW_FARM1_INHOUSE_03_SIT", HERO_POINT):
            if point not in points:
                continue
            await link.debug("player_goto", waypoint=point)
            # Let the native turn finish after ZS_STAND_WP's alignment. Debug npc_goto resolves
            # script symbols for face, whereas PLAYER is the soft-action target alias.
            turn = await link.npc_act(LOBART, "turn_to", npc="PLAYER")
            if not await _until(
                lambda turn=turn: any(
                    isinstance(m, NpcIdle) and m.act == turn and m.reason == "done"
                    for _, m in received
                ),
                10,
            ):
                detail["attempts"].append({"point": point, "turn": "not completed"})
                continue
            last: dict[str, Any] = {"point": point}

            async def ready(last: dict[str, Any] = last) -> bool:
                (
                    player_portal,
                    npc_portal,
                    same_room,
                    can_see,
                    free_los,
                    distance,
                    attitude,
                    bothered,
                ) = await link.story_eval(
                    LOBART,
                    [
                        ["call", "WLD_GETPLAYERPORTALGUILD"],
                        ["call", "NPC_GETPORTALGUILD", ["inst", "SELF"]],
                        ["call", "NPC_ISINPLAYERSROOM", ["inst", "SELF"]],
                        ["call", "NPC_CANSEENPC", ["inst", "SELF"], ["inst", "HERO"]],
                        ["call", "NPC_CANSEENPCFREELOS", ["inst", "SELF"], ["inst", "HERO"]],
                        ["call", "NPC_GETDISTTONPC", ["inst", "SELF"], ["inst", "HERO"]],
                        ["call", "NPC_GETATTITUDE", ["inst", "SELF"], ["inst", "HERO"]],
                        ["fn", "C_NPCISBOTHEREDBYPLAYERROOMGUILD", ["inst", "SELF"]],
                    ],
                )
                sample = await view()
                snapshot = await link.snapshot(LOBART, fresh=True)
                npc_place = service.places.of(sample.nearest_waypoint) if service.places and sample else None
                hero_place = (
                    service.places.of(sample.hero.waypoint)
                    if service.places and sample and sample.hero
                    else None
                )
                household = reactions.home(LOBART)
                mapped_home = (
                    npc_place is not None
                    and npc_place.kind == "house"
                    and npc_place.inside
                    and hero_place is not None
                    and hero_place.id == npc_place.id
                    and household == f"place:{npc_place.id}"
                )
                last.update(
                    {
                        "view": sample.model_dump() if sample is not None else None,
                        "player_portal_guild": player_portal,
                        "npc_portal_guild": npc_portal,
                        "same_room": same_room,
                        "can_see": can_see,
                        "free_los": free_los,
                        "distance": distance,
                        "attitude": attitude,
                        "bothered_guild": bothered,
                        "public_portal": player_portal in PUBLIC_PORTAL_GUILDS,
                        "household": household,
                        "npc_place": npc_place.id if npc_place is not None else None,
                        "hero_place": hero_place.id if hero_place is not None else None,
                        "npc_pos": snapshot.npc.pos,
                        "hero_pos": snapshot.player.pos if snapshot.player is not None else None,
                    }
                )
                return (
                    player_portal not in {-1, 0}
                    and player_portal == npc_portal
                    and bool(same_room)
                    and mapped_home
                    and bool(can_see)
                    and sample is not None
                    and sample.indoors
                    and sample.hero is not None
                    and sample.hero.seen
                    and distance <= 1000
                )

            prepared = await _until_async(ready, bool, 3)
            detail["attempts"].append(last)
            log.info("crime_meet home entry preparation: %s", last)
            if prepared:
                # ASSESSENTERROOM fires at the portal crossing. The first crossing can precede
                # the completed turn, so make the actual test entry with sight already proved.
                if not await away():
                    return False, detail
                if not await _until(lambda: LOBART not in meets._active, 3):
                    return False, detail
                await link.debug("player_goto", waypoint=point)
                return bool(await _until_async(ready, bool, 3)), detail
            # The next candidate is a new physical entry, not a retry of the reaction. Nothing in
            # this helper calls a perception, changes a room's owner, or supplies a crime memory.
            if not await away() or not await placed(LOBART, WITNESS_POINT, ready=True):
                break
        return False, detail

    async def next_meeting() -> float | None:
        nonlocal meeting_origin
        if not await away():
            return None
        last_near = max(
            (
                at
                for at, m in received
                if isinstance(m, NpcEvent) and m.npc == LOBART and m.kind == "player_near"
            ),
            default=time.monotonic() - NEAR_REPEAT_S,
        )
        # Poll the bridge's known throttle boundary; do not mistake a teleport for a new meeting.
        if not await _until(lambda: time.monotonic() - last_near >= NEAR_REPEAT_S, NEAR_REPEAT_S + 1):
            return None
        start = time.monotonic()
        snapshot = await link.snapshot(LOBART, fresh=True)
        target = points[MEET_PATH]
        origin = snapshot.npc.pos
        meeting_origin = tuple(origin)
        delta = (target["x"] - origin[0], target["z"] - origin[2])
        distance = math.hypot(*delta)
        if distance < 500:
            return None
        await link.debug(
            "player_goto",
            pos=(origin[0] + 490 * delta[0] / distance, origin[1], origin[2] + 490 * delta[1] / distance),
        )
        return start if await _until(lambda: bool(events("player_near", start)), 10) else None

    await link.wait_world()
    if link.session is None:
        await wait_message(link, Session, timeout=10)
    hero_symbol.update(await link.story_eval(LOBART, [["sym", "PC_HERO"]]))
    link.send = sending  # type: ignore[method-assign]
    link.listeners.append(record)
    link.raw_body_listeners.append(observe)
    watch = SharedBodyWatch(link, "crime_meet")
    amends._queue_crime = deferred_crime  # type: ignore[method-assign]
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
        needed = {HOUSE, HERO_POINT, WITNESS_POINT, MEET_POINT, MEET_PATH}
        if not check("farm", needed <= points.keys() and {LOBART, HILDA} <= humans):
            return _logged(results)
        origin = points[WITNESS_POINT]
        house_pos = (origin["x"], origin["y"], origin["z"])
        candidates = [p for p in points.values() if p["name"].startswith("NW_FARM1_") and not p.get("room")]

        def distant(point: dict[str, Any]) -> float:
            return math.dist((point["x"], point["y"], point["z"]), house_pos)

        parking = [p for p in candidates if distant(p) > 8000]
        exits = [p for p in candidates if distant(p) > 1800]
        if not check("positions", bool(parking) and bool(exits)):
            return _logged(results)
        parked = min(parking, key=distant)["name"]
        for npc in farm:
            await link.debug("skip_important", near=npc)
        if not check("owner_card", original_cards[LOBART].courage == 2, "production courage=2"):
            return _logged(results)
        if not check("baseline", not facts("crime") and await _save(link, slot + 1) is not None):
            return _logged(results)

        if live is None:
            crime = await theft("home")
            if crime is None:
                return _logged(results)
            cid = str(crime["id"])
            if not check("home_owner_ready", await placed(LOBART, WITNESS_POINT, ready=True)):
                return _logged(results)
            await watch.watch(["PLAYER"])
            entered = time.monotonic()
            before_home = len(sent)
            prepared, preparation = await home_entry()
            if not check("home_entry_ready", prepared, preparation):
                return _logged(results)
            await link.debug("player_hp", hp=10)
            if not check("home_enter_room", await _until(lambda: bool(events("enter_room", entered)), 15)):
                return _logged(results)

            def home_actions(action: str) -> list[NpcActCmd]:
                return [
                    c
                    for c in sent[before_home:]
                    if isinstance(c, NpcActCmd) and c.npc == LOBART and c.action == action
                ]

            def accepted(action: NpcActCmd) -> bool:
                return any(
                    isinstance(m, NpcActResult) and m.id == action.id and m.status == "queued"
                    for _, m in received
                )

            def voiced() -> bool:
                voices = [
                    c
                    for c in home_actions("say_svm")
                    if str(c.args.get("svm", "")).upper().lstrip("$") == "GETOUTOFHERE"
                ]
                return any(
                    accepted(c)
                    and any(
                        isinstance(m, NpcIdle) and m.act == c.id and m.reason == "done" for _, m in received
                    )
                    for c in voices
                )

            if not check(
                "home_warning",
                await _until(voiced, 25),
                [c.model_dump() for c in home_actions("say_svm")],
            ):
                return _logged(results)
            if not check(
                "home_meeting",
                any(f.get("event") == "enter_room" and f.get("reaction") == "room" for f in meet_rows(cid)),
                meet_rows(cid),
            ):
                return _logged(results)
            reason = await link.evaluate(["var", "AR_CLEARROOM"])
            if not check(
                "home_clearroom_fight",
                await _until(
                    lambda: any(
                        accepted(c) and c.args.get("reason") in {"AR_CLEARROOM", reason}
                        for c in home_actions("fight")
                    ),
                    15,
                ),
                [c.model_dump() for c in home_actions("fight")],
            ):
                return _logged(results)
            down = await _until(lambda: any(s["unconscious"] and not s["dead"] for s in hero_states), 60)
            if not check(
                "home_knocked_out",
                down and not any(s["dead"] for s in hero_states),
                {"samples": len(hero_states), "latest": hero_states[-1] if hero_states else None},
            ):
                return _logged(results)
            if not await clean("victim"):
                return _logged(results)

        crime = await theft("victim")
        if crime is None:
            return _logged(results)
        cid = str(crime["id"])
        if not check("victim_position", await placed(LOBART, MEET_POINT), MEET_POINT):
            return _logged(results)
        meets.cards = {**original_meet_cards, LOBART: replace(original_cards[LOBART], courage=3)}
        reactions.cards = {**original_cards, LOBART: replace(original_cards[LOBART], courage=3)}
        if live is None:
            log.info("given: дано: Лобарт храбр при встрече (courage 3 после кражи)")
        else:
            log.info("scenario: victim meeting uses Lobart courage 3 after theft")
            amends.model.llm = DemandTrace()
            amends._try = tried  # type: ignore[method-assign]
            amends._fallback = fallback  # type: ignore[method-assign]
        check("victim_card", True, "scenario only: courage 2 -> 3 after the theft")
        amends._queue_crime = original_queue  # type: ignore[method-assign]
        deadline = time.monotonic() + 120
        if not check("victim_player_near", await next_meeting() is not None):
            return _logged(results)
        approached = await _until(
            lambda: any(
                f.get("npc") == LOBART and f.get("occasion") == f"crime:{cid}" for f in facts("initiative")
            ),
            30,
        )
        detail: object = facts("initiative")[-1:]
        if not approached:
            sample = await view()
            walks = [
                c for c in sent
                if isinstance(c, NpcActCmd) and c.npc == LOBART and c.action == "go_to_npc"
            ]
            detail = {
                "view": sample.model_dump() if sample else None,
                "walks": [c.model_dump() for c in walks],
                "receipts": [
                    m.model_dump() for _, m in received
                    if isinstance(m, NpcActResult | NpcIdle) and m.npc == LOBART
                ],
            }
        if not check("victim_initiative", approached, detail):
            return _logged(results)
        check(
            "victim_meeting",
            await _until(
                lambda: any(
                    f.get("reaction") == "demand" and f.get("source") == "personal"
                    for f in meet_rows(cid)
                ),
                10,
            ),
            meet_rows(cid),
        )
        key = f"crime:{cid}"
        if live is not None:
            # The decision and native installation share the demand's deadline.
            await _until(
                lambda: bool(demand_source)
                and any(f.get("npc") == LOBART and f.get("lie") == key for f in facts("amends_open")),
                max(0, deadline - time.monotonic()),
            )
        else:
            await _until(
                lambda: any(s.npc == LOBART and s.lie == key for s in amends.storylets.values()), 120
            )
        storylet = next((s for s in amends.storylets.values() if s.npc == LOBART and s.lie == key), None)
        if live is not None:
            # An accepted model proposal is only counted after its real native storylet is installed.
            opened = [f for f in facts("amends_open") if f.get("npc") == LOBART and f.get("lie") == key]
            source = demand_source[-1]["source"] if demand_source else None
            live.finish("failure" if fallback_used else "success" if source == "model" and opened
                        else "no_decision", evidence={
                            "decision": source or ("fallback" if fallback_used else "no_demand"),
                            "crime": crime, "meeting": meet_rows(cid), "opened": opened,
                            "fallback_used": fallback_used, "accepted": demand_source,
                            "calls": live.calls(LOBART, "propose_amends"),
                            "rejected": [f for f in facts("amends_rejected") if f.get("lie") == key],
                        })
            return _logged(results)
        price = (await link.story_eval(LOBART, [["field", ["inst", CUP], "VALUE"]]))[0]
        limit = min(100, price) if price > 0 else 50  # the meeting rule when no VM price is available
        if not check(
            "victim_amends",
            storylet is not None and storylet.stage != "done" and 0 < storylet.gold <= limit,
            {"storylet": storylet, "item_price": price, "gold_limit": limit},
        ):
            return _logged(results)
        assert storylet is not None
        if not check(
            "victim_automatic_talk", await _until(lambda: storylet.stage != "open", 30), storylet.talk
        ):
            return _logged(results)
        walks = [c for c in sent if isinstance(c, NpcActCmd) and c.npc == LOBART and c.action == "go_to_npc"]
        accepted_walk = await _until(
            lambda: any(
                isinstance(m, NpcActResult) and m.status == "queued" and m.id in {c.id for c in walks}
                for _, m in received
            ),
            5,
        )
        arrived = await link.snapshot(LOBART, fresh=True)
        moved = (
            math.hypot(arrived.npc.pos[0] - meeting_origin[0], arrived.npc.pos[2] - meeting_origin[2])
            if meeting_origin is not None
            else 0
        )
        if not check(
            "victim_approached",
            # A hero already in native dialogue range may get TALK before the first step.
            accepted_walk and (moved >= 25 or storylet.stage != "open"),
            {
                "origin": meeting_origin,
                "arrived": arrived.npc.pos,
                "moved_cm": moved,
                "native_talk": storylet.stage != "open",
                "walks": [c.model_dump() for c in walks],
            },
        ):
            return _logged(results)
        gold = await count(GOLD)
        if gold < storylet.gold:
            await link.story_effect(LOBART, [{"do": "give", "item": GOLD, "count": storylet.gold - gold}])
            gold = await count(GOLD)
        if not check(
            "victim_payment_offered",
            await _until_async(
                lambda: link.offered_dialogs(LOBART), lambda offered: storylet.pay in offered, 20
            ),
            storylet.pay,
        ):
            return _logged(results)
        await link.choose_dialog(storylet.to, storylet.pay)
        paid = await _until(lambda: storylet.stage == "done", 20)
        if not check(
            "victim_paid",
            paid and storylet.gold > 0 and await count(GOLD) == gold - storylet.gold,
            storylet.gold,
        ):
            return _logged(results)
        if not check(
            "victim_forgiven_paid",
            await _until(
                lambda: any(
                    f.get("npc") == LOBART and f.get("crime") == cid and f.get("paid")
                    for f in facts("forgiven")
                ),
                10,
            ),
            facts("forgiven")[-1:],
        ):
            return _logged(results)
        previous = list(meet_rows(cid))
        previous_walks = len(walks)
        await link.story_effect(LOBART, [{"do": "end"}])
        if not check("paid_next_player_near", await next_meeting() is not None):
            return _logged(results)
        # One fresh native perception sample after that event also gives the meet task a poll.
        await view()
        check(
            "paid_no_reaction",
            meet_rows(cid) == previous
            and len(
                [c for c in sent if isinstance(c, NpcActCmd) and c.npc == LOBART and c.action == "go_to_npc"]
            )
            == previous_walks,
            meet_rows(cid),
        )
        return _logged(results)
    finally:
        if live is not None:
            close_model_traces("rejected")
            amends.model.llm = original_model_llm
            amends._try = original_try  # type: ignore[method-assign]
            amends._fallback = original_fallback  # type: ignore[method-assign]
        reactions.cards, meets.cards = original_cards, original_meet_cards
        amends._queue_crime = original_queue  # type: ignore[method-assign]
        try:
            if not link.closed.is_set():
                await watch.watch([])
        finally:
            watch.close()
            link.send = send  # type: ignore[method-assign]
            link.listeners.remove(record)
            link.raw_body_listeners.remove(observe)
