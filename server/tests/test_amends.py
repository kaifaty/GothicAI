import asyncio
import copy
import json
from pathlib import Path
from typing import Any

import pytest

from gothic_ai.amends import Amends
from gothic_ai.chronicle import Chronicle
from gothic_ai.crime_law import CrimeLaw
from gothic_ai.crimes import Crimes
from gothic_ai.director import Director, Pacing
from gothic_ai.gamedata.storygraph import StoryGraph, load
from gothic_ai.llm import ToolCall
from gothic_ai.llm.fake import FakeBackend, FakeStep
from gothic_ai.memory import MemoryStore
from gothic_ai.story import StoryPatches
from gothic_ai.trust import Trust
from tests.fake_game import FakeGame, connected
from tests.test_dialogue import _until
from tests.test_director import GRAPH, HILDA, LOBART, NAMES, QUEST, SYMBOLS, _node

ANDRE, PECK = "MIL_311_ANDRE", "MIS_ANDRE_PECK"
JOIN, MISSION = "DIA_ANDRE_JOIN", "DIA_ANDRE_FIRSTMISSION"
LIE = f"caught:{LOBART}:{QUEST}"


@pytest.fixture
def graph(tmp_path: Path) -> StoryGraph:
    """Lobart's turnips, and Andre: joining the militia, a first mission."""
    g = copy.deepcopy(GRAPH)
    g["nodes"] += [
        _node(
            JOIN,
            ANDRE,
            [{"do": "set_guild", "npc": ["inst", "OTHER"], "guild": ["int", 2]}],
            description="Я готов вступить в ополчение!",
            permanent=True,
        ),
        _node(MISSION, ANDRE, [{"do": "set", "var": PECK, "value": ["int", 1]}], description="Есть задание?"),
    ]
    g["vars"][PECK] = {"readers": [], "writers": [MISSION], "others": []}
    path = tmp_path / "story.json"
    path.write_text(json.dumps(g, ensure_ascii=False), encoding="utf-8")
    return load(path)


def _proposal(**change: Any) -> ToolCall:
    args = {
        "kind": "amends",
        "close": [JOIN],
        "lines": [
            {"speaker": "npc", "text": "Слыхал я, ты Лобарту про репу наврал."},
            {
                "speaker": "npc",
                "text": "Лжецов в ополчение не берём. Пятьдесят золотых штрафа — тогда поговорим.",
            },
        ],
        "to": "self",
        "gold": 50,
        "description": "Вот штраф — 50 золотых.",
        "amends_lines": [{"speaker": "npc", "text": "Ладно. Считай, забыли."}],
        "title": "Штраф за враньё",
        "journal_now": "Андре прослышал про репу Лобарта. Пока не заплачу 50 золотых, в ополчение не берёт.",
        "journal_done": "Я заплатил Андре штраф.",
        "reason": "Андре знает о лжи от Лобарта, ополчению лжецы не нужны",
    }
    return ToolCall("propose_amends", {**args, **change}, "c1")


async def _amends(
    fake: FakeGame, llm: FakeBackend, graph: StoryGraph, store: MemoryStore, *, crimes: bool = False
):
    fake.vars = {QUEST: 1, "KAPITEL": 1, PECK: 0}
    fake.npc_states = {n: {"state": "alive", "name": name} for n, name in {**NAMES, ANDRE: "Андре"}.items()}
    fake.symbols = {**SYMBOLS, ANDRE: "npc", PECK: "var"}
    link_cm = connected(fake)
    link = await link_cm.__aenter__()
    patches = StoryPatches(link, store)
    chronicle = Chronicle(link, store)
    director = Director(link, graph, patches, chronicle, llm, pacing=Pacing(cooldown_s=0), settle_s=0)
    deeds = Crimes(link, chronicle, names=lambda npc: NAMES.get(npc, npc)) if crimes else None
    amends = Amends(link, graph, patches, chronicle, director, llm, crimes=deeds)
    await _until(lambda: patches.branch is not None and chronicle.branch is not None)
    caught = {"fact": "caught", "quest": QUEST, "npc": LOBART, "name": "Лобарт", "title": "Убрать поле репы"}
    chronicle.record({**caught, "how": "видел сам", "day": 1})
    return link_cm, amends, chronicle


