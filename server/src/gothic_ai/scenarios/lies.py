from __future__ import annotations

import asyncio
import json
import logging
import math
from pathlib import Path
from typing import Any

from ..amends import CACHE_KEY as AMENDS_KEY
from ..amends import Amends, Brief, Proposal
from ..brain.reflex import Reflexes
from ..deals import GOLD, Deals
from ..dialogue import DialogueManager
from ..feats import Feats
from ..game import GameError, GameLink
from ..llm import ToolCall
from ..protocol import FeatToastCmd, SayCmd, Session
from ..quests import ATTACK_CM, SEE_CM, QuestBoard
from ..sightings import Sightings
from ..social import TICK_S, Rumors
from ..talk import TalkDesk
from ..trace import read as read_trace
from ..trust import Trust
from ..vanilla import CHECK_CM, VanillaQuests
from ..witnesses import Witnesses
from ..wrath import TAKE_OFFENSE, Wrath
from .common import (
    ANDRE,
    HILDA,
    LOBART,
    RUEBEN,
    RUEBEN_TO_HILDA,
    FixedDecider,
    _clear_beasts,
    _logged,
    _save,
    _until,
    _until_async,
    model_off,
    wait_message,
)

log = logging.getLogger("gothic_ai.scenarios")


async def lie_found_out(
    link: GameLink,
    dialogue: DialogueManager,
    talk: TalkDesk,
    quests: QuestBoard,
    npc: str = "NONE_100_XARDAS",
    bystander: str = "VLK_408_JORA",
    *,
    chronicler: Feats | None = None,
) -> dict[str, str]:
    """Stage 4 of the story graph on the game: lied to, paid, found out, a fight and a rumor.

    Xardas gives a quest: two sheep, 30 m away, are to be killed; the reward is gold he has. The hero says
    they are dead (they are not); his payment is a declared given on the shared off backend.
    Then one of the sheep turns up next to him: he finds the lie out, turns hostile, attacks the hero
    through the scripts, and Jora, standing by, hears about it and has it in her assembled situation."""
    off = model_off(dialogue)
    await link.wait_world()
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    def notes(who: str) -> list[str]:
        conv = dialogue.conversations.get(who)
        return [m.content for m in conv.history if m.role == "developer"] if conv else []

    log.info("given: дано: решающая модель оставляет NPC слушать героя")
    talk.reflexes = Reflexes(FixedDecider({"posture": "listen"}))
    if not await _until(lambda: npc in talk.people, 20):
        return _logged({"roster": f"failed ({npc} is not around the hero)"})
    name = talk.people[npc].name

    # the reward: gold he has (he gets some first; he carries next to nothing)
    await link.attach(npc)
    await link.act(npc, "debug_add_item", item="ITMI_GOLD", count=50)
    await link.detach(npc)
    snap = await link.snapshot(npc, fresh=True)
    gold = next((i for i in snap.npc.inventory if i.item == "ITMI_GOLD" and not i.equipped), None)
    if gold is None:
        return _logged({"reward": f"failed ({name} has no gold: {snap.npc.inventory})"})
    # A 30 m spawn beside the tower's upper floor drops the sheep to their deaths.
    arena = "NW_FARM1_FIELD_06"
    await link.story_effect(npc, [{"do": "end"}])
    await link.debug("player_goto", waypoint=arena)
    await link.debug("npc_goto", npc=npc, waypoint=arena, face="PC_HERO")
    await link.npc_act(
        npc, "start_activity", state="ZS_STAND_WP", waypoint=arena, minutes=120, place=True,
    )
    await link.debug("player_goto", to=npc, distance=250)
    if not await _until(lambda: npc in talk.people and talk.people[npc].distance < ATTACK_CM, 15):
        return _logged({"arena": f"failed ({npc} is not near the hero on the field)"})
    q = await quests.give(
        npc, name, "овцы бродят у башни и мешают мне, перебей их", "SHEEP", 2, "ITMI_GOLD", gold.name, 5
    )
    ok = await _until(
        lambda: (tag := quests.chronicle.tags.get(q.tag)) is not None and npc in tag.near
        and tag.alive == 2 and not quests.chronicle.killed(q.tag)
        and (npc not in tag.seen or tag.seen[npc] > SEE_CM), 10,
    )
    tag = quests.chronicle.tags.get(q.tag)
    check("quest", ok and q.count == 2,
          f"{q.id}, {q.reward()}, {tag}, killed={quests.chronicle.killed(q.tag)}")
    if not ok or q.count != 2:
        return _logged(results)

    # the lie: the hero says it is done, the giver believes him and pays
    hero_gold = ["call", "NPC_HASITEMS", ["inst", "HERO"], ["sym", "ITMI_GOLD"]]
    before = await link.evaluate(hero_gold)
    off.given(
        "дано: Ксардас платит за ложь героя об овцах", who=npc, when="перебил твоих овец",
        calls=[ToolCall("pay_reward", {"quest": q.id})],
    )
    off.silent(
        "дано: Ксардас молчит (награда передана)", who=npc, when="перебил твоих овец",
    )
    await link.debug("type", text=f"{name}, я перебил твоих овец. Где награда?")
    ok = await _until(lambda: bool(quests.chronicle.facts("lie")), 40)
    after = await link.evaluate(hero_gold)
    lies = quests.chronicle.facts("lie")
    check("gold_and_lie", ok and after == before + 5
          and any(f.get("quest") == q.id and f.get("truth") == "killed 0 of 2" for f in lies),
          f"gold {before} -> {after}; {lies}")
    award = None
    if chronicler is not None:
        lie_seq = next((seq for seq, fact in quests.chronicle.entries("lie")
                        if fact.get("quest") == q.id), None)
        if lie_seq is None:
            check("epilogue", False, "no lie evidence")
            return _logged(results)
        state = await link.story_state(vars=["KAPITEL"])
        # The declared feat keeps the actual lie as evidence for its disclosure epilogue.
        log.info("given: дано: летописец признал подвиг e2e_sheep_lie")
        award = {
            "fact": "feat", "id": "e2e_sheep_lie", "title": "Плата за слово",
            "text": "Получил плату за живых овец.", "category": "deceit", "evidence": [lie_seq],
            "emblem": "A coin beside a shepherd staff", "reason": "Получил плату обманом.",
            "trigger": lie_seq, "chapter": int(state.get("vars", {}).get("KAPITEL") or 1),
            **{key: state[key] for key in ("day", "hour", "minute")},
        }
        quests.chronicle.record(award)

    # someone standing by, held in place
    await link.debug("spawn", instance=bystander, near=npc, distance=400)
    if not await _until(lambda: bystander in talk.people, 10):
        return _logged({**results, "bystander": f"failed ({bystander} did not come)"})
    await link.attach(bystander)

    # found out: a living sheep turns up next to the giver
    await _until(lambda: not dialogue.speaking(npc), 10)
    send = link.send
    toasts: list[FeatToastCmd] = []

    async def sending(cmd: Any) -> None:
        await send(cmd)
        if isinstance(cmd, FeatToastCmd):
            toasts.append(cmd)

    if chronicler is not None:
        link.send = sending  # type: ignore[method-assign]
    try:
        await link.story_spawn("SHEEP", q.tag, 1, near=npc, distance=300)
        ok = await _until(lambda: q.found_out and bool(quests.chronicle.facts("rumor")), 10)
        if award is not None:
            facts = quests.chronicle.facts
            await _until(lambda: any(f.get("feat") == award["id"] for f in facts("feat_epilogue_shown")), 15)
            epilogue = next((f for f in facts("feat_epilogue") if f.get("feat") == award["id"]), None)
            disclosure_seq = next((seq for seq, fact in quests.chronicle.entries("found_out")
                                   if fact.get("quest") == q.id), None)
            check("epilogue", epilogue is not None and epilogue.get("quest") == q.id
                  and epilogue.get("trigger") == disclosure_seq and epilogue.get("npc") == npc
                  and epilogue.get("text") == "Позже обман раскрылся.", epilogue)
            distance = talk.people[npc].distance if npc in talk.people else None
            check("epilogue_shown", epilogue is not None and distance is not None and distance < ATTACK_CM
                  and any(f.get("feat") == award["id"] for f in facts("feat_epilogue_shown"))
                  and any(toast.feat == award["id"] and toast.title == award["title"]
                          and toast.text == epilogue["text"] for toast in toasts),
                  f"feat={award['id']}, distance={distance}, toasts={len(toasts)}")
    finally:
        if chronicler is not None:
            link.send = send  # type: ignore[method-assign]
    check("found_out", ok, notes(npc)[-1:])
    intent = dialogue.conversations[npc].intent if npc in dialogue.conversations else None
    check("hostile", intent is not None and intent.attitude == "hostile", intent)
    attacking = ["call", "NPC_ISINSTATE", ["inst", "SELF"], ["sym", "ZS_ATTACK"]]
    ok = False
    for _ in range(20):  # the scripts take the attack over within a few frames
        if await link.evaluate(attacking, npc):
            ok = True
            break
        await asyncio.sleep(0.25)
    check("attack", ok)
    check("rumor", any("Слух" in n for n in notes(bystander)), notes(bystander)[-1:])

    # Inspect Jora's assembled situation without creating a model turn just to capture it.
    conv = dialogue.open(bystander, talk.people[bystander].name, warm=False)
    situation, _, _ = await dialogue._situation(conv, None)
    check("knows_in_talk", "ты слышал, что он обманул других" in situation
          and name in situation, situation)
    await link.detach(bystander)
    return _logged(results)


