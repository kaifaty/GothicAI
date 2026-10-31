from types import SimpleNamespace

import pytest

from gothic_ai.contracts import Give
from gothic_ai.deals import Debt
from gothic_ai.llm import Message, ToolCall
from gothic_ai.llm.fake import FakeBackend
from gothic_ai.memory import Entry, MemoryStore
from gothic_ai.protocol import Session, Snapshot
from tests.fake_game import FakeGame
from tests.test_deals import GOLD, POTION, _setup, _shown
from tests.test_dialogue import LOBART, XARDAS, _until

UNASKED = "Отклонено: герой не просил у тебя денег, а сам ты их не предлагаешь."
GIFT = (
    "Отклонено: даром ты даёшь герою не больше 50 золотых за всё время, осталось {left}. "
    "Больше — только в долг: что он вернёт, впиши в hero_later."
)
LOAN = (
    "Отклонено: в долг — не больше 200 золотых; сейчас он должен тебе {owed}, с этой сделкой будет {after}."
)
REQUEST = ("дай немного золота",)


def gold(count):
    return [{"item": GOLD, "count": count}] if count else []


@pytest.fixture
async def purse():
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    link_cm, dialogue, chronicle, deals = await _setup(fake, FakeBackend(delay=0), store)
    dialogue.event_turn = lambda *a, **kw: None
    try:
        yield SimpleNamespace(fake=fake, dialogue=dialogue, chronicle=chronicle, deals=deals)
    finally:
        await link_cm.__aexit__(None, None, None)


async def offer(purse, amount, *, later=0, hero_words=REQUEST, **terms):
    args = {"npc_gives": gold(amount), **terms}
    if later:
        args["hero_later"] = [{"item": GOLD, "count": later, "days": 3}]
    return await purse.deals.propose(
        XARDAS,
        "Ксардас",
        args,
        purse.deals.link.snapshots[XARDAS],
        hero_words=hero_words,
    )


async def accept(purse):
    deal = purse.deals.open[XARDAS]
    await purse.deals.accept(deal.id, deal.version)


@pytest.mark.parametrize("words", [None, (), ("Что мне теперь делать?",), ("Я долго шёл",), ("Долгий путь",)])
async def test_gold_requires_request_before_window(purse, words):
    args = {"npc_gives": gold(30)}
    kwargs = {} if words is None else {"hero_words": words}
    out = await purse.deals.propose(XARDAS, "Ксардас", args, purse.deals.link.snapshots[XARDAS], **kwargs)
    assert out.status == "rejected" and out.output == UNASKED
    assert XARDAS not in purse.deals.open and not _shown(purse.fake)


@pytest.mark.parametrize(
    "word",
    [
        "золота",
        "монеты",
        "денег",
        "деньги",
        "денежную помощь",
        "деньжат",
        "грошей",
        "кошелёк",
        "материальная помощь",
        "одолжи",
        "займи",
        "заём",
        "взаймы",
        "ссуду",
        "долг",
        "долга",
        "долгу",
        "долгом",
        "долги",
        "долгов",
        "долгами",
        "долгах",
    ],
)
async def test_money_stems_and_debt_forms_are_requests(purse, word):
    out = await offer(purse, 1, hero_words=(word.upper(),))
    assert out.status == "accepted", out.output
    assert XARDAS in purse.deals.open


@pytest.mark.parametrize(
    ("amount", "later", "expected"),
    [
        (50, 0, "accepted"),
        (51, 0, "gift"),
        (200, 150, "accepted"),
        (200, 100, "gift"),
        (150, 150, "accepted"),
        (200, 200, "accepted"),
        (250, 250, "loan"),
    ],
)
async def test_gift_and_loan_limits(purse, amount, later, expected):
    out = await offer(purse, amount, later=later)
    if expected == "accepted":
        assert out.status == "accepted", out.output
        assert XARDAS in purse.deals.open
    else:
        assert out.status == "rejected"
        assert out.output == (GIFT.format(left=50) if expected == "gift" else LOAN.format(owed=0, after=250))
        assert not _shown(purse.fake) and XARDAS not in purse.deals.open


async def test_only_accepted_gifts_use_allowance_and_survive_reload(purse):
    assert (await offer(purse, 50)).status == "accepted"
    assert (await offer(purse, 30)).status == "accepted"
    await accept(purse)
    assert purse.fake.hero_items[GOLD] == 30
    assert purse.chronicle.facts("deal")[0]["npc_gives"][0]["count"] == 30
    purse.deals.reload()
    out = await offer(purse, 30)
    assert out.status == "rejected" and out.output == GIFT.format(left=20)
    assert XARDAS not in purse.deals.open
    assert (await offer(purse, 20)).status == "accepted"
    await accept(purse)
    assert purse.fake.hero_items[GOLD] == 50