def _ops(fake: FakeGame) -> list[dict[str, Any]]:
    return [op for m in fake.graph for op in m["ops"]]


async def test_andre_hears_of_the_lie_and_asks_for_amends(graph):
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    llm = FakeBackend(script=[FakeStep("", [_proposal()])], delay=0)
    link_cm, amends, chronicle = await _amends(fake, llm, graph, store)
    try:
        assert {s.id for s in amends.stakes(ANDRE)} == {JOIN, MISSION} and amends.stakes(LOBART) == []
        s = await amends.consider(LIE, ANDRE, "Лобарт")
        assert s is not None
        brief = llm.calls[0][1][0].content
        assert llm.cache_keys == ["amends"]  # its own connection (openai_responses.py)
        assert "Человек: Андре (MIL_311_ANDRE)" in brief and "Откуда: слух, рассказал Лобарт" in brief
        assert f"- {JOIN}: «Я готов вступить в ополчение!» — принимает в гильдию" in brief
        assert f"- {MISSION}: «Есть задание?» — даёт работу" in brief
        assert "уверял, что сделал «Убрать поле репы», а это неправда" in brief

        # the talk Andre starts, and joining closed until the hero pays
        ops = _ops(fake)
        talk = ops[0]["node"]
        assert (talk["id"], talk["npc"], talk["important"]) == (f"DIR_AMENDS_{ANDRE}_1_TALK", ANDRE, True)
        assert talk["lines"][0] == {"speaker": "self", "text": "Слыхал я, ты Лобарту про репу наврал."}
        assert ops[1] == {"op": "disable", "node": JOIN}
        assert "Пока не заплатит, для него закрыто: «Я готов вступить в ополчение!»" in amends.situation(
            ANDRE
        )

        # the hero has heard him out: the fine is offered
        await fake.send(type="dialog_chosen", npc=ANDRE, node=talk["id"], made=True)
        await _until(lambda: len(_ops(fake)) == 3)
        pay = _ops(fake)[2]["node"]
        assert pay["npc"] == ANDRE and pay["description"] == "Вот штраф — 50 золотых."
        assert pay["condition"] == [
            ">=",
            ["call", "NPC_HASITEMS", ["inst", "OTHER"], ["sym", "ITMI_GOLD"]],
            ["int", 50],
        ]
        assert {"do": "give", "item": "ITMI_GOLD", "count": 50, "to": "self"} in pay["effects"]

        # he pays: joining opens again, and Andre counts it
        await fake.send(type="dialog_chosen", npc=ANDRE, node=pay["id"], made=True)
        await _until(lambda: chronicle.facts("amends") != [])
        await _until(lambda: _ops(fake)[-1] == {"op": "enable", "node": JOIN})
        assert "он загладил вину за ложь: «Убрать поле репы» — заплатил 50 золотых" in Trust(
            chronicle
        ).situation(ANDRE)
        # Lobart was not paid: his word is still broken for him
        assert "он уже лгал тебе" in Trust(chronicle).situation(LOBART)
        # all of it is in the branch: a load brings back where it stands
        amends._load()
        assert amends.storylets[s.id].stage == "done"
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_a_rejected_storylet_and_one_who_does_not_care(graph):
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    wrong = _proposal(close=["DIA_LOBART_WORKNOW"], gold=500)
    none = ToolCall(
        "propose_amends", {**_proposal().arguments, "kind": "none", "reason": "не его дело"}, "c2"
    )
    llm = FakeBackend(script=[FakeStep("", [wrong]), FakeStep("", [none])], delay=0)
    link_cm, amends, chronicle = await _amends(fake, llm, graph, store)
    try:
        assert await amends.consider(LIE, ANDRE, "Лобарт") is None
        why = chronicle.facts("amends_rejected")[0]["why"]
        assert any("close" in w for w in why) and any("gold: от 0 до 100" in w for w in why)
        assert "Отклонено" in llm.calls[1][1][-1].content
        assert chronicle.facts("amends_none")[0]["reason"] == "не его дело"
        assert _ops(fake) == []
        # asked once per lie
        assert await amends.consider(LIE, ANDRE, "Лобарт") is None and len(llm.calls) == 2
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_amends_to_the_one_lied_to(graph):
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    llm = FakeBackend(script=[FakeStep("", [_proposal(to="victim", gold=20)])], delay=0)
    link_cm, amends, chronicle = await _amends(fake, llm, graph, store)
    try:
        s = await amends.consider(LIE, ANDRE, "Лобарт")
        assert s is not None and s.to == LOBART
        await fake.send(type="dialog_chosen", npc=ANDRE, node=s.talk, made=True)
        await _until(lambda: len(_ops(fake)) == 3)
        assert _ops(fake)[2]["node"]["npc"] == LOBART
        assert "требует, чтобы герой повинился перед тобой и заплатил тебе 20 золотых" in amends.situation(
            LOBART
        )
        await fake.send(type="dialog_chosen", npc=LOBART, node=s.pay, made=True)
        await _until(lambda: chronicle.facts("amends") != [])
        # Lobart was paid: the lie no longer breaks the hero's word with him
        line = Trust(chronicle).situation(LOBART) or ""
        assert "он уже лгал тебе" not in line and "он загладил вину за ложь" in line
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_the_queue_skips_those_with_nothing_at_stake(graph):
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    llm = FakeBackend(script=[FakeStep("", [_proposal()])], delay=0)
    link_cm, amends, chronicle = await _amends(fake, llm, graph, store)
    import asyncio

    run = asyncio.create_task(amends.run())
    try:
        # the caught fact queued Lobart: nothing the hero comes to him for here, no model call
        chronicle.record(
            {
                "fact": "rumor",
                "quest": QUEST,
                "kind": "caught",
                "about": LOBART,
                "from": LOBART,
                "from_name": "Лобарт",
                "to": ANDRE,
                "hops": 1,
            }
        )
        await _until(lambda: chronicle.facts("amends_open") != [])
        assert len(llm.calls) == 1 and chronicle.facts("amends_open")[0]["npc"] == ANDRE
    finally:
        run.cancel()
        await link_cm.__aexit__(None, None, None)