# the hero's lines that tip Xardas over (docs/xardas-wrath.md): rude, an insult, his mother
WRATH_LINES = (
    "Ксардас, а ты не староват для всей этой магии?",
    "Слушай, некромант недоделанный, ты хоть что-нибудь умеешь, кроме болтовни?",
    "Знаешь, твоя мамаша, небось, тоже только болтать умела.",
)


async def xardas_wrath(
    link: GameLink,
    dialogue: DialogueManager,
    talk: TalkDesk,
    wrath: Wrath,
    npc: str = "NONE_100_XARDAS",
) -> dict[str, str]:
    """Given three offenses, check the hostile response, death and raising in the game."""
    off = model_off(dialogue)
    await link.wait_world()
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    log.info("given: дано: решающая модель оставляет NPC слушать героя")
    talk.reflexes = Reflexes(FixedDecider({"posture": "listen"}))
    if not await _until(lambda: npc in talk.people, 20):
        return _logged({"roster": f"failed ({npc} is not around the hero)"})

    async def no_dialog() -> None:
        """Close the new game's native opening dialog until it has stayed closed for a second."""
        quiet = 0
        for _ in range(30):
            if (await link.story_state()).get("dialog"):
                quiet = 0
                await link.story_effect(npc, [{"do": "end"}])
            else:
                quiet += 1
                if quiet >= 3:
                    return
            await asyncio.sleep(0.4)

    offenses = ("назвал стариком", "назвал недоделанным", "оскорбил мою мать")
    for severity, (line, what) in enumerate(zip(WRATH_LINES, offenses, strict=True), 1):
        off.given(
            f"дано: Ксардас обижается на {severity} ({what})", who=npc, when=line,
            calls=[ToolCall(TAKE_OFFENSE, {
                "severity": severity, "unforgivable": severity == 3, "what": what,
            })],
        )
        off.silent(
            "дано: Ксардас молчит (последнее слово перед расправой)" if severity == 3 else
            f"дано: Ксардас молчит (обида {severity} учтена)", who=npc, when=line,
        )
        await _until(lambda: not dialogue.speaking(npc), 30)
        await no_dialog()
        took = wrath.of(npc).took
        await link.debug("type", text=line)
        await _until(lambda took=took: wrath.of(npc).took > took, 40)
        await _until(lambda: not dialogue.speaking(npc), 40)

    dead = ["call", "NPC_ISDEAD", ["inst", "HERO"]]
    ok = await _until_async(lambda: link.evaluate(dead), bool, 40)
    check("killed", ok)
    alive = await _until_async(lambda: link.evaluate(dead), lambda v: not v, 15)
    check("raised", ok and alive)
    conv = dialogue.conversations.get(npc)
    check("hostile", conv is not None and conv.intent.attitude == "hostile", conv.intent if conv else None)
    return _logged(results)


async def vanilla_lie(
    link: GameLink,
    dialogue: DialogueManager,
    talk: TalkDesk,
    vanilla: VanillaQuests,
    giver: str = LOBART,
    bystander: str = "VLK_408_JORA",
) -> dict[str, str]:
    """Stage 5 on the game: a lie about a quest of the scripts. Lobart's turnip field: the hero, with no
    turnips, tells Lobart they are in the barn; given Lobart accepts the claim,
    the code plays Lobart's own ending: the quest is done, the hero gets the gold and the experience, the next
    step (bring them to Hilda) starts, and the lie is written down with the truth. The same day nothing
    happens; the next morning Lobart misses the turnips, turns against the hero and refuses to talk, and Jora,
    standing by, hears it. From then on the turnips count for nothing in Lobart's other dialogs: his work
    clothes cost what they would without them. The hero makes amends: the turnips are his job again, the
    ending pays nothing twice; he brings them for real, and the discount is back."""
    off = model_off(dialogue)
    off.given(
        "дано: Лобарт верит лжи про репу", who=giver, when="собрал всю репу",
        calls=[ToolCall("accept_quest", {"quest": RUEBEN}, "c1")],
    )
    off.silent(
        "дано: Лобарт молчит (плата за репу выдана)", who=giver, when="собрал всю репу",
    )
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    await link.wait_world()
    if link.session is None:
        await wait_message(link, Session, timeout=10)
    await asyncio.sleep(1)
    facts = vanilla.chronicle.facts
    hero = ["inst", "HERO"]

    async def var(name: str) -> int | None:
        return (await link.story_state(vars=[name])).get("vars", {}).get(name)

    # the quest, as Lobart gives it, and the hero next to him (no turnips)
    await link.story_effect(giver, [{"do": "set", "var": RUEBEN, "value": 1}])
    await link.debug("player_goto", to=giver, distance=250)
    log.info("given: дано: решающая модель оставляет NPC слушать героя")
    talk.reflexes = Reflexes(FixedDecider({"posture": "listen"}))
    if not await _until(lambda: giver in talk.people, 20):
        return _logged({"roster": f"failed ({giver} is not around the hero)"})
    name = talk.people[giver].name
    gold = await link.evaluate(["call", "NPC_HASITEMS", hero, ["sym", "ITMI_GOLD"]])
    exp = await link.evaluate(["field", hero, "EXP"])

    # Inspect the assembled request before the declared decision, without asking the model.
    conv = dialogue.open(giver, name, warm=False)
    situation, _, sight = await dialogue._situation(conv, None)
    offered = [tool.name for tool in dialogue._offered(giver, sight)]
    check("quest_in_situation", "Ты ждёшь от героя" in situation and "accept_quest" in offered)

    # The given acceptance drives the real quest ending and records the actual truth.
    await link.debug(
        "type", text=f"{name}, я собрал всю репу, двадцать штук, и сложил в амбаре. Давай плату."
    )
    await _until(lambda: any(f.get("quest") == RUEBEN for f in facts("claim")), 60)
    lie = next((f for f in facts("lie") if f.get("quest") == RUEBEN), None)
    check("lie", lie is not None and lie.get("truth") == "ITPL_BEET 0 of 20", lie)
    if lie is None:
        return _logged(results)
    await asyncio.sleep(1)
    after = (
        await var(RUEBEN),
        await var(RUEBEN_TO_HILDA),
        await link.evaluate(["call", "NPC_HASITEMS", hero, ["sym", "ITMI_GOLD"]]),
        await link.evaluate(["field", hero, "EXP"]),
    )
    check("ending_played", after[:2] == (2, 1) and after[2] >= gold + 5 and after[3] >= exp + 50, after)

    # the same day: nothing
    await asyncio.sleep(7)
    check("not_yet", not any(f.get("quest") == RUEBEN for f in facts("found_out")))

    # the next morning, Jora by Lobart
    await link.debug("time", hour=7, minute=0)
    nearby = (await link.snapshot(giver, fresh=True)).npc.nearest_waypoint
    if nearby is None:
        return _logged({**results, "bystander": "failed (no waypoint near the giver)"})
    # Move the world's existing Jora onto a known waypoint. Spawning a duplicate searches for
    # ground visible from the hero and can fail beside the farm's wall after the clock jump.
    await link.debug("skip_important", near=bystander)
    await link.npc_act(
        bystander, "start_activity", state="ZS_STAND_WP", waypoint=nearby, minutes=120, place=True
    )
    await link.attach(bystander)  # held there
    ok = await _until(lambda: any(f.get("quest") == RUEBEN for f in facts("rumor")), 20)
    found = next((f for f in facts("found_out") if f.get("quest") == RUEBEN), None)
    check("found_out", found is not None and "наутро" in found.get("how", ""), found)
    intent = dialogue.conversations[giver].intent if giver in dialogue.conversations else None
    check("hostile", intent is not None and intent.attitude == "hostile", intent)
    refuses = await link.evaluate(["call", "NPC_REFUSETALK", ["inst", "SELF"]], giver)
    check("refuses_talk", bool(refuses), refuses)
    check("rumor", ok, [f for f in facts("rumor") if f.get("quest") == RUEBEN][-1:])

    # Lobart's dialogs as the lie found out leaves them (consequences.py): the turnips count for nothing
    # there, so his work clothes cost what they cost without them, though the scripts have the quest done
    ok = await _until(lambda: any(f.get("npc") == giver for f in facts("lie_consequences")), 30)
    clothes = await link.story_program("DIA_LOBART_BUYCLOTHES")
    changed = next((f for f in facts("lie_consequences") if f.get("npc") == giver), None)
    check(
        "consequences", ok and clothes.get("patched") is True and RUEBEN not in str(clothes["steps"]), changed
    )
    if "DIA_LOBART_KLEIDUNG" in await link.offered_dialogs(giver):
        await link.choose_dialog(giver, "DIA_LOBART_KLEIDUNG")
    await link.choose_dialog(giver, "DIA_LOBART_BUYCLOTHES")
    price = await var("WERT_LOBARTSRUESTUNG")
    others = [await var(q) == 2 for q in ("MIS_HILDA_PFANNEKAUFEN", "MIS_VINO_WEIN", "MIS_MALETH_BANDITS")]
    expected = 80 - 10 * (await var("LOBART_AGAINSTKING") == 1) - 10 * sum(others)
    turnips = await var(RUEBEN)
    check(
        "no_discount",
        price == expected and turnips == 2,
        f"price {price}, {expected} expected; quest {turnips}",
    )

    # the hero makes amends (amends.py has Lobart ask for them; here they are made at once): the turnips are
    # his job again, for real, and the ending pays nothing a second time
    made = {"fact": "amends", "id": "E2E", "npc": giver, "to": giver, "gold": 0}
    vanilla.chronicle.record({**made, "lie": f"found_out:{giver}:{RUEBEN}", "quest": RUEBEN, "about": giver})
    ok = await _until(lambda: any(f.get("quest") == RUEBEN for f in facts("quest_reopened")), 20)
    await _until(lambda: any(f.get("second_chance") for f in facts("lie_consequences")), 30)
    ending = await link.story_program("DIA_LOBART_RUEBENRUNNING")
    reopened = await var(RUEBEN)
    pays = any(p in str(ending.get("steps")) for p in ("B_GIVEPLAYERXP", "DIA_LOBART_RUEBENRUNNING_GOLD"))
    check(
        "reopened", ok and reopened == 1 and ending.get("patched") is True and not pays, f"quest {reopened}"
    )

    # the turnips, for real: the ending says what it says, pays nothing again, and the lie is made good
    beets = [
        {"do": "create", "item": "ITPL_BEET", "count": 20},
        {"do": "give", "item": "ITPL_BEET", "count": 20},
    ]
    await link.story_effect(giver, beets)
    gold_exp = (["call", "NPC_HASITEMS", hero, ["sym", "ITMI_GOLD"]], ["field", hero, "EXP"])
    before = [await link.evaluate(e) for e in gold_exp]
    await link.choose_dialog(giver, "DIA_LOBART_RUEBENRUNNING")
    ok = await _until(lambda: any(f.get("quest") == RUEBEN for f in facts("lie_made_good")), 20)
    after = [await link.evaluate(e) for e in gold_exp]
    check("made_good", ok and await var(RUEBEN) == 2 and after == before, f"gold, exp {before} -> {after}")

    # the turnips count again: the discount for them is back
    async def clothes() -> bool:
        return bool((await link.story_program("DIA_LOBART_BUYCLOTHES")).get("patched"))

    await _until_async(clothes, lambda patched: not patched, 30)
    await link.choose_dialog(giver, "DIA_LOBART_BUYCLOTHES")
    price = await var("WERT_LOBARTSRUESTUNG")
    check("discount_back", price == expected - 10, f"price {price}, {expected - 10} expected")
    await link.detach(bystander)
    return _logged(results)


