from __future__ import annotations

import asyncio
import logging
import time
from typing import Any

from ..brain.reflex import Reflexes
from ..deals import GOLD, Deals, _parse
from ..dialogue import DialogueManager
from ..game import GameError, GameLink
from ..llm import ToolCall
from ..protocol import Session
from ..talk import TalkDesk
from ..trace import read as read_trace
from ..trust import Trust
from .common import LOBART, FixedDecider, _logged, _until, model_off, wait_message
from .live import LiveCase, LiveSuite

SCENARIOS = {
    "contract": ("GIVEN", {"save": False, "timeout": 240}),
    "deal": ("GIVEN", {"save": True, "timeout": 240}),
    "debt": ("GIVEN", {"save": True, "timeout": 240}),
    "deal_demo": ("GIVEN", {"save": False, "timeout": 240}),
    "haggle": ("LIVE", None),
    "xardas_money": ("GIVEN", {"save": False, "timeout": 240}),
}

log = logging.getLogger("gothic_ai.scenarios")


async def deal_window(
    link: GameLink, dialogue: DialogueManager, talk: TalkDesk, deals: Deals, seller: str = LOBART
) -> dict[str, str]:
    """The deal window and exchange with given prices: 50, then 40 after N, then 30 after haggling.
    Y waits for missing gold and changed terms; accepting exchanges the gold and item together.
    """
    off = model_off(dialogue)
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
    log.info("given: дано: решающая модель оставляет NPC слушать героя")
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

    def offer(price: int) -> ToolCall:
        return ToolCall(
            "propose_deal",
            {
                "npc_gives": [{"item": ware.item, "count": 1}],
                "hero_gives": [{"item": GOLD, "count": price}],
            },
        )

    for cue, price, reason in (
        ("продай", 50, "дано: Лобарт предлагает товар за 50 золотых"),
        ("отказался", 40, "дано: Лобарт после отказа предлагает товар за 40 золотых"),
        ("дорого", 30, "дано: Лобарт после торга предлагает товар за 30 золотых"),
    ):
        off.given(reason, who=seller, when=cue, calls=[offer(price)])
        off.silent("дано: Лобарт молчит (предложение сделки передано)", who=seller, when=cue)
    off.silent("дано: Лобарт молчит (сделка состоялась)", who=seller, when="сделка состоялась")

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
    check("offered_again", ok, window)
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
    if conv is not None and conv.reply is not None:
        await asyncio.wait({conv.reply}, timeout=30)
    return _logged(results)


async def contract(
    link: GameLink, dialogue: DialogueManager, talk: TalkDesk, deals: Deals, npc: str = "NONE_100_XARDAS"
) -> dict[str, str]:
    """Contracts (docs/contracts.md) on the game, with Xardas's offers given in advance.
    1. The hero asks for experience: Xardas gives 500 of it and a rune from his stash (he carries none) for
       1 gold. The hero's experience grows, the rune is his, the stash has one less.
    2. The hero asks for a task: kill a wolf, for strength +5 (his learning points, as a teacher takes them)
       and two potions from the stash. A wolf comes and dies by the hero's hand: the game reports the kill,
       the task is done. The hero speaks to Xardas: the code pays before he answers, and he knows it."""
    off = model_off(dialogue)
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
    log.info("given: дано: решающая модель оставляет NPC слушать героя")
    talk.reflexes = Reflexes(FixedDecider({"posture": "listen"}))
    if not await _until(lambda: npc in talk.people, 20):
        check("near", False, npc)
        return _logged(results)
    name = talk.people[npc].name
    rune, potion, strength = "ITRU_SUMSKEL", "ITPO_HEALTH_02", 4  # ATR_STRENGTH
    if await has(GOLD) < 1:
        await link.story_effect(npc, [{"do": "give", "item": GOLD, "count": 1}])

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
    off.given(
        "дано: Ксардас предлагает 500 опыта и руну за золотой", who=npc, when="опыта",
        calls=[ToolCall("propose_deal", now)],
    )
    off.silent("дано: Ксардас молчит (предложение опыта и руны передано)", who=npc, when="опыта")
    off.given(
        "дано: Ксардас предлагает поручение убить волка", who=npc, when="поручени",
        calls=[ToolCall("propose_deal", task)],
    )
    off.silent("дано: Ксардас молчит (предложение поручения передано)", who=npc, when="поручени")
    off.silent(
        "дано: Ксардас молчит (сделка состоялась)", who=npc, when="сделка состоялась", times=2,
    )
    off.silent("дано: Ксардас молчит (герой убил волка)", who=npc, when="Событие: герой убил: Волк")
    off.silent("дано: Ксардас молчит (герой принёс весть о волке)", who=npc, when="волк мертв")

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
    if conv is not None and conv.reply is not None:
        await asyncio.wait({conv.reply}, timeout=30)
    return _logged(results)


