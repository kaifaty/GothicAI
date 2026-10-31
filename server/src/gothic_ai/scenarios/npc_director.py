"""An NPC theft becomes a director line, testimony, an accepted accusation and a save branch."""

from __future__ import annotations

import asyncio
import json
import logging
import math
import os
import re
from collections.abc import AsyncIterator
from dataclasses import replace
from typing import TYPE_CHECKING, Any

from pydantic import ValidationError

from .. import questbook
from ..brain.reflex import Reflexes
from ..game import GameLink
from ..llm import LLMEvent, Message, ToolCall
from ..llm.fake import FakeBackend, FakeStep
from ..protocol import GameMessage, NpcActResult, NpcIdle, Session
from ..showrunner import Proposal, occasion_members
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
from .npc_deeds import CHEST, HOUSE, ROAD

if TYPE_CHECKING:
    from ..chests import ChestDesk
    from ..dialogue import DialogueManager
    from ..npc_will import NpcWill
    from ..showrunner import Showrunner
    from ..talk import TalkDesk
    from ..witnesses import Witnesses

log = logging.getLogger("gothic_ai.scenarios")
WORDS = "Помоги найти вора. Спроси Хильду о Вино"
PREMISE = "У Лобарта пропала вещь из сундука. Он просит героя убедить его, кто вор."


def _builder(cid: str) -> FakeBackend:
    def talk(description: str, answer: str) -> dict[str, Any]:
        return {
            "description": description,
            "lines": [{"speaker": "hero", "text": description}, {"speaker": "npc", "text": answer}],
            "remember": answer,
        }

    return FakeBackend("", tool_calls=[ToolCall("propose_chain", {
        "cast": [],
        "quests": [{
            "title": "Кто обокрал Лобарта",
            "give": talk("Я помогу найти вора.", "Узнай, кто обокрал мой сундук."),
            "stages": [
                {"journal": "Надо убедить Лобарта, кто вор.", "goals": [{"kind": "blame", "crime": cid}]},
                {"journal": "Лобарт назвал вора. Надо закончить разговор.", "goals": [{
                    "kind": "talk", "npc": "giver",
                    "talk": talk("Теперь ты знаешь, кто взял вещь.", "Да. Спасибо за помощь."),
                }]},
            ],
            "done_journal": "Лобарт убеждён, кто обокрал его сундук.",
            "knows": "Из моего сундука пропала вещь. Я не знаю, кто её взял.",
            "asked": "Убедить меня, кто вор.",
            "gold": 1, "xp": 0,
        }],
        "reason": "Заказчик должен сам назвать вора; герой может убедить его и ложью.",
    })], delay=0)


class InvestigationScript:
    name = "npc-director-script"

    def __init__(self, cid: str) -> None:
        self.cid = cid
        self.asked = self.accused = False
        self.prompts: list[str] = []

    async def stream(self, **kw: Any) -> AsyncIterator[LLMEvent]:
        last = kw["messages"][-1]
        text = last.content if isinstance(last, Message) else ""
        prompt = "\n".join(
            m.content for m in kw["messages"] if isinstance(m, Message) and m.role == "developer"
        )
        step = FakeStep("Я слушаю.")
        if kw.get("cache_key") == LOBART:
            self.prompts.append(prompt)
            if WORDS in text and not self.asked:
                self.asked = True
                step = FakeStep("Спрошу Хильду, что она видела о Вино.", [ToolCall("ask_witness", {
                    "who": "Хильда", "said": WORDS, "about": "Вино",
                })])
            elif not self.accused and any(
                re.match(r"- С\d+:", line)
                and all(word in line for word in ("Хильда", "Вино", self.cid))
                for line in prompt.splitlines()
            ):
                self.accused = True
                step = FakeStep("Хильда видела Вино. Значит, он взял мою вещь.", [
                    ToolCall("blame", {"who": "Вино", "crime": self.cid}),
                ])
        async for event in FakeBackend(script=[step], delay=0).stream(**kw):
            yield event


