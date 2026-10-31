"""An innocent accused on the hero's word, then a thief found through an honest witness."""

from __future__ import annotations

import asyncio
import logging
import math
import re
from collections.abc import AsyncIterator
from dataclasses import replace
from pathlib import Path
from typing import TYPE_CHECKING, Any

from .. import triggers
from ..brain.reflex import Reflexes
from ..game import GameLink
from ..llm import LLMEvent, Message, ToolCall
from ..llm.fake import FakeBackend, FakeStep
from ..protocol import GameMessage, NpcActCmd, NpcActResult, NpcIdle, Session
from ..trace import read as read_trace
from .common import (
    HILDA,
    LOBART,
    VINO,
    FixedDecider,
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
    from ..talk import TalkDesk
    from ..trust import Trust
    from ..witnesses import Witnesses

log = logging.getLogger("gothic_ai.scenarios")
MALAK = "BAU_963_MALAK"
HOUSE, ROAD, RETURN_ROAD = "NW_FARM1_INHOUSE_01", "NW_FARM1_PATH_04", "NW_FARM1_PATH_03"
CHEST = "CHEST_LOBART"
FRAME_WORDS = "Это Малак взял"
INVESTIGATE_WORDS = "Вино взял вещь из твоего сундука. Спроси Хильду о Вино"


async def frame(
    link: GameLink,
    will: NpcWill,
    chests: ChestDesk,
    dialogue: DialogueManager,
    talk: TalkDesk,
    witnesses: Witnesses,
    trust: Trust,
    log_dir: str,
    slot: int = 7,
) -> dict[str, str]:
    """Fake: chest theft, a false belief tested by combat, truthful testimony, restitution and load.

    Live stops after Lobart's one ordinary answer to «Это Малак взял». Either belief or refusal is
    valid. The same courage card supplies the fixture; his conversational persona and prompt remain
    the ordinary production ones. All fights come from the reclaim motive, never this scenario.
    """
    results: dict[str, str] = {}
    service, rumors, people = will.npc_crimes, will.rumors, will.people
    crimes, blame = service.crimes, will.blame
    facts = will.chronicle.facts
    original_cards, original_crime_cards = will.cards, service.cards
    original_llm, original_reflexes, original_busy = dialogue.llm, talk.reflexes, will.busy
    original_gate, original_stage = rumors.gate, rumors.stage
    send = link.send
    fake = isinstance(original_llm, FakeBackend)
    directory = Path(log_dir)
    preparing = {LOBART}
    participants = {LOBART, HILDA, VINO, MALAK}
    commands: list[NpcActCmd] = []
    received: list[NpcActResult | NpcIdle] = []
    bodies: dict[str, list[dict[str, Any]]] = {LOBART: [], VINO: [], MALAK: []}
    symbols: dict[int, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> bool:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"
        log.info("frame %-25s %s", name, results[name])
        return ok

    async def sending(command: Any) -> None:
        if isinstance(command, NpcActCmd):
            commands.append(command)
        await send(command)

    def body(packet: dict[str, Any]) -> None:
        for row in packet["observations"]:
            if (npc := symbols.get(row["script_symbol"])) in bodies:
                bodies[npc].append(row["self"])

    def record(message: GameMessage) -> None:
        if isinstance(message, NpcActResult | NpcIdle):
            received.append(message)

    async def theft_finished(action: str, after: int) -> bool:
        def completed() -> bool:
            queued_voice = {
                m.id for m in received[after:] if isinstance(m, NpcActResult)
                and m.npc == HILDA and m.action == "say_svm" and m.status == "queued"
            }
            return any(isinstance(m, NpcIdle) and m.act == action and m.reason == "done"
                       for m in received[after:]) and any(
                isinstance(m, NpcIdle) and m.act in queued_voice and m.reason == "done"
                for m in received[after:]
            )

        return await _until(completed, 45)

    def turns() -> list[dict[str, Any]]:
        return [
            row for path in sorted((directory / "traces").glob("*.jsonl")) for row in read_trace(path)
            if row.get("type") == "turn" and row.get("npc") == LOBART
        ]

    async def place(npc: str, point: str, **kw: Any) -> bool:
        if point == parked:
            kw.setdefault("require_perception_ready", False)
        ok, detail = await prepare_npc_pose(link, npc, point, **kw)
        return ok or check("pose", False, detail)

    async def count(npc: str, item: str) -> int:
        return int(await link.evaluate(["call", "NPC_HASITEMS", ["inst", npc], ["sym", item]]))

    async def settled() -> bool:
        return await _until(lambda: not witnesses._asking.intersection(participants) and all(
            not dialogue.speaking(npc)
            and not (dialogue.conversations.get(npc) and dialogue.conversations[npc].actions)
            for npc in participants
        ), 60)

    async def talk_ready(*, witness: bool = False) -> bool:
        await link.debug("player_goto", waypoint=ROAD)
        if not await place(LOBART, ROAD):
            return False
        if witness and not await place(HILDA, ROAD, to=LOBART, distance=350, face=LOBART):
            return False
        await link.debug("teleport", near=LOBART, distance=250)
        ready = await _until(lambda: all(
            npc in talk.people and talk.people[npc].distance <= 700 and talk.people[npc].earshot <= 700
            for npc in ((LOBART, HILDA) if witness else (LOBART,))
        ), 20)
        return check("investigation_talk_near" if witness else "frame_talk_near", ready, {
            npc: entry.model_dump() if (entry := talk.people.get(npc)) is not None else None
            for npc in ((LOBART, HILDA) if witness else (LOBART,))
        })

    async def discover(chest: Chest, label: str) -> bool:
        # Put the owner on the floor, then clear the chest's interaction slot for his native opening.
        await link.debug("player_goto", pos=(chest.pos[0], chest.pos[1], chest.pos[2] + 200))
        await link.debug("npc_goto", npc=LOBART, to="PC_HERO", distance=0)
        await link.debug("player_goto", waypoint=HOUSE)
        conv = dialogue.open(LOBART, "Лобарт")
        held = await dialogue.borrow_body(conv, "scenario-frame-chest")
        try:
            opened = await link.act(LOBART, "use_chest", chest=chest.id, take=[], put=[], timeout=90)
            note = await chests.looked(LOBART, chest.id, opened.detail) if opened.status == "done" else ""
        finally:
            if held:
                dialogue.return_body(conv, "scenario-frame-chest")
        return check(f"{label}_discovered", opened.status == "done" and "пропало" in note, note)

    class Script:
        name = "frame-script"

        def __init__(self, cid: str, *, investigation: bool = False) -> None:
            self.cid, self.investigation = cid, investigation
            self.asked = self.accused = False
            self.prompts: list[str] = []

        async def stream(self, **kw: Any) -> AsyncIterator[LLMEvent]:
            last = kw["messages"][-1]
            text = last.content if isinstance(last, Message) else ""
            prompt = "\n".join(
                m.content for m in kw["messages"] if isinstance(m, Message) and m.role == "developer"
            )
            npc = kw.get("cache_key")
            step = FakeStep("Я запомню.")
            if npc == LOBART:
                self.prompts.append(prompt)
                if not self.investigation and FRAME_WORDS in text and not self.accused and self.cid in prompt:
                    self.accused = True
                    step = FakeStep("Значит, Малак взял. Верну своё.", [
                        ToolCall("blame", {"who": "Малак", "crime": self.cid}),
                    ])
                elif self.investigation and INVESTIGATE_WORDS in text and not self.asked:
                    self.asked = True
                    step = FakeStep("Спрошу у Хильды, что она видела о Вино.", [ToolCall("ask_witness", {
                        "who": "Хильда", "said": INVESTIGATE_WORDS, "about": "Вино",
                    })])
                elif self.investigation and not self.accused and any(
                    re.match(r"- С\d+:", line) and all(word in line for word in ("Хильда", "Вино", self.cid))
                    for line in prompt.splitlines()
                ):
                    self.accused = True
                    step = FakeStep("Хильда видела Вино. Потребую своё обратно.", [
                        ToolCall("blame", {"who": "Вино", "crime": self.cid}),
                    ])
            async for event in FakeBackend(script=[step], delay=0).stream(**kw):
                yield event

    async def speak(words: str) -> tuple[bool, str, list[dict[str, Any]]]:
        before = {row["id"] for row in turns()}
        await link.debug("type", text=f"Лобарт, {words}.")
        finished = await _until(lambda: any(
            row["id"] not in before and words in row.get("trigger", {}).get("text", "")
            for row in turns()
        ), 180 if not fake else 60)
        await settled()
        rows = [row for row in turns() if row["id"] not in before
                and words in row.get("trigger", {}).get("text", "")]
        quote = " ".join(
            str(step["text"]).strip() for row in rows for step in row.get("steps", [])
            if step.get("text") and not step.get("silent")
        )
        log.info("frame %s response: %s", "scripted" if fake else "live", quote)
        return finished and any(row.get("status") == "completed" for row in rows), quote, rows

    async def fight(label: str, crime: dict[str, Any], culprit: str) -> bool:
        cid = str(crime["id"])
        preparing.add(LOBART)
        talk.end("gone")
        if not check(f"{label}_released", await _until(lambda: LOBART not in talk.attached, 20)):
            return False
        await link.debug("player_goto", waypoint=ROAD)
        if not await place(HILDA, parked) or not await place(culprit, ROAD):
            return False
        if not await place(LOBART, ROAD, to=culprit, distance=300, face=culprit):
            return False
        assignments = [(culprit, 1, 1000), (culprit, 0, 10), (LOBART, 1, 1000), (LOBART, 0, 1000)]
        trigger = f"DIR_FRAME_HEALTH_{label.upper()}"
        wanted = [hp for _, _, hp in assignments]
        health_reading: dict[str, Any] = {"wanted": wanted}

        def health_matches(values: list[Any]) -> bool:
            health_reading["actual"] = values
            return values == wanted

        await link.patch_graph([triggers.trigger(trigger, triggers.STATE, [
            {"do": "set", "target": ["field", ["inst", npc], "ATTRIBUTE", index], "value": ["int", hp]}
            for npc, index, hp in assignments
        ])])
        try:
            healthy = await _until_async(lambda: link.story_eval(LOBART, [
                ["field", ["inst", npc], "ATTRIBUTE", index] for npc, index, _ in assignments
            ]), health_matches, 5)
        finally:
            await link.patch_graph([triggers.removal(trigger)])
        if not check(f"{label}_health", healthy, health_reading):
            return False
        bodies[culprit].clear()
        bodies[LOBART].clear()
        await link.body_watch([culprit, LOBART], rate_hz=20)
        # The passive hero observes off the combat lane. HP and positions stay native after release.
        await link.debug("player_goto", waypoint=RETURN_ROAD)
        before = {npc: await count(npc, str(crime["item"])) for npc in (culprit, LOBART)}
        if label == "frame" and not check("frame_precombat_empty", before[culprit] == 0,
                                          {"item": crime["item"], "inventory": before}):
            return False
        command_at = len(commands)
        async with will._lock:
            await will.far_world.refresh(will._generation)
            will._hours.clear()
            preparing.discard(LOBART)
        await _until(lambda: any(
            f.get("npc") == LOBART and f.get("crime") == cid and f.get("act") == "fight"
            for f in facts("npc_will")
        ), 30)
        decisions = [f for f in facts("npc_will") if f.get("npc") == LOBART and f.get("crime") == cid]
        if not check(f"{label}_motive", len(decisions) == 1 and decisions[0].get("kind") == "reclaim"
                     and decisions[0].get("act") == "fight" and decisions[0].get("target") == culprit,
                     decisions):
            return False
        await _until(lambda: any(f.get("crime") == cid for f in facts("npc_crime_defense")), 150)
        defense = [f for f in facts("npc_crime_defense") if f.get("crime") == cid]
        sent_fights = [c for c in commands[command_at:] if c.npc == LOBART and c.action == "fight"]
        if not check(f"{label}_native_fight", len(sent_fights) == 1 and len(defense) == 1
                     and any(row["unconscious"] for row in bodies[culprit])
                     and not any(row["dead"] for row in bodies[culprit]),
                     {"defense": defense, "samples": len(bodies[culprit])}):
            return False
        await link.body_watch([])
        preparing.add(LOBART)
        if label == "frame":
            return check("frame_no_goods", defense[0].get("outcome") == "no_goods"
                         and await count(culprit, str(crime["item"])) == 0
                         and await count(LOBART, str(crime["item"])) == before[LOBART], defense)
        return check("investigation_returned", defense[0].get("outcome") == "reclaimed"
                     and await count(culprit, str(crime["item"])) == before[culprit] - 1
                     and await count(LOBART, str(crime["item"])) == before[LOBART] + 1
                     and any(f.get("crime") == cid for f in facts("npc_crime_restitution")), defense)

    link.send = sending  # type: ignore[method-assign]
    link.raw_body_listeners.append(body)
    link.listeners.append(record)
    will.busy = lambda npc: npc in preparing or original_busy(npc)
    try:
        await link.wait_world()
        if link.session is None:
            await wait_message(link, Session, timeout=10)
        if not check("enabled", will.enabled and blame is not None and crimes is not None, log_dir):
            return _logged(results)
        talk.reflexes = Reflexes(FixedDecider({"posture": "listen"}))
        rumors.gate, rumors.stage = lambda *_: False, None
        prepared = {npc: replace(card, honesty=5, greed=1) for npc, card in original_cards.items()}
        prepared[LOBART] = replace(prepared[LOBART], courage=3)
        will.cards = service.cards = prepared
        if fake:
            dialogue.llm = FakeBackend("Я слушаю.", delay=0)
        await link.debug("player_hp", hp=5000)
        await link.debug("time", hour=8, minute=0)
        population = {str(p["npc"]) for p in await link.story_people()}
        points = {p["name"]: p for p in await link.waypoints() if not p.get("fp")}
        if not check("farm", participants <= population and {HOUSE, ROAD, RETURN_ROAD} <= points.keys()
                     and not service.guard.body(MALAK) and not service.guard.body(VINO)):
            return _logged(results)
        symbols.update(zip(
            await link.story_eval(LOBART, [["sym", npc] for npc in bodies]), bodies, strict=True,
        ))
        origin = tuple(points[HOUSE][a] for a in ("x", "y", "z"))
        far = [p for p in points.values() if p["name"].startswith("NW_FARM1_") and not p.get("room")
               and math.dist(tuple(p[a] for a in ("x", "y", "z")), origin) > 8000]
        if not check("parking", bool(far), len(far)):
            return _logged(results)
        parked = min(far, key=lambda p: math.dist(tuple(p[a] for a in ("x", "y", "z")), origin))["name"]
        farm = list(dict.fromkeys([*participants, *[
            p["npc"] for p in await link.story_nearby(LOBART, 7000) if p["npc"] in population
        ]]))
        await link.story_effect(LOBART, [{"do": "end"}])
        await link.debug("player_goto", waypoint=HOUSE)
        for npc in farm:
            if not await place(npc, parked):
                return _logged(results)
        found = await chests.chests(LOBART, fresh=True)
        chest = next((c for c in found if c.id == CHEST and c.items), None)
        if not check("chest", chest is not None, [(c.id, c.why) for c in found]):
            return _logged(results)
        assert chest is not None
        # Native routines may create food for an eating animation. A missing outfit or other
        # nonconsumable keeps the innocent's empty inventory stable through the later encounter.
        consumable = ("ITFO_", "ITPO_", "ITPL_")
        choices = [i for i in chest.items if not i.item.startswith(consumable)
                   and await count(MALAK, i.item) == 0]
        if not check("innocent_has_no_goods", bool(choices), [i.item for i in choices]):
            return _logged(results)
        item = choices[0]
        if not await place(LOBART, HOUSE):
            return _logged(results)
        observed = False
        for distance in (200, 350, 100):
            await link.debug("teleport", near=LOBART, distance=distance)
            await link.npc_act(LOBART, "turn_to", npc="PLAYER")
            observed = await _until_async(lambda: crimes.sightings.desk.fresh(LOBART),
                                         lambda v: bool(v and v.hero and v.hero.seen), 5)
            if observed:
                break
        if not check("hero_visitor", observed and bool(crimes.sightings.hero_trail(LOBART))):
            return _logged(results)
        if not await place(LOBART, parked):
            return _logged(results)
        await link.debug("player_goto", pos=(chest.pos[0] + 100, chest.pos[1], chest.pos[2]))
        nearby = await link.story_nearby("PC_HERO", 3500)
        if not check("hero_alone", not nearby, [p["npc"] for p in nearby]):
            return _logged(results)
        previous = {f["id"] for f in facts("crime")}
        await link.debug("chest_take", chest=CHEST, item=item.item, count=1)
        await _until(lambda: any(f["id"] not in previous for f in facts("crime")), 10)
        theft = next((f for f in facts("crime") if f["id"] not in previous), None)
        if not check("hero_theft", theft is not None and theft.get("owner") == LOBART
                     and theft.get("chest") == CHEST and not theft.get("witnesses"), theft):
            return _logged(results)
        assert theft is not None
        cid = str(theft["id"])
        if not await discover(chest, "hero"):
            return _logged(results)
        if not check("hero_suspected", any(k.crime["id"] == cid and k.degree == "suspects"
                                          for k in crimes.known(LOBART)), crimes.situation(LOBART)):
            return _logged(results)
        # Meeting the innocent now must not become a remembered presence around the earlier theft.
        # The belief is based on the hero's words, rather than an accidental own-trace source.
        await link.debug("time", hour=10, minute=0)
        for npc in farm:
            if not await place(npc, parked):
                return _logged(results)
        if not await talk_ready():
            return _logged(results)
        # Malak is present in the normal situation, rather than an invented name in the fixture.
        if not await place(MALAK, ROAD, to=LOBART, distance=450, face=LOBART):
            return _logged(results)
        if fake:
            dialogue.llm = Script(cid)  # type: ignore[assignment]
        before_trust = trust.of(LOBART)
        finished, quote, rows = await speak(FRAME_WORDS)
        if not check("frame_answer", finished and bool(quote), quote):
            return _logged(results)
        beliefs = [f for f in facts("blamed") if f.get("npc") == LOBART and f.get("crime") == cid]
        if not fake:
            calls = [c for row in rows for step in row.get("steps", []) for c in step.get("calls", [])
                     if c.get("name") == "blame"]
            check("live_decision", True, {"believed": bool(beliefs), "calls": calls, "quote": quote})
            return _logged(results)
        if not check("frame_believed", len(beliefs) == 1 and beliefs[0].get("culprit") == MALAK
                     and beliefs[0].get("truth") is False and beliefs[0].get("because") == "hero_words"
                     and FRAME_WORDS in beliefs[0].get("said", ""), beliefs):
            return _logged(results)
        if not await fight("frame", theft, MALAK):
            return _logged(results)
        await _until(lambda: any(f.get("crime") == cid for f in facts("blame_refuted")), 10)
        await _until(lambda: any(f.get("blamed") == beliefs[0]["id"] for f in facts("caught")), 10)
        refuted = [f for f in facts("blame_refuted") if f.get("crime") == cid]
        caught = [f for f in facts("caught") if f.get("blamed") == beliefs[0]["id"]]
        if not check("frame_refuted", len(refuted) == 1 and refuted[0].get("culprit") == MALAK
                     and refuted[0].get("how") == "no_goods" and blame.active(LOBART, cid) is None
                     and not any(m.get("crime") == cid for m in will.pending()), refuted):
            return _logged(results)
        if not check("hero_lie_revealed", len(caught) == 1 and FRAME_WORDS in caught[0].get("title", "")
                     and "со слов:" not in caught[0].get("how", "")
                     and len(trust.of(LOBART).lied) == len(before_trust.lied) + 1, caught):
            return _logged(results)

        # The second scene retains the first scene's trust consequence. Only the bodies are staged.
        # Keep the hero nearby while the defeated man gets up by himself. Debug movement rightly
        # refuses an unconscious body; the next scene starts only after his native recovery.
        recovery: dict[str, Any] = {}

        async def recovered() -> bool:
            views = await link.npc_view([MALAK])
            view = views[0] if views else None
            recovery["view"] = view.model_dump() if view is not None else None
            return bool(view and view.hp > 0 and view.body in {"stand", "walk", "run"}
                        and view.state not in {"ZS_UNCONSCIOUS", "ZS_ATTACK", "ZS_RANSACKBODY"})

        if not check("frame_recovered", await _until_async(recovered, bool, 60), recovery):
            return _logged(results)
        if not await place(MALAK, parked):
            return _logged(results)
        await link.debug("player_goto", waypoint=HOUSE)
        if not await place(LOBART, parked) or not await place(HILDA, parked):
            return _logged(results)
        chest = next(c for c in await chests.chests(LOBART, fresh=True) if c.id == CHEST)
        if not check("investigation_chest", bool(chest.items) and chest.waypoint in points,
                     {"items": [i.item for i in chest.items], "waypoint": chest.waypoint}):
            return _logged(results)
        assert chest.waypoint is not None
        stolen = next((i.item for i in chest.items if not i.item.startswith(consumable)), chest.items[0].item)
        if not await place(VINO, HOUSE):
            return _logged(results)
        # The owner sees an ordinary visitor, not the later transfer. Opening the robbed chest can
        # therefore identify the new case as a suspicion, distinct from the earlier hero theft. Use
        # the house's open floor: the chest's bed corner can occlude an otherwise nearby visitor.
        # After catching the lie he watches a hero standing on top of him. Leave that observation
        # radius on a real waypoint, while both the owner and visitor stay in native simulation.
        visitor_pos = (await link.snapshot(VINO, fresh=True)).npc.pos
        hero_spots = [p for p in points.values() if not p.get("room")
                      and 1800 <= math.dist(tuple(p[a] for a in ("x", "y", "z")), visitor_pos) <= 2200]
        if not check("investigation_observer_space", bool(hero_spots), [p["name"] for p in hero_spots]):
            return _logged(results)
        hero_spot = min(hero_spots, key=lambda p: abs(
            math.dist(tuple(p[a] for a in ("x", "y", "z")), visitor_pos) - 1900,
        ))
        await link.debug("player_goto", waypoint=hero_spot["name"])
        visitor_memory: dict[str, Any] = {}

        async def visitor() -> bool:
            view = await crimes.sightings.desk.fresh(LOBART)
            remembered = [s.record() for s in crimes.sightings.mind(LOBART).sightings if s.who == VINO]
            visitor_memory.update(view=view.model_dump() if view is not None else None, sightings=remembered)
            return bool(view and any(o.id == VINO for o in view.sees) and remembered)

        visitor_ready, visitor_attempts = False, []
        for offset in (-300, 300, -450, 450):
            posed, pose = await prepare_npc_pose(link, LOBART, HOUSE, to=VINO, distance=offset, face=VINO)
            visible, visibility = await wait_npc_visibility(link, LOBART, VINO) if posed else (False, {})
            visitor_ready = visible and await _until_async(visitor, bool, 5)
            visitor_attempts.append({
                "offset": offset, "pose": pose, "visibility": visibility, "memory": dict(visitor_memory),
            })
            if visitor_ready:
                break
        if not check("investigation_visitor", visitor_ready, visitor_attempts):
            return _logged(results)
        if not await place(LOBART, parked):
            return _logged(results)
        await link.debug("player_goto", waypoint=HOUSE)
        if not await place(VINO, chest.waypoint):
            return _logged(results)
        witness_ready, attempts = False, []
        for offset in (-300, 300, -450, 450):
            posed, pose = await prepare_npc_pose(
                link, HILDA, chest.waypoint, to=VINO, distance=offset, face=VINO,
            )
            visible, detail = await wait_npc_visibility(link, HILDA, VINO) if posed else (False, {})
            attempts.append({"offset": offset, "pose": pose, "visibility": detail})
            if visible:
                witness_ready = True
                break
        if not check("investigation_witness", witness_ready, attempts):
            return _logged(results)
        if not await place(VINO, HOUSE):
            return _logged(results)
        previous_npc = {f["id"] for f in facts("npc_crime")}
        received_at = len(received)
        action = await link.npc_act(VINO, "steal", chest=CHEST, what=stolen, count=1, deed=True)
        await _until(lambda: any(f["id"] not in previous_npc for f in facts("npc_crime")), 90)
        deed = next((f for f in facts("npc_crime") if f["id"] not in previous_npc), None)
        if not check("investigation_theft", deed is not None and deed.get("culprit") == VINO
                     and deed.get("owner") == LOBART and HILDA in deed.get("witnesses", [])
                     and LOBART not in deed.get("witnesses", []), {
                         "crime": deed, "action": action,
                         "received": [message.model_dump() for message in received[received_at:]],
                     }):
            return _logged(results)
        assert deed is not None
        did = str(deed["id"])
        if not check("investigation_closed", await theft_finished(action, received_at), [
            message.model_dump() for message in received[received_at:]
        ]):
            return _logged(results)
        if not await discover(chest, "investigation"):
            return _logged(results)
        if not check("investigation_suspected", any(
            f.get("npc") == LOBART and f.get("crime") == did and f.get("suspect") == VINO
            for f in facts("npc_crime_suspected")
        ), [f for f in facts("npc_crime_suspected") if f.get("crime") == did]):
            return _logged(results)
        if not await talk_ready(witness=True):
            return _logged(results)
        if not await place(VINO, ROAD, to=LOBART, distance=450, face=LOBART):
            return _logged(results)
        script = Script(did, investigation=True)
        dialogue.llm = script  # type: ignore[assignment]
        finished, quote, _ = await speak(INVESTIGATE_WORDS)
        believed = await _until(lambda: any(f.get("crime") == did for f in facts("blamed")), 90)
        await settled()
        testimony = [f for f in facts("testimony") if f.get("npc") == LOBART and f.get("witness") == HILDA
                     and f.get("about") == VINO and did in f.get("crime_told", [])]
        if not check("about_testimony", finished and len(testimony) == 1, testimony):
            return _logged(results)
        lines = [
            line for prompt in script.prompts for line in prompt.splitlines() if re.match(r"- С\d+:", line)
        ]
        if not check("about_situation", any(all(word in line for word in ("Хильда", "Вино", did))
                                           for line in lines), lines):
            return _logged(results)
        correct = [f for f in facts("blamed") if f.get("npc") == LOBART and f.get("crime") == did]
        if not check("investigation_believed", believed and len(correct) == 1
                     and correct[0].get("culprit") == VINO and correct[0].get("truth") is True
                     and correct[0].get("because") == f"testimony:{testimony[0]['id']}", correct):
            return _logged(results)
        if not await fight("investigation", deed, VINO):
            return _logged(results)
        check("true_case_stands", not any(f.get("crime") == did for f in facts("blame_refuted"))
              and not any(f.get("blamed") == correct[0]["id"] for f in facts("caught")), correct)

        # Save branches retain the resolved false belief and the truthful case, without new reactions.
        kinds = {"crime", "crime_suspected", "npc_crime", "npc_crime_seen", "npc_crime_suspected",
                 "testimony", "blamed", "blame_refuted", "caught", "claim", "npc_motive", "npc_will",
                 "npc_will_outcome", "npc_crime_defense", "npc_crime_restitution", "rumor"}
        kept = [f for f in facts() if f.get("fact") in kinds]
        trust_before, people_before = trust.of(LOBART), people.of(LOBART)
        inventory = {npc: await count(npc, stolen) for npc in (VINO, LOBART)}
        saved = await _save(link, slot)
        if not check("saved", saved is not None, len(kept)):
            return _logged(results)
        assert saved is not None
        loading = asyncio.create_task(wait_message(link, Session, lambda m: m.reason == "load", 90))
        await asyncio.sleep(0)
        await link.debug("load", slot)
        session = await loading
        await link.wait_world()
        if not check("loaded", session.parent == saved.branch and session.parent_seq == saved.seq
                     and [f for f in facts() if f.get("fact") in kinds] == kept
                     and trust.of(LOBART) == trust_before and people.of(LOBART) == people_before
                     and {npc: await count(npc, stolen) for npc in (VINO, LOBART)} == inventory, len(kept)):
            return _logged(results)
        await rumors.tick()
        await will.tick()
        check("no_duplicates", [f for f in facts() if f.get("fact") in kinds] == kept
              and blame.active(LOBART, cid) is None and len(facts("blame_refuted")) == 1
              and not any(m.get("crime") == cid for m in will.pending()),
              {"refuted": len(facts("blame_refuted")), "beliefs": len(facts("blamed"))})
        return _logged(results)
    finally:
        will.cards, service.cards = original_cards, original_crime_cards
        dialogue.llm, talk.reflexes, will.busy = original_llm, original_reflexes, original_busy
        rumors.gate, rumors.stage = original_gate, original_stage
        link.send = send  # type: ignore[method-assign]
        try:
            if not link.closed.is_set():
                await link.body_watch([])
        finally:
            link.raw_body_listeners.remove(body)
            link.listeners.remove(record)
