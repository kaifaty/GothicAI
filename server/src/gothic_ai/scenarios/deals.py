from __future__ import annotations

import asyncio
import logging
from typing import Any

from ..brain.reflex import Reflexes
from ..contracts import gold_text
from ..deals import GOLD, Deals
from ..dialogue import DialogueManager
from ..game import GameLink
from ..llm import ToolCall
from ..llm.fake import FakeBackend, FakeStep
from ..protocol import Session
from ..talk import TalkDesk
from ..trust import Trust
from .common import LOBART, ClaimScript, FixedDecider, _logged, _until, wait_message

log = logging.getLogger("gothic_ai.scenarios")


async def deal_window(
    link: GameLink, dialogue: DialogueManager, talk: TalkDesk, deals: Deals, seller: str = LOBART
) -> dict[str, str]:
    """A deal (docs/npc-tools.md, wave 1), on the game. The hero asks Lobart to sell him something he has;
    Lobart (scripted unless LLM=chatgpt) offers it for 50 gold: Y waits, the hero lacks the gold. He declines
    (N): the window goes, Lobart hears of it and offers it for 40; after haggling for 30 the window shows a
    new version of that deal, Y right after the change waits. Then the deal is done: the gold and the thing
    change hands at once, the window goes, Lobart answers."""
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    await link.wait_world()
    if link.session is None:
        await wait_message(link, Session, timeout=10)
    await asyncio.sleep(1)
    hero = ["inst", "HERO"]

    async def has(item: str) -> int:
        return int(await link.evaluate(["call", "NPC_HASITEMS", hero, ["sym", item]]) or 0)

    await link.debug("skip_important", near=seller)  # his important dialog would start as the hero comes
    await link.debug("player_goto", to=seller, distance=250)
    talk.reflexes = Reflexes(FixedDecider({"posture": "listen"}))
    if not await _until(lambda: seller in talk.people, 20):
        check("near", False, seller)
        return _logged(results)
    name = talk.people[seller].name

    # something he has to sell (an apple if nothing), and 10 gold on the hero
    snap = await link.snapshot(seller, fresh=True)
    wares = [i for i in snap.npc.inventory if not i.equipped and i.item != GOLD and i.count > 0]
    if not wares:
        await link.story_effect(seller, [{"do": "give", "item": "ITFO_APPLE", "count": 2, "to": "self"}])
        snap = await link.snapshot(seller, fresh=True)
        wares = [i for i in snap.npc.inventory if not i.equipped and i.item != GOLD and i.count > 0]
    ware = wares[0]
    gold = await has(GOLD)
    if gold > 10:
        await link.story_effect(seller, [{"do": "give", "item": GOLD, "count": gold - 10, "to": "self"}])
    elif gold < 10:
        await link.story_effect(seller, [{"do": "give", "item": GOLD, "count": 10 - gold}])
    had = await has(ware.item)

    if isinstance(dialogue.llm, FakeBackend):

        def offer(price: int) -> ToolCall:
            terms = {
                "npc_gives": [{"item": ware.item, "count": 1}],
                "hero_gives": [{"item": GOLD, "count": price}],
            }
            return ToolCall("propose_deal", terms, f"deal{price}")

        dialogue.llm = ClaimScript(
            said=[
                ("продай", FakeStep(f"{ware.name}? Пятьдесят золотых.", [offer(50)])),
                ("отказался", FakeStep("Ну, как знаешь. Сорок?", [offer(40)])),
                ("дорого", FakeStep("Ладно, тридцать — и по рукам.", [offer(30)])),
            ],
            told=[],
            default="По рукам. Хорошая вещь, не пожалеешь.",
            only=name,
        )

    await link.debug("type", text=f"{name}, продай мне {ware.name}.")
    ok = await _until(lambda: seller in deals.open, 30)
    window = await link.deal_window() or {}
    check("shown", ok and window.get("version") == 1, window)
    if not ok:
        return _logged(results)
    first = deals.open[seller]

    # 10 gold of 50: Y waits, the window marks the gold
    await asyncio.sleep(1.2)
    pressed = await link.press_deal()
    window = await link.deal_window() or {}
    check("waits_gold", pressed == "missing" and window.get("missing") == [GOLD], f"{pressed}, {window}")

    # he declines (N): the window goes at once; Lobart hears of it and offers it for 40
    declined = await link.decline_deal()
    window = await link.deal_window()
    check("declined", declined == "declined" and window is None and first.state == "declined", declined)
    ok = await _until(lambda: seller in deals.open and deals.open[seller].id != first.id, 30)
    window = await link.deal_window() or {}
    check("offered_again", ok and window.get("hero_gives") == [gold_text(40)], window)
    if not ok:
        return _logged(results)
    second = deals.open[seller]

    # 40 gold, and he haggles it down to 30: the same window, a new version, Y waits a moment
    await link.story_effect(seller, [{"do": "give", "item": GOLD, "count": 30}])
    await link.debug("type", text=f"{name}, это дорого, давай за тридцать.")
    ok = await _until(lambda: seller in deals.open and deals.open[seller].version == 2, 30)
    window = await link.deal_window() or {}
    check("haggled", ok and window.get("deal") == second.id and window.get("version") == 2, window)
    pressed = await link.press_deal()
    check("waits_change", pressed == "changed", pressed)

    await asyncio.sleep(1.2)
    pressed = await link.press_deal()
    done = await _until(lambda: bool(deals.chronicle.facts("deal")), 20)
    check("accepted", pressed == "sent" and done, pressed)
    left, got = await has(GOLD), await has(ware.item)
    check("changed_hands", left == 10 and got == had + 1, f"gold 40 -> {left}, {ware.item} {had} -> {got}")
    window = await link.deal_window()
    check("window_gone", window is None and seller not in deals.open, window)
    conv = dialogue.conversations.get(seller)
    answered = await _until(lambda: conv is not None and "assistant" == conv.memory.entries[-1].role, 30)
    check("answered", answered, conv.memory.entries[-1].content if conv and conv.memory.entries else "")
    return _logged(results)


