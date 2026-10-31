"""A known theft follows the hero home, then a brave victim asks for paid amends."""

from __future__ import annotations

import asyncio
import logging
import math
import re
import time
from collections.abc import AsyncIterator
from dataclasses import replace
from typing import TYPE_CHECKING, Any

from ..game import GameError, GameLink
from ..llm import LLMEvent, Message, ToolCall
from ..llm.fake import FakeBackend
from ..protocol import GameMessage, NpcActCmd, NpcActResult, NpcEvent, NpcIdle, NpcView, Session
from .common import (
    HILDA,
    LOBART,
    WitnessReaction,
    _logged,
    _save,
    _until,
    _until_async,
    prepare_theft_witness,
    wait_message,
)
from .crimes import HOUSE

if TYPE_CHECKING:
    from ..amends import Amends
    from ..crime_meet import CrimeMeet
    from ..crime_react import CrimeReact
    from ..crimes import Crimes

log = logging.getLogger(__name__)
CUP, GOLD = "ITMI_SILVERCUP", "ITMI_GOLD"
HERO_POINT, WITNESS_POINT = "NW_FARM1_INHOUSE_01", "NW_FARM1_INHOUSE_02"
MEET_POINT, MEET_PATH = "NW_FARM1_PATH_01", "NW_FARM1_ENTRANCE_03"
NEAR_REPEAT_S = 31.0  # bridge NearPeriodMs=30000; leaving alone does not reset the throttle
PUBLIC_PORTAL_GUILDS = {-1, 0, 15}  # unknown, GIL_NONE and the positive GIL_PUBLIC


class MeetAmendsScript(FakeBackend):
    """The victim demands a real payment through the existing Amends graph."""

    async def stream(self, **kw: Any) -> AsyncIterator[LLMEvent]:
        brief = next(m.content for m in kw["messages"] if isinstance(m, Message))
        stakes = re.findall(r"^- (\S+): «", brief, re.M)
        args = {
            "kind": "amends",
            "close": stakes[:1],
            "lines": [
                {"speaker": "npc", "text": "Я сам видел, как ты взял мой кубок."},
                {"speaker": "npc", "text": "Верни вещь или заплати золотой за обиду."},
            ],
            "to": "self",
            "gold": 1,
            "description": "Вот золотой за кражу.",
            "amends_lines": [{"speaker": "npc", "text": "Ладно, за эту кражу я тебя простил."}],
            "title": "Расчёт с Лобартом",
            "journal_now": "Лобарт видел мою кражу и требует возместить ущерб.",
            "journal_done": "Я заплатил Лобарту за кражу.",
            "reason": "пострадавший сам видел кражу",
        }
        backend = FakeBackend("", tool_calls=[ToolCall("propose_amends", args)], delay=0)
        async for event in backend.stream(**kw):
            yield event


async def crime_meet(
    link: GameLink,
    service: Crimes,
    reactions: CrimeReact,
    meets: CrimeMeet,
    amends: Amends,
    slot: int = 7,
) -> dict[str, str]:
    """Two branches of save 1; a live backend runs only the victim's meeting and payment.

    The native drop owns the cup, the native take supplies the witness, and common helpers prove
    sight and wait for the held rule's recorded SVM. No fixed pause stands in for scene readiness.
    Lobart keeps his production courage 2 during the theft; only the later meeting uses courage 3.
    Murder guards and city-gate dialogue paths are covered by the corresponding module tests.
    """
    results: dict[str, str] = {}
    facts = service.chronicle.facts
    original_cards, original_meet_cards = reactions.cards, meets.cards
    original_model, original_queue = amends.model.llm, amends._queue_crime
    live = original_model.name != "fake"
    sent: list[Any] = []
    received: list[tuple[float, GameMessage]] = []
    hero_states: list[dict[str, Any]] = []
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
        hero_states.extend(row["self"] for row in packet["observations"])

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
        await link.body_watch([])
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
    link.send = sending  # type: ignore[method-assign]
    link.listeners.append(record)
    link.raw_body_listeners.append(observe)
    amends._queue_crime = deferred_crime  # type: ignore[method-assign]
    if not live:
        amends.model.llm = MeetAmendsScript(delay=0)
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

        if not live:
            crime = await theft("home")
            if crime is None:
                return _logged(results)
            cid = str(crime["id"])
            if not check("home_owner_ready", await placed(LOBART, WITNESS_POINT, ready=True)):
                return _logged(results)
            await link.body_watch(["PLAYER"], rate_hz=20)
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
        check("victim_card", True, "scenario only: courage 2 -> 3 after the theft")
        amends._queue_crime = original_queue  # type: ignore[method-assign]
        if not check("victim_player_near", await next_meeting() is not None):
            return _logged(results)
        approached = await _until(
            lambda: any(
                f.get("npc") == LOBART and f.get("occasion") == f"crime:{cid}" for f in facts("initiative")
            ),
            120 if live else 30,
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
        await _until(lambda: any(s.npc == LOBART and s.lie == key for s in amends.storylets.values()), 120)
        storylet = next((s for s in amends.storylets.values() if s.npc == LOBART and s.lie == key), None)
        if not check("victim_amends", storylet is not None):
            return _logged(results)
        assert storylet is not None
        demands = [
            line.get("text", "")
            for op in amends.patches.ops()
            if op.get("op") == "add_node" and op.get("node", {}).get("id") == storylet.talk
            for line in op["node"].get("lines", [])
            if line.get("speaker") == "self"
        ]
        check("victim_demand", bool(demands), " | ".join(demands))
        log.info("crime_meet %s demand: %s", "live" if live else "scripted", " | ".join(demands))
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
        reactions.cards, meets.cards = original_cards, original_meet_cards
        amends.model.llm, amends._queue_crime = original_model, original_queue  # type: ignore[method-assign]
        try:
            if not link.closed.is_set():
                await link.body_watch([])
        finally:
            link.send = send  # type: ignore[method-assign]
            link.listeners.remove(record)
            link.raw_body_listeners.remove(observe)
