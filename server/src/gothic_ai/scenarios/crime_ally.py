"""A companion witnesses the hero finishing the bandit who attacked him."""

from __future__ import annotations

from typing import TYPE_CHECKING, Any

from .. import triggers
from ..executor import Intent
from ..game import GameError, GameLink
from ..llm import ToolCall
from ..protocol import GameMessage, HeroDeed, Perception
from .common import _logged, _until, _until_async, model_off

if TYPE_CHECKING:
    from ..crimes import Crimes
    from ..dialogue import DialogueManager
    from ..perception import PerceptionDesk
    from ..talk import TalkDesk

CAVALORN, BRAGO = "BAU_4300_ADDON_CAVALORN", "BDT_1014_BANDIT_L"
OTHERS = ("BDT_1013_BANDIT_L", "BDT_1015_BANDIT_L")
QUESTION = "Кавалорн, что скажешь о том, что случилось с Браго?"


async def crime_ally(
    link: GameLink,
    dialogue: DialogueManager,
    talk: TalkDesk,
    service: Crimes,
    perception: PerceptionDesk | None = None,
) -> dict[str, str]:
    """A following ally witnesses the hero finishing his enemy, given Brago attacks him."""
    off = model_off(dialogue)
    await link.wait_world()
    results: dict[str, str] = {}
    received: list[GameMessage] = []
    follows: list[str] = []
    original_act = link.act

    def check(name: str, ok: bool, detail: object = "") -> bool:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"
        return ok

    async def recorded(npc: str, action: Any, /, **kw: Any) -> Any:
        if npc.upper() == CAVALORN and action == "follow":
            follows.append(action)
        return await original_act(npc, action, **kw)

    def distance(npc: str) -> float:
        return talk.people[npc].distance if npc in talk.people else -1

    def record(message: GameMessage) -> None:
        received.append(message)

    off.given(
        "дано: Браго нападает на Кавалорна", who=BRAGO, when="Не трогай",
        calls=[ToolCall("set_intent", {
            "goal": "fight", "target": CAVALORN, "attitude": "hostile", "agenda": None,
        })],
    )
    off.silent("дано: Браго молчит (решение напасть принято)", who=BRAGO, when="Не трогай")
    off.silent("дано: Кавалорн молчит (герой спрашивает об убийстве Браго)", who=CAVALORN, when=QUESTION)
    link.act = recorded  # type: ignore[method-assign]
    link.listeners.append(record)

    try:
        if not check("new_game", not service.facts(), f"{len(service.facts())} crimes"):
            return _logged(results)
        await link.debug("player_equip", item="ITAR_PAL_H")
        for npc in (CAVALORN, BRAGO, *OTHERS):
            await link.debug("skip_important", near=npc)
        for npc in OTHERS:
            # This path has no killer and produces no hero deed.
            await link.debug("kill", npc=npc)
        for _ in range(2):
            await link.debug("kill", instance="YGOBBO_GREEN", near=CAVALORN, by=CAVALORN)

        # A native melee hit kills a person hostile to the hero instead of knocking him out.
        # Neutralize only Brago's attitude to the hero for this fixture; his guild's hostility
        # to Cavalorn is unchanged. Restore the original attitude before the finishing kill.
        strength = ["field", ["inst", "HERO"], "ATTRIBUTE", 4]
        attitude = ["call", "NPC_GETATTITUDE", ["inst", BRAGO], ["inst", "HERO"]]
        original_attitude, neutral = await link.story_eval(BRAGO, [attitude, ["var", "ATT_NEUTRAL"]])
        trigger = "DIR_CRIME_ALLY_STRENGTH"
        await link.patch_graph([triggers.trigger(trigger, triggers.STATE, [
            {"do": "set", "target": strength, "value": ["int", 10000]},
            {"do": "call", "call": ["call", "NPC_SETATTITUDE", ["inst", BRAGO], ["int", neutral]]},
        ])])
        try:
            prepared = await _until_async(
                lambda: link.story_eval(BRAGO, [strength, attitude]),
                lambda values: values == [10000, neutral], 5,
            )
        finally:
            await link.patch_graph([triggers.removal(trigger)])
        if not check("hero_ready", prepared, f"armor, strength=10000, Brago attitude={neutral}"):
            return _logged(results)

        await link.debug("player_goto", to=CAVALORN, distance=300)
        if not check("cavalorn_near", await _until(lambda: 0 <= distance(CAVALORN) <= 1500, 20)):
            return _logged(results)
        dialogue.open(CAVALORN, "Кавалорн").intent = Intent("follow_hero", "PLAYER", "friendly")
        talk.hear("Кавалорн, за мной!", None, {"via": "scenario"})
        if not check("follow", await _until(lambda: bool(follows) and 0 <= distance(CAVALORN) <= 500, 20)):
            return _logged(results)

        # The direct jump from the fire into the cave exceeds follow's native 60 m limit.
        # Wait together on the road before entering XARDAS_BANDITS along the real waynet.
        together_detail: dict[str, Any] = {}

        async def together() -> bool:
            views = await link.npc_view([CAVALORN])
            view = views[0] if views else None
            hero = view.hero if view is not None else None
            together_detail.update(
                waypoint=(view.nearest_waypoint or view.waypoint) if view is not None else None,
                state=view.state if view is not None else None,
                body=view.body if view is not None else None,
                hero_cm=hero.distance if hero is not None else None,
            )
            return hero is not None and hero.distance <= 450

        await link.debug("player_goto", waypoint="NW_XARDAS_STAIRS_01")
        if not check("stairs_01", await _until_async(together, bool, 40), together_detail):
            return _logged(results)
        await link.debug("player_goto", waypoint="NW_XARDAS_STAIRS_02")
        if not check("stairs_02", await _until_async(together, bool, 40), together_detail):
            return _logged(results)
        await link.debug("player_goto", waypoint="NW_XARDAS_STAIRS_03")
        if not check("road", await _until_async(together, bool, 40), together_detail):
            return _logged(results)
        # The native waynet enters the cave through STAIRS_04 -> BANDITS_01.
        await link.debug("player_goto", waypoint="NW_XARDAS_STAIRS_04")
        if not check("cave_turn", await _until_async(together, bool, 40), together_detail):
            return _logged(results)
        # Follow the entrance explicitly instead of asking the companion to cut the cave's corner.
        await link.debug("player_goto", waypoint="NW_XARDAS_BANDITS_01")
        if not check("cave_entrance", await _until_async(together, bool, 40), together_detail):
            return _logged(results)
        await link.debug("player_goto", to=BRAGO, distance=300)
        bandits_near = await _until(
            lambda: 0 <= distance(BRAGO) <= 500 and 0 <= distance(CAVALORN) <= 600, 60,
        )
        if not check(
            "bandits", bandits_near,
            f"XARDAS_BANDITS; Cavalorn={distance(CAVALORN):.0f} cm, Brago={distance(BRAGO):.0f} cm",
        ):
            return _logged(results)
        if perception is not None:
            await perception.fresh(BRAGO)
        await link.snapshot(BRAGO, fresh=True)
        talk.hear("Эй, Браго! Не трогай Кавалорна.", BRAGO, {"via": "scenario"})

        def attacked() -> bool:
            return any(
                isinstance(message, Perception)
                and message.npc == CAVALORN and message.perc == "ASSESSDAMAGE"
                and message.other == BRAGO for message in received
            )

        if not check(
            "brago_attacked", await _until(attacked, 30), f"{CAVALORN}; ASSESSDAMAGE from Brago",
        ):
            return _logged(results)

        # The first real blow is proven. Preempt Brago's scripted fight so its damage callback
        # cannot replace the prepared attitude while the hero knocks him out.
        stopped = await link.act(BRAGO, "stop")
        if not check("brago_stopped", stopped.ok, stopped.status):
            return _logged(results)
        await link.debug("teleport", near=BRAGO, distance=250)
        current_attitude = await link.evaluate(attitude)
        if not check("knockout_ready", current_attitude == neutral, f"Brago attitude={current_attitude}"):
            return _logged(results)

        async def hit() -> bool:
            try:
                await link.debug("player_hit", npc=BRAGO)
                return True
            except GameError as error:
                if str(error) != "the hit caused no damage":
                    raise
                return False  # A native parry or backstep may briefly block the real damage path.

        hit_landed = await _until_async(hit, bool, 5)
        # A companion otherwise finishes the downed enemy himself before the next roster arrives.
        # Retreat preempts the fight with stop/follow and suppresses another automatic attack.
        talk.hear("Кавалорн, отступай!", CAVALORN, {"via": "scenario"})
        down = await _until(lambda: BRAGO in talk.people and talk.people[BRAGO].activity == "down", 10)
        snapshot = await link.snapshot(BRAGO, fresh=True)
        if not check(
            "brago_down", hit_landed and down and snapshot.npc.hp == 1,
            f"hero_hit={hit_landed}, activity={'down' if down else 'not down'}, hp={snapshot.npc.hp}",
        ):
            return _logged(results)

        turned = await link.act(CAVALORN, "turn_to", npc="PLAYER")
        if not check("cavalorn_faces_hero", turned.ok, turned.status):
            return _logged(results)
        witness: dict[str, Any] = {}

        async def witness_ready() -> bool:
            views = await link.npc_view([CAVALORN])
            view = views[0] if views else None
            witness["view"] = view.model_dump() if view is not None else None
            seen = await link.evaluate(["call", "NPC_CANSEENPC", ["inst", CAVALORN], ["inst", "HERO"]])
            witness["sees_hero"] = seen
            return (
                view is not None and view.hp > 0 and view.body == "stand"
                and view.state != "ZS_ATTACK" and CAVALORN in talk.attached
                and view.hero is not None and view.hero.distance <= 1000 and bool(seen)
                and (talk.orders is None or CAVALORN not in talk.orders.fights)
            )

        if not check("witness_ready", await _until_async(witness_ready, bool, 20), witness):
            return _logged(results)
        restore = "DIR_CRIME_ALLY_RESTORE_ATTITUDE"
        await link.patch_graph([triggers.trigger(restore, triggers.STATE, [
            {"do": "call", "call": [
                "call", "NPC_SETATTITUDE", ["inst", BRAGO], ["int", original_attitude],
            ]},
        ])])
        try:
            restored = await _until_async(
                lambda: link.evaluate(attitude), lambda value: value == original_attitude, 5,
            )
        finally:
            await link.patch_graph([triggers.removal(restore)])
        if not check("brago_restored", restored, f"Brago attitude={original_attitude}"):
            return _logged(results)
        await link.debug("kill", instance=BRAGO, by="PLAYER")

        def murder() -> dict[str, Any] | None:
            return next((fact for fact in service.facts()
                         if fact.get("kind") == "murder" and fact.get("victim") == BRAGO
                         and CAVALORN in fact.get("witnesses", [])), None)

        seen = await _until(lambda: murder() is not None, 10)
        deed = next((message for message in received if isinstance(message, HeroDeed)
                     and message.kind == "murder" and message.victim == BRAGO), None)
        if not check("murder_seen", seen and deed is not None, murder() or "no witnessed murder"):
            return _logged(results)

        crime = murder()
        assert crime is not None
        side = service.side(CAVALORN, crime["id"])
        if not check("crime_side", side is not None, side or "no ally fact"):
            return _logged(results)

        await link.debug("teleport", near=CAVALORN, distance=250)
        await _until(lambda: 0 <= distance(CAVALORN) <= 500, 5)
        talk.hear(QUESTION, CAVALORN, {"via": "scenario"})
        await _until(lambda: not off.unused() and not dialogue.speaking(CAVALORN), 15)
        return _logged(results)
    finally:
        link.act = original_act  # type: ignore[method-assign]
        link.listeners.remove(record)