async def contract(
    link: GameLink, dialogue: DialogueManager, talk: TalkDesk, deals: Deals, npc: str = "NONE_100_XARDAS"
) -> dict[str, str]:
    """Contracts (docs/contracts.md) on the game, with Xardas (scripted unless LLM=chatgpt).
    1. The hero asks for experience: Xardas gives 500 of it and a rune from his stash (he carries none) for
       1 gold. The hero's experience grows, the rune is his, the stash has one less.
    2. The hero asks for a task: kill a wolf, for strength +5 (his learning points, as a teacher takes them)
       and two potions from the stash. A wolf comes and dies by the hero's hand: the game reports the kill,
       the task is done. The hero speaks to Xardas: the code pays before he answers, and he knows it."""
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    await link.wait_world()
    if link.session is None:
        await wait_message(link, Session, timeout=10)
    await asyncio.sleep(1)
    hero = ["inst", "HERO"]

    async def field(name: str, index: int | None = None) -> int:
        expr: list[Any] = ["field", hero, name] if index is None else ["field", hero, name, index]
        return int(await link.evaluate(expr) or 0)

    async def has(item: str) -> int:
        return int(await link.evaluate(["call", "NPC_HASITEMS", hero, ["sym", item]]) or 0)

    await link.debug("skip_important", near=npc)
    await link.debug("player_goto", to=npc, distance=250)
    talk.reflexes = Reflexes(FixedDecider({"posture": "listen"}))
    if not await _until(lambda: npc in talk.people, 20):
        check("near", False, npc)
        return _logged(results)
    name = talk.people[npc].name
    rune, potion, strength = "ITRU_SUMSKEL", "ITPO_HEALTH_02", 4  # ATR_STRENGTH
    if await has(GOLD) < 1:
        await link.story_effect(npc, [{"do": "give", "item": GOLD, "count": 1}])

    if isinstance(dialogue.llm, FakeBackend):

        def terms(**more: Any) -> dict[str, Any]:
            return {
                "npc_gives": [],
                "hero_gives": [],
                "forgive": [],
                "silence": [],
                "hero_later": [],
                "repay": [],
                "npc_xp": 0,
                "npc_teach": [],
                "task": {"kill": [], "bring": [], "days": 0},
                "reward": {"npc_gives": [], "xp": 0, "teach": []},
                **more,
            }

        now = terms(
            npc_gives=[{"item": rune, "count": 1}], hero_gives=[{"item": GOLD, "count": 1}], npc_xp=500
        )
        task = terms(
            task={"kill": [{"target": "GIL_WOLF", "count": 1}], "bring": [], "days": 0},
            reward={
                "npc_gives": [{"item": potion, "count": 2}],
                "xp": 0,
                "teach": [{"what": "attribute", "arg": "ATR_STRENGTH", "count": 5}],
            },
        )
        dialogue.llm = ClaimScript(
            said=[
                ("опыта", FakeStep("Опыт и руну — за один золотой.", [ToolCall("propose_deal", now, "now")])),
                (
                    "поручени",
                    FakeStep("Убей волка — научу тебя силе.", [ToolCall("propose_deal", task, "task")]),
                ),
            ],
            told=[],
            default="Хорошо.",
            only=name,
        )

    # 1. experience and a rune from the stash, now
    exp, lp, runes = await field("EXP"), await field("LP"), await has(rune)
    await link.debug("type", text=f"{name}, дай мне опыта.")
    ok = await _until(lambda: npc in deals.open, 30)
    window = await link.deal_window() or {}
    check("shown_now", ok and "Опыт +500" in window.get("npc_gives", []), window)
    if not ok:
        return _logged(results)
    await asyncio.sleep(1.2)
    pressed = await link.press_deal()
    done = await _until(lambda: bool(deals.chronicle.facts("deal")), 20)
    check("accepted_now", pressed == "sent" and done, pressed)
    exp2, lp2, runes2 = await field("EXP"), await field("LP"), await has(rune)
    check("experience", exp2 == exp + 500, f"exp {exp} -> {exp2}, lp {lp} -> {lp2}")
    check(
        "rune_from_stash", runes2 == runes + 1 and rune not in deals.stash.left(npc), f"{runes} -> {runes2}"
    )

    # 2. a task: a wolf for strength and potions
    deals_before = len(deals.chronicle.facts("deal"))
    await asyncio.sleep(2)
    await link.debug("type", text=f"{name}, дай мне поручение.")
    ok = await _until(lambda: npc in deals.open, 30)
    window = await link.deal_window() or {}
    check("shown_task", ok and any("волки" in t for t in window.get("hero_gives", [])), window)
    if not ok:
        return _logged(results)
    await asyncio.sleep(1.2)
    pressed = await link.press_deal()
    done = await _until(lambda: len(deals.chronicle.facts("deal")) > deals_before, 20)
    check("contract", pressed == "sent" and done and bool(deals.chronicle.facts("contract")), pressed)

    strength1, lp3, potions = await field("ATTRIBUTE", strength), await field("LP"), await has(potion)
    await link.debug("spawn", instance="WOLF", near=npc, distance=600)
    await asyncio.sleep(1)
    killed = await link.debug("kill", instance="WOLF", near=npc, by="PLAYER")
    done = await _until(lambda: bool(deals.chronicle.facts("contract_done")), 15)
    check("wolf_counted", done, f"kill: {killed}, kills: {deals.chronicle.facts('contract_kill')}")
    check("not_paid_yet", await has(potion) == potions)

    await asyncio.sleep(2)
    await link.debug("type", text=f"{name}, волк мёртв.")
    paid = await _until(lambda: bool(deals.chronicle.facts("contract_paid")), 30)
    strength2, lp4, potions2 = await field("ATTRIBUTE", strength), await field("LP"), await has(potion)
    check(
        "paid",
        paid and strength2 == strength1 + 5 and lp4 < lp3 and potions2 == potions + 2,
        f"str {strength1} -> {strength2}, lp {lp3} -> {lp4}, potions {potions} -> {potions2}",
    )
    conv = dialogue.conversations.get(npc)
    knows = conv is not None and any("исполнил твоё поручение" in e.content for e in conv.memory.entries)
    check("npc_knows", knows)
    return _logged(results)


