"""A native theft witness sells cover; another witness exposes it, including after a load."""

from __future__ import annotations

import asyncio
import logging
import math
import re
from pathlib import Path
from typing import TYPE_CHECKING, Any

from ..brain.reflex import Reflexes
from ..deals import GOLD
from ..game import GameError, GameLink
from ..llm import ToolCall
from ..protocol import Session
from ..trace import read as read_trace
from .common import (
    HILDA,
    LOBART,
    VINO,
    FixedDecider,
    WitnessReaction,
    _logged,
    _save,
    _until,
    _until_async,
    model_off,
    prepare_theft_witness,
    wait_message,
)

if TYPE_CHECKING:
    from ..crimes import Crimes
    from ..deals import Deals
    from ..dialogue import DialogueManager
    from ..people_knowledge import PeopleKnowledge
    from ..sightings import Sightings
    from ..social import Rumor, Rumors
    from ..talk import TalkDesk
    from ..trust import Trust
    from ..witnesses import Witnesses

log = logging.getLogger(__name__)
HOUSE = "NW_FARM1_INHOUSE_01"
DENIAL = "На вашей ферме я утром не был и твой кубок не брал"
VOUCH = "Всё утро он был со мной в городе, на вашей ферме его не было и кубок он не брал"
REQUEST = "Если Лобарт спросит, прикрой меня"


