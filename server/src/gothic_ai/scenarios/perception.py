from __future__ import annotations

import asyncio
import logging
import math
import re
import time
from pathlib import Path
from typing import Any

from ..brain.reflex import Reflexes
from ..dialogue import DialogueManager
from ..game import GameError, GameLink
from ..knowledge import Knowledge, roots
from ..llm import ToolCall, ToolResult
from ..llm.fake import FakeBackend, FakeStep
from ..lore import Lore
from ..perception import PerceptionDesk, Sight, trace_name
from ..places import SIDES
from ..protocol import DebugCmd, Saved, SayCmd, Session, Snapshot
from ..sightings import Sighting, Sightings
from ..social import Rumors
from ..talk import TalkDesk
from ..trace import read as read_trace
from ..trust import Trust
from ..whereabouts import Whereabouts
from .common import (
    LOBART,
    VINO,
    ClaimScript,
    FixedDecider,
    _ByNpc,
    _logged,
    _OnWords,
    _Recorded,
    _trace_evidence_line,
    _until,
    _until_async,
    attach_when_free,
    prepare_npc_pose,
    wait_message,
)

log = logging.getLogger("gothic_ai.scenarios")


async def ground_scenario(
    link: GameLink,
    dialogue: DialogueManager,
    talk: TalkDesk,
    perception: PerceptionDesk,
    npc: str = "BAU_4300_ADDON_CAVALORN",
    item: str = "ITWR_SATURASFIRSTMESSAGE_ADDON_SEALED",
) -> dict[str, str]:
    """Things lying about (the user, 2026-10-14: he threw Cavalorn's letter down at his feet and said «бери»;
    Cavalorn: «я даже не вижу, куда ты его швырнул»). Cavalorn follows the hero, the hero throws the letter
    down. Checks:
    - sees: it is in Cavalorn's view, with its id;
    - situation: its situation tells it, «На земле: …»;
    - take: told «бери», its model (scripted) calls take_item: it walks up and picks it up;
    - has: the letter is in its inventory and no longer on the ground;
    - follows: it goes with the hero again, and catches up when he is 11 m off."""
    from ..executor import Intent

    await link.wait_world()
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    calls: list[tuple[float, str, str]] = []  # (sent, action, status)
    act = link.act

    async def recorded(who: str, action: Any, /, **kw: Any) -> Any:
        t = time.monotonic()
        res = await act(who, action, **kw)
        if who.upper() == npc:
            calls.append((t, action, res.status))
        return res

    link.act = recorded  # type: ignore[method-assign]
    take = ToolCall("take_item", {"item": item})
    dialogue.llm = _ByNpc(
        {"Кавалорн": _OnWords([("бери", FakeStep("Давай сюда.", [take]))], FakeStep("Идём."))},
        FakeBackend("Хм.", delay=0),
    )

    def dist() -> float:
        return talk.people[npc].distance / 100 if npc in talk.people else -1.0

    def things() -> list[str]:
        view = perception.view(npc)
        return [t.item for t in view.items] if view is not None else []

    def since(t0: float, action: str, status: str = "done") -> bool:
        return any(t >= t0 and a == action and s == status for t, a, s in calls)

    try:
        await link.debug("skip_important", near=npc)  # Cavalorn's own greeting would stop the hero
        for _ in range(2):  # the goblins by his fire would beat the hero down
            try:
                await link.debug("kill", instance="YGOBBO_GREEN", near=npc, by=npc)
            except GameError as err:
                log.info("ground: goblin: %s", err)
        await link.debug("player_goto", to=npc, distance=300)
        if not await _until(lambda: 0 <= dist() <= 15, 20):
            return _logged({"roster": f"failed ({npc} is not around the hero: {dist():.1f} m)"})
        dialogue.open(npc, "Кавалорн").intent = Intent("follow_hero", "PLAYER", "friendly")
        talk.hear("Кавалорн, за мной!", None, {"via": "scenario"})
        if not await _until(lambda: since(0, "follow") and 0 <= dist() <= 5, 20):
            return _logged({"follow": f"failed ({dist():.1f} m)"})
        await asyncio.sleep(2)  # it has stopped by him

        await link.debug("player_drop", item=item)
        sees = await _until(lambda: item in things(), 8)
        check("sees", sees, things() or "nothing on the ground")
        sight = perception.sight(npc)
        ground = sight.ground if sight is not None else None
        check("situation", ground is not None and item in ground, ground or "no line")

        t0 = time.monotonic()
        talk.hear("Кавалорн, вот твоё письмо, бери.", None, {"via": "scenario"})
        took = await _until(lambda: since(t0, "take_item") or since(t0, "take_item", "failed"), 45)
        check("take", since(t0, "take_item"), [a for t, a, s in calls if t >= t0] if took else "no pick-up")
        snap = await link.snapshot(npc, fresh=True)
        has = any(i.item == item for i in snap.npc.inventory)
        await asyncio.sleep(1.5)  # a pushed view after it
        check("has", has and item not in things(), f"in hands: {has}, on the ground: {things()}")

        following = await _until(lambda: since(t0, "follow"), 10)
        await link.debug("player_goto", near=npc, distance=1100)
        await _until(lambda: dist() >= 9, 3)
        back = await _until(lambda: 0 <= dist() <= 5, 20)
        check("follows", following and back, f"{dist():.1f} m")
    finally:
        link.act = act  # type: ignore[method-assign]
    return _logged(results)