async def deal_debt(
    link: GameLink,
    dialogue: DialogueManager,
    talk: TalkDesk,
    deals: Deals,
    lender: str = LOBART,
) -> dict[str, str]:
    """A debt (docs/npc-tools.md, wave 1 step 2), on the game. Lobart (scripted unless LLM=chatgpt) lends the
    hero 20 gold for 25 back by the end of the next day. The clock goes two days on: the debt is a broken
    word, Lobart takes nothing on his word any more, and it goes round as a rumor. Then the hero pays it
    back, late."""
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    await link.wait_world()
    if link.session is None:
        await wait_message(link, Session, timeout=10)
    await asyncio.sleep(1)
    hero = ["inst", "HERO"]

    async def has(item: str) -> int:
        return int(await link.evaluate(["call", "NPC_HASITEMS", hero, ["sym", item]]) or 0)

    async def come() -> str | None:
        await link.debug("teleport", near=lender, distance=250)
        talk.reflexes = Reflexes(FixedDecider({"posture": "listen"}))
        ready = await _until(
            lambda: (entry := talk.people.get(lender)) is not None
            and entry.distance <= 600 and entry.earshot <= 600 and entry.activity != "down",
            20,
        )
        return talk.people[lender].name if ready else None

    await link.debug("skip_important", near=lender)  # his important dialog would start as the hero comes
    await link.attach(lender)  # gold to lend
    await link.act(lender, "debug_add_item", item=GOLD, count=100)
    await link.detach(lender)
    name = await come()
    if name is None:
        check("near", False, lender)
        return _logged(results)
    script: ClaimScript | None = None
    lend = {
        "npc_gives": [{"item": GOLD, "count": 20}],
        "hero_gives": [],
        "forgive": [],
        "silence": [],
        "hero_later": [{"item": GOLD, "count": 25, "days": 1}],
        "repay": [],
    }
    if isinstance(dialogue.llm, FakeBackend):
        offer = FakeStep(
            "Двадцать дам. Вернёшь двадцать пять — к завтрашнему вечеру.",
            [ToolCall("propose_deal", lend, "lend")],
        )
        script = dialogue.llm = ClaimScript(
            said=[("одолжи", offer)], told=[], default="Смотри не подведи.", only=name
        )

    # he borrows: 20 now, 25 back by the end of the next day
    gold = await has(GOLD)
    await link.debug("type", text=f"{name}, одолжи мне двадцать золотых, я верну.")
    ok = await _until(lambda: lender in deals.open and bool(deals.open[lender].later), 60)
    window = await link.deal_window() or {}
    check("offered", ok, window.get("hero_gives"))
    if not ok:
        return _logged(results)
    await asyncio.sleep(1.2)
    pressed = await link.press_deal()
    ok = await _until(lambda: any(d.npc == lender for d in deals.debts.values()), 20)
    debt = next((d for d in deals.debts.values() if d.npc == lender), None)
    got = await has(GOLD)
    check(
        "lent",
        pressed == "sent" and ok and got == gold + 20,
        f"{gold} -> {got} gold, {debt.what() if debt else '-'}",
    )
    if debt is None:
        return _logged(results)

    # two days on: past its day, a broken word
    await link.debug("time", hour=7, minute=0)
    await link.debug("time", hour=6, minute=59)
    ok = await _until(lambda: debt.broken, 40)
    trust = Trust(deals.chronicle)
    check("broken", ok and trust.broken(lender), (trust.situation(lender) or "").splitlines()[-1:])

    # the morning goes by: it goes round
    await link.debug("time", hour=12, minute=0)
    ok = await _until(lambda: any(f.get("kind") == "broken" for f in deals.chronicle.facts("rumor")), 60)
    heard = [
        f.get("to_name") or f.get("to") for f in deals.chronicle.facts("rumor") if f.get("kind") == "broken"
    ]
    check("rumor", ok, heard[:3])

    # late, he pays it back; his word stays broken
    if await has(GOLD) < 25:
        await link.story_effect(lender, [{"do": "give", "item": GOLD, "count": 30}])
    name = await come() or name
    if script is not None:
        back = dict(lend, npc_gives=[], hero_later=[], repay=[debt.id])
        script.said.append(
            ("долг", FakeStep("Наконец-то. Давай сюда.", [ToolCall("propose_deal", back, "repay")]))
        )
        script.only = name
    before = await has(GOLD)
    await link.debug("type", text=f"{name}, вот твой долг, прости, что поздно.")
    ok = await _until(lambda: lender in deals.open and bool(deals.open[lender].repay), 60)
    if ok:
        await asyncio.sleep(1.2)
        await link.press_deal()
        ok = await _until(lambda: debt.paid, 20)
    after = await has(GOLD)
    check("repaid", ok and after == before - 25, f"{before} -> {after} gold")
    check("word_stays", Trust(deals.chronicle).broken(lender))
    return _logged(results)