async def test_one_confession_settles_both_who_asked_for_it(graph):
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    to_lobart = _proposal(to="victim", gold=0, description="Повиниться перед Лобартом")
    # joining is closed by the first: the second closes the mission
    also = _proposal(to="victim", gold=0, description="Повиниться перед Лобартом", close=[MISSION])
    llm = FakeBackend(script=[FakeStep("", [to_lobart]), FakeStep("", [also])], delay=0)
    link_cm, amends, chronicle = await _amends(fake, llm, graph, store)
    try:
        first = await amends.consider(LIE, ANDRE, "Лобарт")
        assert first is not None
        # the next one asked about the same lie sees what is asked already
        amends.stakes = lambda npc: amends.__class__.stakes(amends, ANDRE)  # Hilda has the same dealings here
        second = await amends.consider(LIE, HILDA, "Лобарт")
        assert (
            second is not None
            and "тогда одна расплата закроет оба требования): Андре — повиниться — Лобарт"
            in llm.calls[1][1][0].content
        )
        for s in (first, second):
            await fake.send(type="dialog_chosen", npc=s.npc, node=s.talk, made=True)
        await _until(lambda: first.stage == "told" and second.stage == "told")
        # he confesses to Lobart once: both are settled, the other confession is gone from Lobart's menu
        await fake.send(type="dialog_chosen", npc=LOBART, node=first.pay, made=True)
        await _until(lambda: len(chronicle.facts("amends")) == 2)
        assert second.stage == "done"
        assert {"op": "remove_node", "node": second.pay} in _ops(fake)
        assert len(chronicle.facts("amends")) == 2
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_amends_made_in_a_deal(graph):
    fake = FakeGame(branch="A")
    store = MemoryStore(":memory:")
    llm = FakeBackend(script=[FakeStep("", [_proposal()])], delay=0)
    link_cm, amends, chronicle = await _amends(fake, llm, graph, store)
    try:
        s = await amends.consider(LIE, ANDRE, "Лобарт")
        assert s is not None
        assert f"загладить вину за ложь ({LIE})" in (amends.situation(ANDRE) or "")
        # before the talk is even over Andre lets the hero off for 40 gold in a deal (deals.py)
        chronicle.record({"fact": "deal", "npc": ANDRE, "forgive": [LIE], "gold": 40, "silence": []})
        await _until(lambda: s.stage == "done")
        await _until(lambda: {"op": "enable", "node": JOIN} in _ops(fake))
        assert {"op": "remove_node", "node": s.talk} in _ops(fake)  # his talk leaves the menu
        fact = chronicle.facts("amends")[0]
        assert (fact["id"], fact["gold"], fact["lie"]) == (s.id, 40, LIE)
        assert "заплатил 40 золотых" in (Trust(chronicle).situation(ANDRE) or "")
        amends._load()
        assert amends.storylets[s.id].stage == "done"
    finally:
        await link_cm.__aexit__(None, None, None)