async def cave_scenario(
    link: GameLink,
    dialogue: DialogueManager,
    talk: TalkDesk,
    whereabouts: Whereabouts,
    npc: str = "BAU_4300_ADDON_CAVALORN",
) -> dict[str, str]:
    """The map of places (docs/npc-tools.md М3): Cavalorn goes with the hero to the steps below Xardas's
    tower and is told «зайди в пещеру», no place named. Checks:
    - follow: he goes with the hero;
    - listed: among the places around him, the bandits' cave, of the kind «пещера», which way from the hero,
      its way in on the steps;
    - where: `where{пещера}` finds the same cave, its way in and what is next to it;
    - go_to: «Кавалорн, зайди в пещеру.» — he goes into the bandits' cave and waits there.
    The model is scripted with LLM=fake (it is sent to XARDAS_BANDITS on «зайди в пещеру»); a live one
    (LLM=chatgpt) chooses the cave itself from the places around."""
    from ..executor import Intent

    await link.wait_world()
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    def dist() -> float:
        return talk.people[npc].distance / 100 if npc in talk.people else -1.0

    entries: list[dict[str, Any]] = []
    sink = dialogue.journal.sink

    def keep(entry: dict[str, Any]) -> None:
        entries.append(entry)
        if sink is not None:
            sink(entry)

    dialogue.journal.sink = keep
    live = dialogue.llm.name != "fake"
    if not live:
        go = ToolCall("go_to", {"place": "XARDAS_BANDITS"})
        dialogue.llm = _ByNpc(
            {
                "Кавалорн": _OnWords(
                    [("зайди в пещеру", FakeStep("Иду.", [go])), ("пришёл первым", FakeStep("Тут пусто."))],
                    FakeStep("Да."),
                )
            },
            FakeBackend("Хм.", delay=0),
        )
    try:
        await link.debug("player_equip", item="ITAR_PAL_H")  # the bandits by the steps may fall on him
    except GameError as err:
        log.info("cave: no armor for the hero: %s", err)
    await link.debug("skip_important", near=npc)  # Cavalorn's own greeting would stop the hero
    for _ in range(2):  # the goblins by his fire
        try:
            await link.debug("kill", instance="YGOBBO_GREEN", near=npc, by=npc)
        except GameError as err:
            log.info("cave: goblin: %s", err)
    await link.debug("player_goto", to=npc, distance=300)
    if not await _until(lambda: 0 <= dist() <= 15, 20):
        return _logged({"roster": f"failed ({npc} is not around the hero: {dist():.1f} m)"})
    dialogue.open(npc, "Кавалорн").intent = Intent("follow_hero", "PLAYER", "friendly")
    talk.hear("Кавалорн, за мной!", None, {"via": "scenario"})
    following = await _until(
        lambda: talk.orders is not None and talk.orders.companion(npc) and dist() <= 5, 20
    )
    check("follow", following, f"{dist():.1f} m")

    # the hero walks to the steps below the tower; Cavalorn comes after him
    book = whereabouts.book
    steps = book.get("XARDAS_STAIRS") if book is not None else None
    if steps is None or steps.center is None:
        return _logged({**results, "book": "failed (no XARDAS_STAIRS in the book of places)"})
    await link.debug("player_goto", waypoint="NW_XARDAS_STAIRS_02")

    middle = steps.center

    def by_steps(who: Snapshot | None) -> bool:
        return who is not None and math.dist(who.npc.pos, middle) < 1500

    there = await _until_async(lambda: link.snapshot(npc, fresh=True), by_steps, 40)
    await asyncio.sleep(1)
    await link.snapshot(npc, fresh=True)
    log.info("cave: %s by the steps: %s, %.1f m from the hero", npc, there, dist())
    situation = talk.orders.situation(npc) if talk.orders is not None else None
    line = next((x for x in (situation or "").splitlines() if x.startswith("Места рядом")), "")
    cave = next((part for part in line.split("; XARDAS") if "пещера бандитов" in part), "")
    check(
        "listed",
        "пещера бандитов" in cave
        and ": пещера," in cave
        and any(side in cave for side in SIDES.values())
        and "вход — на ступенях" in line,
        line or "no places around",
    )
    answer = whereabouts.where(npc, "пещера")
    check(
        "where",
        answer.startswith("XARDAS_BANDITS — ") and "вход — на ступенях" in answer and "Рядом:" in answer,
        answer.replace("\n", " / "),
    )

    # «зайди в пещеру», no place named
    t0 = len(entries)
    talk.hear("Кавалорн, зайди в пещеру.", None, {"via": "scenario"})

    def sent() -> list[dict[str, Any]]:
        return [e for e in entries[t0:] if e.get("kind") == "action" and e.get("action") == "go_to"]

    went = await _until(sent, 60)
    where = sent()[0].get("arguments", {}).get("place") if went else None
    standing = talk.orders.standing.get(npc) if talk.orders is not None else None

    def place_of(snap: Snapshot) -> str | None:
        # where it stands: its nearest point (`waypoint` is where it walks to)
        place = book.of(snap.npc.nearest_waypoint) if book is not None else None
        return place.id if place is not None else None

    inside = await _until_async(
        lambda: link.snapshot(npc, fresh=True), lambda snap: place_of(snap) == "XARDAS_BANDITS", 120
    )
    snap = await link.snapshot(npc, fresh=True)
    check(
        "go_to",
        where == "XARDAS_BANDITS" and standing is not None and standing.kind == "go" and inside,
        f"go_to {where or 'not called'}, at {snap.npc.nearest_waypoint} ({place_of(snap)})",
    )
    said = [e.get("text", "") for e in entries[t0:] if e.get("kind") == "npc" and e.get("npc") == npc]
    log.info("cave: %s said %s", npc, said)
    return _logged(results)


async def knows_scenario(
    link: GameLink,
    dialogue: DialogueManager,
    talk: TalkDesk,
    knowledge: Knowledge,
    lore: Lore,
    whereabouts: Whereabouts,
    npc: str = "BAU_4300_ADDON_CAVALORN",
) -> dict[str, str]:
    """What an npc knows of where people are (docs/npc-tools.md М4): Cavalorn by his fire knows the bandits
    who robbed him sit in the cave below the steps, though the hero has told him nothing. Checks:
    - known: among his facts the story lets him tell now, one of the bandits' cave (XARDAS_BANDITS);
    - situation: the hero asks «что тут поблизости опасного?»; the fact is in the situation of his turn;
    - where: `where{пещера}` tells it too;
    - said (live model only): his answer speaks of the bandits or the cave.
    The model is scripted with LLM=fake; LLM=chatgpt answers itself."""
    await link.wait_world()
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    entries: list[dict[str, Any]] = []
    sink = dialogue.journal.sink

    def keep(entry: dict[str, Any]) -> None:
        entries.append(entry)
        if sink is not None:
            sink(entry)

    dialogue.journal.sink = keep
    live = dialogue.llm.name != "fake"
    model = _Recorded(dialogue.llm if live else FakeBackend("Держи ухо востро.", delay=0))
    dialogue.llm = model
    await link.debug("skip_important", near=npc)  # Cavalorn's own greeting would stop the hero
    for _ in range(2):  # the goblins by his fire
        try:
            await link.debug("kill", instance="YGOBBO_GREEN", near=npc, by=npc)
        except GameError as err:
            log.info("knows: goblin: %s", err)
    await link.debug("player_goto", to=npc, distance=300)
    if not await _until(lambda: npc in talk.people and talk.people[npc].distance <= 600, 20):
        return _logged({"roster": f"failed ({npc} is not around the hero)"})

    await knowledge.prepare(npc)
    cave = [f for f in lore.known(npc) if f.place == "XARDAS_BANDITS"]
    check("known", bool(cave), cave[0].ru if cave else [f.ru for f in lore.known(npc)])

    t0 = len(entries)
    talk.hear("Кавалорн, что тут поблизости опасного?", npc, {"via": "scenario"})
    answered = await _until(
        lambda: any(e.get("kind") == "npc" and e.get("npc") == npc for e in entries[t0:]), 60
    )
    said = " ".join(e.get("text", "") for e in entries[t0:] if e.get("kind") == "npc" and e.get("npc") == npc)
    shown = bool(cave) and any(cave[0].ru in asked for asked in model.asked)
    check("situation", answered and shown, said or "no answer")
    answer = whereabouts.where(npc, "пещера")
    check("where", "Ты знаешь:" in answer and bool(cave) and cave[0].ru in answer, answer.split("\n")[-1])
    if live:
        words = roots(said)
        check("said", any(w.startswith(("бандит", "пещер", "дыр")) for w in words), said)
    return _logged(results)