async def xardas_money(
    link: GameLink,
    dialogue: DialogueManager,
    talk: TalkDesk,
    deals: Deals,
    npc: str = "NONE_100_XARDAS",
) -> dict[str, str]:
    """Xardas's money on a new game: gifts and loans when asked, within his purse limits.

    Fake mode gives 30, lends 150 for three days and refuses another gift of 100. Live mode first
    checks that he offers no money unasked, then asks for 200 and accepts the terms he chooses.
    """
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    await link.wait_world()
    if link.session is None:
        await wait_message(link, Session, timeout=10)
    await asyncio.sleep(1)
    hero = ["inst", "HERO"]

    async def gold() -> int:
        return int(await link.evaluate(["call", "NPC_HASITEMS", hero, ["sym", GOLD]]) or 0)

    def cash_offer() -> bool:
        deal = deals.open.get(npc)
        return deal is not None and (
            any(g.item == GOLD for g in deal.npc_gives)
            or bool(deal.npc_later and any(g.item == GOLD for g in deal.npc_later.gives))
        )

    def said() -> str:
        conv = dialogue.conversations.get(npc)
        return " | ".join(e.content for e in conv.memory.entries if e.role == "assistant") if conv else ""

    await link.debug("skip_important", near=npc)
    await link.debug("player_goto", to=npc, distance=250)
    talk.reflexes = Reflexes(FixedDecider({"posture": "listen"}))
    if not await _until(lambda: npc in talk.people and talk.people[npc].distance <= 600, 20):
        check("near", False, npc)
        return _logged(results)
    name = talk.people[npc].name

    if isinstance(dialogue.llm, FakeBackend):

        def offer(amount: int, loan: int = 0) -> ToolCall:
            return ToolCall(
                "propose_deal",
                {
                    "npc_gives": [{"item": GOLD, "count": amount}],
                    "hero_gives": [],
                    "hero_later": [{"item": GOLD, "count": loan, "days": 3}] if loan else [],
                },
                f"money{amount}_{loan}",
            )

        script = dialogue.llm = ClaimScript(
            said=[
                ("немного золота", FakeStep("Возьми тридцать золотых.", [offer(30)])),
                ("одолжи", FakeStep("Сто пятьдесят. Вернёшь через три дня.", [offer(150, 150)])),
                ("сто золотых даром", FakeStep("Дам ещё сто даром.", [offer(100)])),
            ],
            told=[],
            default="Хорошо.",
            only=name,
        )
        for key, line, amount, loan in (
            ("gift", f"{name}, дай немного золота.", 30, 0),
            ("loan", f"{name}, одолжи сто пятьдесят золотых на три дня.", 150, 150),
        ):
            before = await gold()
            day = int((await link.story_state()).get("day", 0))
            await link.debug("type", text=line)
            if not await _until(lambda: npc in deals.open, 30):
                check(key, False, f"no offer; {script.outputs}")
                return _logged(results)
            deal = deals.open[npc]
            offered = sum(g.count for g in deal.npc_gives if g.item == GOLD)
            later = sum(x.give.count for x in deal.later if x.give.item == GOLD)
            due = all(x.due == day + 3 for x in deal.later)
            await asyncio.sleep(1.2)
            pressed = await link.press_deal()
            accepted = await _until(lambda deal=deal: deal.state == "done", 20)
            after = await gold()
            debt = [d for d in deals.debts.values() if d.npc == npc and not d.paid]
            owed = sum(d.give.count for d in debt if d.give.item == GOLD)
            ok = (
                offered == amount and later == loan and due and pressed == "sent" and accepted
                and after == before + amount and owed == loan
            )
            check(key, ok, f"gold {before} -> {after}, debt {owed}; {deal.terms()}")
            if not ok:
                return _logged(results)
            await asyncio.sleep(2)

        before, facts = await gold(), len(deals.chronicle.facts("deal"))
        outputs = len(script.outputs)
        await link.debug("type", text=f"{name}, дай ещё сто золотых даром.")
        expected = (
            "Отклонено: даром ты даёшь герою не больше 50 золотых за всё время, осталось 20. "
            "Больше — только в долг: что он вернёт, впиши в hero_later."
        )
        rejected = await _until(lambda: any(
            result.startswith(expected) for result in script.outputs[outputs:]
        ), 30)
        window = await link.deal_window()
        check(
            "over",
            rejected and npc not in deals.open and window is None and await gold() == before
            and len(deals.chronicle.facts("deal")) == facts,
            script.outputs[outputs:],
        )
        return _logged(results)

    await link.debug("type", text=f"{name}, что мне теперь делать?")
    unasked = not await _until(cash_offer, 20)
    check("unasked", unasked, f"20 s; {said()}")
    if not unasked:
        return _logged(results)

    for line in (
        f"{name}, мне нужно двести золотых: сто стражникам у ворот и сто на первое время. "
        "Дашь или одолжишь?",
        f"{name}, согласен на твои условия, давай по рукам.",
        f"{name}, хорошо, я согласен. Оформим сделку.",
    ):
        await link.debug("type", text=line)
        if await _until(cash_offer, 45):
            break
    conv = dialogue.conversations.get(npc)
    reply = conv.reply if conv else None
    settled = reply is None or await _until(reply.done, 45)
    deal = deals.open.get(npc)
    offered = cash_offer() and settled
    check("offered", offered, f"{said()}; {deal.terms() if deal else 'no offer'}")
    if not offered or deal is None:
        return _logged(results)
    before = await gold()
    loan = sum(x.give.count for x in deal.later if x.give.item == GOLD)
    given = sum(g.count for g in deal.npc_gives if g.item == GOLD)
    paid = sum(g.count for g in deal.hero_gives if g.item == GOLD)
    paid += sum(d.give.count for d in deal.repay if d.give.item == GOLD)
    debt_ids = set(deals.debts)
    await asyncio.sleep(1.2)
    pressed = await link.press_deal()
    accepted = await _until(lambda: deal.state == "done", 20)
    after = await gold()
    owed = sum(
        d.give.count for key, d in deals.debts.items()
        if key not in debt_ids and d.npc == npc and d.give.item == GOLD and not d.paid
    )
    check(
        "accepted",
        pressed == "sent" and accepted and given > paid
        and after == before + given - paid and owed == loan,
        f"gold {before} -> {after}, debt {owed}; {said()}; {deal.terms()}",
    )
    return _logged(results)


