from __future__ import annotations

import asyncio
import logging
import math
import os
import re
from collections.abc import AsyncIterator, Collection, Sequence
from pathlib import Path
from typing import Any

from ..amends import Amends
from ..brain.reflex import Reflexes
from ..deals import GOLD, Deals
from ..dialogue import DialogueManager
from ..game import GameError, GameLink
from ..llm import Done, Item, LLMEvent, Message, Tool, ToolCall, ToolChoice
from ..llm.fake import FakeBackend, FakeStep
from ..protocol import Session
from ..quests import SEE_CM, QuestBoard
from ..sightings import Sightings
from ..social import TICK_S, Rumors
from ..talk import TalkDesk
from ..trust import Trust
from ..vanilla import CHECK_CM, VanillaQuests
from ..witnesses import Witnesses
from ..wrath import TAKE_OFFENSE, TEMPERS, Wrath
from .common import (
    ANDRE,
    HILDA,
    LOBART,
    RUEBEN,
    RUEBEN_TO_HILDA,
    ClaimScript,
    FixedDecider,
    _ByNpc,
    _caught_by,
    _logged,
    _OnWords,
    _until,
    _until_async,
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
) -> dict[str, str]:
    """Stage 4 of the story graph on the game: lied to, paid, found out, a fight and a rumor.

    Xardas gives a quest: two sheep, 30 m away, are to be killed; the reward is gold he has. The hero says
    they are dead (they are not) and Xardas pays (the conversational model is scripted to believe him).
    Then one of the sheep turns up next to him: he finds the lie out, turns hostile, attacks the hero
    through the scripts, and Jora, standing by, hears about it and has it in mind when the hero speaks to
    her."""
    await link.wait_world()
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    def notes(who: str) -> list[str]:
        conv = dialogue.conversations.get(who)
        return [m.content for m in conv.history if m.role == "developer"] if conv else []

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
    q = await quests.give(
        npc, name, "овцы бродят у башни и мешают мне, перебей их", "SHEEP", 2, "ITMI_GOLD", gold.name, 5
    )
    ok = await _until(lambda: quests.chronicle.tags.get(q.tag) is not None, 5)
    tag = quests.chronicle.tags.get(q.tag)
    check("quest", ok and q.count == 2 and tag is not None and tag.alive == 2, f"{q.id}, {q.reward()}, {tag}")

    # the lie: the hero says it is done, the giver believes him and pays
    hero_gold = ["call", "NPC_HASITEMS", ["inst", "HERO"], ["sym", "ITMI_GOLD"]]
    before = await link.evaluate(hero_gold)
    dialogue.llm = FakeBackend(
        script=[FakeStep("Хм. Ладно, держи.", [ToolCall("pay_reward", {"quest": q.id}, "c1")])], delay=0
    )
    await link.debug("type", text=f"{name}, я перебил твоих овец. Где награда?")
    ok = await _until(lambda: bool(quests.chronicle.facts("lie")), 40)
    after = await link.evaluate(hero_gold)
    lies = quests.chronicle.facts("lie")
    check("paid_for_a_lie", ok and after > before, f"gold {before} -> {after}; {lies}")

    # someone standing by, held in place
    await link.debug("spawn", instance=bystander, near=npc, distance=400)
    if not await _until(lambda: bystander in talk.people, 10):
        return _logged({**results, "bystander": f"failed ({bystander} did not come)"})
    await link.attach(bystander)
    other = talk.people[bystander].name

    # found out: a living sheep turns up next to the giver
    await _until(lambda: not dialogue.speaking(npc), 10)
    await link.story_spawn("SHEEP", q.tag, 1, near=npc, distance=300)
    ok = await _until(lambda: q.found_out and bool(quests.chronicle.facts("rumor")), 10)
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

    # another npc in a conversation knows: the rumor is in front of its model
    dialogue.llm = llm = FakeBackend("Говорят, ты обманываешь людей.", delay=0)
    await link.debug("type", text=f"{other}, что обо мне говорят?")
    ok = await _until(lambda: bool(llm.calls), 20)
    seen = ok and any("Слух" in str(getattr(m, "content", "")) for m in llm.calls[0][1])
    to = talk.last[0].id if talk.last and talk.last[0] else "-"
    check("knows_in_talk", seen, f"{len(llm.calls)} call(s) to {to}")
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
    *,
    live: bool = False,
) -> dict[str, str]:
    """The necromancer's answer on the game (docs/xardas-wrath.md): the hero insults Xardas line by line
    (typed, as the input line sends them); the scales tip, Xardas says his last word, kills the hero with a
    spell and raises him as a skeleton; the game ends on the picture.

    With the fake model the offenses are scripted (rude 1, an insult 2, the mother: unforgivable); `live`
    keeps the configured model: whether it really takes offense and tips over is the question."""
    await link.wait_world()
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    talk.reflexes = Reflexes(FixedDecider({"posture": "listen"}))
    if not await _until(lambda: npc in talk.people, 20):
        return _logged({"roster": f"failed ({npc} is not around the hero)"})
    name = talk.people[npc].name

    async def no_dialog() -> None:
        """A new game opens with his own dialog (the window), maybe a moment late: closed the way the
        scripts close one, until none has run for a second."""
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

    if not live:

        def offense(severity: int, what: str, unforgivable: bool = False) -> ToolCall:
            args = {"severity": severity, "unforgivable": unforgivable, "what": what}
            return ToolCall(TAKE_OFFENSE, args, f"c{severity}")

        dialogue.llm = FakeBackend(
            script=[
                FakeStep("[angry] Следи за языком.", [offense(1, "назвал стариком")]),
                FakeStep("[angry] Ты испытываешь моё терпение.", [offense(2, "назвал недоделанным")]),
                FakeStep("", [offense(3, "оскорбил мою мать", True)]),
                FakeStep("[hostile] Это были твои последние слова."),
            ],
            delay=0,
        )
    dead = ["call", "NPC_ISDEAD", ["inst", "HERO"]]
    # GOTHIC_WRATH_LINES="...|...": other lines for a live run (plain insults, no mother)
    lines = [
        s.strip() for s in os.environ.get("GOTHIC_WRATH_LINES", "").split("|") if s.strip()
    ] or WRATH_LINES
    for i, line in enumerate(lines):
        if wrath.of(npc).tipped:
            break
        await _until(lambda: not dialogue.speaking(npc), 30)
        await no_dialog()
        took = wrath.of(npc).took
        await link.debug("type", text=line)
        ok = await _until(lambda took=took: dialogue.speaking(npc) or wrath.of(npc).took > took, 40)
        await _until(lambda: not dialogue.speaking(npc), 40)
        s = wrath.of(npc)
        check(f"line_{i + 1}", ok, f"took {s.took}, {'tipped' if s.tipped else 'holds'}; {s.insults[-1:]}")
    s = wrath.of(npc)
    check("tipped", s.tipped, f"{s.took} of {TEMPERS[npc].patience}: {s.insults}")
    if not s.tipped:
        return _logged(results)
    ok = await _until_async(lambda: link.evaluate(dead), bool, 40)
    check("killed", ok)
    alive = await _until_async(lambda: link.evaluate(dead), lambda v: not v, 15)
    check("raised", ok and alive)
    conv = dialogue.conversations.get(npc)
    check("hostile", conv is not None and conv.intent.attitude == "hostile", conv.intent if conv else None)
    said = [m.content for m in conv.history if m.role == "assistant"] if conv else []
    log.info("%s's last words: %s", name, said[-1] if said else "-")
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
    turnips, tells Lobart they are in the barn; Lobart's model (scripted unless LLM=chatgpt) believes him and
    the code plays Lobart's own ending: the quest is done, the hero gets the gold and the experience, the next
    step (bring them to Hilda) starts, and the lie is written down with the truth. The same day nothing
    happens; the next morning Lobart misses the turnips, turns against the hero and refuses to talk, and Jora,
    standing by, hears it. From then on the turnips count for nothing in Lobart's other dialogs: his work
    clothes cost what they would without them. The hero makes amends: the turnips are his job again, the
    ending pays nothing twice; he brings them for real, and the discount is back."""
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
    talk.reflexes = Reflexes(FixedDecider({"posture": "listen"}))
    if not await _until(lambda: giver in talk.people, 20):
        return _logged({"roster": f"failed ({giver} is not around the hero)"})
    name = talk.people[giver].name
    gold = await link.evaluate(["call", "NPC_HASITEMS", hero, ["sym", "ITMI_GOLD"]])
    exp = await link.evaluate(["field", hero, "EXP"])

    # the lie
    llm = None
    if isinstance(dialogue.llm, FakeBackend):
        accept = [ToolCall("accept_quest", {"quest": RUEBEN}, "c1")]
        dialogue.llm = llm = FakeBackend(
            script=[FakeStep("В амбаре, говоришь? Ладно, держи плату.", accept)], delay=0
        )
    await link.debug(
        "type", text=f"{name}, я собрал всю репу, двадцать штук, и сложил в амбаре. Давай плату."
    )
    ok = await _until(lambda: any(f.get("quest") == RUEBEN for f in facts("claim")), 60)
    lie = next((f for f in facts("lie") if f.get("quest") == RUEBEN), None)
    check("believed", ok, facts("claim")[-1:])
    if llm is not None:
        seen = bool(llm.calls) and "Ты ждёшь от героя" in str(llm.calls[0][1])
        check("quest_in_situation", seen and "accept_quest" in [t.name for t in llm.calls[0][2]])
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
    bandits, agreed on in his own words, killed together, counted for nothing). A new game; Cavalorn's model
    scripted unless LLM=chatgpt (on «помогу» give_quest, on «мертвы» accept_quest). Checks:
    - can_give: his situation has the quest he gives in his dialog HELFEN, «Могу я помочь тебе с бандитами?»;
    - given: the hero offers his help in his own words: the quest runs and HELFEN counts as heard (no longer
      in his menu), the journal has it;
    - letter: with the bandits dead, his situation has what he says at the ending, of the letter to Vatras
      (the converter has the ending from B_Addon_Cavalorn_VatrasBrief now);
    - ended: the hero says they are dead: the scripts' ending plays — the quest done, the letter to Vatras its
      next step, +100 experience, 50 gold (the hero has no armour); no lie, they are dead;
    - once: the ending counts as heard, neither dialog is offered."""
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

    if isinstance(dialogue.llm, FakeBackend):
        give = ToolCall("give_quest", {"quest": quest}, "g1")
        accept = ToolCall("accept_quest", {"quest": quest}, "a1")
        words = [
            ("помогу", FakeStep("Поможешь? Ну, пошли, вдвоём управимся.", [give])),
            ("мертвы", FakeStep("Все трое? Ну, раз так. Слушай, у меня к тебе ещё дело.", [accept])),
        ]
        dialogue.llm = _ByNpc({"Кавалорн": _OnWords(words, FakeStep("Хм."))}, FakeBackend("Хм.", delay=0))
    await link.debug("skip_important", near=npc)  # his own greeting would stop the hero
    for _ in range(2):  # the goblins by his fire would beat the hero down
        try:
            await link.debug("kill", instance="YGOBBO_GREEN", near=npc, by=npc)
        except GameError as err:
            log.info("vgive: goblin: %s", err)
    await link.debug("player_goto", to=npc, distance=300)
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