async def hearing_scenario(
    link: GameLink,
    talk: TalkDesk,
    npc: str = "BAU_4300_ADDON_CAVALORN",
    bandit: str = "BDT_1013_BANDIT_L",
    brago: str = "BDT_1014_BANDIT_L",
) -> dict[str, str]:
    """Who hears the hero (the user, 2026-10-14: at Cavalorn's fire his words went to a bandit he thought
    was in the cave behind the rock). The hero by Cavalorn's fire. Checks:
    - free: nothing between him and Cavalorn, his voice goes straight on (`heard` = `distance`);
    - line: the bandit by the steps below the tower, 36 m off, is in a straight line from him (the words
      that went to it on 2026-10-14 went by its name, not through the rock);
    - words: «…помочь тебе с бандитами», looking at Cavalorn, are to Cavalorn, not to the bandit;
    - cave: the hero in the bandits' cave by Brago: the bandit by the steps outside does not hear him;
    - round: the hero in the passage to the cave: his voice goes round the bend to Brago, farther than
      straight on."""

    await link.wait_world()
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    def at(who: str) -> str:
        e = talk.people.get(who)
        return f"{e.distance / 100:.1f} m, heard {e.earshot / 100:.1f} m" if e is not None else "not around"

    await link.debug("skip_important", near=npc)  # Cavalorn's own greeting would stop the hero
    for _ in range(2):  # the goblins by his fire
        try:
            await link.debug("kill", instance="YGOBBO_GREEN", near=npc, by=npc)
        except GameError as err:
            log.info("hearing: goblin: %s", err)
    await link.debug("player_goto", to=npc, distance=150)
    near = await _until(
        lambda: npc in talk.people and talk.people[npc].distance <= 500 and bandit in talk.people, 20
    )
    if not near:
        return _logged({"roster": f"failed ({npc}: {at(npc)}, {bandit}: {at(bandit)})"})
    await asyncio.sleep(1.5)  # a roster or two where he stands
    log.info(
        "hearing: %s",
        [
            (e.id, round(e.distance), round(e.earshot))
            for e in sorted(talk.people.values(), key=lambda e: e.distance)
        ],
    )
    me, him = talk.people[npc], talk.people[bandit]
    check("free", me.heard is not None and abs(me.earshot - me.distance) < 1, at(npc))
    check("line", him.heard is not None and abs(him.earshot - him.distance) < 1, at(bandit))
    talk.hear("Можешь отдать за моё обещание помочь тебе с бандитами?", npc, {"via": "scenario"})
    to = talk.last[0] if talk.last is not None else None
    check("words", to is not None and to.id == npc, f"to {to.id if to else None} ({to.why if to else ''})")

    await link.debug("skip_important", near=brago)
    await link.debug("player_goto", to=brago, distance=300)
    inside = await _until(lambda: brago in talk.people and talk.people[brago].distance <= 600, 20)
    await asyncio.sleep(1.5)
    log.info(
        "hearing in the cave: %s",
        [
            (e.id, round(e.distance), round(e.earshot))
            for e in sorted(talk.people.values(), key=lambda e: e.distance)
        ],
    )
    out = talk.people.get(bandit)
    walled = out is not None and out.earshot > out.distance + 100
    check("cave", inside and walled, f"{brago}: {at(brago)}, {bandit}: {at(bandit)}")
    await link.debug("player_goto", waypoint="NW_XARDAS_BANDITS_02")  # in the passage, the cave round a bend
    await asyncio.sleep(1.5)
    log.info(
        "hearing in the passage: %s",
        [
            (e.id, round(e.distance), round(e.earshot))
            for e in sorted(talk.people.values(), key=lambda e: e.distance)
        ],
    )
    e = talk.people.get(brago)
    check("round", e is not None and e.distance + 100 < e.earshot < 20000, at(brago))
    return _logged(results)