CANTHAR, LUTERO = "VLK_468_CANTHAR", "VLK_404_LUTERO"


async def haggle(
    link: GameLink,
    dialogue: DialogueManager,
    talk: TalkDesk,
    deals: Deals,
    npcs: tuple[str, str] = (CANTHAR, LUTERO),
    ware: str = "ITMI_SILVERCUP",
) -> dict[str, str]:
    """Character in a deal (docs/npc-tools.md, wave 1 step 3), live only (LLM=chatgpt). The hero offers the
    same silver cup to two traders who would buy it, a greedy one and a fairer one by their character cards
    (Canthar, Lutero), and asks for more, as a player would: he agrees in words when a price is named. What
    each offers first and after the haggling is logged; the greedy one should offer less."""
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    if isinstance(dialogue.llm, FakeBackend):
        check("live", False, "run with LLM=chatgpt")
        return _logged(results)
    await link.wait_world()
    if link.session is None:
        await wait_message(link, Session, timeout=10)
    await asyncio.sleep(1)
    talk.reflexes = Reflexes(FixedDecider({"posture": "listen"}))
    offers: dict[str, list[int]] = {}

    def buying(npc: str, version: int) -> int | None:
        deal = deals.open.get(npc)
        if deal is None or deal.version < version or not any(g.item == ware for g in deal.hero_gives):
            return None
        return sum(g.count for g in deal.npc_gives if g.item == GOLD)

    for npc in npcs:
        await link.attach(npc)
        await link.act(npc, "debug_add_item", item=GOLD, count=300)
        await link.act(npc, "debug_add_item", item=ware, count=1)
        await link.detach(npc)
        await link.story_effect(npc, [{"do": "give", "item": ware, "count": 1}])  # the cup is the hero's
        await link.debug("skip_important", near=npc)
        await link.debug("player_goto", to=npc, distance=250)
        # the roster must have him close: else the words would go to the one the hero talked to before
        if not await _until(lambda npc=npc: npc in talk.people and talk.people[npc].distance <= 600, 20):
            check(f"{npc}_near", False)
            continue
        name = talk.people[npc].name
        got: list[int] = []

        async def say_until(lines: list[str], version: int, npc: str = npc) -> int | None:
            """Say the lines one by one until the npc offers a deal of `version` or later."""
            for line in lines:
                await link.debug("type", text=line)
                if await _until(lambda: buying(npc, version) is not None, 45):
                    return buying(npc, version)
            return None

        first = await say_until(
            [
                f"{name}, купишь у меня серебряный кубок? Сколько дашь?",
                f"{name}, назови цену.",
                f"{name}, согласен, давай по рукам.",
            ],
            1,
        )
        if first is not None:
            got.append(first)
            version = deals.open[npc].version
            more = await say_until(
                [
                    f"{name}, маловато. Кубок серебряный, накинь ещё.",
                    f"{name}, ладно, на твою новую цену согласен.",
                ],
                version + 1,
            )
            if more is not None:
                got.append(more)
            await link.decline_deal()
        offers[npc] = got
        check(f"{npc}_offered", bool(got), got)
        await asyncio.sleep(2)
    cards = dialogue.personas.cards
    greedy, generous = sorted(npcs, key=lambda n: -(cards[n].greed if n in cards else 3))
    a, b = offers.get(greedy, []), offers.get(generous, [])
    detail = f"{greedy} (greed {cards[greedy].greed if greedy in cards else '?'}): {a}; {generous}: {b}"
    check("greedy_pays_less", bool(a) and bool(b) and a[0] < b[0], detail)
    return _logged(results)