CRIME = "theft-in-lobarts-house"
STOLEN = "ITMI_SILVERCUP"


def _crime(chronicle: Chronicle, *, kind: str = "theft") -> None:
    chronicle.record(
        {
            "fact": "crime",
            "id": CRIME,
            "kind": kind,
            "victim": LOBART,
            "owner": LOBART,
            "item": STOLEN if kind == "theft" else "",
            "count": 3 if kind == "theft" else 0,
            "witnesses": [HILDA],
            "community": "farm",
            "day": 1,
            "hour": 10,
            "minute": 0,
        }
    )


def _crime_proof(chronicle: Chronicle, how: str) -> None:
    if how == "seen":
        chronicle.record({"fact": "crime_seen", "crime": CRIME, "npc": LOBART})
    elif how == "rumor":
        chronicle.record({"fact": "rumor", "crime": CRIME, "from": HILDA, "to": LOBART, "hops": 1})
    else:
        chronicle.record({"fact": "testimony", "npc": LOBART, "witness": HILDA, "crime_told": [CRIME]})


def _crime_proposal(**change: Any) -> ToolCall:
    return _proposal(
        **{
            "close": [],
            "gold": 10,
            "lines": [
                {"speaker": "npc", "text": "Хильда видела, как ты взял мою чашу."},
                {"speaker": "npc", "text": "Плати за неё десять золотых."},
            ],
            "title": "Возместить Лобарту",
            "journal_now": "Лобарт требует десять золотых за чашу.",
            "journal_done": "Я возместил Лобарту ущерб.",
            **change,
        }
    )


@pytest.mark.parametrize("kind,how", [("theft", "seen"), ("theft", "rumor"), ("attack", "testimony")])
async def test_victim_gets_amends_only_after_learning_of_the_hero(graph, kind, how):
    fake = FakeGame(branch="A")
    llm = FakeBackend(script=[FakeStep("", [_crime_proposal()])], delay=0)
    link_cm, amends, chronicle = await _amends(fake, llm, graph, MemoryStore(":memory:"), crimes=True)
    try:
        _crime(chronicle, kind=kind)
        assert await amends.consider(f"crime:{CRIME}", LOBART, "") is None
        chronicle.record({"fact": "crime_suspected", "crime": CRIME, "npc": LOBART, "suspect": "PLAYER"})
        assert await amends.consider(f"crime:{CRIME}", LOBART, "") is None
        _crime_proof(chronicle, how)
        s = await amends.consider(
            f"crime:{CRIME}", LOBART, "видел сам" if how == "seen" else "слышал от Хильда"
        )
        assert s is not None and s.npc == LOBART and s.to == LOBART and s.close == []
        brief = llm.calls[0][1][0].content
        assert f"Дело: crime:{CRIME}" in brief and "Пострадавший: Лобарт" in brief
        assert (STOLEN in brief) == (kind == "theft")
        assert "подозревает" not in brief
        assert len(chronicle.facts("amends_open")) == 1
        assert await amends.consider(s.lie, LOBART, "снова слышал") is None
        assert await amends.consider(s.lie, HILDA, "видела сама") is None
        assert len(llm.calls) == 1
    finally:
        await link_cm.__aexit__(None, None, None)