async def remembers_scenario(
    link: GameLink,
    dialogue: DialogueManager,
    talk: TalkDesk,
    sightings: Sightings,
    perception: PerceptionDesk,
    npc: str = "BAU_4300_ADDON_CAVALORN",
    steps: str = "BDT_1013_BANDIT_L",
    cave: tuple[str, ...] = ("BDT_1014_BANDIT_L", "BDT_1015_BANDIT_L"),
) -> dict[str, str]:
    """Seen lately (step M2 of docs/npc-tools.md): Cavalorn goes into the bandits' cave first and sees them
    there, then comes back out to the hero by the steps. Out of the cave the hero says «Кавалорн, там в
    пещере бандиты, разберись с ними.»; its model (scripted, LLM=fake) sets it on Brago. Checks:
    - sees_cave: in the cave it sees one of them;
    - remembers: back out, it sees neither, but its situation has them «Недавно видел» with their ids,
      and Brago is a target of its intent;
    - goes_there: set on him, it walks into the cave first;
    - fights: it fights Brago, and he is down."""
    from ..executor import Intent

    await link.wait_world()
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    brago = cave[0]
    calls: list[tuple[float, str, str, dict[str, Any]]] = []  # (sent, action, status, args)
    act = link.act

    async def recorded(who: str, action: Any, /, **kw: Any) -> Any:
        t = time.monotonic()
        res = await act(who, action, **kw)
        if who.upper() == npc:
            calls.append((t, action, res.status, kw))
        return res

    def since(t0: float, action: str) -> list[tuple[float, str, str, dict[str, Any]]]:
        return [c for c in calls if c[0] >= t0 and c[1] == action]

    def dist(who: str = npc) -> float:
        return talk.people[who].distance / 100 if who in talk.people else -1.0

    go = ToolCall("go_to", {"place": "XARDAS_BANDITS"})
    fight = {"goal": "fight", "target": brago, "attitude": "friendly", "agenda": None}
    dialogue.llm = _ByNpc(
        {
            "Кавалорн": _OnWords(
                [
                    ("пещеру первым", FakeStep("Пойду первым.", [go])),
                    ("пришёл первым", FakeStep("Тут двое.")),
                    ("разберись", FakeStep("Сейчас я им покажу!", [ToolCall("set_intent", fight)])),
                ],
                FakeStep("Идём."),
            )
        },
        FakeBackend("Хм.", delay=0),
    )
    link.act = recorded  # type: ignore[method-assign]
    try:
        try:
            await link.debug("player_equip", item="ITAR_PAL_H")
        except GameError as err:
            log.info("remembers: no armor for the hero: %s", err)
        await link.debug("skip_important", near=npc)
        for _ in range(2):  # the goblins by his fire
            try:
                await link.debug("kill", instance="YGOBBO_GREEN", near=npc, by=npc)
            except GameError as err:
                log.info("remembers: goblin: %s", err)
        await link.debug("player_goto", to=npc, distance=300)
        if not await _until(lambda: 0 <= dist() <= 15, 20):
            return _logged({"roster": f"failed ({npc} is not around the hero: {dist():.1f} m)"})
        dialogue.open(npc, "Кавалорн").intent = Intent("follow_hero", "PLAYER", "friendly")
        talk.hear("Кавалорн, за мной!", None, {"via": "scenario"})
        await _until(lambda: 0 <= dist() <= 5, 20)
        await link.debug("player_goto", to=steps, distance=400)
        await _until(lambda: 0 <= dist() <= 6, 30)

        # in: it goes ahead, looks around and sees them
        t0 = time.monotonic()
        talk.hear("Кавалорн, иди в пещеру первым.", None, {"via": "scenario"})
        await _until(lambda: any(c[2] == "done" for c in since(t0, "go_to_waypoint")), 100)
        await _until(lambda: "Тут двое." in [m.content for m in dialogue.conversations[npc].history], 90)
        view = await perception.fresh(npc)
        seen = [o.id for o in view.sees if o.id.upper() in cave] if view is not None else []
        check("sees_cave", bool(seen), seen or "nobody")

        # out: back with the hero, it sees them no more and remembers them
        talk.hear("Кавалорн, за мной!", None, {"via": "scenario"})
        await _until(lambda: 0 <= dist() <= 6, 40)
        await asyncio.sleep(3)
        view = await perception.fresh(npc)
        in_sight = [o.id for o in view.sees if o.id.upper() in cave] if view is not None else []
        situation = sightings.situation(npc) or ""
        sight = perception.sight(npc)
        targets = sight.targets if sight is not None else {}
        noted = (
            situation.split("Недавно видел, сейчас не видишь:")[-1] if "Недавно видел" in situation else ""
        )
        check(
            "remembers",
            not in_sight and any(c in noted for c in cave) and brago in targets,
            noted.strip() or f"in sight {in_sight}; {situation[-200:]}",
        )

        # set on him: it goes in there first, then fights
        t1 = time.monotonic()
        talk.hear("Кавалорн, там в пещере бандиты, разберись с ними.", None, {"via": "scenario"})
        fought = await _until(
            lambda: any(c[3].get("npc", "").upper() == brago for c in since(t1, "fight")), 120
        )
        first_fight = min((c[0] for c in since(t1, "fight")), default=float("inf"))
        walked = [c for c in since(t1, "go_to_waypoint") if c[0] < first_fight]
        check(
            "goes_there",
            fought and bool(walked),
            f"{len(walked)} walk(s) before the fight" if fought else "no fight",
        )
        down = await _until(lambda: brago not in talk.people or talk.people[brago].activity == "down", 90)
        state = (await link.story_state(npcs=[brago])).get("npcs", {}).get(brago, {}).get("state")
        check("fights", fought and (down or state == "dead"), f"Brago {state}, down {down}")
    finally:
        link.act = act  # type: ignore[method-assign]
    return _logged(results)


async def see_perception(
    link: GameLink,
    dialogue: DialogueManager,
    talk: TalkDesk,
    perception: PerceptionDesk,
    rumors: Rumors,
    npc: str = "NONE_100_XARDAS",
    farmer: str = LOBART,
) -> dict[str, str]:
    """What npc's perceive (wave 1 step 4), on the game, with the engine's own sight. Xardas sees the hero in
    front of him and not through the tower's walls and floors. Lobart calls the people he knows by name and
    the hero, whom he does not know, by his look; in militia armor the hero is a militiaman to him. The view
    is in the conversational model's request (scripted here) and `look` answers; the nearest npc's views
    are pushed once a second for the behavioral model."""
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    await link.wait_world()
    if not await _until(lambda: npc in talk.people, 20):
        return _logged({"roster": f"failed ({npc} is not around the hero)"})

    # 1. sight: in front of him he sees the hero; walls and floors hide him even within 20 m
    await link.debug("teleport", near=npc, distance=300)
    await asyncio.sleep(1)
    view = await perception.fresh(npc)
    front = view is not None and view.hero is not None and view.hero.seen
    check(
        "sees_in_front",
        front,
        view.hero.model_dump(include={"distance", "seen"}) if view and view.hero else "",
    )
    spots = []
    for cm in (600, 900, 1200, 1500, 1800):
        try:
            await link.debug("player_goto", near=npc, distance=cm)
        except GameError as err:
            spots.append(f"{cm}: {err}")
            continue
        await asyncio.sleep(1)
        view = await perception.fresh(npc)
        if view is not None and view.hero is not None:
            spots.append((round(view.hero.distance / 100), view.hero.seen))
    hidden = [s for s in spots if isinstance(s, tuple) and not s[1] and s[0] < 20]
    check("not_through_walls", bool(hidden), f"(m, seen) at the spots tried: {spots}")

    # 2. Lobart: whom he knows by name, the hero by his look
    if not await _until(lambda: rumors.social is not None, 30):
        check("acquaintances", False, "no acquaintances: the world's people were not read")
        return _logged(results)
    await link.debug("teleport", near=farmer, distance=300)
    if not await _until(lambda: farmer in talk.people and talk.people[farmer].distance < 800, 20):
        check("farmer", False, f"{farmer} is not around the hero")
        return _logged(results)
    await asyncio.sleep(1)
    await perception.fresh(farmer)
    sight = perception.sight(farmer)
    hero = sight.hero or "" if sight else ""
    check("hero_by_look", "ты его не знаешь" in hero, hero)
    # whom the npc's around call by name, whom by look: over the views pushed near the hero, next to
    # Lobart and then next to the people he knows, until someone sees a person it knows
    named: list[str] = []
    strangers: list[str] = []

    def scan() -> None:
        for v in perception.views():
            people = (perception.sight(v.npc) or Sight("", None, "")).people
            for o in v.sees:
                if o.kind != "human":
                    continue
                if perception.knows(v.npc, o) and o.name in people:
                    named.append(f"{v.name} → {o.name}")
                elif not perception.knows(v.npc, o) and "незнакомец" in people:
                    strangers.append(f"{v.name} → {o.name}")

    scan()
    social = rumors.social
    for tie in sorted(social.ties.get(farmer, {}) if social is not None else [])[:6]:
        if named:
            break
        try:
            await link.debug("teleport", near=tie, distance=300)
        except GameError:
            continue
        await asyncio.sleep(1.5)  # a push or two
        scan()
    check(
        "acquaintances", bool(named), f"by name: {sorted(set(named))}; by look: {sorted(set(strangers))[:6]}"
    )
    await link.debug("teleport", near=farmer, distance=300)
    await asyncio.sleep(1)

    # 3. in militia armor, to Lobart who does not know him, the hero is a militiaman
    await link.debug("player_equip", item="ITAR_MIL_L")
    await asyncio.sleep(1)
    await perception.fresh(farmer)
    sight = perception.sight(farmer)
    hero = sight.hero or "" if sight else ""
    check("fake_guild", "по виду ополченец" in hero, hero)

    # 4. a turn: the view is in the request, `look` answers
    llm = FakeBackend(
        script=[FakeStep("Хм…", [ToolCall("look", {"who": "герой"}, "c1")]), FakeStep("Ополченец, значит?")],
        delay=0,
    )
    dialogue.llm = llm
    await link.debug("type", text="Лобарт, ты меня знаешь?")
    await _until(lambda: len(llm.calls) >= 2, 30)
    request = str(llm.calls[0][1]) if llm.calls else ""
    looked = next((i.output for c in llm.calls[1:] for i in c[1] if isinstance(i, ToolResult)), "")
    check(
        "in_the_turn",
        "по виду ополченец" in request and "Рядом:" not in request and looked.startswith("Герой"),
        looked,
    )

    # 5. the nearest npc's views come by themselves once a second (the behavioral model reads these)
    perception._views.clear()
    pushed = await _until(lambda: perception.view(farmer) is not None, 5)
    check("pushed", pushed, "; ".join(perception.english(farmer)))

    # 6. the dead: a wolf the hero kills in front of Lobart is a wolf to him still, a dead one («о, мёртвый
    # волк»), with its id; `look` at «мёртвый волк» finds it
    await link.debug("spawn", instance="WOLF", near=farmer, distance=600)
    await asyncio.sleep(0.5)
    await link.debug("kill", instance="WOLF", near=farmer)
    await asyncio.sleep(1)
    await perception.fresh(farmer)
    sight = perception.sight(farmer)
    people = sight.people if sight else "(no view)"
    looked = perception.look(farmer, "мёртвый волк")
    check("dead_beast", "туша: Волк" in people and looked.startswith("туша: Волк"), f"{people} | {looked}")

    # 7. places by name (step 5): at Xardas's tower, on Lobart's and Onar's farms, in the city
    if perception.places is None:
        check("places", False, "no book of places (gamedata/places.json)")
        return _logged(results)
    said = []
    for who in (npc, farmer, "BAU_900_ONAR", "VLK_412_HARAD"):
        try:
            await link.debug("teleport", near=who, distance=300)
        except GameError as err:
            said.append(f"{who}: {err}")
            continue
        await asyncio.sleep(1)
        view = await perception.fresh(who)
        place = perception.place(view) if view is not None else None
        sight = perception.sight(who)
        said.append(f"{who}: {sight.place if sight else '(no view)'}" + ("" if place else " [no name]"))
    named = [line for line in said if "[no name]" not in line and "(no view)" not in line and ": " in line]
    check("places", len(named) == 4, " | ".join(said))
    return _logged(results)