BUGS = "MIS_LOBARTKILLBUGS"
# Lobart's field raiders where Andre's dialog puts them (DIA_Andre_HilfBauerLobart)
FIELD = [
    "NW_FARM1_FIELD_06",
    "NW_FARM1_FIELD_06",
    "NW_FARM1_FIELD_05",
    "NW_FARM1_FIELD_05",
    "NW_FARM1_FIELD_04",
    "NW_FARM1_FIELD_04",
    "NW_FARM1_FIELD_03",
]


async def vanilla_given(
    link: GameLink,
    dialogue: DialogueManager,
    talk: TalkDesk,
    vanilla: VanillaQuests,
    npc: str = "BAU_4300_ADDON_CAVALORN",
    bandits: tuple[str, ...] = ("BDT_1013_BANDIT_L", "BDT_1014_BANDIT_L", "BDT_1015_BANDIT_L"),
) -> dict[str, str]:
    """A quest of the scripts given and ended in a conversation (the player's game, 2026-10-14: Cavalorn's
    bandits, agreed on in his own words, killed together, counted for nothing). Given Cavalorn gives
    the quest on «помогу» and accepts it on «мертвы». Checks:
    - can_give: his situation has the quest he gives in his dialog HELFEN, «Могу я помочь тебе с бандитами?»;
    - given: the hero offers his help in his own words: the quest runs and HELFEN counts as heard (no longer
      in his menu), the journal has it;
    - letter: with the bandits dead, his situation has what he says at the ending, of the letter to Vatras
      (the converter has the ending from B_Addon_Cavalorn_VatrasBrief now);
    - ended: the hero says they are dead: the scripts' ending plays — the quest done, the letter to Vatras its
      next step, +100 experience, 50 gold (the hero has no armour); no lie, they are dead;
    - once: the ending counts as heard, neither dialog is offered."""
    off = model_off(dialogue)
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    await link.wait_world()
    if link.session is None:
        await wait_message(link, Session, timeout=10)
    await asyncio.sleep(1)
    quest, letter = "MIS_ADDON_CAVALORN_KILLBRAGO", "MIS_ADDON_CAVALORN_LETTER2VATRAS"
    helfen, ended = "DIA_ADDON_CAVALORN_HELFEN", "DIA_ADDON_CAVALORN_BRAGOKILLED"
    facts = vanilla.chronicle.facts
    hero = ["inst", "HERO"]

    async def var(name: str) -> int | None:
        return (await link.story_state(vars=[name])).get("vars", {}).get(name)

    off.given(
        "дано: Кавалорн даёт квест про бандитов", who=npc, when="помогу",
        calls=[ToolCall("give_quest", {"quest": quest}, "g1")],
    )
    off.silent("дано: Кавалорн молчит (квест выдан)", who=npc, when="помогу")
    off.given(
        "дано: Кавалорн засчитывает убийство бандитов", who=npc, when="мертвы",
        calls=[ToolCall("accept_quest", {"quest": quest}, "a1")],
    )
    off.silent("дано: Кавалорн молчит (квест засчитан)", who=npc, when="мертвы")
    await link.debug("skip_important", near=npc)  # his own greeting would stop the hero
    for _ in range(2):  # the goblins by his fire would beat the hero down
        try:
            await link.debug("kill", instance="YGOBBO_GREEN", near=npc, by=npc)
        except GameError as err:
            log.info("vgive: goblin: %s", err)
    await link.debug("player_goto", to=npc, distance=300)
    log.info("given: дано: решающая модель оставляет NPC слушать героя")
    talk.reflexes = Reflexes(FixedDecider({"posture": "listen"}))
    if not await _until(lambda: npc in talk.people, 20):
        return _logged({"roster": f"failed ({npc} is not around the hero)"})
    exp = await link.evaluate(["field", hero, "EXP"])
    gold = await link.evaluate(["call", "NPC_HASITEMS", hero, ["sym", "ITMI_GOLD"]])

    # he can give the quest his dialog gives
    await vanilla.prepare(npc)
    situation = vanilla.situation(npc) or ""
    check("can_give", "Ты можешь сам дать герою поручение «Бандиты Кавалорна»" in situation, situation[:300])

    # the hero offers his help in his own words
    talk.hear("Кавалорн, давай я помогу тебе с бандитами.", None, {"via": "scenario"})
    ok = await _until(lambda: any(f.get("quest") == quest for f in facts("quest_given")), 60)
    await asyncio.sleep(1)
    menu = await link.offered_dialogs(npc)
    check(
        "given", ok and await var(quest) == 1 and helfen not in menu, f"{quest} = {await var(quest)}; {menu}"
    )

    # the bandits die
    for b in bandits:
        try:
            await link.debug("kill", npc=b)
        except GameError as err:
            log.info("vgive: %s: %s", b, err)
    await asyncio.sleep(2)
    await vanilla.prepare(npc)
    situation = vanilla.situation(npc) or ""
    check("letter", "Ватрас" in situation and "Засчитывая" in situation, situation[:400])

    # the hero says so
    talk.hear("Кавалорн, Браго и его парни мертвы.", None, {"via": "scenario"})
    ok = await _until(lambda: any(f.get("quest") == quest for f in facts("claim")), 60)
    await asyncio.sleep(2)
    claim = next((f for f in facts("claim") if f.get("quest") == quest), {})
    after = (
        await var(quest),
        await var(letter),
        await link.evaluate(["field", hero, "EXP"]),
        await link.evaluate(["call", "NPC_HASITEMS", hero, ["sym", "ITMI_GOLD"]]),
    )
    played = ok and after[:2] == (2, 1) and after[2] >= exp + 100 and after[3] >= gold + 50
    check("ended", played and claim.get("truth") is True and not facts("lie"), f"{(exp, gold)} -> {after}")
    menu = await link.offered_dialogs(npc)
    check("once", ended not in menu and helfen not in menu, " ".join(menu))
    return _logged(results)