async def deal_demo(link: GameLink, deals: Deals, npc: str) -> dict[str, str]:
    """The deal window on screen for the test bench (scripts/bench/deal.bench): the npc offers a thing it has
    for 50 gold the hero lacks, a moment later for 30 he has; it stays a while, then the hero accepts."""
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    await link.wait_world()
    if link.session is None:
        await wait_message(link, Session, timeout=10)
    await asyncio.sleep(6)  # the bench sets its camera
    npc, hero = npc.upper(), ["inst", "HERO"]
    # a potion to sell, and gold to leave the hero 40
    await link.attach(npc)
    await link.act(npc, "debug_add_item", item="ITPO_HEALTH_01", count=2)
    await link.act(npc, "debug_add_item", item=GOLD, count=100)
    await link.detach(npc)
    gold = int(await link.evaluate(["call", "NPC_HASITEMS", hero, ["sym", GOLD]]) or 0)
    if gold != 40:
        to = {"to": "self"} if gold > 40 else {}
        await link.story_effect(npc, [{"do": "give", "item": GOLD, "count": abs(gold - 40), **to}])
    snap = await link.snapshot(npc, fresh=True)
    wares = [i for i in snap.npc.inventory if not i.equipped and i.item != GOLD and i.count > 0]
    check("stocked", bool(wares), [i.item for i in snap.npc.inventory])
    if not wares:
        return _logged(results)

    def terms(price: int) -> dict[str, Any]:
        return {
            "npc_gives": [{"item": wares[0].item, "count": 1}],
            "hero_gives": [{"item": GOLD, "count": price}],
        }

    name = snap.npc.name or npc
    first = await deals.propose(npc, name, terms(50), snap)
    check("offered", first.status == "accepted", first.output)
    await asyncio.sleep(5)
    second = await deals.propose(npc, name, terms(30), snap)
    check("haggled", second.status == "accepted", second.output)
    await asyncio.sleep(6)
    pressed = await link.press_deal()
    check(
        "accepted",
        pressed == "sent" and await _until(lambda: bool(deals.chronicle.facts("deal")), 10),
        pressed,
    )
    await asyncio.sleep(3)
    return _logged(results)