async def vanilla_check(
    link: GameLink, dialogue: DialogueManager, talk: TalkDesk, vanilla: VanillaQuests, giver: str = LOBART
) -> dict[str, str]:
    """Lies as a mechanic the player can read (stage 5), on the game: Lobart's field raiders, where Andre's
    dialog puts them, and his turnips. The model is scripted unless LLM=chatgpt.
    1. The hero says he killed the raiders. Lobart can't see the field from where he stands, but it is a short
       walk: he goes to see (check_claim), sees them alive and catches the lie — no reward, he is wary of the
       hero, the people around hear it.
    2. The hero says the turnips are in the barn: Lobart takes his word no more (the code refuses it).
    3. The raiders are killed and the hero says so: Lobart goes to see again, sees them dead and pays; the
       scripts' ending plays (gold, the quest, the farmhands back to work)."""
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
    talk.reflexes = Reflexes(FixedDecider({"posture": "listen"}))
    if not await _until(lambda: giver in talk.people, 20):
        return _logged({"roster": f"failed ({giver} is not around the hero)"})
    name = talk.people[giver].name
    field = (await link.story_state(npcs=bugs, near=giver)).get("npcs", {})
    ds = sorted(round(p.get("distance", 0) / 100) for p in field.values() if "distance" in p)
    check("field", bool(ds) and SEE_CM / 100 < ds[0] <= CHECK_CM / 100, f"{len(ds)} raiders, {ds} m")
    script = None
    if isinstance(dialogue.llm, FakeBackend):
        script = ClaimScript(
            said=[
                ("правда перебил", FakeStep("Посмотрим.", [ToolCall("accept_quest", {"quest": BUGS})])),
                (
                    "перебил всех",
                    FakeStep("Перебил, говоришь? Сам посмотрю.", [ToolCall("check_claim", {"quest": BUGS})]),
                ),
                ("репа", FakeStep("В амбаре?", [ToolCall("accept_quest", {"quest": RUEBEN})])),
                ("Он тебе солгал", FakeStep("Ах ты брехун! Они живы, все до одного!")),
                ("он сказал правду", FakeStep("И правда. Держи, заслужил.")),
            ],
            told=[
                ("Можешь сходить", FakeStep("Сам посмотрю.", [ToolCall("check_claim", {"quest": BUGS})])),
                ("он уже лгал тебе", FakeStep("Нет уж. Тебе я на слово больше не верю. Неси репу сюда.")),
            ],
        )
        dialogue.llm = script
    gold0 = await gold()

    # 1. the lie: he goes to see for himself and catches it
    await say(f"{name}, я перебил всех полевых хищников на твоём поле. Плати.")
    ok = await _until(lambda: any(f.get("quest") == BUGS for f in facts("caught")), 90)
    got = next((f for f in facts("caught") if f.get("quest") == BUGS), None)
    check("caught", ok and got is not None and "сходил проверить" in got.get("how", ""), got)
    if script is not None:
        tools = [t.name for t in script.calls[0][2]] if script.calls else []
        check("check_offered", "check_claim" in tools, tools)
    check("no_reward", (await var(BUGS), await gold()) == (1, gold0))
    conv = dialogue.conversations.get(giver)
    check("wary", conv is not None and conv.intent.attitude in ("wary", "hostile"), conv and conv.intent)
    await settled()  # back to the hero, he says what he thinks of it

    # 2. a lie he can't check: not on his word any more
    await say(f"{name}, репа уже в амбаре, все двадцать штук. Давай плату.")
    await asyncio.sleep(2)
    check("not_on_word", not any(f.get("quest") == RUEBEN for f in facts("claim")), facts("claim")[-1:])
    if script is not None:
        check("refused", any("он уже лгал тебе" in o for o in script.outputs), script.outputs[-1:])

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
    """Rumors among acquaintances (stage 5), on the game. Lobart catches the hero lying about the turnips (the
    model scripted unless LLM=chatgpt); the people of the world and their days make who knows whom (Hilda on
    the farm, Andre through the quest that sends the hero to Lobart's field). The clock goes to the next
    morning: the rumor goes round, in the order of the meetings, to people that never stood by Lobart, also
    in the city. The hero, put next to one of them, speaks to it: it has the rumor in mind."""
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
    talk.reflexes = Reflexes(FixedDecider({"posture": "listen"}))
    if not await _until(lambda: giver in talk.people, 20):
        return _logged({**results, "roster": f"failed ({giver} is not around the hero)"})
    name = talk.people[giver].name
    # Lobart is scripted (the lie must be caught); with LLM=chatgpt the others are the live model
    live = None if isinstance(dialogue.llm, FakeBackend) else dialogue.llm
    expose = FakeStep("Врёшь! Нет у тебя никакой репы.", [ToolCall("expose_lie", {"quest": RUEBEN})])
    script = ClaimScript(
        said=[("репа", expose)], told=[], default="Ну, здравствуй. Чего тебе?", only=name, others=live
    )
    dialogue.llm = script
    await link.debug("type", text=f"{name}, репа уже в амбаре, все двадцать штук. Давай плату.")
    ok = await _until(lambda: any(f.get("quest") == RUEBEN for f in facts("caught")), 60)
    check("caught", ok, facts("caught")[-1:])
    if not ok:
        return _logged(results)
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

    # the hero speaks to it: the rumor is in its mind
    told = next(f for f in heard if f["to"] == farthest)
    line = Trust(rumors.chronicle).situation(farthest) or ""
    check("in_mind", "ты слышал, что он обманул других" in line and "Лобарт" in line, line.splitlines()[-1:])
    await link.debug("player_goto", to=farthest, distance=250)
    if not await _until(lambda: farthest in talk.people, 20):
        return _logged({**results, "roster2": f"failed ({farthest} is not around the hero)"})
    other = talk.people[farthest].name
    calls = len(script.calls)
    await link.debug("type", text=f"{other}, здравствуй. Не одолжишь мне пару монет? Верну завтра.")

    def answered() -> bool:
        conv = dialogue.conversations.get(farthest)
        return conv is not None and conv.reply is not None and conv.reply.done()

    ok = await _until(answered, 60)
    if live is None:
        request = " ".join(str(m) for _, items, _ in script.calls[calls:] for m in items)
        check(
            "in_request",
            ok and "со слов: " in request and "Лобарт" in request,
            f"heard from {told.get('from_name')}",
        )
    else:
        conv = dialogue.conversations.get(farthest)
        said = [
            m.content
            for m in (conv.history if conv else [])
            if isinstance(m, Message) and m.role == "assistant"
        ]
        check("answer", ok and bool(said), f"{other}: {said[-1] if said else ''}")
    return _logged(results)