async def deal_debt(
    link: GameLink,
    dialogue: DialogueManager,
    talk: TalkDesk,
    deals: Deals,
    lender: str = LOBART,
) -> dict[str, str]:
    """A debt (docs/npc-tools.md, wave 1 step 2), on the game. Given: Lobart lends the
    hero 20 gold for 25 back by the end of the next day. The clock goes two days on: the debt is a broken
    word, Lobart takes nothing on his word any more, and it goes round as a rumor. Then the hero pays it
    back, late."""
    off = model_off(dialogue)
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

    log.info("given: дано: решающая модель оставляет NPC слушать героя")

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
    lend = {
        "npc_gives": [{"item": GOLD, "count": 20}],
        "hero_gives": [],
        "forgive": [],
        "silence": [],
        "hero_later": [{"item": GOLD, "count": 25, "days": 1}],
        "repay": [],
    }
    off.given(
        "дано: Лобарт даёт в долг 20 золотых за 25 через день", who=lender, when="одолжи",
        calls=[ToolCall("propose_deal", lend)],
    )
    off.silent("дано: Лобарт молчит (предложение займа передано)", who=lender, when="одолжи")
    off.silent(
        "дано: Лобарт молчит (сделка состоялась)", who=lender, when="сделка состоялась", times=2,
    )

    # he borrows: 20 now, 25 back by the end of the next day
    gold = await has(GOLD)
    await link.debug("type", text=f"{name}, одолжи мне двадцать золотых, я верну.")
    ok = await _until(lambda: lender in deals.open and bool(deals.open[lender].later), 60)
    if not ok:
        check("lent", False, "no loan window")
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
    back = dict(lend, npc_gives=[], hero_later=[], repay=[debt.id])
    off.given(
        "дано: Лобарт принимает возврат долга сделкой repay", who=lender, when="долг",
        calls=[ToolCall("propose_deal", back)],
    )
    off.silent("дано: Лобарт молчит (предложение возврата долга передано)", who=lender, when="долг")
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
    conv = dialogue.conversations.get(lender)
    if conv is not None and conv.reply is not None:
        await asyncio.wait({conv.reply}, timeout=30)
    return _logged(results)