@pytest.mark.parametrize("cover", [True, False])
async def test_old_covered_testimony_opens_amends_only_with_cover_disabled(graph, cover):
    fake = FakeGame(branch="A")
    link_cm, amends, chronicle = await _amends(
        fake, FakeBackend("…", delay=0), graph, MemoryStore(":memory:"), crimes=True
    )
    amends.crimes.cover = cover
    try:
        _crime(chronicle)
        chronicle.record({
            "fact": "testimony", "npc": LOBART, "witness": HILDA, "cover": "D1",
            "crime_saw": [CRIME], "crime_told": [],
        })
        assert (f"crime:{CRIME}" in amends.lies) is not cover
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_crime_payment_forgives_the_case_and_stops_its_news(graph):
    fake = FakeGame(branch="A")
    llm = FakeBackend(script=[FakeStep("", [_crime_proposal()])], delay=0)
    link_cm, amends, chronicle = await _amends(fake, llm, graph, MemoryStore(":memory:"), crimes=True)
    try:
        _crime(chronicle)
        chronicle.record({"fact": "crime_seen", "crime": CRIME, "npc": HILDA, "guild": 8, "npctype": 0})
        _crime_proof(chronicle, "rumor")
        law = CrimeLaw(amends.crimes)
        law.tick(1 * 1440 + 10 * 60)
        assert any(report.status == "pending" for report in law.deliveries())
        s = await amends.consider(f"crime:{CRIME}", LOBART, "слышал от Хильда")
        assert s is not None
        assert not chronicle.facts("forgiven")
        await fake.send(type="dialog_chosen", npc=LOBART, node=s.talk, made=True)
        await _until(lambda: s.stage == "told")
        await _until(lambda: any(op.get("node", {}).get("id") == s.pay for op in _ops(fake)))
        await fake.send(type="dialog_chosen", npc=LOBART, node=s.pay, made=True)
        await _until(lambda: bool(chronicle.facts("forgiven")))
        paid = chronicle.facts("forgiven")[0]
        assert (paid["npc"], paid["crime"], paid["paid"], paid["gold"]) == (LOBART, CRIME, True, 10)
        assert amends.crimes.known(LOBART)[0].forgiven
        law.tick(1 * 1440 + 11 * 60)
        assert all(report.status == "stopped" and report.reason == "repaid" for report in law.deliveries())
        # Crime compensation never creates a remembered lie or reopens a vanilla quest.
        fact = chronicle.facts("amends")[0]
        assert fact["crime"] == CRIME and "quest" not in fact and "about" not in fact
        assert "загладил вину за ложь" not in (Trust(chronicle, crimes=amends.crimes).situation(LOBART) or "")
        amends._load()
        assert amends.storylets[s.id].stage == "done"
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_crime_compensation_is_bounded_by_item_value(graph):
    fake = FakeGame(branch="A")
    fake.fields[STOLEN, "VALUE"] = 6
    llm = FakeBackend(
        script=[FakeStep("", [_crime_proposal(gold=19)]), FakeStep("", [_crime_proposal(gold=18)])], delay=0
    )
    link_cm, amends, chronicle = await _amends(fake, llm, graph, MemoryStore(":memory:"), crimes=True)
    try:
        _crime(chronicle)
        _crime_proof(chronicle, "rumor")
        s = await amends.consider(f"crime:{CRIME}", LOBART, "слышал от Хильда")
        assert s is not None and s.gold == 18
        brief = llm.calls[0][1][0].content
        assert "Стоимость взятого: 18 золотых." in brief and "Предел расплаты: 18 золотых." in brief
        assert chronicle.facts("amends_rejected")[0]["why"] == ["gold: от 1 до 18"]
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_crime_repayment_in_a_deal_settles_amends(graph):
    fake = FakeGame(branch="A")
    llm = FakeBackend(script=[FakeStep("", [_crime_proposal()])], delay=0)
    link_cm, amends, chronicle = await _amends(fake, llm, graph, MemoryStore(":memory:"), crimes=True)
    try:
        _crime(chronicle)
        _crime_proof(chronicle, "rumor")
        s = await amends.consider(f"crime:{CRIME}", LOBART, "слышал от Хильда")
        assert s is not None
        chronicle.record(
            {
                "fact": "forgiven",
                "crime": CRIME,
                "npc": LOBART,
                "paid": True,
                "hero_gives": [{"item": "ITMI_GOLD", "count": 12}],
            }
        )
        await _until(lambda: bool(chronicle.facts("amends")))
        assert s.stage == "done" and s.gold == 12
        assert len(chronicle.facts("forgiven")) == 1
        assert {"op": "remove_node", "node": s.talk} in _ops(fake)
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_no_crimes_does_not_start_amends_for_crime_facts(graph):
    fake = FakeGame(branch="A")
    llm = FakeBackend(delay=0)
    link_cm, amends, chronicle = await _amends(fake, llm, graph, MemoryStore(":memory:"))
    try:
        _crime(chronicle)
        _crime_proof(chronicle, "seen")
        _crime_proof(chronicle, "rumor")
        _crime_proof(chronicle, "testimony")
        amends._load()
        assert await amends.consider(f"crime:{CRIME}", LOBART, "видел сам") is None
        assert not llm.calls and not chronicle.facts("amends_open")
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_saved_crime_amends_can_be_paid_with_crimes_disabled(graph):
    fake = FakeGame(branch="A")
    llm = FakeBackend(script=[FakeStep("", [_crime_proposal()])], delay=0)
    link_cm, amends, chronicle = await _amends(fake, llm, graph, MemoryStore(":memory:"), crimes=True)
    try:
        _crime(chronicle)
        _crime_proof(chronicle, "rumor")
        created = await amends.consider(f"crime:{CRIME}", LOBART, "слышал от Хильда")
        assert created is not None
        amends.crimes = None
        amends._load()
        saved = amends.storylets[created.id]
        assert saved.lie not in amends.lies and amends._queue.empty()
        await fake.send(type="dialog_chosen", npc=LOBART, node=saved.talk, made=True)
        await _until(lambda: saved.stage == "told")
        await _until(lambda: any(op.get("node", {}).get("id") == saved.pay for op in _ops(fake)))
        await fake.send(type="dialog_chosen", npc=LOBART, node=saved.pay, made=True)
        await _until(lambda: bool(chronicle.facts("forgiven")))
        forgiven = chronicle.facts("forgiven")[0]
        assert (forgiven["crime"], forgiven["npc"], forgiven["paid"]) == (CRIME, LOBART, True)
        assert chronicle.facts("amends")[0]["crime"] == CRIME
        _crime_proof(chronicle, "seen")
        assert amends._queue.empty() and len(llm.calls) == 1
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_meeting_demand_reuses_existing_amends_and_its_payment(graph):
    fake = FakeGame(branch="A")
    llm = FakeBackend(script=[FakeStep("", [_crime_proposal()])], delay=0)
    link_cm, amends, chronicle = await _amends(fake, llm, graph, MemoryStore(":memory:"), crimes=True)
    try:
        _crime(chronicle)
        _crime_proof(chronicle, "seen")
        storylet = await amends.demand(LOBART, CRIME)
        assert storylet is not None
        assert await amends.demand(LOBART, CRIME) is storylet
        await fake.send(type="dialog_chosen", npc=LOBART, node=storylet.talk, made=True)
        await _until(lambda: storylet.stage == "told")
        assert await amends.demand(LOBART, CRIME) is storylet
        await fake.send(type="dialog_chosen", npc=LOBART, node=storylet.pay, made=True)
        await _until(lambda: bool(chronicle.facts("forgiven")))
        assert not amends.active_crime(LOBART, CRIME)
        assert await amends.demand(LOBART, CRIME) is None
        assert len(llm.calls) == 1 and chronicle.facts("forgiven")[0]["paid"]
    finally:
        await link_cm.__aexit__(None, None, None)


