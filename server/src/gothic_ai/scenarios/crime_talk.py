"""Crime knowledge on Lobart's farm: a rumor, testimony, silence and a missing chest item."""

from __future__ import annotations

import asyncio
import logging
import math
from collections.abc import AsyncIterator
from typing import TYPE_CHECKING, Any

from ..brain.reflex import Reflexes
from ..game import GameError, GameLink
from ..llm import Done, LLMEvent, Message, ToolCall
from ..llm.fake import FakeBackend, FakeStep
from ..protocol import Session
from .common import (
    HILDA,
    LOBART,
    ClaimScript,
    FixedDecider,
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
    from ..chests import ChestDesk
    from ..concerns import Concerns
    from ..crimes import Crimes
    from ..deals import Deals
    from ..dialogue import DialogueManager
    from ..sightings import Sightings
    from ..social import Rumors
    from ..talk import TalkDesk
    from ..witnesses import Witnesses

log = logging.getLogger(__name__)
DENIAL = "я ничего не брал в твоем доме"  # the game's input normalizes ё to е


async def crime_talk(
    link: GameLink,
    service: Crimes,
    dialogue: DialogueManager,
    talk: TalkDesk,
    rumors: Rumors,
    witnesses: Witnesses,
    sightings: Sightings,
    deals: Deals,
    chests: ChestDesk,
    concerns: Concerns,
    slot: int = 7,
) -> dict[str, str]:
    """Three independent scenes from save 1. With a live model only the first scene runs.

    Every theft uses real native ownership and real witnesses. Hilda's rule and native SVM finish
    before she is moved to the field for speech. No perception is disabled. A
    rumor gate chooses the Hilda--Lobart meeting and removes its random lot; it never bypasses the
    knowledge, hop, freshness or silence checks. Reloading resets the scripts between scenes.
    """
    results: dict[str, str] = {}
    live = dialogue.llm.name != "fake"
    original_llm, original_gate, original_stage = dialogue.llm, rumors.gate, rumors.stage
    meeting = False
    facts = service.chronicle.facts

    def check(name: str, ok: bool, detail: object = "") -> bool:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"
        log.info("crime_talk %-22s %s", name, results[name])
        return ok

    def knows(npc: str, crime: str, degree: str | None = None) -> bool:
        return any(
            k.crime.get("id") == crime and (degree is None or k.degree == degree) for k in service.known(npc)
        )

    async def stand(npc: str, point: str, *, face: str | None = None) -> None:
        await link.debug("skip_important", near=npc)
        await link.npc_act(
            npc, "start_activity", state="ZS_STAND_WP", waypoint=point, minutes=120, place=True
        )
        if face is not None:
            await link.npc_act(npc, "turn_to", npc=face)

    async def drop() -> bool:
        try:
            await link.debug("npc_drop", npc=LOBART, item="ITMI_SILVERCUP")
        except GameError as err:
            if "could not drop it now" not in str(err):
                raise
            return False
        return True

    async def clean(label: str) -> bool:
        nonlocal meeting
        meeting = False
        dialogue.llm = original_llm
        loading = asyncio.create_task(wait_message(link, Session, lambda m: m.reason == "load", 60))
        await asyncio.sleep(0)
        await link.debug("load", slot + 1)
        await loading
        await link.wait_world()
        await asyncio.sleep(0.5)
        return check(f"{label}_branch", not facts("crime"), f"{len(facts('crime'))} crimes")

    async def theft(label: str) -> dict[str, Any] | None:
        await link.debug("player_goto", waypoint="NW_FARM1_INHOUSE_01")
        for npc in farm:
            await stand(npc, parked)
        await stand(LOBART, HOUSE)
        await asyncio.sleep(0.5)
        if not check(f"{label}_drop", await _until_async(drop, bool, 10), "native silver cup"):
            return None
        await asyncio.sleep(1)
        await stand(LOBART, parked)
        prepared, preparation = await prepare_theft_witness(link, HILDA)
        if not check(f"{label}_witness_ready", prepared, preparation):
            return None
        before = {f["id"] for f in facts("crime")}
        with WitnessReaction(link, service.chronicle, HILDA) as reaction:
            await link.debug("player_take", item="ITMI_SILVERCUP", distance=1200)
            await _until(lambda: any(f["id"] not in before for f in facts("crime")), 10)
            crime = next((f for f in facts("crime") if f["id"] not in before), None)
            if crime is not None and not check(
                f"{label}_reaction", await reaction.wait(str(crime["id"])), reaction.detail
            ):
                return None
        # Replacing the activity earlier can interrupt the held rule or its native voice.
        await stand(HILDA, field)
        if not check(
            f"{label}_theft",
            crime is not None
            and crime.get("kind") == "theft"
            and crime.get("owner") == LOBART
            and set(crime.get("witnesses", ())) == {HILDA}
            and knows(HILDA, str(crime["id"]), "seen")
            and not knows(LOBART, str(crime["id"])),
            crime,
        ):
            return None
        return crime

    async def next_meeting() -> None:
        nonlocal meeting
        meeting = True
        await link.debug("time", hour=7, minute=0)
        await rumors.tick()
        await asyncio.sleep(0.5)

    await link.wait_world()
    if link.session is None:
        await wait_message(link, Session, timeout=10)
    talk.reflexes = Reflexes(FixedDecider({"posture": "listen"}))
    rumors.gate = lambda _r, a, b: meeting and a == HILDA and b == LOBART
    rumors.stage = None  # bare facts, no background gossip model calls
    try:
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
        if not check("farm", HOUSE in points and {LOBART, HILDA} <= humans, HOUSE):
            return _logged(results)
        house = points[HOUSE]
        house_pos = (house["x"], house["y"], house["z"])
        outside = [p for p in points.values() if p["name"].startswith("NW_FARM1_") and not p.get("room")]
        far = [p for p in outside if math.dist((p["x"], p["y"], p["z"]), house_pos) > 8000]
        fields = [p for p in outside if "FIELD" in p["name"]]
        if not check("positions", bool(far) and bool(fields), f"{len(far)} parking, {len(fields)} field"):
            return _logged(results)
        parked = min(far, key=lambda p: math.dist((p["x"], p["y"], p["z"]), house_pos))["name"]
        field = min(fields, key=lambda p: math.dist((p["x"], p["y"], p["z"]), house_pos))["name"]
        if not await _until(lambda: rumors.social is not None, 30):
            return _logged({**results, "social": "failed (no social graph)"})
        assert rumors.social is not None
        if not check("tie", LOBART in rumors.social.ties.get(HILDA, {}), "Hilda knows Lobart"):
            return _logged(results)
        if not check("baseline", not facts("crime") and await _save(link, slot + 1) is not None):
            return _logged(results)

        # Scene 1: Lobart did not see it, hears Hilda's account and asks her about the denial.
        crime = await theft("heard")
        if crime is None:
            return _logged(results)
        cid = str(crime["id"])
        await next_meeting()
        heard = await _until(lambda: knows(LOBART, cid, "heard"), 15)
        situation = service.situation(LOBART) or ""
        if not check("heard_summary", heard and "со слов" in situation and "Хильд" in situation, situation):
            return _logged(results)
        await stand(LOBART, field, face="PLAYER")
        await stand(HILDA, field)
        await link.debug("player_goto", to=LOBART, distance=300)
        if not check("talk_ready", await _until(lambda: LOBART in talk.people and HILDA in talk.people, 15)):
            return _logged(results)
        await witnesses.prepare(LOBART)
        captured: list[str] = []
        replies: list[str] = []
        inner: Any = original_llm
        if not live:
            inner = ClaimScript(
                said=[
                    (
                        "Ответ:",
                        FakeStep(
                            "Врёшь! Хильда видела, как ты брал кубок.",
                            [ToolCall("catch_lie", {"said": DENIAL, "seen": "С1"})],
                        ),
                    ),
                    (
                        DENIAL,
                        FakeStep(
                            "Хильда! Скажи, что ты видела.",
                            [ToolCall("ask_witness", {"who": "Хильда", "said": DENIAL})],
                        ),
                    ),
                ],
                told=[],
                default="За кражу с тебя спрос будет.",
                only="Лобарт",
                others=FakeBackend("Я сама видела, как он взял серебряный кубок в нашем доме.", delay=0),
            )

        class Recorded:
            name = inner.name
            model = getattr(inner, "model", None)

            def __getattr__(self, attr: str) -> Any:
                return getattr(inner, attr)

            async def stream(self, **kw: Any) -> AsyncIterator[LLMEvent]:
                lobart = kw.get("cache_key") == LOBART
                if lobart:
                    captured.append(
                        "\n".join(
                            m.content
                            for m in kw["messages"]
                            if isinstance(m, Message) and m.role == "developer"
                        )
                    )
                async for event in inner.stream(**kw):
                    if lobart and isinstance(event, Done):
                        replies.append(event.text)
                    yield event

        dialogue.llm = Recorded()  # type: ignore[assignment]
        await link.debug("type", text=f"Лобарт, {DENIAL}.")
        await _until(lambda: bool(replies), 120 if live else 45)
        if not live:
            await _until(lambda: any(f.get("crime") == cid for f in facts("caught")), 90)
        await _until(
            lambda: (
                LOBART not in witnesses._asking
                and not dialogue.speaking(LOBART)
                and not dialogue.speaking(HILDA)
            ),
            120 if live else 30,
        )
        prompt = "\n".join(captured)
        check(
            "prompt_knowledge",
            "со слов" in prompt and "Хильд" in prompt and service.describe(crime) in prompt,
            [line for line in prompt.splitlines() if service.describe(crime) in line],
        )
        check(
            "reply_theft",
            bool(replies)
            and any(
                w in " ".join(replies).lower()
                for w in ("краж", "укра", "вор", "взял", "брал", "кубок", "стащ", "утащ")
            ),
            replies,
        )
        caught = [f for f in facts("caught") if f.get("crime") == cid]
        if live:
            check("live_caught_observation", True, caught or "модель не вызвала catch_lie")
            return _logged(results)
        testimony = [f for f in facts("testimony") if f.get("witness") == HILDA]
        check("testimony", bool(testimony) and bool(caught) and caught[-1].get("witness") == HILDA, testimony)
        check("caught", bool(caught), caught)

        # Scene 2: the same real theft, but the witness accepts silence before the meeting.
        if not await clean("silence"):
            return _logged(results)
        crime = await theft("silence")
        if crime is None:
            return _logged(results)
        cid = str(crime["id"])
        await link.debug("player_goto", to=HILDA, distance=300)
        await _until(lambda: HILDA in talk.people, 15)
        dialogue.llm = ClaimScript(
            said=[
                (
                    "молчи",
                    FakeStep(
                        "Ладно, об этом деле никому ни слова.",
                        [
                            ToolCall(
                                "propose_deal",
                                {"npc_gives": [], "hero_gives": [], "silence": [f"crime:{cid}"]},
                            )
                        ],
                    ),
                )
            ],
            told=[],
            default="Договорились.",
            only="Хильда",
        )
        await link.debug("type", text="Хильда, молчи об этом кубке. Договорились?")
        offered = await _until(lambda: HILDA in deals.open, 30)
        if not check("silence_offered", offered):
            return _logged(results)
        await asyncio.sleep(1.2)
        pressed = await link.press_deal()
        accepted = await _until(
            lambda: any(f.get("npc") == HILDA and f.get("crime") == cid for f in facts("silenced")), 15
        )
        if not check("silence_accepted", pressed == "sent" and accepted, pressed):
            return _logged(results)
        await next_meeting()
        await asyncio.sleep(4)
        check(
            "silence_stops",
            not knows(LOBART, cid)
            and not any(f.get("crime") == cid and f.get("to") == LOBART for f in facts("rumor")),
            service.situation(LOBART) or "Лобарт не знает",
        )

        # Scene 3: the owner sees only the visitor, then opens his robbed chest himself.
        if not await clean("suspect"):
            return _logged(results)
        await link.debug("player_goto", waypoint="NW_FARM1_INHOUSE_02")
        for npc in farm:
            await stand(npc, parked)
        await stand(LOBART, HOUSE)
        for distance in (200, 350, 100):
            await link.debug("teleport", near=LOBART, distance=distance)
            await link.npc_act(LOBART, "turn_to", npc="PLAYER")
            observed = await _until_async(
                lambda: sightings.desk.fresh(LOBART),
                lambda v: v is not None and v.hero is not None and v.hero.seen,
                5,
            )
            if observed:
                break
        view = sightings.desk.view(LOBART)
        if not check(
            "suspect_visitor",
            observed and bool(sightings.hero_trail(LOBART)),
            {
                "sightings": [s.record() for s in sightings.hero_trail(LOBART)],
                "hero": view.hero.model_dump() if view is not None and view.hero is not None else None,
            },
        ):
            return _logged(results)
        found = [c for c in await chests.chests(LOBART, fresh=True) if c.items and c.distance < 4000]
        found.sort(key=lambda c: (c.why != "owner", c.why != "key", c.distance))
        if not check("suspect_chest", bool(found), [(c.id, c.why) for c in found]):
            return _logged(results)
        chest = found[0]
        await stand(LOBART, parked)
        await link.debug("player_goto", pos=(chest.pos[0] + 100, chest.pos[1], chest.pos[2]))
        alone = await link.story_nearby("PC_HERO", 3500)
        if not check("suspect_alone", not alone, [p["npc"] for p in alone]):
            return _logged(results)
        item = next((i for i in chest.items if i.item == "ITMI_GOLD" and i.count > 1), chest.items[0])
        await link.debug("chest_take", chest=chest.id, item=item.item, count=1)
        await _until(lambda: any(f.get("chest") == chest.id for f in facts("crime")), 10)
        crime = next((f for f in facts("crime") if f.get("chest") == chest.id), None)
        if not check("suspect_theft", crime is not None and not crime.get("witnesses"), crime):
            return _logged(results)
        assert crime is not None
        cid = str(crime["id"])
        # BED_02 is the bed's point, not the chest's interaction slot. Put the free owner on the
        # floor in front of the chest, then leave its slot while staying inside the active house.
        await link.debug("player_goto", pos=(chest.pos[0], chest.pos[1], chest.pos[2] + 200))
        await link.debug("npc_goto", npc=LOBART, to="PC_HERO", distance=0)
        await link.debug("player_goto", waypoint="NW_FARM1_INHOUSE_02")
        conv = dialogue.open(LOBART, "Лобарт")
        held = await dialogue.borrow_body(conv, "scenario-chest")
        try:
            opened = await link.act(
                LOBART,
                "use_chest",
                chest=chest.id,
                take=[{"item": item.item, "count": 1}],
                put=[],
                timeout=90,
            )
            note = (
                await chests.looked(LOBART, chest.id, opened.detail)
                if opened.status == "done"
                else opened.detail
            )
        finally:
            if held:
                dialogue.return_body(conv, "scenario-chest")
        snap = link.snapshots.get(LOBART)
        if not check(
            "suspect_opened",
            opened.status == "done" and "пропало" in (note or ""),
            {
                "status": opened.status,
                "detail": note,
                "npc": snap.npc.pos if snap is not None else None,
                "chest": chest.pos,
            },
        ):
            return _logged(results)
        check(
            "suspect_knowledge",
            knows(LOBART, cid, "suspects") and "подозрева" in (service.situation(LOBART) or ""),
            service.situation(LOBART),
        )
        check("suspect_concern", any(cid in c.key for c in concerns.of(LOBART)), concerns.of(LOBART))
        return _logged(results)
    finally:
        dialogue.llm = original_llm
        rumors.gate, rumors.stage = original_gate, original_stage