async def cover_scenario(
    link: GameLink,
    service: Crimes,
    dialogue: DialogueManager,
    talk: TalkDesk,
    deals: Deals,
    rumors: Rumors,
    witnesses: Witnesses,
    sightings: Sightings,
    trust: Trust,
    log_dir: str,
    slot: int = 7,
    *,
    subject: bool = False,
    people: PeopleKnowledge | None = None,
) -> dict[str, str]:
    """Given cover and witness decisions, check payment, evidence, exposure and save/load.

    `subject` continues through the NPC rumor and witness warnings, reading the assembled situation
    without an extra model turn.
    """
    off = model_off(dialogue)
    results: dict[str, str] = {}
    original_reflexes = talk.reflexes
    original_gate, original_stage = rumors.gate, rumors.stage
    chronicle, directory = service.chronicle, Path(log_dir)
    facts = chronicle.facts
    participants = {LOBART, HILDA, VINO}
    if subject and people is None:
        return _logged({"subject_setup": "failed (cover_subject needs people knowledge)"})

    def check(name: str, ok: bool, detail: object) -> bool:
        results[name] = f"{'done' if ok else 'failed'} ({detail})"
        log.info("cover %-24s %s", name, results[name])
        return ok

    def turns(npc: str) -> list[dict[str, Any]]:
        return [
            row
            for path in sorted((directory / "traces").glob("*.jsonl"))
            for row in read_trace(path)
            if row.get("type") == "turn" and row.get("npc") == npc
        ]

    def exposed() -> list[dict[str, Any]]:
        return [f for f in facts("exposed") if f.get("npc") == LOBART and f.get("liar") == HILDA]

    async def stand(npc: str, point: str, *, face: str | None = None) -> None:
        await link.debug("skip_important", near=npc)
        await link.debug("npc_goto", npc=npc, waypoint=point)
        await link.npc_act(
            npc, "start_activity", state="ZS_STAND_WP", waypoint=point, minutes=120, place=False
        )
        if face is not None:
            await link.npc_act(npc, "turn_to", npc=face)

    async def stage_investigation(point: str, *, third: str = VINO) -> tuple[bool, dict[str, Any]]:
        trio = (HILDA, third, LOBART)
        for npc in trio:
            await stand(npc, point)
        await link.debug("teleport", near=LOBART, distance=300)
        detail: dict[str, Any] = {}

        async def ready() -> bool:
            state = await link.story_state(npcs=trio, near=LOBART)
            views = {view.npc: view for view in await link.npc_view(trio)}
            detail.update({"npcs": state.get("npcs", {}), "views": [v.model_dump() for v in views.values()]})
            detail["roster"] = {
                npc: {"name": e.name, "distance": e.distance, "earshot": e.earshot}
                for npc in trio if (e := talk.people.get(npc)) is not None
            }
            return all(
                npc in views
                and views[npc].state == "ZS_STAND_WP"
                and views[npc].body == "stand"
                and (st := detail["npcs"].get(npc, {})).get("distance") is not None
                and float(st["distance"]) <= 500
                for npc in trio
            ) and all(
                npc in talk.people and talk.people[npc].earshot <= 1000
                and talk.people[npc].distance <= 1000
                for npc in trio
            ) and talk.people[LOBART].name == "Лобарт"

        return await _until_async(ready, bool, 20), detail

    async def drop() -> bool:
        try:
            await link.debug("npc_drop", npc=LOBART, item="ITMI_SILVERCUP")
        except GameError as err:
            if "could not drop it now" not in str(err):
                raise
            return False
        return True

    async def settled() -> bool:
        return await _until(
            lambda: (
                not witnesses._asking.intersection(participants)
                and all(
                    not dialogue.speaking(npc)
                    and not (dialogue.conversations.get(npc) and dialogue.conversations[npc].actions)
                    for npc in participants
                )
            ),
            60,
        )

    try:
        await link.wait_world()
        if link.session is None:
            await wait_message(link, Session, timeout=10)
        log.info("given: дано: решающая модель оставляет NPC слушать героя")
        talk.reflexes = Reflexes(FixedDecider({"posture": "listen"}))
        rumors.gate, rumors.stage = lambda *_: False, None
        await link.debug("player_hp", hp=5000)
        await link.debug("time", hour=8, minute=0)
        humans = {p["npc"] for p in await link.story_people()}
        farm = list(
            dict.fromkeys(
                [
                    LOBART,
                    HILDA,
                    VINO,
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
        if not check("owned_cup", await _until_async(drop, bool, 10), "Lobart drops his native silver cup"):
            return _logged(results)
        await stand(LOBART, parked)
        prepared, preparation = await prepare_theft_witness(link, HILDA)
        if not check("native_witness_ready", prepared, preparation):
            return _logged(results)
        before = {f["id"] for f in facts("crime")}
        with WitnessReaction(link, chronicle, HILDA) as reaction:
            await link.debug("player_take", item="ITMI_SILVERCUP", distance=1200)
            await _until(lambda: any(f["id"] not in before for f in facts("crime")), 10)
            crime = next((f for f in facts("crime") if f["id"] not in before), None)
            if not check(
                "native_reaction",
                crime is not None and await reaction.wait(str(crime["id"])),
                reaction.detail,
            ):
                return _logged(results)
        if not check(
            "native_theft",
            crime is not None
            and crime.get("kind") == "theft"
            and crime.get("owner") == LOBART
            and set(crime.get("witnesses", [])) == {HILDA}
            and any(k.crime["id"] == crime["id"] and k.degree == "seen" for k in service.known(HILDA))
            and not service.known(LOBART),
            crime,
        ):
            return _logged(results)
        assert crime is not None
        cid = str(crime["id"])
        await stand(HILDA, field)
        await link.debug("teleport", near=HILDA, distance=250)
        deal_position: dict[str, Any] = {}

        async def deal_ready() -> bool:
            state = await link.story_state(npcs=[HILDA])
            current = (state.get("npcs") or {}).get(HILDA) or {}
            entry = talk.people.get(HILDA)
            deal_position.update({"state": current, "roster": entry.model_dump() if entry else None})
            return (
                current.get("state") == "alive"
                and entry is not None and entry.name == "Хильда"
                and entry.distance <= 600 and entry.earshot <= 600
            )

        if not check(
            "deal_near",
            await _until_async(deal_ready, bool, 20),
            deal_position,
        ):
            return _logged(results)
        state = await link.story_state(npcs=[HILDA])
        day = int(state.get("day", 0))
        hero_gold = int(await link.evaluate(["call", "NPC_HASITEMS", ["inst", "HERO"], ["sym", GOLD]]) or 0)
        if hero_gold < 100:
            await link.story_effect(HILDA, [{"do": "give", "item": GOLD, "count": 100 - hero_gold}])
        off.given(
            "дано: Хильда продаёт прикрытие за пять золотых",
            who=HILDA, when=REQUEST,
            calls=[ToolCall("propose_deal", {
                "hero_gives": [{"item": GOLD, "count": 5}],
                "cover": {"crime": cid, "day": day + 1, "vouch": VOUCH},
            })],
        )
        off.silent("дано: Хильда молчит (прикрытие предложено)", who=HILDA, when=REQUEST)
        off.silent(
            "дано: Хильда молчит (сделка прикрытия состоялась)",
            who=HILDA, when="сделка состоялась",
        )
        await link.debug(
            "type",
            text=(
                f"Хильда, ты видела, как я взял кубок. {REQUEST}: "
                "скажи, что я был с тобой. Что хочешь взамен?"
            ),
        )
        await _until(lambda: HILDA in deals.open, 45)
        await settled()
        deal = deals.open.get(HILDA)
        window = await link.deal_window() or {}
        if not check(
            "cover_window",
            deal is not None and any("прикрыть" in str(text).lower() for text in window.get("npc_gives", [])),
            window,
        ):
            return _logged(results)
        assert deal is not None
        did = deal.id
        await asyncio.sleep(1.2)
        pressed = await link.press_deal()
        accepted = await _until(lambda: any(f.get("deal") == did for f in facts("deal")), 30)
        if not check(
            "cover_accepted",
            pressed == "sent"
            and accepted
            and any(
                f.get("npc") == HILDA and f.get("crime") == cid and f.get("deal") == did
                for f in facts("silenced")
            )
            and await link.deal_window() is None,
            {"key": pressed, "deal": did, "silenced": facts("silenced")},
        ):
            return _logged(results)
        await settled()
        talk.end("gone")
        if not check(
            "deal_released",
            await _until(lambda: HILDA not in talk.attached, 20),
            "Hilda detached before the next native activity",
        ):
            return _logged(results)

        # Vino independently sees the hero on the farm. He need not see the cup: the exact same
        # denial also claims the hero was never on the farm, which his real sighting disproves.
        await stand(VINO, field, face="PLAYER")
        await link.debug("teleport", near=VINO, distance=250)
        await link.npc_act(VINO, "turn_to", npc="PLAYER")

        async def vino_sees() -> bool:
            view = await sightings.desk.fresh(VINO)
            return bool(view and view.hero and view.hero.seen and sightings.hero_trail(VINO))

        if not check(
            "second_source",
            await _until_async(vino_sees, bool, 10),
            [s.record() for s in sightings.hero_trail(VINO)],
        ):
            return _logged(results)
        # JUST_MIN omits the current encounter from testimony: ask about the earlier morning,
        # after Vino's independent sighting and Hilda's actual theft perception have aged.
        await link.debug("time", hour=9, minute=0)
        ready, preparation = await stage_investigation(field)
        if not check("ask_near", ready, preparation):
            return _logged(results)

        async def investigate(
            who: str, name: str, *, asker: str = LOBART, judge: bool = True,
        ) -> tuple[str, str, list[dict[str, Any]]]:
            prior = {r["id"] for r in turns(asker)}
            asker_name = talk.people[asker].name
            # A load rebuilds acquaintances asynchronously; they supply witness candidates.
            await _until(lambda: rumors.social is not None, 20)
            question = await dialogue.situation(asker, asker_name)
            off.given(
                f"дано: {asker_name} спрашивает свидетеля {name}",
                who=asker, when=DENIAL,
                calls=[ToolCall("ask_witness", {"who": name, "said": DENIAL})],
            )
            off.silent(
                f"дано: {asker_name} молчит (вопрос свидетелю передан)", who=asker, when=DENIAL,
            )
            off.given(
                f"дано: реплика свидетеля {name}", who=who, when="спрашивает тебя",
                line=VOUCH if who == HILDA else "Я видел его утром на ферме.",
            )
            if judge:
                off.given(
                    f"дано: {asker_name} уличает героя по показанию {name}",
                    who=asker, when="Ответ:",
                    calls=[ToolCall("catch_lie", {"said": DENIAL, "seen": "С1"})],
                )
            off.silent(
                f"дано: {asker_name} молчит (показание {name} получено)",
                who=asker, when="Ответ:",
            )
            await link.debug("type", text=f"{asker_name}, {DENIAL}. Спроси у {name}.")
            await _until(
                lambda: any(
                    row["id"] not in prior and "Ответ:" in row.get("trigger", {}).get("text", "")
                    for row in turns(asker)
                ),
                90,
            )
            await settled()
            evidence = await dialogue.situation(asker, asker_name)
            calls = [
                c
                for r in turns(asker)
                if r["id"] not in prior
                for step in r.get("steps", [])
                for c in step.get("calls", [])
                if c.get("name") == "catch_lie"
            ]
            return question, evidence, calls

        _, evidence, calls = await investigate(HILDA, "Хильда")
        testimony = next(
            (f for f in reversed(facts("testimony")) if f.get("npc") == LOBART and f.get("witness") == HILDA),
            None,
        )
        if not check(
            "covered_testimony",
            testimony is not None
            and testimony.get("cover") == did
            and bool(testimony.get("saw"))
            and cid in testimony.get("crime_saw", [])
            and not testimony.get("told")
            and not testimony.get("crime_told")
            and testimony.get("vouch") == VOUCH,
            testimony,
        ):
            return _logged(results)
        lines = [
            line
            for line in evidence.splitlines()
            if re.match(r"- С\d+:", line)
        ]
        check("vouch_row", any("Хильда говорит:" in line and VOUCH in line for line in lines), lines)
        if not check(
            "cover_not_evidence",
            len(calls) == 1
            and calls[0].get("outcome") == "rejected"
            and not any(f.get("npc") == LOBART and f.get("title") == DENIAL for f in facts("caught")),
            calls,
        ):
            return _logged(results)
        # Keep this testimony newer than Hilda's so its real evidence row is deterministically С1.
        talk.end("gone")
        await _until(lambda: not participants.intersection(talk.attached), 20)
        await link.debug("time", hour=9, minute=10)
        ready, preparation = await stage_investigation(field)
        if not check("second_ask_near", ready, preparation):
            return _logged(results)
        _, _, calls = await investigate(VINO, "Вино")
        caught = [f for f in facts("caught") if f.get("npc") == LOBART and f.get("title") == DENIAL]
        if not check(
            "caught_second_source",
            len(calls) == 1
            and calls[0].get("outcome") == "accepted"
            and len(caught) == 1
            and caught[0].get("witness") == VINO,
            {"calls": calls, "caught": caught},
        ):
            return _logged(results)
        await _until(lambda: bool(exposed()), 10)
        if not check(
            "exposed",
            len(exposed()) == 1
            and exposed()[0].get("testimony") == testimony["id"]
            and exposed()[0].get("cover") == did,
            exposed(),
        ):
            return _logged(results)
        talk.end("gone")
        await settled()
        if subject:
            assert people is not None
            if not check(
                "subject_social",
                await _until(lambda: rumors.social is not None, 20),
                "native acquaintances are ready",
            ):
                return _logged(results)
            social = rumors.social
            assert social is not None
            common = sorted(
                (social.ties.get(LOBART, {}).keys() & social.ties.get(HILDA, {}).keys())
                - {LOBART, HILDA},
                key=lambda npc: (npc != "VLK_439_VATRAS", npc),
            )
            state = await link.story_state(npcs=common)
            third = next(
                (npc for npc in common if (state.get("npcs", {}).get(npc) or {}).get("state") == "alive"),
                "",
            )
            if not check(
                "subject_tie",
                bool(third),
                {"third": third, "common": common, "ties": social.describe(third) if third else []},
            ):
                return _logged(results)
            third_name = social.names[third]
            participants.add(third)

            def hilda_rumors() -> list[Rumor]:
                return [r for r in rumors.rumors.values() if r.kind == "cover" and r.subject == HILDA]

            sources = hilda_rumors()
            if not check(
                "subject_source",
                len(sources) == 1
                and sources[0].known == {LOBART: 0}
                and bool(people.of(LOBART))
                and not people.of(third),
                {"rumors": [(r.subject, r.known) for r in sources], "first": people.context(LOBART)},
            ):
                return _logged(results)
            rumor = sources[0]
            ready, preparation = await stage_investigation(field, third=third)
            if not check("subject_ask_near", ready, preparation):
                return _logged(results)
            rumors.stage = original_stage
            await rumors.tick()
            hero_trust = trust.of(third)
            rumors.gate = lambda r, a, b: r.key == rumor.key and (a, b) == (LOBART, third)
            # They really meet here, outside their routine's hours; the ordinary staging guard applies.
            passed = rumors.offer_status(rumor, LOBART, third)
            heard = [
                f for f in facts("rumor")
                if f.get("kind") == "cover" and f.get("subject") == HILDA
            ]
            if not check(
                "subject_spread",
                passed == "told"
                and len(heard) == 1
                and heard[0].get("from") == LOBART
                and heard[0].get("to") == third
                and heard[0].get("hops") == 1
                and rumor.known[third] == 1
                and not rumor.tells(HILDA, rumor.since.get(third, day)),
                {"offer": passed, "facts": heard},
            ):
                return _logged(results)
            check("subject_hero_trust", trust.of(third) == hero_trust, trust.of(third))
            knowledge = people.context(third) or ""
            check(
                "subject_context",
                "Что ты знаешь о людях вокруг" in knowledge
                and "Хильда" in knowledge
                and "Лобарт" in knowledge,
                knowledge,
            )
            await link.debug("teleport", near=third, distance=250)
            if not check(
                "subject_asker_near",
                await _until(
                    lambda: (entry := talk.people.get(third)) is not None
                    and entry.name == third_name and entry.distance <= 600 and entry.earshot <= 600,
                    20,
                ),
                {"third": third, "roster": talk.people.get(third)},
            ):
                return _logged(results)
            question, evidence, _ = await investigate(HILDA, "Хильда", asker=third, judge=False)
            warning = people.warning(third, HILDA)
            testimony_rows = [
                line for line in evidence.splitlines() if re.match(r"- С\d+:", line)
            ]
            check(
                "subject_model_context",
                knowledge in question,
                {"knowledge": knowledge},
            )
            check(
                "subject_candidate_warning",
                bool(warning)
                and any(line.startswith("- Хильда —") and warning in line for line in question.splitlines()),
                [line for line in question.splitlines() if line.startswith("- Хильда —")],
            )
            check(
                "subject_testimony_warning",
                bool(warning)
                and any("Хильда говорит:" in line and warning in line for line in testimony_rows)
                and any(
                    f.get("npc") == third and f.get("witness") == HILDA
                    for f in facts("testimony")
                ),
                testimony_rows,
            )
            talk.end("gone")
            await settled()
            kinds = {
                "crime", "crime_seen", "deal", "covered", "silenced", "testimony", "caught", "exposed",
                "rumor",
            }
            snapshot = [f for f in facts() if f.get("fact") in kinds]
            before_people = people.of(third)
            before_trust = trust.of(third)
            before_known = dict(rumor.known)
            head = sightings.memory.head(chronicle.branch) if chronicle.branch else 0
            saved = await _save(link, slot)
            if saved is None:
                check("subject_branch", False, "save was refused")
                return _logged(results)
            loading = asyncio.create_task(wait_message(link, Session, lambda m: m.reason == "load", 60))
            await asyncio.sleep(0)
            await link.debug("load", slot)
            session = await loading
            await link.wait_world()
            await rumors.tick()
            loaded = hilda_rumors()
            check(
                "subject_branch",
                saved.seq >= head
                and session.parent == saved.branch
                and session.parent_seq == saved.seq
                and [f for f in facts() if f.get("fact") in kinds] == snapshot
                and len(loaded) == 1
                and loaded[0].known == before_known
                and people.of(third) == before_people
                and people.context(third) == knowledge
                and trust.of(third) == before_trust,
                {"facts": len(snapshot), "rumors": len(loaded), "knowledge": people.context(third)},
            )
            if loaded:
                check(
                    "subject_no_duplicates",
                    rumors.offer_status(loaded[0], LOBART, third) == "no"
                    and [f for f in facts() if f.get("fact") in kinds] == snapshot
                    and len(exposed()) == 1
                    and people.of(third) == before_people,
                    {"exposed": len(exposed()), "rumor_facts": len(heard), "known": loaded[0].known},
                )
            return _logged(results)
        kinds = {"crime", "crime_seen", "deal", "covered", "silenced", "testimony", "caught", "exposed"}
        snapshot = [f for f in facts() if f.get("fact") in kinds]
        head = sightings.memory.head(chronicle.branch) if chronicle.branch else 0
        await asyncio.sleep(0.5)
        saved = await _save(link, slot)
        if saved is None:
            check("branch", False, "save was refused")
            return _logged(results)
        loading = asyncio.create_task(wait_message(link, Session, lambda m: m.reason == "load", 60))
        await asyncio.sleep(0)
        await link.debug("load", slot)
        session = await loading
        await link.wait_world()
        loaded = [f for f in facts() if f.get("fact") in kinds]
        if not check(
            "branch",
            saved.seq >= head
            and session.parent == saved.branch
            and session.parent_seq == saved.seq
            and loaded == snapshot
            and len(exposed()) == 1
            and any(k.crime["id"] == cid and k.silenced for k in service.known(HILDA))
            and bool(trust.of(LOBART).lied),
            {"before": len(snapshot), "after": len(loaded), "exposed": exposed()},
        ):
            return _logged(results)
        # ask_witness reuses an answer for an hour; cross that window to prove the loaded cover is
        # applied again, rather than merely displaying the saved testimony.
        await link.debug("time", hour=12, minute=0)
        ready, preparation = await stage_investigation(field)
        if not check("loaded_ask_near", ready, preparation):
            return _logged(results)
        before_testimony = len(facts("testimony"))
        await investigate(HILDA, "Хильда")
        reloaded = facts("testimony")[before_testimony:]
        check(
            "cover_after_load",
            any(
                f.get("witness") == HILDA
                and f.get("cover") == did
                and f.get("vouch") == VOUCH
                and not f.get("told")
                and not f.get("crime_told")
                for f in reloaded
            ),
            reloaded,
        )
        talk.end("gone")
        await _until(lambda: not participants.intersection(talk.attached), 20)
        await link.debug("time", hour=12, minute=10)
        ready, preparation = await stage_investigation(field)
        if not check("repeated_ask_near", ready, preparation):
            return _logged(results)
        _, _, repeated = await investigate(VINO, "Вино")
        check(
            "no_duplicate_exposed",
            len(repeated) == 1
            and repeated[0].get("outcome") == "rejected"
            and len(exposed()) == 1
            and exposed()[0] in snapshot,
            {"calls": repeated, "exposed": exposed()},
        )
        return _logged(results)
    finally:
        talk.reflexes = original_reflexes
        rumors.gate, rumors.stage = original_gate, original_stage


async def cover_subject_scenario(
    link: GameLink,
    service: Crimes,
    dialogue: DialogueManager,
    talk: TalkDesk,
    deals: Deals,
    rumors: Rumors,
    witnesses: Witnesses,
    sightings: Sightings,
    trust: Trust,
    log_dir: str,
    *,
    people: PeopleKnowledge,
) -> dict[str, str]:
    """Continue native cover through a rumor about Hilda, witness warnings and a save branch."""
    return await cover_scenario(
        link, service, dialogue, talk, deals, rumors, witnesses, sightings, trust, log_dir,
        subject=True, people=people,
    )