@pytest.mark.parametrize("proposal", ["none", "invalid"])
async def test_meeting_rule_requires_validated_amends_even_if_model_declines(graph, proposal):
    fake = FakeGame(branch="A")
    args = {"kind": "none"} if proposal == "none" else {"gold": 5000}
    llm = FakeBackend(script=[FakeStep("", [_crime_proposal(**args)])], delay=0)
    link_cm, amends, chronicle = await _amends(fake, llm, graph, MemoryStore(":memory:"), crimes=True)
    try:
        _crime(chronicle)
        _crime_proof(chronicle, "seen")
        storylet = await amends.demand(LOBART, CRIME)
        assert storylet is not None and 1 <= storylet.gold <= 50
        assert len(llm.calls) == 2 and not chronicle.facts("amends_none")
        assert "Правило встречи уже решило" in llm.calls[0][1][0].content
        talk = next(op["node"] for op in _ops(fake) if op.get("node", {}).get("id") == storylet.talk)
        assert talk["important"] and len(talk["lines"]) >= 2
    finally:
        await link_cm.__aexit__(None, None, None)


@pytest.mark.parametrize("resolution", ["silenced", "died"])
async def test_silence_and_death_withdraw_native_amends_and_survive_reload(graph, resolution):
    fake = FakeGame(branch="A")
    llm = FakeBackend(script=[FakeStep("", [_crime_proposal()])], delay=0)
    link_cm, amends, chronicle = await _amends(fake, llm, graph, MemoryStore(":memory:"), crimes=True)
    try:
        _crime(chronicle)
        _crime_proof(chronicle, "seen")
        storylet = await amends.demand(LOBART, CRIME)
        assert storylet is not None
        chronicle.record({"fact": resolution, "npc": LOBART, "crime": CRIME})
        await _until(lambda: bool(chronicle.facts("amends_withdrawn")))
        assert {"op": "remove_node", "node": storylet.talk} in _ops(fake)
        assert not amends.active_crime(LOBART, CRIME)
        assert await amends.demand(LOBART, CRIME) is None
        amends._load()
        assert amends.storylets[storylet.id].stage == "done"
    finally:
        await link_cm.__aexit__(None, None, None)