TOWNSFOLK = ("MIL_319_PABLO", "VLK_417_CONSTANTINO", "VLK_424_ALWIN")  # in the street by Harad's smithy


async def seen_memory(
    link: GameLink,
    dialogue: DialogueManager,
    talk: TalkDesk,
    sightings: Sightings,
    farmer: str = LOBART,
    smith: str = "VLK_412_HARAD",
    slot: int = 7,
) -> dict[str, str]:
    """What npc's saw (wave 1 step 6), on the game. At eight in the morning the hero stands before Lobart on
    his farm: Lobart keeps the sighting — when, where by name, how the hero looked. The hero goes to the city
    (Harad sees him there), an hour passes, he comes back and tells Lobart he was in the city all morning:
    Lobart saw him on the farm an hour ago and catches the lie (`catch_lie`; the model scripted unless
    LLM=chatgpt) — his trust, wariness, a rumor. The memory follows the save. The sight of check_claim is the
    engine's: what story_state says the townsman sees is what his view sees."""
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    desk = sightings.desk
    chronicle = sightings.chronicle
    await link.wait_world()
    if link.session is None:
        await wait_message(link, Session, timeout=10)

    # 1. the morning on the farm: Lobart sees the hero
    await link.debug("time", hour=8, minute=0)
    await link.debug("teleport", near=farmer, distance=300)
    if not await _until(lambda: farmer in talk.people and talk.people[farmer].distance < 800, 20):
        return _logged({"farmer": f"failed ({farmer} is not around the hero)"})
    await asyncio.sleep(2)
    view = await desk.fresh(farmer)
    hero = view.hero if view is not None else None
    check(
        "time_in_view",
        view is not None and view.time is not None and hero is not None and hero.waypoint is not None,
        f"time {view.time if view else None}, hero at {hero.waypoint if hero else None}",
    )
    seen = await _until(lambda: bool(sightings.hero_trail(farmer)), 5)
    first = sightings.hero_trail(farmer)[0] if seen else None
    check(
        "sighting",
        first is not None and not first.places[0].startswith("у точки"),
        sightings.words(first) if first else "Lobart kept nothing",
    )

    # 2. to the city: someone there turns to the man who came up and sees him; Lobart's sighting is over
    tried, who = [], []
    for townsman in (smith, *TOWNSFOLK):
        try:
            # not stopped for a scripted talk (Pablo's «you are wanted»): it would hold the game from saving
            await link.debug("skip_important", near=townsman)
            await link.debug("teleport", near=townsman, distance=300)
        except GameError as err:
            tried.append(f"{townsman}: {err}")
            continue
        await asyncio.sleep(1)
        await link.npc_act(townsman, "turn_to")
        await _until(lambda: [n for n in sightings.witnesses() if n != farmer.upper()], 4)
        view = await desk.fresh(townsman)
        tried.append(f"{townsman} {view.state if view else '?'}/{view.body if view else '?'}")
        who = [n for n in sightings.witnesses() if n != farmer.upper()]
        if who:
            smith = townsman
            break
    check(
        "elsewhere",
        bool(who) and all(not sightings.hero_trail(n)[0].places[0].startswith("у точки") for n in who),
        "; ".join(f"{n}: {sightings.words(sightings.hero_trail(n)[0])}" for n in who[:3])
        or f"nobody saw him ({', '.join(tried)})",
    )
    # check_claim's sight is the engine's: story_state `seen` agrees with what that one's view sees
    view = await desk.fresh(smith)
    near = [o.id for o in (view.sees if view else []) if o.kind == "human"][:3]
    around = [p["npc"] for p in await link.story_nearby(smith, 2000)]
    unseen = [n for n in around if n not in near and n.upper() != smith.upper()][:3]
    far = "NONE_100_XARDAS"
    asked = [*near, *unseen, far]
    state = await link.story_state(npcs=asked, near=smith)
    said = {n: (state.get("npcs", {}).get(n) or {}).get("seen") for n in asked}
    agree = (
        bool(near)
        and sum(1 for n in near if said[n]) >= len(near) - 1  # one may have moved meanwhile
        and sum(1 for n in unseen if said[n] is False) >= len(unseen) - 1
        and said[far] is False
    )
    check("story_state_sight", agree, f"in the view {near}, not {unseen}: {said}")

    # 3. an hour later, back on the farm
    morning = first.end if first is not None else 0
    hour, minute = divmod(morning % 1440 + 60, 60)
    await link.debug("time", hour=hour, minute=minute)
    await asyncio.sleep(2)  # the pushes in the city carry the new time: Lobart's sighting is over
    await link.debug("teleport", near=farmer, distance=300)
    back = await _until(lambda: len(sightings.hero_trail(farmer)) >= 2, 10)
    # whom he talks to is who is before him now (the roster comes once a second)
    await _until(lambda: farmer in talk.people and talk.people[farmer].distance < 800, 10)
    await asyncio.sleep(1.5)
    trail = sightings.hero_trail(farmer)
    check(
        "back",
        back and trail[0].start - trail[1].end >= 55,
        " | ".join(
            f"{s.start % 1440 // 60:02d}:{s.start % 60:02d}–{s.end % 1440 // 60:02d}:{s.end % 60:02d}"
            for s in trail[:3]
        ),
    )

    # 4. the lie: «I was in the city all morning»
    name = talk.people[farmer].name if farmer in talk.people else "Лобарт"
    await desk.fresh(farmer)
    situation = sightings.situation(farmer) or ""
    sid = next((m.group(1) for m in re.finditer(r"- (В\d): [^\n]*около часа назад", situation)), "В2")
    talk.reflexes = Reflexes(FixedDecider({"posture": "listen"}))
    script = None
    if isinstance(dialogue.llm, FakeBackend):
        catch = FakeStep(
            "Врёшь! Я тебя час назад здесь, на ферме, видел.",
            [ToolCall("catch_lie", {"said": "всё утро был в городе", "seen": sid})],
        )
        dialogue.llm = script = ClaimScript(said=[("город", catch)], told=[], default="Ну?", only=name)
    await link.debug("type", text=f"{name}, я всё утро был в городе, только что оттуда.")
    caught = await _until(lambda: any(f.get("said") for f in chronicle.facts("caught")), 60)
    if script is not None:
        request = str(script.calls[0][1]) if script.calls else ""
        check(
            "in_the_turn",
            "Что ты видел сам" in request and f"{sid}: сегодня" in request and "около часа назад" in request,
            next((line for line in situation.splitlines() if line.startswith(f"- {sid}")), situation[:200]),
        )
    fact = next((f for f in chronicle.facts("caught") if f.get("said")), None)
    conv = dialogue.conversations.get(farmer)
    replies = [e.content for e in conv.memory.entries if e.role == "assistant"][-2:] if conv else []
    check("caught", caught, f"{fact.get('title')!r}: {fact.get('how')}; said {replies}" if fact else replies)
    check(
        "wary", conv is not None and conv.intent.attitude in ("wary", "hostile"), conv.intent if conv else ""
    )
    lied = Trust(chronicle).of(farmer).lied
    check("trust", bool(lied), lied)

    # 5. the memory follows the save
    head = sightings.memory.head(link.session.branch) if link.session else 0
    await asyncio.sleep(0.5)  # the game has the head by now
    for attempt in range(30):
        try:
            saving = asyncio.ensure_future(wait_message(link, Saved, timeout=60))
            await link.debug("save", slot)
            break
        except GameError as err:
            saving.cancel()
            if attempt == 29:
                raise
            log.info("save refused (%s), retrying", err)
            await asyncio.sleep(2)
    saved = await saving
    loaded = asyncio.ensure_future(wait_message(link, Session, lambda m: m.reason == "load"))
    await link.debug("load", slot)
    session = await loaded
    await link.wait_world()
    trail = sightings.hero_trail(farmer)
    check(
        "follows_the_save",
        saved.seq >= head and session.branch != saved.branch and len(trail) >= 2,
        f"{session.branch} from {session.parent}@{session.parent_seq}: {len(trail)} sighting(s) of the hero",
    )

    return _logged(results)