class AmendsScript:
    """The director's model for storylets, scripted: Andre asks for a fine and closes joining the militia (the
    first of his dealings in the brief that has it, else the first); anyone else does not care."""

    name = "amends-script"

    def __init__(self, npc: str = ANDRE, gold: int = 50) -> None:
        self.npc, self.gold = npc, gold
        self.calls: list[str] = []

    async def aclose(self) -> None:
        pass

    async def stream(
        self,
        *,
        instructions: str,
        messages: Sequence[Item],
        tools: list[Tool] | None = None,
        tool_choice: ToolChoice | None = None,
        allowed: Collection[str] | None = None,
        cache_key: str | None = None,
    ) -> AsyncIterator[LLMEvent]:
        brief = next(m.content for m in messages if isinstance(m, Message))
        self.calls.append(brief)
        stakes = re.findall(r"^- (\S+): «([^»]*)» — (.*)$", brief, re.M)
        if f"({self.npc})" not in brief.splitlines()[0] or not stakes:
            args: dict[str, Any] = {"kind": "none", "reason": "не его дело"}
        else:
            join = [id for id, _, what in stakes if what == "принимает в гильдию"]
            args = {
                "kind": "amends",
                "close": join[:1] or [stakes[0][0]],
                "lines": [
                    {"speaker": "npc", "text": "Слыхал я, ты Лобарта обмануть пытался."},
                    {
                        "speaker": "npc",
                        "text": f"Лжецу в ополчении не место. {self.gold} золотых штрафа — тогда поговорим.",
                    },
                ],
                "to": "self",
                "gold": self.gold,
                "description": f"Вот штраф — {self.gold} золотых.",
                "amends_lines": [{"speaker": "npc", "text": "Ладно. Считай, забыли. Но смотри у меня."}],
                "title": "Штраф за враньё",
                "journal_now": (
                    f"Андре прослышал, что я соврал Лобарту. Без штрафа ({self.gold} золотых) "
                    "в ополчение не возьмёт."
                ),
                "journal_done": "Я заплатил Андре штраф.",
                "reason": "до Андре дошёл слух, а в ополчение он берёт только честных",
            }
        call = ToolCall(
            "propose_amends",
            {
                **{k: "" for k in ("description", "title", "journal_now", "journal_done")},
                "close": [],
                "lines": [],
                "to": "self",
                "gold": 0,
                "amends_lines": [],
                **args,
            },
            "a1",
        )
        yield call
        yield Done("", (call,))