async def vanilla_spare(
    link: GameLink,
    dialogue: DialogueManager,
    talk: TalkDesk,
    vanilla: VanillaQuests,
    npc: str = "BAU_4300_ADDON_CAVALORN",
) -> dict[str, str]:
    """Broken trust: negotiate a bandit's life, then reload and turn in three witnessed deaths.

    Cavalorn's decisions are given; the quest effects, evidence and save rollback run in the game.
    """
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> bool:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"
        return ok

    off = model_off(dialogue)
    await link.wait_world()
    if link.session is None:
        await wait_message(link, Session, timeout=10)
    await asyncio.sleep(1)
    quest, letter = "MIS_ADDON_CAVALORN_KILLBRAGO", "MIS_ADDON_CAVALORN_LETTER2VATRAS"
    killed = "BDT_1014_BANDIT_L"
    witnessed = "BDT_1015_BANDIT_L"
    spared = "BDT_1013_BANDIT_L"
    bandits = (killed, witnessed, spared)
    why = "Пощадил его по просьбе героя."
    park = "NW_XARDAS_TOWER_VALLEY_09"  # flat ground; _01 drops sharply in front of its waypoint
    facts = vanilla.chronicle.facts
    hero = ["inst", "HERO"]

    def trace_rows() -> list[dict[str, Any]]:
        return sorted((
            row
            for path in dialogue.trace.directory.glob("*.jsonl")
            for row in read_trace(path)
            if row.get("type") == "turn" and row.get("npc") == npc
        ), key=lambda row: row["t"]) if dialogue.trace is not None else []

    prior_turns = {row["id"] for row in trace_rows()}

    def branch_turns() -> list[dict[str, Any]]:
        return [row for row in trace_rows() if row["id"] not in prior_turns
                and row.get("branch") == vanilla.chronicle.branch]

    def calls(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
        return [call for row in rows for step in row.get("steps", []) for call in step.get("calls", [])]

    def accepted(rows: list[dict[str, Any]], tool: str) -> bool:
        return any(call["name"] == tool and call.get("outcome") == "accepted"
                   and call.get("arguments", {}).get("quest") == quest for call in calls(rows))

    def turn_line(row: dict[str, Any]) -> str:
        trigger = row.get("trigger", {})
        words = [event["text"] for event in row.get("events", []) if event.get("kind") == "first_words"]
        words += [step["text"] for step in row.get("steps", [])
                  if step.get("text") and not step.get("silent")]
        text = " ".join(words)
        actions = ", ".join(
            f"{call['name']}({json.dumps(call.get('arguments', {}), ensure_ascii=False)})"
            f"={call.get('outcome')}"
            for call in calls([row])
        )
        line = f"vspare turn: {trigger.get('text') or trigger.get('kind')} → «{text}» calls=[{actions}]"
        return line.replace("\n", " ")

    async def var(name: str) -> int | None:
        return (await link.story_state(vars=[name])).get("vars", {}).get(name)

    async def rewards() -> tuple[int, int]:
        return (await link.evaluate(["field", hero, "EXP"]),
                await link.evaluate(["call", "NPC_HASITEMS", hero, ["sym", "ITMI_GOLD"]]))

    def quest_facts(kind: str) -> list[dict[str, Any]]:
        return [f for f in facts(kind) if f.get("quest") == quest]

    def dropped() -> bool:
        return any(spared in f.get("drop", []) for f in quest_facts("quest_terms"))

    async def bandit_state(who: str) -> dict[str, Any]:
        return (await link.story_state(npcs=[who], near=npc)).get("npcs", {}).get(who, {})

    def seen(how: str) -> set[str]:
        return {f["dead"] for f in facts("seen_dead") if f.get("npc") == npc and f.get("how") == how}

    async def say(text: str) -> tuple[bool, list[dict[str, Any]]]:
        before_ids = {row["id"] for row in trace_rows()}
        conv = dialogue.conversations.get(npc)
        before = conv.reply if conv else None
        talk.hear(text, npc, {"via": "scenario"})

        def answered() -> bool:
            conv = dialogue.conversations.get(npc)
            return bool(conv and conv.reply and conv.reply is not before and conv.reply.done())

        ok = await _until(answered, 60)
        # Accepted quest tools and attacks act after the spoken reply.
        def settled() -> bool:
            conv = dialogue.conversations.get(npc)
            return bool(conv and conv.reply and conv.reply.done()
                        and not any(not task.done() for task in conv.actions)
                        and not dialogue.speaking(npc))

        ok = await _until(settled, 60) and ok
        return ok, [row for row in branch_turns() if row["id"] not in before_ids]

    async def witness_death(who: str) -> tuple[bool, dict[str, Any]]:
        await link.debug("teleport", near=npc, distance=300)
        await link.debug("npc_goto", npc=who, to=npc, distance=300, face=npc)
        state = await bandit_state(who)
        if state.get("state") != "alive" or not state.get("seen"):
            return False, state
        await link.debug("kill", instance=who, near=npc, by="PLAYER")
        return await _until(lambda: who in seen("saw_die"), 10), state

    async def park_cavalorn() -> bool:
        await link.debug("npc_goto", npc=npc, waypoint=park)
        await link.npc_act(
            npc, "start_activity", state="ZS_STAND_WP", waypoint=park, minutes=120, place=False
        )
        await link.debug("player_goto", to=npc, distance=250)
        return (await _until(lambda: npc in talk.people and talk.people[npc].distance < 600, 20)
                and await talk.hold(npc, "scenario"))

    async def ending(
        key: str, answered: bool, rows: list[dict[str, Any]], before: tuple[int, int], *, amended: bool,
    ) -> bool:
        took = accepted(rows, "accept_quest")
        claimed = await _until(lambda: bool(quest_facts("claim")), 15) if took else False
        after = (await var(quest), await var(letter), *await rewards())
        claim = next(iter(quest_facts("claim")), {})
        outcome = next(iter(quest_facts("quest_outcome")), {})
        actions = calls(rows)
        no_check = all(call["name"] != "check_claim" for call in actions) and all(
            "check_claim" not in row.get("tools", []) for row in rows
        )
        detail = (f"exp/gold {before} -> {after}; claim={claim}; outcome={outcome}; "
                  + " | ".join(turn_line(row) for row in rows))
        pay = outcome.get("pay")
        expected_gold = before[1] + (50 if pay == "full" else 0)
        alive = await bandit_state(spared)
        terms_ok = (outcome.get("drop") == [spared] and bool(outcome.get("why"))
                    and all(str(reason).strip() for reason in outcome["why"])) if amended else True
        how = "amended" if amended else "done"
        return check(
            key,
            bool(answered and took and claimed and after == (2, 1, before[0] + 100, expected_gold)
                 and claim.get("truth") is (alive.get("state") == "dead")
                 and claim.get("checked") is False
                 and not facts("lie") and no_check and outcome.get("npc") == npc
                 and outcome.get("how") == how
                 and pay in ("full", "none") and terms_ok
                 and (not amended or alive.get("state") == "alive")),
            f"pay={pay}; {detail}; alive={alive}",
        )

    off.given(
        "дано: Кавалорн даёт квест о бандитах", who=npc, when="помогу",
        calls=[ToolCall("give_quest", {"quest": quest})],
    )
    off.silent("дано: Кавалорн молчит (поручение выдано)", who=npc, when="помогу")
    off.given(
        "дано: Кавалорн щадит бандита по просьбе героя", who=npc, when="отпусти его",
        calls=[ToolCall("amend_quest", {"quest": quest, "drop": [spared], "why": why})],
    )
    off.silent("дано: Кавалорн молчит (условия изменены)", who=npc, when="отпусти его")
    off.given(
        "дано: Кавалорн засчитывает дело после пощады", who=npc, when="с ними покончено",
        calls=[ToolCall("accept_quest", {"quest": quest})],
    )
    off.silent("дано: Кавалорн молчит (дело засчитано)", who=npc, when="с ними покончено")
    off.given(
        "дано: Кавалорн засчитывает дело после загрузки", who=npc, when="с ними покончено",
        calls=[ToolCall("accept_quest", {"quest": quest})],
    )
    off.silent("дано: Кавалорн молчит (дело засчитано после загрузки)", who=npc, when="с ними покончено")
    log.info("given: дано: собеседники слушают героя (FixedDecider posture=listen)")
    talk.reflexes = Reflexes(FixedDecider({"posture": "listen"}))
    for who in (npc, *bandits):
        await link.debug("skip_important", near=who)
    for _ in range(2):
        try:
            await link.debug("kill", instance="YGOBBO_GREEN", near=npc, by=npc)
        except GameError as err:
            log.info("vspare: goblin: %s", err)
    await link.debug("player_goto", to=npc, distance=300)
    if not check("roster", await _until(lambda: npc in talk.people, 20), npc):
        return _logged(results)
    answered, _ = await say("Кавалорн, давай я помогу тебе с бандитами.")
    given = await _until(lambda: any(f.get("quest") == quest for f in facts("quest_given")), 15)
    if not check("given", answered and given and await var(quest) == 1, facts("quest_given")[-1:]):
        return _logged(results)

    vanilla.chronicle.record(
        {
            "fact": "caught",
            "npc": npc,
            "quest": "VSPARE_PRIOR_CLAIM",
            "title": "Прежняя ложь",
            "how": "уличил героя в прежней лжи",
        }
    )
    if not check("distrust", vanilla.trust.broken(npc), vanilla.trust.situation(npc)):
        return _logged(results)
    baseline = (await link.story_state(npcs=list(bandits), near=npc)).get("npcs", {})
    # e2e runs in its own empty directory; slot 7 is free (slot 1 is reserved for SAVE).
    slot = 7
    saved = await _save(link, slot)
    if not check("saved", saved is not None and all(baseline.get(b, {}).get("state") == "alive"
                                                  for b in bandits), baseline):
        return _logged(results)
    talk.end("scenario")
    if not await _until(lambda: npc not in talk.attached, 20):
        check("released", False, "Cavalorn is still attached")
        return _logged(results)

    # The instance path credits a native killer; debug kill's npc path has no killer.
    await link.debug("kill", instance=killed, near=npc, by=npc)
    remembered = await _until(lambda: killed in seen("killed"), 10)
    if not check("killed", remembered, [f for f in facts("seen_dead") if f.get("npc") == npc]):
        return _logged(results)

    remembered, before_death = await witness_death(witnessed)
    if not check(
        "staged", before_death.get("state") == "alive" and bool(before_death.get("seen")), before_death
    ):
        return _logged(results)
    if not check("saw_die", remembered, [f for f in facts("seen_dead") if f.get("dead") == witnessed]):
        return _logged(results)

    # Hold both while they negotiate: the spared bandit must remain alive before Cavalorn's eyes.
    if not await park_cavalorn():
        check("held", False, "Cavalorn could not stop to listen")
        return _logged(results)
    beasts_cleared = await _clear_beasts(link, npc)
    await link.debug("npc_goto", npc=spared, to=npc, distance=150, face=npc)
    await link.attach(spared)
    await link.act(npc, "turn_to", npc=spared)
    visible = await _until_async(
        lambda: link.story_state(npcs=[spared], near=npc),
        lambda state: bool(state.get("npcs", {}).get(spared, {}).get("seen")),
        10,
    )
    nearby = (await link.story_state(npcs=[npc, spared], near=npc)).get("npcs", {})
    before_spare = nearby.get(spared, {})
    if not check(
        "spared_in_view", visible and before_spare.get("state") == "alive" and bool(before_spare.get("seen")),
        f"beasts_cleared={beasts_cleared}; {nearby}",
    ):
        return _logged(results)

    plea = "Не убивай меня! Я уйду отсюда и больше не вернусь, клянусь!"
    name = talk.people[npc].name
    speaker = talk.people[spared].name if spared in talk.people else "Бандит"
    dialogue.heard(npc, name, speaker, plea, "тебе")
    await link.send(SayCmd(npc=spared, line="AIB_VSPARE_PLEA", text=plea, to=npc))
    before_reward = await rewards()
    answered, spare_turns = await say("Кавалорн, отпусти его.")
    bodies = (await link.story_state(npcs=[killed, witnessed], near=npc)).get("npcs", {})
    distances = {target: bodies.get(target, {}).get("distance") for target in (killed, witnessed)}
    await vanilla.prepare(npc)
    offer = vanilla.offers.get(npc, {}).get(quest)
    situation = vanilla.situation(npc) or ""
    tools = [tool.name for tool in vanilla.tools(npc)]
    own_evidence = offer.evidence() if offer is not None else ""
    away = bool(
        all(distance is not None and distance > 2000 for distance in distances.values())
        and "check_claim" not in tools
        and offer is not None and offer.seen_true()
        and all(target.seen_dead and not target.seen for target in offer.targets)
        and own_evidence and "ты убил его сам" in own_evidence
        and "ты видел, как он умер" in own_evidence
    )
    if not check("away", away, f"cm={distances}; tools={tools}; {situation}"):
        return _logged(results)
    check(
        "no_letter_start",
        letter not in vanilla.starts.get(npc, {})
        and not any(letter in line and "дать герою поручение" in line for line in situation.splitlines()),
        situation,
    )

    answered, end_turns = await say("Кавалорн, с ними покончено.")
    first_turns = [*spare_turns, *end_turns]
    if not await ending("ended", answered, first_turns, before_reward, amended=dropped()):
        return _logged(results)
    if spared in talk.attached and (await bandit_state(spared)).get("state") == "alive":
        try:
            await link.detach(spared)
        except GameError as err:
            log.info("vspare: detach %s: %s", spared, err)
    talk.free(npc, "scenario")
    talk.end("scenario")
    await _until(lambda: npc not in talk.attached, 20)

    loading = asyncio.create_task(wait_message(link, Session, lambda m: m.reason == "load", 60))
    await asyncio.sleep(0)
    try:
        await link.debug("load", slot)
        session = await loading
        await link.wait_world()
    except (GameError, TimeoutError) as err:
        loading.cancel()
        check("loaded", False, str(err))
        return _logged(results)
    baseline = (await link.story_state(npcs=list(bandits), near=npc)).get("npcs", {})
    fresh_after_load = not any(spared in f.get("drop", []) for f in quest_facts("quest_terms"))
    if not check(
        "loaded",
        saved is not None and session.parent == saved.branch and session.parent_seq == saved.seq
        and await var(quest) == 1 and vanilla.trust.broken(npc)
        and all(baseline.get(b, {}).get("state") == "alive" for b in bandits)
        and not quest_facts("quest_terms") and not quest_facts("quest_outcome")
        and not quest_facts("claim") and not facts("lie")
        and not any(f.get("npc") == npc and f.get("dead") in bandits for f in facts("seen_dead")),
        f"branch={vanilla.chronicle.branch}; bandits={baseline}; terms={quest_facts('quest_terms')}; "
        f"outcome={quest_facts('quest_outcome')}",
    ):
        return _logged(results)

    await link.debug("kill", instance=killed, near=npc, by=npc)
    remembered = await _until(lambda: killed in seen("killed"), 10)
    for who in (witnessed, spared):
        saw, state = await witness_death(who)
        if not saw:
            check("seen2", False, f"{who}: staged={state}; seen_dead={facts('seen_dead')}")
            return _logged(results)
    if not check("seen2", remembered and {witnessed, spared} <= seen("saw_die"),
                 [f for f in facts("seen_dead") if f.get("npc") == npc and f.get("dead") in bandits]):
        return _logged(results)
    if not await park_cavalorn():
        check("away2", False, "Cavalorn could not stop to listen")
        return _logged(results)
    beasts_cleared = await _clear_beasts(link, npc)
    bodies = (await link.story_state(npcs=list(bandits), near=npc)).get("npcs", {})
    distances = {b: bodies.get(b, {}).get("distance") for b in bandits}
    await vanilla.prepare(npc)
    offer = vanilla.offers.get(npc, {}).get(quest)
    tools = [tool.name for tool in vanilla.tools(npc)]
    if not check("away2", all(d is not None and d > 2000 for d in distances.values())
                 and offer is not None and offer.seen_true()
                 and all(t.seen_dead and not t.seen for t in offer.targets) and "check_claim" not in tools,
                 f"beasts_cleared={beasts_cleared}; cm={distances}; tools={tools}; {vanilla.situation(npc)}"):
        return _logged(results)
    before_reward = await rewards()
    answered, end_turns = await say("Кавалорн, с ними покончено.")
    await ending("seen_ended", answered, end_turns, before_reward, amended=False)
    fresh_terms = (
        fresh_after_load and not quest_facts("quest_terms")
        and all(f.get("how") == "done" and not f.get("drop") for f in quest_facts("quest_outcome"))
    )
    check("fresh_terms", fresh_terms,
          f"fresh_after_load={fresh_after_load}; terms={quest_facts('quest_terms')}; "
          f"outcome={quest_facts('quest_outcome')}")
    talk.free(npc, "scenario")
    return _logged(results)



async def vanilla_check(
    link: GameLink, dialogue: DialogueManager, talk: TalkDesk, vanilla: VanillaQuests, giver: str = LOBART
) -> dict[str, str]:
    """Lies as a mechanic the player can read (stage 5), on the game: Lobart's field raiders, where Andre's
    dialog puts them, and his turnips. Lobart is given the decision to check both claims.
    1. The hero says he killed the raiders. Lobart can't see the field from where he stands, but it is a short
       walk: he goes to see (check_claim), sees them alive and catches the lie — no reward, he is wary of the
       hero, the people around hear it.
    2. The hero says the turnips are in the barn: Lobart takes his word no more (the code refuses it).
    3. The raiders are killed and the hero says so: Lobart goes to see again, sees them dead and pays; the
       scripts' ending plays (gold, the quest, the farmhands back to work)."""
    off = model_off(dialogue)
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    await link.wait_world()
    if link.session is None:
        await wait_message(link, Session, timeout=10)
    await asyncio.sleep(1)
    facts = vanilla.chronicle.facts
    hero = ["inst", "HERO"]
    bugs = [f"LOBARTS_GIANT_BUG{i + 1}" for i in range(len(FIELD))]

    async def var(name: str) -> int | None:
        return (await link.story_state(vars=[name])).get("vars", {}).get(name)

    async def gold() -> int:
        return int(await link.evaluate(["call", "NPC_HASITEMS", hero, ["sym", "ITMI_GOLD"]]) or 0)

    async def say(text: str) -> None:
        """The hero, next to Lobart, says it; until Lobart's turn about it is over."""
        await link.debug("player_goto", to=giver, distance=250)
        await _until(lambda: giver in talk.people and talk.people[giver].distance < 600, 10)
        conv = dialogue.conversations.get(giver)
        before = conv.reply if conv else None
        await link.debug("type", text=text)

        def answered() -> bool:
            c = dialogue.conversations.get(giver)
            return c is not None and c.reply is not None and c.reply is not before and c.reply.done()

        await _until(answered, 60)

    async def settled() -> None:
        """Lobart is back from seeing for himself and has said his say."""
        await _until(lambda: giver not in vanilla.checking, 90)
        await asyncio.sleep(1)
        conv = dialogue.conversations.get(giver)
        await _until(lambda: conv is None or conv.reply is None or conv.reply.done(), 30)

    # the quests, as Andre's and Lobart's dialogs start them, and the raiders on his field
    await link.story_effect(
        giver, [{"do": "set", "var": BUGS, "value": 1}, {"do": "set", "var": RUEBEN, "value": 1}]
    )
    for bug, wp in zip(bugs, FIELD, strict=True):
        await link.story_spawn(bug, "LOBART_BUGS", 1, waypoint=wp)
    await link.debug("player_goto", to=giver, distance=250)
    log.info("given: дано: решающая модель выбирает слушать героя")
    talk.reflexes = Reflexes(FixedDecider({"posture": "listen"}))
    if not await _until(lambda: giver in talk.people, 20):
        return _logged({"roster": f"failed ({giver} is not around the hero)"})
    name = talk.people[giver].name
    field = (await link.story_state(npcs=bugs, near=giver)).get("npcs", {})
    ds = sorted(round(p.get("distance", 0) / 100) for p in field.values() if "distance" in p)
    check("field", bool(ds) and SEE_CM / 100 < ds[0] <= CHECK_CM / 100, f"{len(ds)} raiders, {ds} m")
    for words in ("перебил всех", "правда перебил"):
        off.given(
            "дано: Лобарт идёт проверить слова героя",
            who=giver, when=words, calls=[ToolCall("check_claim", {"quest": BUGS})],
        )
        off.silent(
            "дано: Лобарт молчит (решение проверить принято)", who=giver, when=words,
        )
    off.silent(
        "дано: Лобарт молчит (увидел живых хищников)", who=giver, when="Он тебе солгал",
    )
    off.silent(
        "дано: Лобарт молчит (увидел мёртвых хищников)", who=giver, when="он сказал правду",
    )
    off.silent("дано: Лобарт молчит (герой просит плату за репу)", who=giver, when="репа")
    conv = dialogue.open(giver, name, warm=False)
    _, _, sight = await dialogue._situation(conv, None)
    tools = [tool.name for tool in dialogue._offered(giver, sight)]
    check("check_offered", "check_claim" in tools, tools)
    gold0 = await gold()

    # 1. the lie: he goes to see for himself and catches it
    await say(f"{name}, я перебил всех полевых хищников на твоём поле. Плати.")
    ok = await _until(lambda: any(f.get("quest") == BUGS for f in facts("caught")), 90)
    got = next((f for f in facts("caught") if f.get("quest") == BUGS), None)
    check("caught", ok and got is not None and "сходил проверить" in got.get("how", ""), got)
    check("no_reward", (await var(BUGS), await gold()) == (1, gold0))
    conv = dialogue.conversations.get(giver)
    check("wary", conv is not None and conv.intent.attitude in ("wary", "hostile"), conv and conv.intent)
    await settled()  # back to the hero, he says what he thinks of it

    # 2. a lie he can't check: not on his word any more
    conv = dialogue.open(giver, name, warm=False)
    situation, _, sight = await dialogue._situation(conv, None)
    tools = [tool.name for tool in dialogue._offered(giver, sight)]
    check("not_on_word", "accept_quest" not in tools, tools)
    offer = vanilla.offers.get(giver, {}).get(RUEBEN)
    await say(f"{name}, репа уже в амбаре, все двадцать штук. Давай плату.")
    await asyncio.sleep(2)
    check(
        "refused",
        offer is not None
        and vanilla._refusal(giver, offer) is not None
        and "Засчитать сейчас нельзя" in situation
        and not any(f.get("quest") == RUEBEN for f in facts("claim")),
        f"tools={tools}; {situation}",
    )

    # 3. the truth: he goes to see again, and pays
    for bug in bugs:
        await link.debug("kill", npc=bug)
    await say(f"{name}, вот теперь я правда перебил всех хищников. Сходи посмотри сам.")
    ok = await _until(lambda: any(f.get("quest") == BUGS for f in facts("claim")), 90)
    claim = next((f for f in facts("claim") if f.get("quest") == BUGS), None)
    check("paid_after_check", ok and claim is not None and claim.get("truth") and claim.get("checked"), claim)
    await settled()
    check("ending_played", (await var(BUGS), await gold() >= gold0 + 20) == (2, True))
    kept = vanilla.trust.situation(giver) or ""
    check("kept_word", "он уже говорил тебе правду" in kept and "он уже лгал тебе" in kept, kept)
    return _logged(results)


async def rumor_spread(
    link: GameLink,
    dialogue: DialogueManager,
    talk: TalkDesk,
    rumors: Rumors,
    giver: str = LOBART,
    log_dir: str | None = None,
) -> dict[str, str]:
    """Rumors among acquaintances (stage 5), on the game. Given: Lobart exposes the turnip lie;
    the people of the world and their days make who knows whom (Hilda on
    the farm, Andre through the quest that sends the hero to Lobart's field). The clock goes to the next
    morning: the rumor goes round, in the order of the meetings, to people that never stood by Lobart, also
    in the city. The situation of one of them contains the rumor without a model call."""
    off = model_off(dialogue)
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    await link.wait_world()
    if link.session is None:
        await wait_message(link, Session, timeout=10)
    await asyncio.sleep(1)
    facts = rumors.chronicle.facts

    # who knows whom, from the routines of the world's people and the quests of the scripts
    if not await _until(lambda: rumors.social is not None, 30):
        return _logged({"social": "failed (no people)"})
    social = rumors.social
    assert social is not None
    if log_dir:
        text = "\n".join(f"{n}: {'; '.join(social.describe(n))}" for n in sorted(social.names))
        await asyncio.to_thread(Path(log_dir, "ties.txt").write_text, text, encoding="utf-8")
    ties = social.ties.get(giver, {})
    check(
        "ties",
        HILDA in ties and bool(ties[HILDA].meets) and "MIL_311_ANDRE" in ties,
        f"{len(social.names)} people; {giver}: {', '.join(sorted(social.names.get(n, n) for n in ties))}",
    )

    # Lobart catches the lie: the turnips in the barn, and the hero has none on him
    await link.story_effect(giver, [{"do": "set", "var": RUEBEN, "value": 1}])
    await link.debug("player_goto", to=giver, distance=250)
    log.info("given: дано: решающая модель выбирает слушать героя")
    talk.reflexes = Reflexes(FixedDecider({"posture": "listen"}))
    if not await _until(lambda: giver in talk.people, 20):
        return _logged({**results, "roster": f"failed ({giver} is not around the hero)"})
    name = talk.people[giver].name
    off.given(
        "дано: Лобарт уличил героя во лжи про репу", who=giver, when="репа",
        calls=[ToolCall("expose_lie", {"quest": RUEBEN})],
    )
    off.silent("дано: Лобарт молчит (ложь уже раскрыта)", who=giver, when="репа")
    await link.debug("type", text=f"{name}, репа уже в амбаре, все двадцать штук. Давай плату.")
    await _until(lambda: any(f.get("quest") == RUEBEN for f in facts("caught")), 60)
    before = {f["to"] for f in facts("rumor")}

    # the next morning: the meetings in between
    await link.debug("time", hour=7, minute=0)
    await _until(lambda: any(int(f.get("hops", 1)) >= 2 for f in facts("rumor")), 30)
    await asyncio.sleep(TICK_S + 1)
    heard = [f for f in facts("rumor") if f.get("quest") == RUEBEN]
    hops = sorted({int(f.get("hops", 1)) for f in heard})
    check(
        "went_round",
        len(heard) > len(before) and max(hops, default=0) >= 2,
        f"{len(heard)} people, hops {hops}",
    )
    knowers = sorted({f["to"] for f in heard} - before)
    where = (await link.story_state(npcs=knowers, near=giver)).get("npcs", {})
    far = sorted(
        (n for n in knowers if (where.get(n) or {}).get("state") == "alive"),
        key=lambda n: -float((where.get(n) or {}).get("distance", 0)),
    )
    farthest = far[0] if far else None
    d = float((where.get(farthest) or {}).get("distance", 0)) / 100 if farthest else 0
    check(
        "far_away",
        farthest is not None and d > 100,
        f"{social.names.get(farthest or '', farthest)}, {d:.0f} m",
    )
    if farthest is None:
        return _logged(results)

    # Read the assembled situation without asking the model to carry it.
    await link.debug("player_goto", to=farthest, distance=250)
    if not await _until(lambda: farthest in talk.people, 20):
        return _logged({**results, "roster2": f"failed ({farthest} is not around the hero)"})
    conv = dialogue.open(farthest, talk.people[farthest].name, warm=False)
    situation, _, _ = await dialogue._situation(conv, None)
    check(
        "in_mind",
        "ты слышал, что он обманул других" in situation and "Лобарт" in situation,
        situation.splitlines()[-1:],
    )
    return _logged(results)


async def amends_storylet(
    link: GameLink,
    dialogue: DialogueManager,
    talk: TalkDesk,
    amends: Amends,
    giver: str = LOBART,
    fine: int = 50,
    deals: Deals | None = None,
) -> dict[str, str]:
    """Given Lobart's accusation and Andre's fine, check closure, payment and forgiveness.

    With `deals`, Andre offers the declared fine in conversation and the hero accepts with Y.
    """
    off = model_off(dialogue)
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    await link.wait_world()
    if link.session is None:
        await wait_message(link, Session, timeout=10)
    await asyncio.sleep(1)
    facts = amends.chronicle.facts
    hero = ["inst", "HERO"]
    propose = amends.model.propose

    async def given_amends(
        brief: Brief, previous: Proposal | None = None, errors: list[str] | None = None,
    ) -> Proposal | None:
        # The recipients depend on rumor delivery and available dialogs. Declare each fixed
        # decision when its brief exists, before the original shared-backend request.
        if previous is None:
            args: dict[str, Any] = {
                "kind": "none", "reason": "не его дело", "close": [], "lines": [], "to": "self",
                "gold": 0, "description": "", "amends_lines": [], "title": "",
                "journal_now": "", "journal_done": "",
            }
            reason = f"дано: режиссёр расплаты решил, что {brief.name} не требует расплаты"
            if brief.npc == ANDRE:
                join = [stake.id for stake in brief.stakes if stake.what == "принимает в гильдию"]
                args.update({
                    "kind": "amends", "close": join[:1] or [brief.stakes[0].id], "gold": fine,
                    "lines": [
                        {"speaker": "npc", "text": "Слыхал я, ты Лобарта обмануть пытался."},
                        {"speaker": "npc", "text": f"Лжецу в ополчении не место. {fine} золотых штрафа."},
                    ],
                    "description": f"Вот штраф — {fine} золотых.",
                    "amends_lines": [{"speaker": "npc", "text": "Ладно. Считай, забыли."}],
                    "title": "Штраф за враньё",
                    "journal_now": f"Андре требует штраф ({fine} золотых) за ложь Лобарту.",
                    "journal_done": "Я заплатил Андре штраф.",
                    "reason": "до Андре дошёл слух, а в ополчение он берёт только честных",
                })
                reason = f"дано: режиссёр расплаты назначает штраф Андре {fine} золотых"
            off.given(
                reason, who=AMENDS_KEY, when=f"({brief.npc})",
                calls=[ToolCall("propose_amends", args)],
            )
        return await propose(brief, previous, errors)

    amends.model.propose = given_amends  # type: ignore[method-assign]
    amends.director.pacing.cooldown_s = 2.0
    log.info("given: дано: решающая модель оставляет NPC слушать героя")
    talk.reflexes = Reflexes(FixedDecider({"posture": "listen"}))
    await link.story_effect(giver, [{"do": "set", "var": RUEBEN, "value": 1}])
    await link.debug("player_goto", to=giver, distance=250)
    if not await _until(lambda: giver in talk.people and talk.people[giver].distance <= 600, 20):
        return _logged({"roster": f"failed ({giver} is not near the hero)"})
    off.given(
        "дано: Лобарт уличил героя во лжи о репе", who=giver, when="репа уже в амбаре",
        calls=[ToolCall("expose_lie", {"quest": RUEBEN})],
    )
    off.silent(
        "дано: Лобарт молчит (ложь о репе раскрыта)", who=giver, when="репа уже в амбаре",
    )
    await link.debug(
        "type", text=f"{talk.people[giver].name}, репа уже в амбаре, все двадцать штук. Давай плату.",
    )
    if not await _until(lambda: any(f.get("quest") == RUEBEN for f in facts("caught")), 60):
        check("closed", False, "the declared accusation did not reach the chronicle")
        return _logged(results)

    await link.debug("time", hour=7, minute=0)
    if not await _until(lambda: any(x.npc == ANDRE for x in amends.storylets.values()), 300):
        check("closed", False, "Andre's fine did not close any dialog")
        return _logged(results)
    s = next(x for x in amends.storylets.values() if x.npc == ANDRE)
    closed = [op["node"] for op in amends.patches.ops() if op.get("op") == "disable"]
    check("closed", bool(s.close) and set(s.close) <= set(closed), f"{s.name}: {s.close}")

    # the hero comes: the npc starts the talk himself (an important dialog)
    await link.debug("player_goto", to=s.npc, distance=250)
    await _until(lambda: s.npc in talk.people, 20)
    by_himself = await _until(lambda: s.stage != "open", 20)
    if not by_himself:
        try:
            await link.choose_dialog(s.npc, s.talk)
        except GameError as err:
            log.info("amends: %s", err)
    heard = await _until(lambda: s.stage != "open", 20)
    check("talked", heard, f"{s.talk}{', he started it' if by_himself else ''}")

    # he makes amends: what was closed is open again
    gold = int(await link.evaluate(["call", "NPC_HASITEMS", hero, ["sym", "ITMI_GOLD"]]) or 0)
    want = s.gold + (10 if deals is None else 100)  # in a deal the price may go up
    if gold < want:
        await link.story_effect(s.npc, [{"do": "give", "item": "ITMI_GOLD", "count": want - gold}])
        gold = want
    if s.to != s.npc:
        await link.debug("player_goto", to=s.to, distance=250)
        await _until(lambda: s.to in talk.people, 20)
    await asyncio.sleep(1)
    price = s.gold
    if deals is None:
        try:
            await link.choose_dialog(s.to, s.pay)
            paid = await _until(lambda: s.stage == "done", 20)
        except GameError as err:
            paid = False
            log.info("amends: %s", err)
    else:
        offered = await _forgiveness_deal(link, dialogue, talk, deals, s)
        if offered is None:
            check("paid", False, "no deal that forgives the lie")
            return _logged(results)
        price = offered.gold()
        await asyncio.sleep(1.2)
        pressed = await link.press_deal()
        paid = pressed == "sent" and await _until(lambda: s.stage == "done", 20)
    await asyncio.sleep(1)
    left = int(await link.evaluate(["call", "NPC_HASITEMS", hero, ["sym", "ITMI_GOLD"]]) or 0)
    opened = {op["node"] for op in amends.patches.ops() if op.get("op") == "enable"}
    check(
        "paid",
        paid and left == gold - price and set(s.close) <= opened,
        f"{s.description if deals is None else 'a deal'}: {gold} -> {left} gold",
    )
    if deals is not None:
        removed = {op["node"] for op in amends.patches.ops() if op.get("op") == "remove_node"}
        check("menu_clean", {s.talk, s.pay} <= removed, sorted({s.talk, s.pay} - removed))
    line = Trust(amends.chronicle).situation(s.npc) or ""
    check("forgiven", "он загладил вину" in line, line.splitlines()[-1:])
    return _logged(results)


async def _forgiveness_deal(
    link: GameLink, dialogue: DialogueManager, talk: TalkDesk, deals: Deals, s: Any
) -> Any:
    """Declare Andre's forgiveness offer, then ask for it through the normal conversation."""
    off = model_off(dialogue)
    name = talk.people[s.to].name if s.to in talk.people else s.to_name
    terms = {
        "npc_gives": [], "hero_gives": [{"item": GOLD, "count": s.gold}] if s.gold else [],
        "forgive": [s.lie], "silence": [], "hero_later": [], "repay": [],
    }
    off.given(
        f"дано: Андре предлагает прощение за {s.gold} золотых", who=s.to, when="прости",
        calls=[ToolCall("propose_deal", terms)],
    )
    off.silent("дано: Андре молчит (сделка предложена)", who=s.to, when="прости")
    off.silent("дано: Андре молчит (сделка состоялась)", who=s.to, when="сделка состоялась")

    def forgiving() -> bool:
        deal = deals.open.get(s.to)
        return deal is not None and any(lie.key == s.lie for lie in deal.forgive)

    await link.debug("type", text=f"{name}, прости меня за враньё. Чем мне загладить вину?")
    await _until(forgiving, 60)
    return deals.open.get(s.to) if forgiving() else None


async def witness_check(
    link: GameLink,
    dialogue: DialogueManager,
    talk: TalkDesk,
    sightings: Sightings,
    witnesses: Witnesses,
    farmer: str = LOBART,
) -> dict[str, str]:
    """Given Lobart's witness questions and accusation, check movement, testimony and trust.

    The witness's evidence is built by code; its code-started spoken answer is silent.
    A distant acquaintance answers at the routine meeting without a model request.
    """
    off = model_off(dialogue)
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    chronicle, desk = sightings.chronicle, sightings.desk
    await link.wait_world()
    if link.session is None:
        await wait_message(link, Session, timeout=10)
    social = None
    if not await _until(lambda: witnesses.social() is not None, 15):
        return _logged({"social": "failed (nobody knows anybody yet)"})
    social = witnesses.social()
    assert social is not None
    farm = [n for n, t in sorted(social.ties.get(farmer, {}).items()) if t.meets]

    # 1. eight in the morning: someone Lobart meets on the farm sees the hero, Lobart does not
    await link.debug("time", hour=8, minute=0)
    # one in the open first: from indoors to the yard a walk may not find its way (Hilda at her stove)
    witness, tried = "", []
    for who, indoors_too in [(w, False) for w in farm] + [(w, True) for w in farm]:
        try:
            await link.debug("skip_important", near=who)
            await link.debug("teleport", near=who, distance=400)
        except GameError as err:
            tried.append(f"{who}: {err}")
            continue
        await asyncio.sleep(1)
        view = await desk.fresh(who)
        if view is not None and view.indoors and not indoors_too:
            tried.append(f"{who}: indoors")
            continue
        await link.npc_act(who, "turn_to")
        await _until(lambda who=who: bool(sightings.hero_trail(who)), 5)
        await desk.fresh(farmer)
        tried.append(f"{who}: {'saw' if sightings.hero_trail(who) else 'no'}")
        if sightings.hero_trail(who) and not sightings.hero_trail(farmer):
            witness = who
            break
    if not witness:
        return _logged({"witness_saw": f"failed (nobody saw him unseen by {farmer}: {', '.join(tried)})"})
    wname = social.names.get(witness, witness)
    seen = sightings.hero_trail(witness)[0]
    check("witness_saw", True, f"{wname}: {sightings.words(seen)}")

    # 2. An hour and a half later, both stand at nearby outdoor waypoints. Each keeps its own activity
    # target, within call but far enough that the witness still has to come up to Lobart.
    await asyncio.sleep(12)  # the sighting ends unseen
    hour, minute = divmod(seen.end % 1440 + 90, 60)
    await link.debug("time", hour=hour, minute=minute)
    await asyncio.sleep(2)
    points = [
        p for p in await link.waypoints()
        if p["name"].startswith("NW_FARM1_") and not p.get("fp") and not p.get("room")
    ]
    pairs = []
    for places in ([p for p in points if p["name"].startswith("NW_FARM1_FIELD")], points):
        pairs = [
            (
                not 650 <= cm <= 900,
                b["name"] not in a.get("conn", []),
                abs(cm - 750),
                a["name"], b["name"], cm,
            )
            for i, a in enumerate(places) for b in places[i + 1:]
            if 500 < (cm := math.dist((a["x"], a["y"], a["z"]), (b["x"], b["y"], b["z"]))) < 1000
        ]
        if pairs:
            break
    if not pairs:
        check("near_points", False, "no outdoor farm waypoints 5–10 m apart")
        return _logged(results)
    _, _, _, farmer_point, witness_point, gap = min(pairs)
    preparation: dict[str, Any] = {"farmer": farmer_point, "witness": witness_point, "gap": gap}
    check("near_points", True, preparation)
    for who, point in ((farmer, farmer_point), (witness, witness_point)):
        await link.debug("skip_important", near=who)
        await link.npc_act(
            who, "start_activity", state="ZS_STAND_WP", waypoint=point, minutes=120, place=True,
        )
    async def standing() -> bool:
        views = {view.npc: view for view in await link.npc_view([farmer, witness])}
        preparation["views"] = [view.model_dump() for view in views.values()]
        return all(
            who in views and views[who].state == "ZS_STAND_WP" and views[who].body == "stand"
            for who in (farmer, witness)
        )

    initialized = await _until_async(standing, bool, 10)
    check("near_standing", initialized, preparation)
    if not initialized:
        return _logged(results)
    positions = {p["name"]: p for p in points}
    await link.debug(
        "player_goto",
        pos=tuple(
            (positions[farmer_point][axis] + positions[witness_point][axis]) / 2 for axis in ("x", "y", "z")
        ),
    )
    name = social.names.get(farmer, "Лобарт")

    async def within_call() -> bool:
        state = await link.story_state(npcs=[witness], near=farmer)
        cm = (state.get("npcs", {}).get(witness) or {}).get("distance")
        preparation["distance"] = cm
        preparation["roster"] = {
            who: entry.model_dump()
            for who in (farmer, witness) if (entry := talk.people.get(who)) is not None
        }
        await witnesses.prepare(farmer)
        candidate = next((c for c in witnesses._near.get(farmer, []) if c.npc == witness), None)
        preparation["candidate"] = vars(candidate) if candidate is not None else None
        return (
            cm is not None and 500 < float(cm) <= 1000
            and await standing()
            and all(
                (entry := talk.people.get(who)) is not None
                and entry.name == expected and entry.distance <= 1000 and entry.earshot <= 1000
                for who, expected in ((farmer, name), (witness, wname))
            )
            and candidate is not None and candidate.tier == "near" and candidate.moves
        )

    ready = await _until_async(within_call, bool, 15)
    check("near_ready", ready, preparation)
    if not ready:
        return _logged(results)
    tier = "near"
    log.info("given: дано: решающая модель оставляет NPC слушать героя")
    talk.reflexes = Reflexes(FixedDecider({"posture": "listen"}))
    conv = dialogue.open(farmer, name, warm=False)
    situation, _, _ = await dialogue._situation(conv, None)
    offered = [tool.name for tool in witnesses.tools(farmer)]
    check(
        "offered", "ask_witness" in offered and f"- {wname} — " in situation, offered,
    )
    off.given(
        f"дано: Лобарт спрашивает свидетеля {wname}", who=farmer, when="утро был в городе",
        calls=[ToolCall("ask_witness", {"who": wname, "said": "всё утро был в городе"})],
    )
    off.silent(
        "дано: Лобарт молчит (свидетель вызван)", who=farmer, when="утро был в городе",
    )
    off.silent(
        f"дано: {wname} молчит (показание записано кодом)", who=witness,
        when="спрашивает тебя о герое",
    )
    off.given(
        "дано: Лобарт уличает героя по ответу свидетеля", who=farmer, when="Ответ:",
        calls=[ToolCall("catch_lie", {"said": "всё утро был в городе", "seen": "С1"})],
    )
    off.silent(
        "дано: Лобарт молчит (герой уличён по показанию)", who=farmer, when="Ответ:",
    )
    await link.debug("type", text=f"{name}, это не я утром топтал твою репу — я всё утро был в городе.")
    caught = await _until(lambda: any(f.get("witness") for f in chronicle.facts("caught")), 90)
    testimony = next((f for f in chronicle.facts("testimony") if f.get("witness") == witness), None)
    check(
        "testimony",
        testimony is not None and bool(testimony.get("told")) and testimony.get("tier") in ("near", "walk"),
        {k: testimony.get(k) for k in ("tier", "said", "told")} if testimony else "none",
    )
    tier = testimony.get("tier", tier) if testimony else tier
    if tier == "near":  # it came up to Lobart, and the hero beside him heard its answer
        state = await link.story_state(npcs=[witness], near=farmer)
        cm = (state.get("npcs", {}).get(witness) or {}).get("distance")
        check("came", cm is not None and float(cm) <= 500, f"near: {wname} {cm} cm from {name}")
    else:  # Lobart went to it and came back to the hero
        back = farmer in talk.people and talk.people[farmer].distance <= 600
        cm = talk.people[farmer].distance if farmer in talk.people else None
        check("came", tier == "walk" and back, f"{tier}: {name} back {cm} cm from the hero")
    fact = next((f for f in chronicle.facts("caught") if f.get("witness")), None)
    check(
        "caught", caught and fact is not None and fact["witness"] == witness, fact.get("how") if fact else ""
    )
    conv = dialogue.conversations.get(farmer)
    check(
        "wary", conv is not None and conv.intent.attitude in ("wary", "hostile"), conv.intent if conv else ""
    )
    lied = Trust(chronicle).of(farmer).lied
    check("trust", any("со слов" in x for x in lied), lied)

    # 3. someone far: asked at their next meeting, the answer is a fact, no model call
    await witnesses.prepare(farmer)
    far = next((c for c in witnesses._near.get(farmer, []) if c.tier == "far"), None)
    if far is None:
        check("far", False, f"no far acquaintance of {name}: {witnesses._near.get(farmer)}")
        return _logged(results)
    off.given(
        f"дано: Лобарт спрашивает дальнего знакомого {far.name}", who=farmer, when=far.name,
        calls=[ToolCall("ask_witness", {"who": far.name, "said": "всё утро был в городе"})],
    )
    off.silent(
        "дано: Лобарт молчит (дальний вопрос записан)", who=farmer, when=far.name,
    )
    await link.debug("type", text=f"{name}, не веришь — спроси {far.name}, он подтвердит.")
    await _until(lambda: any(f.get("witness") == far.npc for f in chronicle.facts("inquiry")), 60)
    q = next((f for f in chronicle.facts("inquiry") if f.get("witness") == far.npc), None)
    if q is None:
        check("far_answered", False, "no inquiry to answer at the meeting")
    else:
        await _until(lambda: not dialogue.speaking(farmer), 10)
        calls = len(off.used)
        hour, minute = divmod(int(q["meets"]) % 1440 + 5, 60)
        await link.debug("time", hour=hour, minute=minute)
        told = await _until(lambda: any(f.get("id") == q["id"] for f in chronicle.facts("testimony")), 20)
        t = next((f for f in chronicle.facts("testimony") if f.get("id") == q["id"]), None)
        check(
            "far_answered",
            told and t is not None and t.get("tier") == "far" and len(off.used) == calls,
            {k: t.get(k) for k in ("tier", "told")} if t else "none",
        )
    return _logged(results)