@pytest.mark.parametrize(("amount", "left"), [(180, 20), (200, 0)])
async def test_accepted_mixed_gift_and_loan_uses_only_gift_allowance(purse, amount, left):
    assert (await offer(purse, amount, later=150)).status == "accepted"
    await accept(purse)
    fact = purse.chronicle.facts("deal")[0]
    assert fact["npc_gives"][0]["count"] == amount and fact["later"][0]["count"] == 150
    out = await offer(purse, left + 1)
    assert out.status == "rejected" and out.output == GIFT.format(left=left)
    if left:
        assert (await offer(purse, left)).status == "accepted"


async def test_gift_allowance_follows_save_branch(purse):
    saved_seq = purse.chronicle.memory.head("A")
    assert (await offer(purse, 50)).status == "accepted"
    await accept(purse)
    out = await offer(purse, 1)
    assert out.status == "rejected" and out.output == GIFT.format(left=0)
    session = Session(type="session", branch="B", parent="A", parent_seq=saved_seq, reason="load")
    purse.chronicle._on_session(session)
    purse.deals._on_message(session)
    assert purse.chronicle.facts("deal") == []
    assert (await offer(purse, 50)).status == "accepted"


async def test_old_excess_gift_allows_trade_and_loan_but_no_new_gift(purse):
    purse.chronicle.record(
        {"fact": "deal", "deal": "old-gift", "npc": XARDAS, "npc_gives": gold(100)}
    )
    out = await offer(
        purse,
        0,
        hero_words=(),
        npc_gives=[{"item": "ITRU_SUMSKEL", "count": 1}],
        hero_gives=gold(10),
    )
    assert out.status == "accepted", out.output
    out = await offer(purse, 100, later=100, hero_words=REQUEST)
    assert out.status == "accepted", out.output
    out = await offer(purse, 10)
    assert out.status == "rejected" and out.output == GIFT.format(left=0)


async def test_old_excess_debt_allows_trade_and_repayment_but_no_new_loan(purse):
    purse.chronicle.record(
        {"fact": "debt", "id": "old-debt", "npc": XARDAS, "item": GOLD, "count": 250, "due": 4}
    )
    purse.deals.reload()
    out = await offer(
        purse,
        0,
        hero_words=(),
        npc_gives=[{"item": "ITRU_SUMSKEL", "count": 1}],
        hero_gives=gold(10),
    )
    assert out.status == "accepted", out.output
    out = await offer(purse, 0, hero_words=(), repay=["old-debt"])
    assert out.status == "accepted", out.output
    out = await offer(purse, 10, later=10)
    assert out.status == "rejected" and out.output == LOAN.format(owed=250, after=260)


@pytest.mark.parametrize("hero_gives", [gold(50), [{"item": POTION, "count": 1}]])
async def test_only_gold_paid_now_covers_gift(purse, hero_gives):
    purse.fake.hero_items[GOLD] = 50
    out = await offer(purse, 100, hero_gives=hero_gives)
    if hero_gives[0]["item"] == GOLD:
        assert out.status == "accepted", out.output
        await accept(purse)
        assert purse.fake.hero_items[GOLD] == 100
        out = await offer(purse, 1)
        assert out.status == "rejected" and out.output == GIFT.format(left=0)
    else:
        assert out.status == "rejected" and out.output == GIFT.format(left=50)


@pytest.mark.parametrize("amount", [50, 51])
async def test_npc_later_gold_counts_as_gift_and_requires_request(purse, amount):
    terms = {"npc_later": {"items": gold(amount), "day": 3}}
    unasked = await offer(purse, 0, hero_words=(), **terms)
    assert unasked.status == "rejected" and unasked.output == UNASKED
    assert not _shown(purse.fake)
    out = await offer(purse, 0, **terms)
    if amount == 51:
        assert out.status == "rejected" and out.output == GIFT.format(left=50)
    else:
        assert out.status == "accepted"
        await accept(purse)
        assert purse.chronicle.facts("deal")[0]["npc_later"]["items"][0]["count"] == 50
        out = await offer(purse, 1)
        assert out.status == "rejected" and out.output == GIFT.format(left=0)


@pytest.mark.parametrize("broken", [False, True])
async def test_outstanding_and_overdue_debt_count_but_repayment_frees_limit(purse, broken):
    assert (await offer(purse, 150, later=150)).status == "accepted"
    await accept(purse)
    (debt,) = purse.deals.debts.values()
    assert debt.npc == XARDAS and debt.give.item == GOLD and debt.give.count == 150
    if broken:
        assert purse.deals.overdue(debt.due + 1) == [debt]
        assert debt.broken and not debt.paid
    out = await offer(purse, 100, later=100)
    assert out.status == "rejected" and out.output == LOAN.format(owed=150, after=250)
    assert XARDAS not in purse.deals.open
    out = await offer(purse, 100, later=100, repay=[debt.id])
    assert out.status == "accepted", out.output
    await accept(purse)
    assert debt.paid and purse.fake.hero_items[GOLD] == 100
    assert sum(d.give.count for d in purse.deals.debts.values() if not d.paid) == 100


