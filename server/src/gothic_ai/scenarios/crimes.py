"""Layer one of crimes: native deeds and witnesses on the save at Lobart's farm."""

from __future__ import annotations

import asyncio
import math
from typing import TYPE_CHECKING, Any

from ..game import GameError, GameLink
from ..protocol import HeroDeed, Session
from .common import HILDA, LOBART, _logged, _save, _until, _until_async, wait_message

if TYPE_CHECKING:
    from ..crimes import Crimes

HOUSE = "NW_FARM1_INHOUSE_BED_02"
POCKET = "DIA_LOBART_PICKPOCKET"
DOIT = f"{POCKET}_DOIT"
PREPARE = "DIR_CRIMES_PREPARE"
CITY_OWNER = "VLK_416_MATTEO"
CITY_HOUSE = "NW_CITY_BED_MATTEO"
ANDRE = "MIL_311_ANDRE"


async def crimes(link: GameLink, service: Crimes, slot: int = 7) -> dict[str, str]:
    """Exercise the actual engine/VM paths; reload the clean save between unrelated crimes.

    Reloading also resets vanilla's own theft memories and combat. No perception is disabled, and no
    ownership is supplied by the scenario: the NPC's native drop, room and container data decide it.
    All saves live in the isolated e2e directory. Slot ``slot + 1`` is the clean parent branch.
    """
    results: dict[str, str] = {}
    deeds: list[HeroDeed] = []

    def check(name: str, ok: bool, detail: object = "") -> bool:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"
        return ok

    def facts() -> list[dict[str, Any]]:
        return service.chronicle.facts("crime")

    def record(msg: object) -> None:
        if isinstance(msg, HeroDeed):
            deeds.append(msg)

    async def load(clean: int) -> None:
        loading = asyncio.create_task(wait_message(link, Session, lambda m: m.reason == "load", 60))
        await asyncio.sleep(0)
        await link.debug("load", clean)
        await loading
        await link.wait_world()
        await asyncio.sleep(0.5)  # finish the new world's first native perception tick
        deeds.clear()

    async def fresh() -> bool:
        await load(slot + 1)
        return check("clean_branch", not facts(), f"{len(facts())} crimes")

    async def stand(npc: str, point: str, *, face: str | None = None) -> None:
        await link.debug("skip_important", near=npc)
        await link.npc_act(
            npc, "start_activity", state="ZS_STAND_WP", waypoint=point, minutes=120, place=True
        )
        if face is not None:
            await link.npc_act(npc, "turn_to", npc=face)

    async def received(kind: str, start: int = 0) -> HeroDeed | None:
        ready = await _until(lambda: any(d.kind == kind for d in deeds[start:]), 10)
        return next((d for d in deeds[start:] if d.kind == kind), None) if ready else None

    async def drop(npc: str, item: str) -> bool:
        try:
            await link.debug("npc_drop", npc=npc, item=item)
        except GameError as err:
            if "could not drop it now" not in str(err):
                raise
            return False  # the old routine's pose may still be blending out
        return True

    async def crime_of(deed: HeroDeed | None) -> dict[str, Any] | None:
        if deed is None:
            return None
        await _until(lambda: any(f.get("id") == deed.id for f in facts()), 5)
        return next((f for f in facts() if f.get("id") == deed.id), None)

    def seen_agrees(fact: dict[str, Any] | None) -> bool:
        if fact is None:
            return False
        seen = {
            f.get("npc") for f in service.chronicle.facts("crime_seen") if f.get("crime") == fact.get("id")
        }
        return seen == set(fact.get("witnesses", ()))

    async def house_witnesses() -> bool:
        # Separate standing places keep Hilda clear of the owner's collision capsule.
        # The house's walk points have a clear ray; BED_02 is among furniture. As in crime_react,
        # move the hero in front of Hilda and prove real sight before the single native take.
        places = {HILDA: "NW_FARM1_INHOUSE_02", LOBART: "NW_FARM1_INHOUSE_01"}
        last: dict[str, Any] = {}

        async def ready() -> bool:
            views = await link.npc_view([HILDA, LOBART])
            last["views"] = [view.model_dump() for view in views]
            hilda = next((view for view in views if view.npc == HILDA), None)
            return (
                hilda is not None
                and hilda.theft_ready is True
                and hilda.state == "ZS_STAND_WP"
                and hilda.body == "stand"
                and hilda.weapon == "none"
                and hilda.nearest_waypoint == places[HILDA]
                and hilda.hero is not None
                and hilda.hero.seen
                and hilda.hero.distance <= 1500
            )

        for distance in (180, 300, 120):
            views = {v.npc: v for v in await link.npc_view([HILDA, LOBART])}
            for npc, point in places.items():
                view = views.get(npc)
                await link.debug("npc_goto", npc=npc, waypoint=point)
                if (
                    view is None
                    or view.state != "ZS_STAND_WP"
                    or view.body != "stand"
                    or view.waypoint != point
                ):
                    await link.npc_act(
                        npc,
                        "start_activity",
                        state="ZS_STAND_WP",
                        waypoint=point,
                        minutes=120,
                        place=False,
                    )

            # Wait for Hilda's actual callback after any native quiet-sound/state transition.
            async def initialized() -> bool:
                current = await link.npc_view([HILDA, LOBART])
                last["views"] = [view.model_dump() for view in current]
                hilda = next((view for view in current if view.npc == HILDA), None)
                return (
                    hilda is not None
                    and hilda.theft_ready is True
                    and hilda.state == "ZS_STAND_WP"
                    and hilda.body == "stand"
                    and hilda.nearest_waypoint == places[HILDA]
                )

            if not await _until_async(initialized, bool, 5):
                continue
            await link.debug("teleport", near=HILDA, distance=distance)
            for npc, point in places.items():
                await link.debug("npc_goto", npc=npc, waypoint=point, face="PLAYER")
            last["hero_in_front_cm"] = distance
            if await _until_async(ready, bool, 2):
                return check("house_witnesses_ready", True, last)
        return check("house_witnesses_ready", False, last)

    await link.wait_world()
    if link.session is None:
        await wait_message(link, Session, timeout=10)
    link.listeners.append(record)
    try:
        await link.debug("player_hp", hp=5000)
        await link.debug("time", hour=8, minute=0)
        people = await link.story_people()
        humans = {p["npc"] for p in people}
        farm = [p["npc"] for p in await link.story_nearby(LOBART, 7000) if p["npc"] in humans]
        for npc in dict.fromkeys([LOBART, HILDA, *farm]):
            await link.debug("skip_important", near=npc)
        await link.story_effect(LOBART, [{"do": "end"}])
        points = {p["name"]: p for p in await link.waypoints() if not p.get("fp")}
        if not check(
            "farm",
            {HOUSE, "NW_FARM1_INHOUSE_01", "NW_FARM1_INHOUSE_02"} <= points.keys()
            and HILDA in humans
            and LOBART in humans,
            HOUSE,
        ):
            return _logged(results)
        house = points[HOUSE]
        house_pos = (house["x"], house["y"], house["z"])
        far = [
            p
            for p in points.values()
            if p["name"].startswith("NW_FARM1_")
            and not p.get("room")
            and math.dist((p["x"], p["y"], p["z"]), house_pos) > 8000
        ]
        fields = [
            p
            for p in points.values()
            if p["name"].startswith("NW_FARM1_") and "FIELD" in p["name"] and not p.get("room")
        ]
        if not check("positions", bool(far) and bool(fields), f"{len(far)} parking, {len(fields)} field"):
            return _logged(results)
        parked = min(far, key=lambda p: math.dist((p["x"], p["y"], p["z"]), house_pos))["name"]
        field = min(fields, key=lambda p: math.dist((p["x"], p["y"], p["z"]), house_pos))["name"]
        await asyncio.sleep(0.5)
        if not check("baseline", not facts() and await _save(link, slot + 1) is not None, len(facts())):
            return _logged(results)

        # Lobart and Hilda are OUT in this save: the household theft has no vanilla community or
        # lawkeeper. Hilda stays free and receives ASSESSTHEFT normally. The native drop owns the cup;
        # the room's ownership is reported as it is, without assigning it a farmer guild.
        await link.debug("player_goto", waypoint="NW_FARM1_INHOUSE_01")
        await link.debug("npc_goto", npc=HILDA, waypoint=parked)
        await stand(LOBART, HOUSE)
        await asyncio.sleep(1)
        views = await link.npc_view([LOBART])
        room_detail = f"indoors={views[0].indoors}, room_guild={views[0].room_guild}" if views else "no view"
        if not check(
            "house_prepared",
            await _until_async(lambda: drop(LOBART, "ITMI_SILVERCUP"), bool, 10),
            f"native Lobart drop; {room_detail}",
        ):
            return _logged(results)
        if not await house_witnesses():
            return _logged(results)
        await link.debug("player_take", item="ITMI_SILVERCUP", distance=1200)
        deed = await received("take_item")
        fact = await crime_of(deed)
        if not check(
            "house_theft",
            deed is not None
            and fact is not None
            and fact.get("kind") == "theft"
            and deed.owner == LOBART
            and fact.get("community") is None
            and not service.lawkeepers(fact.get("community"))
            and HILDA in fact.get("witnesses", ())
            and HILDA in deed.witnesses
            and seen_agrees(fact),
            {"crime": fact, "deed": deed.model_dump() if deed else "no hero_deed"},
        ):
            return _logged(results)

        # Matteo's actual house is just inside the south gate, the nearest lower-city house on the
        # road from Lobart. Its real VLK owner supplies the community; no guild or room is rewritten.
        if not await fresh():
            return _logged(results)
        if not check("city_house", CITY_OWNER in humans and CITY_HOUSE in points, CITY_HOUSE):
            return _logged(results)
        await stand(CITY_OWNER, CITY_HOUSE)
        await link.debug("player_goto", waypoint=CITY_HOUSE)
        await asyncio.sleep(1)
        views = await link.npc_view([CITY_OWNER])
        identity = await link.story_eval(
            CITY_OWNER,
            [["field", ["inst", "SELF"], "GUILD"], ["field", ["inst", "SELF"], "NPCTYPE"]],
        )
        if not check(
            "city_house_prepared",
            bool(views)
            and views[0].indoors
            and len(identity) == 2
            and service.community(*identity) == "city"
            and await _until_async(lambda: drop(CITY_OWNER, "ITMI_SILVERCUP"), bool, 10),
            f"{CITY_HOUSE}, guild/npctype={identity}, indoors={views[0].indoors if views else None}",
        ):
            return _logged(results)
        await asyncio.sleep(1)
        await link.debug("player_take", item="ITMI_SILVERCUP", distance=800)
        deed = await received("take_item")
        fact = await crime_of(deed)
        if not check(
            "city_theft",
            deed is not None
            and deed.owner == CITY_OWNER
            and fact is not None
            and fact.get("kind") == "theft"
            and fact.get("owner") == CITY_OWNER
            and fact.get("community") == "city"
            and bool(fact.get("witnesses"))
            and set(fact.get("witnesses", ())) == set(deed.witnesses)
            and ANDRE in service.lawkeepers(fact.get("community"))
            and seen_agrees(fact),
            {"crime": fact, "deed": deed.model_dump() if deed else "no hero_deed"},
        ):
            return _logged(results)

        # ChestDesk's "home" association is only a way to locate a real chest. It never supplies its
        # owner to Crimes: the emitted deed must carry the world's own container or room ownership.
        if not await fresh():
            return _logged(results)
        chests = [c for c in await link.npc_chests(LOBART) if c.items and c.distance < 4000]
        chests.sort(key=lambda c: (c.why != "owner", c.why != "key", c.distance))
        if not check("chest_prepared", bool(chests), [(c.id, c.why) for c in chests]):
            return _logged(results)
        chest = chests[0]
        await link.debug("player_goto", pos=(chest.pos[0] + 100, chest.pos[1], chest.pos[2]))
        nearby = await link.story_nearby("PC_HERO", 7000)
        for person in nearby:
            await stand(person["npc"], parked)
        await asyncio.sleep(1)
        nearby = await link.story_nearby("PC_HERO", 3500)
        if not check("chest_alone", not nearby, [p["npc"] for p in nearby]):
            return _logged(results)
        item = next((i for i in chest.items if i.item == "ITMI_GOLD"), chest.items[0])
        await link.debug("chest_take", chest=chest.id, item=item.item, count=1)
        deed = await received("take_item")
        if not check(
            "chest_ownership",
            deed is not None
            and bool(deed.chest_owner or deed.room_owner or deed.chest_guild >= 0 or deed.room_guild >= 0),
            deed.model_dump() if deed else "no hero_deed",
        ):
            return _logged(results)
        fact = await crime_of(deed)
        if not check(
            "chest_theft",
            fact is not None
            and fact.get("kind") == "theft"
            and fact.get("item") == item.item
            and fact.get("count") == 1
            and not fact.get("witnesses")
            and deed is not None
            and not deed.witnesses
            and seen_agrees(fact),
            fact,
        ):
            return _logged(results)

        if not await fresh():
            return _logged(results)
        await link.debug("player_goto", waypoint=field)
        await asyncio.sleep(0.5)
        await link.debug("spawn_item", item="ITFO_APPLE", distance=100)
        await link.debug("player_take", item="ITFO_APPLE", distance=400)
        deed = await received("take_item")
        await asyncio.sleep(0.5)
        if not check(
            "public_item",
            deed is not None and not deed.owner and not deed.room and not facts(),
            deed.model_dump() if deed else "no hero_deed",
        ):
            return _logged(results)

        if not await fresh():
            return _logged(results)
        await stand(LOBART, field)
        await stand(HILDA, field)
        await link.debug("teleport", near=LOBART, distance=200)
        await asyncio.sleep(1)
        await link.debug("player_hit", npc=LOBART)
        deed = await received("attack")
        fact = await crime_of(deed)
        if not check(
            "attack",
            deed is not None
            and not deed.victim_in_fight
            and fact is not None
            and fact.get("kind") == "attack"
            and fact.get("victim") == LOBART
            and bool(fact.get("witnesses"))
            and seen_agrees(fact),
            fact or (deed.model_dump() if deed else "no hero_deed"),
        ):
            return _logged(results)

        if not await fresh():
            return _logged(results)
        await link.debug("teleport", near=LOBART, distance=300)
        await link.debug("kill", instance="SHEEP", near=LOBART, by="PLAYER")
        deed = await received("murder")
        fact = await crime_of(deed)
        if not check(
            "sheep",
            deed is not None
            and deed.victim_is_sheep
            and fact is not None
            and fact.get("kind") == "sheep"
            and fact.get("victim") == deed.victim
            and seen_agrees(fact),
            fact or (deed.model_dump() if deed else "no hero_deed"),
        ):
            return _logged(results)

        # Only preparation is authored: a talent, dexterity and the victim's one-use flag. The parent
        # C_Beklauen(65, 80) and B_Beklauen behind DOIT remain the scripts' original programs.
        for success, dexterity in ((True, 100), (False, 55)):
            if not await fresh():
                return _logged(results)
            await link.debug("teleport", near=LOBART, distance=250)
            await link.patch_graph(
                [
                    {
                        "op": "add_node",
                        "node": {
                            "id": PREPARE,
                            "npc": LOBART,
                            "description": "Подготовить проверку карманной кражи",
                            "program": [
                                {
                                    "do": "set",
                                    "target": ["field", ["inst", "OTHER"], "ATTRIBUTE", 5],
                                    "value": ["int", dexterity],
                                },
                                {
                                    "do": "set",
                                    "target": ["field", ["inst", "SELF"], "AIVAR", 6],
                                    "value": ["int", 0],
                                },
                                {
                                    "do": "call",
                                    "call": [
                                        "call",
                                        "NPC_SETTALENTSKILL",
                                        ["inst", "OTHER"],
                                        ["int", 12],
                                        ["int", 1],
                                    ],
                                },
                            ],
                        },
                    }
                ]
            )
            await link.choose_dialog(LOBART, PREPARE)
            offered = await link.offered_dialogs(LOBART)
            label = "pickpocket_success" if success else "pickpocket_failure"
            if not check(f"{label}_prepared", POCKET in offered, f"dexterity {dexterity}, {offered}"):
                return _logged(results)
            parent = service.graph.node(POCKET).description if service.graph else ""
            choice = service.graph.node(DOIT).description if service.graph else ""
            await link.start_talk(LOBART)
            menu = await _until_async(link.dialog_menu, lambda m, text=parent: bool(text) and text in m, 30)
            if not check(f"{label}_menu", menu, parent):
                return _logged(results)
            await link.pick_in_menu(parent)
            choices = await _until_async(
                link.dialog_menu, lambda m, text=choice: bool(text) and text in m, 30
            )
            if not check(f"{label}_choice", choices, choice):
                return _logged(results)
            await link.pick_in_menu(choice)
            deed = await received("pickpocket")
            fact = await crime_of(deed)
            before = {i.item: i.count for i in deed.inventory_before} if deed else {}
            after = {i.item: i.count for i in deed.inventory_after} if deed else {}
            gained = after.get("ITMI_GOLD", 0) - before.get("ITMI_GOLD", 0)
            expected = (
                gained == 80 and not fact.get("witnesses")
                if success and fact is not None
                else gained == 0 and fact is not None and LOBART in fact.get("witnesses", ())
            )
            if not check(
                label,
                deed is not None
                and fact is not None
                and fact.get("kind") == "theft"
                and fact.get("victim") == LOBART
                and expected
                and seen_agrees(fact),
                f"gold +{gained}, {fact or (deed.model_dump() if deed else 'no hero_deed')}",
            ):
                return _logged(results)
            await link.story_effect(LOBART, [{"do": "end"}, {"do": "calm", "attitude": "ATT_FRIENDLY"}])

        kept = [dict(f) for f in service.chronicle.facts() if f.get("fact") in {"crime", "crime_seen"}]
        await asyncio.sleep(0.5)  # let the game's saved memory head catch up with the latest fact
        saved = await _save(link, slot)
        if not check("saved", saved is not None and bool(kept), len(kept)):
            return _logged(results)
        await load(slot)
        restored = [f for f in service.chronicle.facts() if f.get("fact") in {"crime", "crime_seen"}]
        if not check("after_load", restored == kept, f"{len(kept)} -> {len(restored)} facts"):
            return _logged(results)
        await load(slot + 1)
        check(
            "new_branch",
            not facts() and not service.chronicle.facts("crime_seen"),
            f"{len(facts())} crimes, {len(service.chronicle.facts('crime_seen'))} witnesses",
        )
        return _logged(results)
    finally:
        link.listeners.remove(record)
