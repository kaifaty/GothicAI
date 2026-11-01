"""Community law on save 1: Matteo's theft reaches Andre, then a witness keeps silent."""

from __future__ import annotations

import asyncio
import logging
import math
from typing import TYPE_CHECKING, Any

from ..brain.reflex import Reflexes
from ..crime_law_graph import law_var
from ..game import GameError, GameLink
from ..llm import ToolCall
from ..llm.fake import FakeBackend, FakeStep
from ..protocol import DialogChosen, Session
from .common import (
    ClaimScript,
    FixedDecider,
    _logged,
    _save,
    _until,
    _until_async,
    prepare_npc_parking,
    prepare_npc_pose,
    prepare_street_theft_witness,
    wait_message,
)
from .crimes import ANDRE, CITY_OWNER

if TYPE_CHECKING:
    from ..crime_law_runtime import CrimeLawRuntime
    from ..crimes import Crimes
    from ..deals import Deals
    from ..dialogue import DialogueManager
    from ..social import Rumors
    from ..talk import TalkDesk

log = logging.getLogger(__name__)
PETZMASTER = "DIA_ANDRE_PETZMASTER"
PAYNOW = f"{PETZMASTER}_PAYNOW"
PMSCHULDEN = "DIA_ANDRE_PMSCHULDEN"
COUNTERS = [f"PETZCOUNTER_CITY_{kind}" for kind in ("THEFT", "ATTACK", "MURDER", "SHEEPKILLER")]
THEFT_POINT, HERO_POINT = "NW_CITY_MERCHANT_TRADE_01", "NW_CITY_MAINSTREET_03"