async def test_repay_does_not_cover_new_gift(purse):
    assert (await offer(purse, 150, later=150)).status == "accepted"
    await accept(purse)
    (debt,) = purse.deals.debts.values()
    out = await offer(purse, 100, repay=[debt.id])
    assert out.status == "rejected" and out.output == GIFT.format(left=50)
    assert not debt.paid and XARDAS not in purse.deals.open


async def test_paid_other_npc_and_non_gold_debts_do_not_count(purse):
    for debt in (
        Debt("paid", XARDAS, "Ксардас", Give(GOLD, 200, "Золото"), 3, paid=True),
        Debt("other", LOBART, "Лобарт", Give(GOLD, 200, "Золото"), 3),
        Debt("potion", XARDAS, "Ксардас", Give(POTION, 1, "Эликсир лечения"), 3),
    ):
        purse.deals.debts[debt.id] = debt
    assert (await offer(purse, 200, later=200)).status == "accepted"


async def test_rejections_check_request_then_gift_then_loan(purse):
    out = await offer(purse, 400, later=250, hero_words=())
    assert out.status == "rejected" and out.output == UNASKED
    out = await offer(purse, 400, later=250)
    assert out.status == "rejected" and out.output == GIFT.format(left=50)
    out = await offer(purse, 250, later=250)
    assert out.status == "rejected" and out.output == LOAN.format(owed=0, after=250)
    assert not _shown(purse.fake)


async def test_reward_gold_is_unlimited_and_needs_no_request(purse):
    assert (await offer(purse, 50)).status == "accepted"
    await accept(purse)
    out = await offer(
        purse,
        0,
        hero_words=(),
        task={"bring": [{"item": POTION, "count": 1}]},
        reward={"npc_gives": gold(300)},
    )
    assert out.status == "accepted", out.output
    await accept(purse)
    purse.fake.hero_items[POTION] = 1
    await purse.deals.contracts.prepare(XARDAS)
    assert purse.fake.hero_items[GOLD] == 350
    assert len(purse.chronicle.facts("contract_paid")) == 1
    out = await offer(purse, 1)
    assert out.status == "rejected" and out.output == GIFT.format(left=0)


async def test_npc_without_purse_keeps_unrestricted_gold_deals(purse):
    purse.fake.npc_items[LOBART] = {GOLD: 500}
    raw = purse.fake.snapshot(LOBART)
    raw["npc"]["inventory"] = [{"item": GOLD, "name": "Золото", "count": 500, "equipped": False}]
    snap = Snapshot.model_validate(raw)
    out = await purse.deals.propose(LOBART, "Лобарт", {"npc_gives": gold(300)}, snap)
    assert out.status == "accepted", out.output
    assert LOBART in purse.deals.open
    out = await purse.deals.propose(
        LOBART,
        "Лобарт",
        {"npc_gives": gold(300), "hero_later": [{"item": GOLD, "count": 300, "days": 3}]},
        snap,
    )
    assert out.status == "accepted", out.output


@pytest.mark.parametrize(
    ("entries", "trigger", "accepted"),
    [
        ([], Message("user", "Дай немного золота"), True),
        ([("user", "Дай немного золота")], Message("user", "Согласен"), True),
        (
            [("user", "Дай немного золота"), ("developer", "[Новая встреча с героем, день 3]")],
            Message("user", "Что мне делать?"),
            False,
        ),
        (
            [
                ("user", "Дай немного золота"),
                ("developer", "[Новая встреча с героем]"),
                ("user", "Одолжи монет"),
            ],
            None,
            True,
        ),
        (
            [("assistant", "Дам тебе денег"), ("developer", "Герою нужны деньги")],
            Message("developer", "Предложи золото"),
            False,
        ),
        ([], Message("assistant", "Дам золото"), False),
    ],
)
async def test_handle_uses_only_hero_words_from_current_meeting(purse, entries, trigger, accepted):
    conv = purse.dialogue.conversations[XARDAS]
    conv.memory.entries = [Entry(i, role, text) for i, (role, text) in enumerate(entries, 1)]
    conv.trigger = trigger
    call = ToolCall("propose_deal", {"npc_gives": gold(30)}, "money")
    out = await purse.deals.handle(purse.dialogue, conv, call, purse.deals.link.snapshots[XARDAS])
    assert out is not None
    if accepted:
        assert out.status == "accepted", out.output
        await _until(lambda: len(_shown(purse.fake)) == 1)
    else:
        assert out.status == "rejected" and out.output == UNASKED
        assert XARDAS not in purse.deals.open and not _shown(purse.fake)