async def xardas_money(
    link: GameLink,
    dialogue: DialogueManager,
    talk: TalkDesk,
    deals: Deals,
    npc: str = "NONE_100_XARDAS",
    *, live: LiveCase | None = None,
) -> dict[str, str]:
    """Given gifts and a loan exercise transfers, debt terms and Xardas's purse limit."""
    off = model_off(dialogue) if live is None else None
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

    await link.debug("skip_important", near=npc)
    await link.debug("player_goto", to=npc, distance=250)
    if live is None:
        log.info("given: дано: решающая модель оставляет NPC слушать героя")
    if live is None or live.spec.name != "money_unasked":
        talk.reflexes = Reflexes(FixedDecider({"posture": "listen"}))
    if not await _until(lambda: npc in talk.people and talk.people[npc].distance <= 600, 20):
        check("near", False, npc)
        return _logged(results)
    name = talk.people[npc].name
    if live is not None:
        purse = deals.purse.rules.get(npc, {})
        before = await gold()
        request = None
        if live.spec.name == "money_unasked":
            await asyncio.sleep(20)
            cutoff = round(time.time(), 3)
            conv = dialogue.conversations.get(npc)
            reply = conv.reply if conv is not None else None
            effects_settled = await _until(
                lambda: (reply is None or reply.done()) and (conv is None or not conv.actions),
                90,
            )
        else:
            request = f"{name}, дай мне двести золотых."
            await link.debug("type", text=request)
            effects_settled = await _until(
                lambda: bool(live.turns(npc, request)) and (
                    not any(call.get("name") == "propose_deal" and call.get("outcome") == "accepted"
                            for call in live.calls(npc)) or npc in deals.open
                ),
                90,
            )
        rows = live.turns(npc, request)
        records = rows
        if request is None:
            # Turns begun in the observation window may close after it; later turns are unrelated.
            records = [row for row in live.records if row.get("npc") == npc
                       and row.get("t", cutoff) <= cutoff]
            rows = [row for row in records if row.get("status") in {"completed", "step_cap"}]
        calls = [call for row in records for step in row.get("steps", [])
                 for call in step.get("calls", [])]
        money_calls = []
        for call in calls:
            if call.get("name") != "propose_deal":
                continue
            args = call.get("arguments", {})
            gives, _ = _parse(args.get("npc_gives"))
            later = args.get("npc_later")
            deferred, _ = _parse(later.get("items")) if isinstance(later, dict) else ({}, "")
            if (gives or {}).get(GOLD, 0) or (deferred or {}).get(GOLD, 0):
                money_calls.append(call)
        deal = deals.open.get(npc)
        offered = bool(
            deal is not None
            and any(g.item == GOLD and g.count > 0 for g in (
                *deal.npc_gives, *(deal.npc_later.gives if deal.npc_later else ()),
            ))
            and any(call.get("outcome") == "accepted" for call in money_calls)
        )
        outcome = (
            "failure" if money_calls else "success" if effects_settled else "no_decision"
        ) if live.spec.name == "money_unasked" else (
            "success" if offered else "failure" if rows else "no_decision"
        )
        live.finish(outcome, evidence={
            "decision": "money_offered" if money_calls else "no_money_offer",
            "observation_seconds": 20 if live.spec.name == "money_unasked" else 90,
            "effects_settled": effects_settled,
            "calls": calls, "money_calls": money_calls, "purse": purse,
            "terms": deal.terms() if deal is not None else None,
            "gold_before": before, "gold_after": await gold(),
            "words": [step.get("text", "") for row in rows
                      for step in row.get("steps", []) if step.get("text")],
        })
        return _logged(results)

    assert off is not None

    def offer(amount: int, loan: int = 0) -> ToolCall:
        return ToolCall(
            "propose_deal",
            {
                "npc_gives": [{"item": GOLD, "count": amount}],
                "hero_gives": [],
                "hero_later": [{"item": GOLD, "count": loan, "days": 3}] if loan else [],
            },
        )

    for cue, amount, loan, reason in (
        ("немного золота", 30, 0, "дано: Ксардас предлагает подарок 30 золотых"),
        ("одолжи", 150, 150, "дано: Ксардас предлагает заём 150 золотых на три дня"),
        ("сто золотых даром", 100, 0, "дано: Ксардас предлагает подарок 100 сверх предела кошелька"),
    ):
        off.given(reason, who=npc, when=cue, calls=[offer(amount, loan)])
        off.silent("дано: Ксардас молчит (предложение денег проверено)", who=npc, when=cue)
    off.silent(
        "дано: Ксардас молчит (сделка состоялась)", who=npc, when="сделка состоялась", times=2,
    )
    for key, line, amount, loan in (
        ("gift", f"{name}, дай немного золота.", 30, 0),
        ("loan", f"{name}, одолжи сто пятьдесят золотых на три дня.", 150, 150),
    ):
        before = await gold()
        day = int((await link.story_state()).get("day", 0))
        await link.debug("type", text=line)
        if not await _until(lambda: npc in deals.open, 30):
            check(key, False, "no offer")
            return _logged(results)
        deal = deals.open[npc]
        await asyncio.sleep(1.2)
        pressed = await link.press_deal()
        accepted = await _until(lambda deal=deal: deal.state == "done", 20)
        after = await gold()
        debt = [d for d in deals.debts.values() if d.npc == npc and not d.paid]
        owed = sum(d.give.count for d in debt if d.give.item == GOLD)
        due = all(d.due == day + 3 for d in debt)
        ok = pressed == "sent" and accepted and after == before + amount and owed == loan and due
        check(key, ok, f"gold {before} -> {after}, debt {owed}; {deal.terms()}")
        if not ok:
            return _logged(results)
        await asyncio.sleep(2)

    if dialogue.trace is None:
        raise RuntimeError("xardas_money requires turn traces to check the purse-limit rejection")
    before, facts = await gold(), len(deals.chronicle.facts("deal"))
    await link.debug("type", text=f"{name}, дай ещё сто золотых даром.")
    expected = (
        "Отклонено: даром ты даёшь герою не больше 50 золотых за всё время, осталось 20. "
        "Больше — только в долг: что он вернёт, впиши в hero_later."
    )

    def rejected_calls() -> list[dict[str, Any]]:
        return [
            call
            for row in read_trace(dialogue.trace.path()) if row.get("npc") == npc
            and "сто золотых даром" in row.get("trigger", {}).get("text", "")
            for step in row.get("steps", []) for call in step.get("calls", [])
            if call.get("name") == "propose_deal" and call.get("outcome") == "rejected"
            and call.get("result", "").startswith(expected)
        ]

    rejected = await _until(lambda: bool(rejected_calls()), 30)
    window = await link.deal_window()
    check(
        "over",
        rejected and npc not in deals.open and window is None and await gold() == before
        and len(deals.chronicle.facts("deal")) == facts,
        rejected_calls(),
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
    """Compare the traders' actual offers for the same silver cup, with live decisions."""
    suite = LiveSuite("haggle", dialogue.trace)
    await link.wait_world()
    if link.session is None:
        await wait_message(link, Session, timeout=10)
    await asyncio.sleep(1)
    talk.reflexes = Reflexes(FixedDecider({"posture": "listen"}))

    def buying(npc: str, version: int) -> int | None:
        deal = deals.open.get(npc)
        if (deal is None or deal.version < version or len(deal.hero_gives) != 1
                or deal.hero_gives[0].item != ware or deal.hero_gives[0].count != 1
                or not deal.npc_gives or any(g.item != GOLD for g in deal.npc_gives)):
            return None
        return sum(g.count for g in deal.npc_gives if g.item == GOLD)

    for case_id in suite.selected:
        with suite.case(case_id) as case:
            offers: dict[str, list[int]] = {}
            terms: dict[str, list[dict[str, Any]]] = {}
            for npc in npcs:
                await link.attach(npc)
                await link.act(npc, "debug_add_item", item=GOLD, count=300)
                await link.act(npc, "debug_add_item", item=ware, count=1)
                await link.detach(npc)
                await link.story_effect(npc, [{"do": "give", "item": ware, "count": 1}])
                await link.debug("skip_important", near=npc)
                await link.debug("player_goto", to=npc, distance=250)
                if not await _until(
                    lambda npc=npc: npc in talk.people and talk.people[npc].distance <= 600, 20,
                ):
                    raise GameError(f"haggle: {npc} did not come within talking distance")
                name = talk.people[npc].name
                got: list[int] = []
                terms[npc] = []

                async def say_until(
                    lines: list[str], version: int, npc: str = npc,
                    terms: dict[str, list[dict[str, Any]]] = terms,
                ) -> int | None:
                    for line in lines:
                        await link.debug("type", text=line)
                        offered = await _until(lambda: buying(npc, version) is not None, 45)
                        conv = dialogue.conversations.get(npc)
                        if conv is not None and conv.reply is not None:
                            await asyncio.wait({conv.reply}, timeout=90)
                            if not conv.reply.done():
                                conv.reply.cancel()
                                await asyncio.gather(conv.reply, return_exceptions=True)
                        if offered and (price := buying(npc, version)) is not None:
                            deal = deals.open[npc]
                            terms[npc].append({
                                "price": price,
                                "hero_gives": [
                                    give.term().model_dump(mode="json") for give in deal.hero_gives
                                ],
                                "npc_gives": [give.term().model_dump(mode="json") for give in deal.npc_gives],
                            })
                            return price
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
                    conv = dialogue.conversations.get(npc)
                    if conv is not None and conv.reply is not None:
                        await asyncio.wait({conv.reply}, timeout=90)
                offers[npc] = got
                await asyncio.sleep(2)
            canthar, lutero = (offers.get(npc, []) for npc in npcs)
            outcome = ("no_decision" if not canthar or not lutero
                       else "success" if canthar[0] < lutero[0] else "failure")
            case.finish(outcome, evidence={
                "item": ware,
                "offers": offers,
                "terms": terms,
                "compared": "first price for the same cup",
                "facts": deals.chronicle.facts("deal"),
            })
    return suite.results()


async def deal_demo(link: GameLink, deals: Deals, npc: str) -> dict[str, str]:
    """The deal window on screen for the test bench (scripts/bench/deal.bench): the npc offers a thing it has
    for 50 gold the hero lacks, a moment later for 30 he has; it stays a while, then the hero accepts."""
    off = model_off(deals.dialogue)
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    await link.wait_world()
    if link.session is None:
        await wait_message(link, Session, timeout=10)
    await asyncio.sleep(6)  # the bench sets its camera
    npc, hero = npc.upper(), ["inst", "HERO"]
    off.silent("дано: продавец молчит (демо-сделка состоялась)", who=npc, when="сделка состоялась")
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
    deals.dialogue.open(npc, name, warm=False)
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