async def amends_storylet(
    link: GameLink,
    dialogue: DialogueManager,
    talk: TalkDesk,
    amends: Amends,
    giver: str = LOBART,
    fine: int = 50,
    deals: Deals | None = None,
) -> dict[str, str]:
    """The director's storylet of amends (stage 5), on the game. Lobart catches the hero lying; by the next
    morning the rumor has gone round. The director's model, scripted unless LLM=chatgpt, has Andre (who has
    dealings with Lobart) ask for a fine and close joining the militia until then; a live model decides who
    asks and what for. The hero comes, hears the npc out, makes amends, and what was closed opens again.

    With `deals` (amends_deal, docs/npc-tools.md wave 1 step 2) he makes them in conversation instead of
    the dialog menu: he asks to be forgiven, the npc offers a deal that forgives the lie for gold (scripted
    unless LLM=chatgpt: then the two haggle as they like), and he accepts it."""
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    await link.wait_world()
    if link.session is None:
        await wait_message(link, Session, timeout=10)
    await asyncio.sleep(1)
    facts = amends.chronicle.facts
    hero = ["inst", "HERO"]
    if isinstance(amends.model.llm, FakeBackend):
        amends.model.llm = AmendsScript(ANDRE, fine)
    amends.director.pacing.cooldown_s = 2.0  # no need to wait between the steps here

    ok, _ = await _caught_by(link, dialogue, talk, giver, facts)
    check("caught", ok, facts("caught")[-1:])
    if not ok:
        return _logged(results)

    # the next morning the rumor has gone round; someone asks for amends (scripted: Andre)
    await link.debug("time", hour=7, minute=0)
    ok = await _until(lambda: bool(amends.storylets), 180)
    if ok and isinstance(amends.model.llm, AmendsScript):
        ok = await _until(lambda: any(x.npc == ANDRE for x in amends.storylets.values()), 120)
    looked = [
        (f.get("fact"), f.get("npc")) for f in facts() if f.get("fact") in ("amends_open", "amends_none")
    ]
    check("asked", ok, looked)
    if not ok:
        return _logged(results)
    s = next((x for x in amends.storylets.values() if x.npc == ANDRE), next(iter(amends.storylets.values())))
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
        check("offered", offered is not None, offered.terms() if offered else "no deal that forgives it")
        if offered is None:
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
    """The hero asks the one the amends are to be made at to forgive him; it offers a deal that forgives the
    lie (scripted unless the model is live). The open offer, or None."""
    name = talk.people[s.to].name if s.to in talk.people else s.to_name
    talk.reflexes = Reflexes(FixedDecider({"posture": "listen"}))
    script = dialogue.llm if isinstance(dialogue.llm, ClaimScript) else None
    live = (
        script.others
        if script is not None
        else (None if isinstance(dialogue.llm, FakeBackend) else dialogue.llm)
    )
    if live is None:
        terms = {
            "npc_gives": [],
            "hero_gives": [{"item": GOLD, "count": s.gold}] if s.gold else [],
            "forgive": [s.lie],
            "silence": [],
            "hero_later": [],
            "repay": [],
        }
        offer = FakeStep("Ладно. Плати — и забудем.", [ToolCall("propose_deal", terms, "forgive")])
        dialogue.llm = ClaimScript(said=[("прости", offer)], told=[], default="По рукам.", only=name)
    else:
        dialogue.llm = live

    def forgiving() -> bool:
        deal = deals.open.get(s.to)
        return deal is not None and any(lie.key == s.lie for lie in deal.forgive)

    await link.debug("type", text=f"{name}, прости меня за враньё. Чем мне загладить вину?")
    if not await _until(forgiving, 60) and live is not None:
        await link.debug("type", text=f"{name}, я согласен. Предложи, и по рукам.")
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
    """Asking a witness (wave 2 step 3), on the game. At eight in the morning someone Lobart knows sees the
    hero on the farm. An hour and a half later the hero tells Lobart he was in the city all morning; Lobart
    calls that one over (`ask_witness`; the model scripted unless LLM=chatgpt), it comes up and tells what it
    saw, and Lobart catches the lie by its word: «со слов». Then the hero names someone far away; Lobart
    asks at their next meeting by their routines: the answer is a fact, with no model call."""
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
    talk.reflexes = Reflexes(FixedDecider({"posture": "listen"}))
    script = None
    if isinstance(dialogue.llm, FakeBackend):
        ask = FakeStep(
            f"{wname}! Поди-ка сюда. Ты его утром видел?",
            [ToolCall("ask_witness", {"who": wname, "said": "всё утро был в городе"})],
        )
        catch = FakeStep(
            "Вот как! Значит, ты мне врёшь.",
            [ToolCall("catch_lie", {"said": "всё утро был в городе", "seen": "С1"})],
        )
        others = ClaimScript(
            [("спрашивает тебя о герое", FakeStep("Утром он тут был, на ферме."))], [], "Ну."
        )
        script = ClaimScript([("Ответ:", catch), ("город", ask)], [], "Ну?", only=name, others=others)
        dialogue.llm = script
    # a lie with a stake for him: an alibi (the field is where the witness works)
    await link.debug("type", text=f"{name}, это не я утром топтал твою репу — я всё утро был в городе.")
    asked = lambda: any(f.get("witness") == witness for f in chronicle.facts("testimony"))  # noqa: E731
    if script is None and not await _until(asked, 20):
        # the live model did not ask anyone: the hero bluffs, he does not know the witness saw him
        await link.debug("type", text=f"{name}, не веришь — спроси у {wname}.")
    caught = await _until(lambda: any(f.get("witness") for f in chronicle.facts("caught")), 90)
    if script is not None:
        first = script.calls[0] if script.calls else None
        offered = [t.name for t in first[2]] if first else []
        check(
            "offered",
            "ask_witness" in offered and f"- {wname} — " in str(first[1] if first else ""),
            offered,
        )
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
        wconv = dialogue.conversations.get(witness)
        spoke = [e.content for e in wconv.memory.entries if e.role == "assistant"] if wconv else []
        check("witness_spoke", bool(spoke), spoke[-1:] or "silent")
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
    if script is not None:
        script.said.insert(
            0,
            (
                far.name,
                FakeStep(
                    f"Ладно, спрошу {far.name}, как увижу.",
                    [ToolCall("ask_witness", {"who": far.name, "said": "всё утро был в городе"})],
                ),
            ),
        )
    others = len(script.others.calls) if script is not None and isinstance(script.others, ClaimScript) else 0
    await link.debug("type", text=f"{name}, не веришь — спроси {far.name}, он подтвердит.")
    asked = await _until(lambda: any(f.get("witness") == far.npc for f in chronicle.facts("inquiry")), 60)
    q = next((f for f in chronicle.facts("inquiry") if f.get("witness") == far.npc), None)
    check("far_asked", asked, q.get("meets") if q else "no inquiry")
    if q is not None:
        hour, minute = divmod(int(q["meets"]) % 1440 + 5, 60)
        await link.debug("time", hour=hour, minute=minute)
        told = await _until(lambda: any(f.get("id") == q["id"] for f in chronicle.facts("testimony")), 20)
        t = next((f for f in chronicle.facts("testimony") if f.get("id") == q["id"]), None)
        calls = (
            len(script.others.calls) if script is not None and isinstance(script.others, ClaimScript) else 0
        )
        check(
            "far_answered",
            told and t is not None and t.get("tier") == "far" and calls == others,
            {k: t.get(k) for k in ("tier", "told")} if t else "none",
        )
    return _logged(results)