async def crime_law(
    link: GameLink,
    service: Crimes,
    law: CrimeLawRuntime,
    dialogue: DialogueManager,
    talk: TalkDesk,
    rumors: Rumors,
    deals: Deals,
    slot: int = 7,
) -> dict[str, str]:
    """Exercise native ownership, perceptions, menus, voices and payment without model calls.

    Only the scene is staged: nearby people wait elsewhere, and Matteo's rule-based theft reaction ends
    after he has actually witnessed it. Random rumor meetings are disabled so the first scene tests
    the deadline itself. The second scene accepts an ordinary scripted silence deal before it.
    """
    results: dict[str, str] = {}
    facts = service.chronicle.facts
    chosen: list[DialogChosen] = []
    original_llm, original_reflexes = dialogue.llm, talk.reflexes
    original_gate, original_stage = rumors.gate, rumors.stage

    def check(name: str, ok: bool, detail: object = "") -> bool:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"
        log.info("crime_law %-24s %s", name, results[name])
        return ok

    def record(message: object) -> None:
        if isinstance(message, DialogChosen):
            chosen.append(message)

    def news(cid: str) -> list[dict[str, Any]]:
        return [f for f in facts("crime_known") if f.get("community") == "city" and f.get("crime") == cid]

    async def values(*variables: str) -> list[Any]:
        return await link.story_eval(ANDRE, [["var", var] for var in variables])

    async def gold() -> int:
        return int(await link.evaluate(["call", "NPC_HASITEMS", ["inst", "HERO"], ["sym", "ITMI_GOLD"]]))

    async def stand(npc: str, point: str, *, face: str | None = None) -> bool:
        if point == parked:
            expected = points[point]
            ready, detail = await prepare_npc_parking(
                link, npc, point, (expected["x"], expected["y"], expected["z"]),
            )
            if not ready:
                check("parking_pose", False, detail)
            return ready
        if npc == CITY_OWNER:
            # Both actors use native public street waypoints. Keep the hero separated before
            # the common helper's first initialization wait, without projecting through shop walls.
            await link.debug("skip_important", near=npc)
            await link.debug("npc_goto", npc=npc, waypoint=point)
            await link.debug("player_goto", waypoint=HERO_POINT)
        ready, detail = await prepare_npc_pose(
            link, npc, point, face=face,
        )
        if not ready:
            check("pose", False, detail)
        return ready

    async def theft_posture(label: str) -> bool:
        """Keep Matteo's initialized activity while proving actual sight and a fresh free roster."""
        ready, detail = await prepare_street_theft_witness(
            link, CITY_OWNER, THEFT_POINT, "ITMI_SILVERCUP",
        )
        detail.update(npc_point=THEFT_POINT, hero_point=HERO_POINT)
        return check(f"{label}_witness_ready", ready, detail)

    async def drop() -> bool:
        try:
            await link.debug("npc_drop", npc=CITY_OWNER, item="ITMI_SILVERCUP")
        except GameError as err:
            if "could not drop it now" not in str(err):
                raise
            return False
        return True

    async def theft(label: str) -> dict[str, Any] | None:
        await link.debug("time", hour=8, minute=0)
        await link.debug("player_goto", waypoint=HERO_POINT)
        # Find people at the real scene after the hero has activated that part of the city. Their
        # native guilds, ownership and perceptions remain unchanged throughout both scenes.
        for person in await link.story_nearby("PC_HERO", 7000):
            if person["npc"] in humans and person["npc"] != CITY_OWNER:
                if not await stand(person["npc"], parked):
                    return None
        if not await stand(CITY_OWNER, THEFT_POINT, face="PC_HERO"):
            return None
        if not check(f"{label}_drop", await _until_async(drop, bool, 10), "native Matteo silver cup"):
            return None
        if not await theft_posture(label):
            return None
        before = {f["id"] for f in facts("crime")}
        await link.debug("player_take", item="ITMI_SILVERCUP", distance=800)
        await _until(lambda: any(f["id"] not in before for f in facts("crime")), 10)
        crime = next((f for f in facts("crime") if f["id"] not in before), None)
        # Let layer 4a finish the held perception before replacing its fight for the law scene.
        reacted = crime is not None and await _until(
            lambda: any(
                f.get("crime") == crime["id"] and f.get("npc") == CITY_OWNER
                and f.get("verdict") == "server" for f in facts("crime_reaction")
            ),
            10,
        )
        if not check(
            f"{label}_theft",
            crime is not None
            and reacted
            and crime.get("kind") == "theft"
            and crime.get("owner") == CITY_OWNER
            and crime.get("community") == "city"
            and set(crime.get("witnesses", ())) == {CITY_OWNER}
            and any(k.crime["id"] == crime["id"] and k.degree == "seen" for k in service.known(CITY_OWNER)),
            {"crime": crime, "reactions": [
                fact for fact in facts("crime_reaction")
                if crime is not None and fact.get("crime") == crime["id"]
            ]},
        ):
            return None
        if not await stand(CITY_OWNER, THEFT_POINT, face="PC_HERO"):
            return None
        await law.tick()
        return crime

    async def tomorrow() -> None:
        # debug time follows the game's sleeping rule: an earlier hour is the next day. Cross the
        # full 24-hour theft delay even if the scene itself consumed a few game minutes.
        await link.debug("time", hour=7, minute=59)
        await link.debug("time", hour=9, minute=0)
        await law.tick()

    async def clean() -> bool:
        loading = asyncio.create_task(wait_message(link, Session, lambda m: m.reason == "load", 60))
        await asyncio.sleep(0)
        await link.debug("load", slot + 1)
        await loading
        await link.wait_world()
        await law.tick()
        chosen.clear()
        return check("silence_branch", not facts("crime") and not facts("crime_known"))

    if not isinstance(dialogue.llm, FakeBackend):
        return _logged({"scripted_model": "failed (crime_law requires LLM=fake)"})
    await link.wait_world()
    if link.session is None:
        await wait_message(link, Session, timeout=10)
    talk.reflexes = Reflexes(FixedDecider({"posture": "listen"}))
    rumors.gate, rumors.stage = lambda *_args: False, None
    link.listeners.append(record)
    try:
        await link.debug("player_hp", hp=5000)
        await link.debug("time", hour=8, minute=0)
        humans = {p["npc"] for p in await link.story_people()}
        points = {p["name"]: p for p in await link.waypoints() if not p.get("fp")}
        if not check(
            "city", {CITY_OWNER, ANDRE} <= humans and {THEFT_POINT, HERO_POINT} <= points.keys(),
            {"owner": THEFT_POINT, "hero": HERO_POINT},
        ):
            return _logged(results)
        home = points[THEFT_POINT]
        position = (home["x"], home["y"], home["z"])
        far = [
            p
            for p in points.values()
            if p["name"].startswith("NW_FARM1_")
            and not p.get("room")
            and math.dist((p["x"], p["y"], p["z"]), position) > 10000
        ]
        if not check("parking", bool(far), len(far)):
            return _logged(results)
        parked = min(far, key=lambda p: math.dist((p["x"], p["y"], p["z"]), position))["name"]
        await link.story_effect(CITY_OWNER, [{"do": "end"}])
        reset = [*COUNTERS, "ABSOLUTIONLEVEL_CITY"]
        reset += [
            f"{npc}_{var}"
            for npc in ("ANDRE", "HAGEN")
            for var in ("SCHULDEN", "LASTPETZCOUNTER", "LASTPETZCRIME")
        ]
        await link.story_effect(ANDRE, [{"do": "set", "var": var, "value": 0} for var in reset])
        if await gold() < 100:
            await link.story_effect(ANDRE, [{"do": "give", "item": "ITMI_GOLD", "count": 100}])
        await law.tick()
        if not check("baseline", not facts("crime") and await _save(link, slot + 1) is not None):
            return _logged(results)

        crime = await theft("delayed")
        if crime is None:
            return _logged(results)
        cid = str(crime["id"])
        initial = await values(law_var("city"), *COUNTERS)
        memory = await link.story_eval(CITY_OWNER, [
            ["field", ["inst", "SELF"], "AIVAR", 1, "AIV_NPCSAWPLAYERCOMMIT"],
        ])
        offered = await link.offered_dialogs(ANDRE)
        # A held theft projects personal memory without B_MemorizePlayerCrime's native PETZ write.
        # The law counters stay empty until the delayed report below teaches the community law.
        if not check(
            "locked_before_news",
            not news(cid) and initial == [0, 0, 0, 0, 0] and memory == [3] and PETZMASTER not in offered,
            {"gate_and_native_counters": initial, "owner_memory": memory, "offered": offered},
        ):
            return _logged(results)
        await tomorrow()
        reached = await _until(lambda: bool(news(cid)), 10)
        synced = await values(law_var("city"), *COUNTERS)
        offered = await link.offered_dialogs(ANDRE)
        if not check(
            "news_unlocks",
            reached and synced == [1, 1, 0, 0, 0] and PETZMASTER in offered,
            {"news": news(cid), "gate_and_counters": synced, "offered": offered},
        ):
            return _logged(results)

        await link.debug("skip_important", near=ANDRE)
        await link.debug("player_goto", to=ANDRE, distance=250)
        await link.start_talk(ANDRE)
        payment = service.graph.node(PAYNOW).description if service.graph is not None else ""
        ready = await _until_async(link.dialog_menu, lambda menu: bool(payment) and payment in menu, 90)
        debt = await values("ANDRE_SCHULDEN")
        if not check(
            "native_fine_50",
            ready and debt == [50] and any(m.node == PETZMASTER and not m.made for m in chosen),
            {"debt": debt, "menu": await link.dialog_menu()},
        ):
            return _logged(results)
        before = await gold()
        await link.pick_in_menu(payment)
        paid = await _until(
            lambda: any(
                f.get("community") == "city" and cid in f.get("crimes", ()) for f in facts("absolved")
            ),
            30,
        )
        await link.story_effect(ANDRE, [{"do": "end"}])
        await law.tick()
        after = await gold()
        final = await values(law_var("city"), *COUNTERS, "ANDRE_SCHULDEN")
        check(
            "native_payment",
            paid and after == before - 50 and any(m.node == PAYNOW and not m.made for m in chosen),
            {"gold": [before, after], "absolved": facts("absolved")},
        )
        offered = await link.offered_dialogs(ANDRE)
        check("paid_lock", final == [0, 0, 0, 0, 0, 0] and not {PETZMASTER, PMSCHULDEN} & set(offered), final)
        summary = service.situation(CITY_OWNER) or ""
        check(
            "matteo_remembers",
            service.describe(crime) in summary
            and any(k.crime["id"] == cid and not k.forgiven for k in service.known(CITY_OWNER)),
            summary,
        )

        if not await clean():
            return _logged(results)
        crime = await theft("silence")
        if crime is None:
            return _logged(results)
        cid = str(crime["id"])
        await link.debug("player_goto", to=CITY_OWNER, distance=300)
        if not check("silence_talk_ready", await _until(lambda: CITY_OWNER in talk.people, 15)):
            return _logged(results)
        name = service.names(CITY_OWNER)
        dialogue.llm = ClaimScript(
            said=[
                (
                    "молчи",
                    FakeStep(
                        "Ладно, об этом кубке никому ни слова.",
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
            only=name,
        )
        await link.debug("type", text=f"{name}, молчи об этом кубке. Договорились?")
        if not check("silence_offered", await _until(lambda: CITY_OWNER in deals.open, 30)):
            return _logged(results)
        await asyncio.sleep(1.2)
        pressed = await link.press_deal()
        accepted = await _until(
            lambda: any(f.get("npc") == CITY_OWNER and f.get("crime") == cid for f in facts("silenced")), 15
        )
        if not check("silence_accepted", pressed == "sent" and accepted, pressed):
            return _logged(results)
        await tomorrow()
        await asyncio.sleep(2)
        await law.tick()
        gate = await values(law_var("city"))
        offered = await link.offered_dialogs(ANDRE)
        check(
            "silence_stops_news",
            not news(cid) and gate == [0] and not {PETZMASTER, PMSCHULDEN} & set(offered),
            {"news": news(cid), "gate": gate, "offered": offered},
        )
        return _logged(results)
    finally:
        dialogue.llm, talk.reflexes = original_llm, original_reflexes
        rumors.gate, rumors.stage = original_gate, original_stage
        link.listeners.remove(record)