async def npc_director(
    link: GameLink,
    will: NpcWill,
    director: Showrunner,
    chests: ChestDesk,
    dialogue: DialogueManager,
    talk: TalkDesk,
    witnesses: Witnesses,
    log_dir: str,
    slot: int = 7,
) -> dict[str, str]:
    """Fake plays one theft line; live asks only the ordinary showrunner model once, without building.

    The live preparation does not consume important vanilla dialogs or change character cards. Its
    decision uses the production brief, instructions and canon, without a retry or a suggested plot.
    """
    results: dict[str, str] = {}
    service, rumors = will.npc_crimes, will.rumors
    facts = will.chronicle.facts
    live = director.model.llm.name != "fake"
    original_llm, original_reflexes, original_busy = dialogue.llm, talk.reflexes, will.busy
    original_model, original_builder = director.model.llm, director.chains.model.llm
    original_cards, original_crime_cards = will.cards, service.cards
    original_gate, original_stage = rumors.gate, rumors.stage
    participants = {LOBART, HILDA, VINO}
    preparing = set(participants)
    received: list[NpcActResult | NpcIdle] = []

    def check(name: str, ok: bool, detail: object = "") -> bool:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"
        log.info("npc_director %-24s %s", name, results[name])
        return ok

    def record(message: GameMessage) -> None:
        if isinstance(message, NpcActResult | NpcIdle):
            received.append(message)

    async def count(npc: str, item: str) -> int:
        return int(await link.evaluate(["call", "NPC_HASITEMS", ["inst", npc], ["sym", item]]) or 0)

    async def place(npc: str, point: str, **kw: Any) -> bool:
        if not live:
            kw.setdefault("require_perception_ready", point != parked)
            ok, detail = await prepare_npc_pose(link, npc, point, **kw)
            return ok or check("pose", False, detail)
        # Unlike the generic scenario helper, this path leaves important vanilla dialogs alone.
        await link.debug("npc_goto", npc=npc, waypoint=point)
        await link.npc_act(
            npc, "start_activity", state="ZS_STAND_WP", waypoint=point, minutes=120, place=False,
        )

        async def ready() -> bool:
            views = await link.npc_view([npc])
            return bool(views and views[0].state == "ZS_STAND_WP" and views[0].body == "stand"
                        and (point == parked or views[0].theft_ready is True))

        if not await _until_async(ready, bool, 15):
            return check("pose", False, {"npc": npc, "waypoint": point})
        placement: dict[str, Any] = {"npc": npc}
        placement.update({"to": kw["to"], "distance": kw.get("distance", 300)}
                         if "to" in kw else {"waypoint": point})
        if "face" in kw:
            placement["face"] = kw["face"]
        await link.debug("npc_goto", **placement)
        return True

    async def settled() -> bool:
        return await _until(lambda: not witnesses._asking.intersection(participants) and all(
            not dialogue.speaking(npc)
            and not (dialogue.conversations.get(npc) and dialogue.conversations[npc].actions)
            for npc in participants
        ), 60)

    def theft_finished(action: str, after: int) -> bool:
        voices = {m.id for m in received[after:] if isinstance(m, NpcActResult)
                  and m.npc == HILDA and m.action == "say_svm" and m.status == "queued"}
        return any(isinstance(m, NpcIdle) and m.act == action and m.reason == "done"
                   for m in received[after:]) and any(
            isinstance(m, NpcIdle) and m.act in voices and m.reason == "done" for m in received[after:]
        )

    link.listeners.append(record)
    will.busy = lambda npc: npc in preparing or original_busy(npc)
    dialogue.llm = FakeBackend("Я слушаю.", delay=0)
    talk.reflexes = Reflexes(FixedDecider({"posture": "listen"}))
    rumors.gate, rumors.stage = lambda *_: False, None
    try:
        await link.wait_world()
        if link.session is None:
            await wait_message(link, Session, timeout=10)
        if not check("enabled", will.enabled and service.enabled and will.blame is not None, log_dir):
            return _logged(results)
        if not live:
            will.cards = service.cards = {n: replace(c, honesty=5, greed=1) for n, c in will.cards.items()}
        await link.debug("player_hp", hp=5000)
        await link.debug("time", hour=8, minute=0)
        humans = {str(p["npc"]) for p in await link.story_people()}
        points = {p["name"]: p for p in await link.waypoints() if not p.get("fp")}
        if not check("farm", participants <= humans and {HOUSE, ROAD} <= points.keys()
                     and not service.guard.body(VINO)):
            return _logged(results)
        origin = tuple(points[HOUSE][axis] for axis in ("x", "y", "z"))
        far = [p for p in points.values() if p["name"].startswith("NW_FARM1_") and not p.get("room")
               and math.dist(tuple(p[a] for a in ("x", "y", "z")), origin) > 8000]
        if not check("parking", bool(far), len(far)):
            return _logged(results)
        parked = min(far, key=lambda p: math.dist(tuple(p[a] for a in ("x", "y", "z")), origin))["name"]
        farm = list(dict.fromkeys([*sorted(participants), *[
            p["npc"] for p in await link.story_nearby(LOBART, 7000) if p["npc"] in humans
        ]]))
        preparing.update(farm)
        if not live:
            await link.story_effect(LOBART, [{"do": "end"}])
        await link.debug("player_goto", waypoint=HOUSE)
        for npc in farm:
            if not await place(npc, parked):
                return _logged(results)
        found = await chests.chests(LOBART, fresh=True)
        chest = next((c for c in found if c.id == CHEST and c.items), None)
        if not check("chest", chest is not None and chest.waypoint in points,
                     [(c.id, c.waypoint) for c in found]):
            return _logged(results)
        assert chest is not None and chest.waypoint is not None
        # Choose existing goods through the same protection check as the eventual command. The
        # farmer's outfit is script-owned; gold (as in npc_deeds) is ordinary property to steal.
        item, rejected = "", {}
        for candidate in sorted(chest.items, key=lambda i: i.item != "ITMI_GOLD"):
            reason = await service.check_command(VINO, "steal", {
                "chest": CHEST, "what": candidate.item, "count": 1, "deed": True,
            })
            if reason is None:
                item = candidate.item
                break
            rejected[candidate.item] = reason
        if not check("theft_item", bool(item), {"item": item, "rejected": rejected}):
            return _logged(results)
        if not await place(VINO, chest.waypoint):
            return _logged(results)
        visible, attempts = False, []
        for offset in (-300, 300, -450, 450):
            posed = await place(HILDA, chest.waypoint, to=VINO, distance=offset, face=VINO)
            visible, detail = await wait_npc_visibility(link, HILDA, VINO) if posed else (False, {})
            attempts.append({"offset": offset, "visibility": detail})
            if visible:
                break
        if not check("witness_ready", visible, attempts) or not await place(VINO, HOUSE):
            return _logged(results)
        before = {f["id"] for f in facts("npc_crime")}
        inventory, received_at = await count(VINO, item), len(received)
        action = await link.npc_act(VINO, "steal", chest=CHEST, what=item, count=1, deed=True)
        await _until(lambda: any(f["id"] not in before for f in facts("npc_crime")), 90)
        crime = next((f for f in facts("npc_crime") if f["id"] not in before), None)
        if not check("theft", crime is not None and crime.get("culprit") == VINO
                     and crime.get("owner") == LOBART and HILDA in crime.get("witnesses", [])
                     and LOBART not in crime.get("witnesses", [])
                     and await count(VINO, item) == inventory + 1, crime):
            return _logged(results)
        assert crime is not None
        cid = str(crime["id"])
        if not check("theft_closed", await _until(lambda: theft_finished(action, received_at), 45),
                     [m.model_dump() for m in received[received_at:]]):
            return _logged(results)

        # Lobart discovers the loss himself. No one tells him who took it before the line exists.
        await link.debug("player_goto", pos=(chest.pos[0], chest.pos[1], chest.pos[2] + 200))
        await link.debug("npc_goto", npc=LOBART, to="PC_HERO", distance=0)
        await link.debug("player_goto", waypoint=HOUSE)
        conv = dialogue.open(LOBART, "Лобарт")
        held = await dialogue.borrow_body(conv, "scenario-npc-director-chest")
        try:
            opened = await link.act(LOBART, "use_chest", chest=CHEST, take=[], put=[], timeout=90)
            note = await chests.looked(LOBART, CHEST, opened.detail) if opened.status == "done" else ""
        finally:
            if held:
                dialogue.return_body(conv, "scenario-npc-director-chest")
        if not check("loss_discovered", opened.status == "done" and "пропало" in note, note):
            return _logged(results)

        if not live:
            await link.debug("player_goto", waypoint=ROAD)
            if not await place(LOBART, ROAD) or not await place(
                HILDA, ROAD, to=LOBART, distance=350, face=LOBART,
            ):
                return _logged(results)
            if not await place(VINO, ROAD, to=LOBART, distance=450, face=LOBART):
                return _logged(results)
            await link.debug("teleport", near=LOBART, distance=250)
            ready = await _until(lambda: all(n in talk.people and talk.people[n].distance <= 700
                                            and talk.people[n].earshot <= 700 for n in (LOBART, HILDA)), 20)
            if not check("talk_near", ready):
                return _logged(results)
        if not await _until(lambda: any(f.get("crime") == cid for f in facts("occasion")), 20):
            check("deed", False, cid)
            return _logged(results)
        occasion = await asyncio.wait_for(director.occasions.next(), director.occasions.max_gather_s + 20)
        if not check("deed", any(f.get("crime") == cid for f in occasion_members(occasion)), occasion):
            return _logged(results)

        if live:
            # GOTHIC_DIRECTOR_ASKS=N asks the same brief N times to see how often a hidden challenge is laid
            asks = max(1, int(os.environ.get("GOTHIC_DIRECTOR_ASKS", "1")))
            brief = await director.brief(occasion)
            challenges: list[str] = []
            for ask in range(asks):
                suffix = f"_{ask + 1}" if asks > 1 else ""
                raw = await director.model.propose(brief)
                try:
                    proposal = Proposal.of(raw or {})
                    errors = director.canon(proposal, brief)
                    if not errors:
                        errors = await director._requirements(proposal)
                except ValidationError as error:
                    return _logged({**results, f"live_decision{suffix}": f"failed ({error})"})
                outcome = {"fact": "line_none" if proposal.kind == "none" else "line",
                           "proposal": raw, "canon": errors}
                log.info("npc_director live outcome: %s", json.dumps(outcome, ensure_ascii=False))
                check(f"live_decision{suffix}", bool(proposal.reason.strip()) if proposal.kind == "none"
                      else bool(proposal.premise.strip()), outcome)
                check(f"live_canon{suffix}", not errors, errors)
                if proposal.kind != "none":
                    challenges.append((proposal.challenge or {}).get("kind", "") or "none")
            check("live_challenges", True, ", ".join(challenges) or "no lines")
            return _logged(results)

        director.model.llm = FakeBackend("", tool_calls=[ToolCall("propose_line", {
            "kind": "quest", "giver": LOBART, "place": "", "new_people": False,
            "premise": PREMISE, "reason": "Лобарт обнаружил пропажу и хочет найти вора.",
            "follows": "", "crime": cid, "requires": [],
        })], delay=0)
        director.chains.model.llm = _builder(cid)
        outcome = await director.decide(occasion)
        if not check("line", outcome is not None and outcome.get("fact") == "line"
                     and outcome.get("giver") == LOBART and outcome.get("crime") == cid, outcome):
            return _logged(results)
        assert outcome is not None
        made = [director.book.all()[qid] for qid in outcome.get("quests", []) if qid in director.book.all()]
        if not check("blame_goal", len(made) == 1 and made[0].stages[0].goals[0].kind == "blame"
                     and made[0].stages[0].goals[0].crime == cid, outcome.get("quests")):
            return _logged(results)
        q = made[0]
        await link.choose_dialog(LOBART, questbook.give_node(q))
        if not check("quest_given", await _until(
            lambda: director.book.progress(q.id).state == "given", 20,
        ), q.id):
            return _logged(results)
        script = InvestigationScript(cid)
        dialogue.llm = script  # type: ignore[assignment]
        await link.debug("type", text=f"Лобарт, {WORDS}.")
        believed = await _until(lambda: any(f.get("npc") == LOBART and f.get("crime") == cid
                                           for f in facts("blamed")), 90)
        await settled()
        testimony = [f for f in facts("testimony") if f.get("npc") == LOBART and f.get("witness") == HILDA
                     and f.get("about") == VINO and cid in f.get("crime_told", [])]
        if not check("about_testimony", len(testimony) == 1 and script.asked, testimony):
            return _logged(results)
        beliefs = [f for f in facts("blamed") if f.get("npc") == LOBART and f.get("crime") == cid]
        if not check("giver_blamed", believed and len(beliefs) == 1 and beliefs[0].get("culprit") == VINO
                     and beliefs[0].get("truth") is True
                     and beliefs[0].get("because") == f"testimony:{testimony[0]['id']}", beliefs):
            return _logged(results)
        offered = await _until_async(lambda: link.offered_dialogs(LOBART),
                                    lambda rows: questbook.done_node(q) in rows, 30)
        if not check("stage_completed", offered, director.book.progress(q.id)):
            return _logged(results)
        gold_before = await count("HERO", "ITMI_GOLD")
        await link.choose_dialog(LOBART, questbook.done_node(q))
        await _until_async(lambda: link.story_state(vars=[q.id]),
                           lambda state: state.get("vars", {}).get(q.id) == 2, 20)
        await director.book.settle(LOBART)
        if not check("quest_completed", director.book.progress(q.id).state == "done"
                     and await count("HERO", "ITMI_GOLD") == gold_before + 1, q.id):
            return _logged(results)

        kinds = {"npc_crime", "npc_crime_seen", "npc_crime_suspected", "testimony", "blamed",
                 "occasion", "line", "chain", "story_quest", "authored_done"}
        kept = [f for f in facts() if f.get("fact") in kinds]
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
                     and director.book.progress(q.id).state == "done", len(kept)):
            return _logged(results)
        replay = await director.decide(occasion)
        await director.occasions.poll()
        check("no_duplicates", replay == outcome
              and len([f for f in facts("occasion") if f.get("crime") == cid]) == 1
              and [f for f in facts() if f.get("fact") in kinds] == kept, len(kept))
        return _logged(results)
    finally:
        dialogue.llm, talk.reflexes, will.busy = original_llm, original_reflexes, original_busy
        director.model.llm, director.chains.model.llm = original_model, original_builder
        will.cards, service.cards = original_cards, original_crime_cards
        rumors.gate, rumors.stage = original_gate, original_stage
        link.listeners.remove(record)