@pytest.mark.parametrize("change", ["silenced", "forgiven", "died", "branch"])
async def test_inflight_meeting_demand_does_not_create_dialog_after_knowledge_or_load_changes(graph, change):
    fake = FakeGame(branch="A")
    llm = FakeBackend(script=[FakeStep("", [_crime_proposal()])], delay=0, first=0.1)
    link_cm, amends, chronicle = await _amends(fake, llm, graph, MemoryStore(":memory:"), crimes=True)
    try:
        _crime(chronicle)
        _crime_proof(chronicle, "seen")
        demand = asyncio.create_task(amends.demand(LOBART, CRIME))
        await _until(lambda: bool(llm.calls))
        if change == "branch":
            await fake.send(type="loading")
            await fake.send(type="session", branch="B", parent=None, parent_seq=0, reason="load")
            await _until(lambda: chronicle.branch == "B")
        else:
            chronicle.record({"fact": change, "npc": LOBART, "crime": CRIME})
        assert await demand is None
        assert not chronicle.facts("amends_open")
        assert not any(op.get("op") == "add_node" for op in _ops(fake))
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_meeting_demand_accepts_known_owner_when_victim_field_is_empty(graph):
    fake = FakeGame(branch="A")
    llm = FakeBackend(script=[FakeStep("", [_crime_proposal()])], delay=0)
    link_cm, amends, chronicle = await _amends(fake, llm, graph, MemoryStore(":memory:"), crimes=True)
    try:
        _crime(chronicle)
        case = {**chronicle.facts("crime")[0], "id": "OWN", "victim": ""}
        chronicle.record(case)
        chronicle.record({"fact": "crime_seen", "crime": "OWN", "npc": LOBART})
        storylet = await amends.demand(LOBART, "OWN")
        assert storylet is not None and storylet.to == LOBART
    finally:
        await link_cm.__aexit__(None, None, None)