async def trace_memory(
    link: GameLink,
    dialogue: DialogueManager,
    talk: TalkDesk,
    sightings: Sightings,
    log_dir: str,
    farmer: str = LOBART,
) -> dict[str, str]:
    """Native arrows at Lobart's house: an unseen NPC's arrow proves no culprit; a new arrow after
    Lobart saw the hero there contradicts his claim that it is a week old. The bench only shoots and
    waits for the server; views, memory, requests, tools and consequences use the conversation path."""
    results: dict[str, str] = {}
    desk, chronicle = sightings.desk, sightings.chronicle
    directory = Path(log_dir)
    original = dialogue.llm
    control_denial = "это не я стрелял в твою стену, я ничего не трогал, я только пришел"
    age_lie = "стрела эта давно там торчит, с прошлой недели"
    house = "NW_FARM1_INHOUSE_01"
    parked = "NW_FARM1_PATH_CITY_16"
    target = "BAU_951_HILDA"

    def check(name: str, ok: bool, detail: object = "") -> bool:
        results[name] = f"{'done' if ok else 'failed'} ({detail})"
        log.info("trace %-20s %s", name, results[name])
        return ok

    async def mark(name: str) -> None:
        path = directory / "events.jsonl"
        if not await _until(
            lambda: (
                path.exists()
                and any(r.get("kind") == "mark" and r.get("text") == name for r in read_trace(path))
            ),
            30,
        ):
            raise RuntimeError(f"trace bench did not reach {name}")

    async def release(name: str) -> None:
        # The controlled observer's overlay releases waitreply without a model turn or memory entry.
        await link.send(SayCmd(npc=farmer, line=f"AIB_TRACE_{name}", text=".", overlay=True))

    async def move(
        who: str,
        waypoint: str | None = None,
        *,
        near: str | None = None,
        distance: int = 0,
        pos: tuple[float, float, float] | None = None,
        attach: bool = False,
    ) -> None:
        try:
            await link.detach(who)
        except GameError as err:
            if "npc is not attached" not in str(err):
                raise
        if pos is not None:
            hero = (await link.snapshot(who, fresh=True)).player
            assert hero is not None and hero.ahead is not None
            anchor = (pos[0] - 150 * hero.ahead[0], pos[1], pos[2] - 150 * hero.ahead[1])
            await link.debug("player_goto", pos=anchor)
            await asyncio.sleep(1)  # settle the separate anchor before placing the NPC 150 cm ahead
            ready, pose = await prepare_npc_pose(
                link, who, house, to="PC_HERO", distance=150, require_perception_ready=False,
            )
            if not ready:
                raise GameError(f"trace preparation: {pose}")
            await link.debug("player_goto", pos=hero.pos)
        else:
            await link.debug(
                "npc_goto",
                npc=who,
                waypoint=waypoint,
                to="PC_HERO" if near == "PLAYER" else near,
                distance=distance,
                face="PC_HERO",
            )
        if attach:
            await attach_when_free(link, who)

    def turns() -> list[dict[str, Any]]:
        return [
            r
            for p in sorted((directory / "traces").glob("*.jsonl"))
            for r in read_trace(p)
            if r.get("type") == "turn" and r.get("npc") == farmer
        ]

    async def deny(*, observation: Sighting | None = None) -> dict[str, Any]:
        denial = f"Это не я. Да и {age_lie}" if observation is not None else control_denial
        before = {r["id"] for r in turns()}
        await link.debug("teleport", near=farmer, distance=250)
        await _until(lambda: farmer in talk.people and talk.people[farmer].distance < 800, 10)
        await desk.fresh(farmer)
        if isinstance(original, FakeBackend):
            step = FakeStep("Не видел, кто стрелял. Одной стрелы мало, чтобы обвинять тебя.")
            if observation is not None:
                sid, _ = _trace_evidence_line(sightings.situation(farmer) or "", observation)
                step = FakeStep(
                    "Не с прошлой недели! Стрела свежая, и я только что её увидел.",
                    [ToolCall("catch_lie", {"said": age_lie, "seen": sid})],
                )
            dialogue.llm = ClaimScript([(denial, step)], [], "Я запомню.", only="Лобарт")
        await link.debug("type", text=f"Лобарт, {denial}.")
        completed = await _until(
            lambda: any(
                r["id"] not in before and denial in r.get("trigger", {}).get("text", "") for r in turns()
            ),
            90,
        )
        await _until(lambda: not dialogue.speaking(farmer), 30)
        rows = [
            r for r in turns() if r["id"] not in before and denial in r.get("trigger", {}).get("text", "")
        ]
        row = rows[-1] if rows else {}
        log.info(
            "trace %s reply: %s",
            "positive" if observation is not None else "control",
            [s.get("text") for s in row.get("steps", [])],
        )
        check(
            "positive_turn" if observation is not None else "control_turn",
            completed and row.get("status") == "completed",
            row.get("status"),
        )
        return row

    def calls(row: dict[str, Any]) -> list[dict[str, Any]]:
        return [c for s in row.get("steps", []) for c in s.get("calls", []) if c.get("name") == "catch_lie"]

    def request(row: dict[str, Any]) -> str:
        return "\n".join(
            str(i.get("content", ""))
            for s in row.get("steps", [])
            for i in s.get("added", [])
            if i.get("role") == "developer"
        )

    def evidence(trace_id: str) -> tuple[str, str]:
        situation = sightings.situation(farmer) or ""
        sid = next(
            (
                sid
                for sid, s in sightings._shown.get(farmer, {}).items()
                if s.who == f"TRACE:{trace_id}:arrow"
            ),
            "",
        )
        line = next((line for line in situation.splitlines() if sid and line.startswith(f"- {sid}:")), "")
        return sid, line

    def shots(actor: str) -> list[str]:
        return [
            r.get("text", "")
            for r in read_trace(directory / "events.jsonl")
            if r.get("kind") == "note"
            and r.get("text", "").startswith(("arrow shot:", "dice shot:"))
            and f"by {actor}" in r["text"]
        ]

    await link.wait_world()
    await mark("trace_ready")
    talk.reflexes = Reflexes(FixedDecider({"posture": "listen"}))
    for who in (farmer, VINO, target):
        await link.debug("skip_important", near=who)
    await link.debug("player_hp", hp=5000)
    # Changing the clock relocates scripted routines, even for an attached body: set it before staging.
    await link.debug("time", hour=8, minute=0)
    # Lobart is away for both shots. The wall is only the arrow's target, not the observer's hiding place.
    await move(farmer, parked, attach=True)
    await link.debug("player_goto", waypoint=house)
    inside = (await link.snapshot(target, fresh=True)).player
    if inside is None:
        return _logged({"house": "failed (no player position)"})
    points = await link._request(DebugCmd(req=link._next_id("trace-"), cmd="waypoints"), 10)
    waypoints = points.value
    distant = sorted(
        [
            p
            for p in waypoints
            if p["name"].startswith("NW_FARM1_")
            and not p["fp"]
            and math.dist((p["x"], p["y"], p["z"]), inside.pos) > 6000
        ],
        key=lambda p: math.dist((p["x"], p["y"], p["z"]), inside.pos),
    )
    if not distant:
        return _logged({"preparation": "failed (no farm waypoint beyond 60 m)"})
    parked = distant[0]["name"]
    await move(farmer, parked, attach=True)
    # Native ray queries choose a wooden door/wall: stone reflects arrows rather than retaining them.
    candidates = [
        (
            inside.pos[0] + 750 * math.cos(angle * math.pi / 72),
            inside.pos[1],
            inside.pos[2] + 750 * math.sin(angle * math.pi / 72),
        )
        for angle in range(144)
    ]
    rays = [
        (*point[:1], point[1] + height, point[2], inside.pos[0], inside.pos[1] + 140, inside.pos[2], 1)
        for height in (140, 170)
        for point in candidates
    ]
    (directory / "trace-rays.tsv").write_text(
        "".join(" ".join(str(value) for value in ray) + "\n" for ray in rays), encoding="utf-8"
    )
    await release("wall_query")
    await mark("wall_sample")
    surfaces = {
        r["id"]: r
        for r in read_trace(directory / "physics-results.jsonl")
        if r.get("kind") == "ray"
        and r.get("hit")
        and r.get("material") == 3
        and 0.2 <= r.get("fraction", 0) <= 0.9
    }
    # Vob rays may have a zero normal. Keep the shooter at the ray's supported
    # origin, rather than placing him in the hit surface and letting collision
    # recovery move him through the wall. Both nock-height rays must hit wood:
    # an occluded NPC view alone does not guarantee an arrow misses a doorway.
    candidates = [
        point
        for angle, point in enumerate(candidates)
        if angle in surfaces and angle + 144 in surfaces
    ]
    outside = None
    for point in candidates:
        await move(VINO, pos=point, attach=True)
        await link.act(VINO, "turn_to", npc="PLAYER")
        view = await desk.fresh(VINO)
        position = (await link.snapshot(VINO, fresh=True)).npc.pos
        if (
            view is not None
            and view.hero is not None
            and not view.hero.seen
            and view.hero.distance < 1500
            and abs(position[1] - inside.pos[1]) < 180
            and math.dist((position[0], position[2]), (point[0], point[2])) < 20
        ):
            outside = position
            break
    if not check(
        "house_wall", bool(outside), f"inside {house}, outside {outside}; {len(candidates)} candidates"
    ):
        return _logged(results)
    # Settling may change ground height. Check the actual shot corridor again
    # before releasing the archer, at and around the bow's nock height.
    confirmation = [
        (
            outside[0],
            outside[1] + height,
            outside[2],
            inside.pos[0],
            inside.pos[1] + 140,
            inside.pos[2],
            1,
        )
        for height in (140, 155, 170)
    ]
    (directory / "trace-shot-rays.tsv").write_text(
        "".join(" ".join(str(value) for value in ray) + "\n" for ray in confirmation),
        encoding="utf-8",
    )
    await release("shot_query")
    await mark("shot_sample")
    corridor = [
        r for r in read_trace(directory / "physics-results.jsonl") if r.get("kind") == "ray"
    ]
    if not check(
        "wooden_shot_corridor",
        len(corridor) == len(confirmation)
        and all(
            r.get("hit") and r.get("material") == 3 and 0.2 <= r.get("fraction", 0) <= 0.9
            for r in corridor
        ),
        corridor,
    ):
        return _logged(results)
    view = await desk.fresh(farmer)
    if not check(
        "control_unseen",
        view is not None
        and view.hero is not None
        and not view.hero.seen
        and view.hero.distance > 5000
        and not sightings.hero_trail(farmer),
        f"parked {parked}; {view.hero if view else 'no view'}",
    ):
        return _logged(results)
    shooter_view = await desk.fresh(VINO)
    baseline = {t.id for t in shooter_view.traces} if shooter_view else set()
    heard = len(sightings.mind(farmer).heard)
    await release("control_start")
    await mark("control_shot")
    check("npc_shot", bool(shots(VINO)) and not shots("PC_HERO"), shots(VINO))
    check("control_not_heard", len(sightings.mind(farmer).heard) == heard, f"heard {heard}")
    await move(farmer, near=VINO, attach=True)
    await move(VINO, parked)
    await link.act(farmer, "turn_to", npc="PLAYER")
    view = await desk.fresh(farmer)
    control = (
        next((t for t in view.traces if t.kind == "arrow" and t.id not in baseline), None) if view else None
    )
    if not check(
        "control_trace",
        control is not None and not sightings.hero_trail(farmer),
        view.model_dump() if view else "no view",
    ):
        return _logged(results)
    sid, line = evidence(control.id)
    control_observed = sightings.mind(farmer).traces[f"TRACE:{control.id}:arrow"]
    await asyncio.sleep(5)  # the trace predates the hero's first visible arrival by at least a game minute
    row = await deny()
    control_sid, control_line = _trace_evidence_line(request(row), control_observed)
    check(
        "control_in_request",
        bool(control_sid) and "Следы, которые ты видел" in request(row),
        control_line,
    )
    check("control_no_catch", not calls(row) and not chronicle.facts("caught"), calls(row))
    talk.end("gone")

    # A new hour, a witnessed arrival, then Lobart is too far to see the actual shot.
    await mark("positive_ready")
    await link.debug("time", hour=9, minute=0)
    await move(VINO, parked)
    await link.debug("teleport", near=farmer, distance=300)
    await desk.fresh(farmer)
    trail = sightings.hero_trail(farmer)
    hero_start = trail[0].start if trail else -1
    if not check(
        "hero_nearby",
        bool(trail) and trail[0].end % 1440 >= 9 * 60,
        sightings.words(trail[0]) if trail else "no sighting",
    ):
        return _logged(results)
    await move(farmer, parked, attach=True)
    await link.debug("player_goto", pos=outside)
    view = await desk.fresh(farmer)
    check(
        "shot_unseen",
        view is not None and view.hero is not None and not view.hero.seen and view.hero.distance > 2000,
        view.hero if view else "no view",
    )
    baseline = set(sightings.mind(farmer).traces)
    heard = len(sightings.mind(farmer).heard)
    await release("hero_start")
    await mark("hero_shot")
    check("hero_shot", bool(shots("PC_HERO")), shots("PC_HERO"))
    hidden = await desk.fresh(farmer)
    check(
        "act_not_seen",
        hidden is not None
        and hidden.hero is not None
        and not hidden.hero.seen
        and hidden.hero.distance > 2000
        and set(sightings.mind(farmer).traces) == baseline,
        hidden.hero if hidden else "no view",
    )
    check("shot_not_heard", len(sightings.mind(farmer).heard) == heard, f"heard {heard}")
    await move(farmer, near="PLAYER", attach=True)
    await link.debug("player_goto", waypoint=house)
    await link.act(farmer, "turn_to", npc="PLAYER")
    view = await desk.fresh(farmer)
    arrow = (
        next((t for t in view.traces if t.kind == "arrow" and f"TRACE:{t.id}:arrow" not in baseline), None)
        if view
        else None
    )
    if not check(
        "new_trace", arrow is not None and arrow.age is not None, view.model_dump() if view else "no view"
    ):
        return _logged(results)
    sid, line = evidence(arrow.id)
    observed = sightings.mind(farmer).traces[f"TRACE:{arrow.id}:arrow"]
    check(
        "trace_memory",
        bool(sid)
        and bool(line)
        and 0 <= arrow.age <= 5
        and view.time - arrow.age <= observed.start <= view.time,
        f"{line}; appearance {view.time - arrow.age}, age {arrow.age} min",
    )
    row = await deny(observation=observed)
    prompt = request(row)
    actual_sid, actual_line = _trace_evidence_line(prompt, observed)
    fresh_trace = re.search(
        rf"{re.escape(trace_name(observed.trace_kind, observed.name))} "
        rf"\([^)]*{re.escape(observed.places[0])}[^)]*давность [0-5] мин\)",
        prompt,
    )
    # Relative age may change during the turn; the stable time/name/place and the В id must survive.
    check(
        "trace_in_request",
        bool(actual_sid) and "Следы, которые ты видел" in prompt and bool(fresh_trace),
        f"{actual_line}; {fresh_trace.group() if fresh_trace else 'fresh trace absent'}",
    )
    hero_ids = [
        sid
        for sid, s in sightings._shown.get(farmer, {}).items()
        if s.who == "PLAYER" and s.start == hero_start
    ]
    hero_lines = [
        line for line in prompt.splitlines() if any(line.startswith(f"- {sid}:") for sid in hero_ids)
    ]
    check("hero_in_request", any("ферм" in line for line in hero_lines), hero_lines)
    caught_calls = calls(row)
    check(
        "catch_uses_trace",
        any(
            c.get("arguments", {}).get("seen") == actual_sid and c.get("outcome") == "accepted"
            for c in caught_calls
        ),
        caught_calls,
    )
    await _until(lambda: bool(chronicle.facts("caught")), 15)
    caught = chronicle.facts("caught")
    check(
        "caught",
        len(caught) == 1
        and bool(_trace_evidence_line(f"- В1: {caught[0].get('how', '')}", observed)[0])
        and "сам видел:" in caught[0].get("how", ""),
        caught,
    )
    dialogue.llm = original
    await release("done")
    return _logged(results)