async def test_cancelled_demand_removes_talk_already_sent_without_storylet(graph):
    fake = FakeGame(branch="A")
    llm = FakeBackend(script=[FakeStep("", [_crime_proposal()])], delay=0)
    link_cm, amends, chronicle = await _amends(fake, llm, graph, MemoryStore(":memory:"), crimes=True)
    original_apply = amends.patches.apply
    sent = asyncio.Event()

    async def waiting_for_ack(op: dict[str, Any]) -> None:
        if op.get("op") == "add_node":
            # The engine has applied TALK, while StoryPatches.apply still awaits its result.
            await amends.link.patch_graph([op])
            sent.set()
            await asyncio.Event().wait()
        else:
            await original_apply(op)

    amends.patches.apply = waiting_for_ack
    try:
        _crime(chronicle)
        _crime_proof(chronicle, "seen")
        demand = asyncio.create_task(amends.demand(LOBART, CRIME))
        await sent.wait()
        demand.cancel()
        with pytest.raises(asyncio.CancelledError):
            await demand
        talk = next(op["node"]["id"] for op in _ops(fake) if op.get("op") == "add_node")
        assert {"op": "remove_node", "node": talk} in _ops(fake)
        assert not amends.storylets and not chronicle.facts("amends_open")
    finally:
        amends.patches.apply = original_apply
        await link_cm.__aexit__(None, None, None)


@pytest.mark.parametrize("callback", ["talk", "paid", "forgiven"])
@pytest.mark.parametrize("loading", [False, True])
async def test_dialog_callback_does_not_write_loaded_branch_during_pending_patch_ack(
    graph, callback, loading
):
    fake = FakeGame(branch="A")
    llm = FakeBackend(script=[FakeStep("", [_crime_proposal()])], delay=0)
    link_cm, amends, chronicle = await _amends(fake, llm, graph, MemoryStore(":memory:"), crimes=True)
    original_patch = amends.link.patch_graph
    sent, acknowledge = asyncio.Event(), asyncio.Event()

    async def waiting_for_ack(ops, *, replace=False):
        await original_patch(ops, replace=replace)
        if not replace and not sent.is_set():
            # The native operation already ran; its delayed acknowledgement must not
            # let StoryPatches or the callback record that old operation in branch B.
            sent.set()
            await acknowledge.wait()

    try:
        _crime(chronicle)
        _crime_proof(chronicle, "seen")
        storylet = await amends.demand(LOBART, CRIME)
        assert storylet is not None
        # Restored Amends can also reopen native dialogs. Two entries prove that
        # loading during the first await cannot send the next mutation to the new world.
        storylet.close = [JOIN, MISSION]
        amends.link.patch_graph = waiting_for_ack
        if callback == "forgiven":
            chronicle.record({"fact": "forgiven", "npc": LOBART, "crime": CRIME})
        else:
            await fake.send(
                type="dialog_chosen", npc=LOBART,
                node=storylet.talk if callback == "talk" else storylet.pay, made=True,
            )
        await asyncio.wait_for(sent.wait(), 1)
        pending = tuple(amends._background)
        old_ops = len(_ops(fake))
        if loading:
            await fake.send(type="loading")
            await _until(lambda: amends._loading)
        await fake.send(type="session", branch="B", parent=None, parent_seq=0, reason="load")
        await _until(lambda: chronicle.branch == "B")
        acknowledge.set()
        await asyncio.gather(*pending, return_exceptions=True)
        assert not amends.storylets
        assert amends.patches.ops() == []
        assert chronicle.facts() == []
        assert _ops(fake)[old_ops:] == []
    finally:
        acknowledge.set()
        amends.link.patch_graph = original_patch
        await link_cm.__aexit__(None, None, None)
