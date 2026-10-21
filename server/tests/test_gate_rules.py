"""Gate guarantees and named claims follow recorded truth and the city's law."""

from __future__ import annotations

import asyncio
import copy
from typing import Any

import pytest

import gothic_ai.gates as gate_code
from gothic_ai.gamedata.storygraph import Node, Quest
from gothic_ai.llm import ToolCall
from gothic_ai.trust import Trust
from tests.test_gates import CITY, PAIR, GatesFake, _has_pass, _until, setup_gate
from tests.test_witnesses import World, lobart

LOTHAR = "PAL_203_LOTHAR"
HARAD = "VLK_412_HARAD"
BOSPER = "VLK_413_BOSPER"
CONSTANTINO = "VLK_417_CONSTANTINO"
STRANGER = "VLK_999_STRANGER"
DAY = 1440


async def _tool(system, name: str, args: dict[str, Any], *, npc: str = CITY):
    link, dialogue, _, gates, _, guard = system
    conv = guard if npc == guard.npc else dialogue.open(npc, "Лотар" if npc == LOTHAR else npc)
    jobs = []
    original = dialogue.after_speech
    dialogue.after_speech = lambda conv, job: jobs.append(job)
    try:
        result = await gates.handle(
            dialogue, conv, ToolCall(name, args, "gate-rule"), link.snapshots.get(npc),
        )
        for job in jobs:
            _, ok = await job()
            assert ok
        return result
    finally:
        dialogue.after_speech = original


async def _vouch(system):
    outcome = await _tool(
        system, "vouch_for_hero", {"note": "Я отвечаю за него."}, npc=LOTHAR,
    )
    assert outcome is not None and outcome.status == "accepted"
    return system[2].facts("gate_vouch")[-1]


async def _admit(system, *, basis: str = "vouch", about: str = ""):
    return await _tool(system, "let_pass", {
        "basis": basis,
        "claim": "Меня ждёт Гарад." if basis == "claim" else "",
        "about": about,
    })


def _reachable(fake: GatesFake, npc: str = HARAD, *, guild: int = 3, state: str = "alive") -> None:
    fake.symbols[npc] = "npc"
    fake.npc_states[npc] = {"state": state, "name": "Гарад", "distance": 3500.0}
    fake.people.append({
        "npc": npc, "name": "Гарад", "guild": guild, "npctype": 0,
        "routine": [{"start": 0, "end": 1440, "waypoint": "NW_CITY_SMITH_01", "pos": [0, 0, 0]}],
    })


async def test_only_respected_people_can_vouch():
    async with setup_gate(GatesFake()) as system:
        gates, chronicle = system[3], system[2]
        assert "vouch_for_hero" in [tool.name for tool in gates.tools(LOTHAR)]
        assert "vouch_for_hero" not in [tool.name for tool in gates.tools(STRANGER)]
        outcome = await _tool(system, "vouch_for_hero", {"note": "Trust him."}, npc=STRANGER)
        assert outcome is not None and outcome.status == "rejected"
        assert chronicle.facts("gate_vouch") == []


async def test_vouch_records_voucher_and_full_game_time():
    fake = GatesFake()
    fake.day, fake.hour, fake.minute = 4, 14, 30
    async with setup_gate(fake) as system:
        fact = await _vouch(system)
        assert fact["id"] and fact["npc"] == LOTHAR
        assert fact["name"] == "Лотар" and "паладин" in fact["role"].lower()
        assert fact["note"] == "Я отвечаю за него."
        assert (fact["day"], fact["hour"], fact["minute"]) == (4, 14, 30)
        assert fact["at"] == 4 * DAY + 14 * 60 + 30
        assert fact["expires"] - fact["at"] == 2 * DAY


@pytest.mark.parametrize("distance,accepted", [(999, True), (1000, True), (1001, False)])
async def test_guard_can_use_vouch_only_when_voucher_is_within_ten_metres(distance, accepted):
    fake = GatesFake()
    fake.npc_states[LOTHAR] = {"state": "alive", "name": "Лотар", "distance": distance}
    async with setup_gate(fake) as system:
        await _vouch(system)
        gates = system[3]
        await gates.prepare(CITY)
        situation = gates.situation(CITY) or ""
        assert ("basis vouch" in situation) is accepted
        if accepted:
            assert "Лотар" in situation and "паладин" in situation.lower()
        result = await _admit(system)
        assert result.status == ("accepted" if accepted else "rejected")
        assert _has_pass(fake, CITY) is accepted and _has_pass(fake, PAIR) is accepted


async def test_vouch_delivered_to_one_guard_is_not_known_by_another_guard():
    fake = GatesFake()
    fake.npc_states[LOTHAR] = {"state": "alive", "name": "Лотар", "distance": 2000}
    async with setup_gate(fake) as system:
        fact = await _vouch(system)
        chronicle, gates = system[2], system[3]
        chronicle.record({"fact": "gate_vouch_heard", "vouch": fact["id"], "to": PAIR})
        await gates.prepare(CITY)
        assert (await _admit(system)).status == "rejected"
        chronicle.record({"fact": "gate_vouch_heard", "vouch": fact["id"], "to": CITY})
        await gates.prepare(CITY)
        assert "basis vouch" in (gates.situation(CITY) or "")
        assert (await _admit(system)).status == "accepted"
        assert _has_pass(fake, CITY) and _has_pass(fake, PAIR)


async def test_vouchers_news_becomes_known_only_after_delivery_to_named_guard():
    async with setup_gate(GatesFake()) as system:
        chronicle, gates = system[2], system[3]
        vouch = await _vouch(system)
        await gates.prepare(CITY)
        news = gates.news(LOTHAR, CITY)
        assert news is not None and "Лотар" in news[1]
        assert chronicle.facts("gate_vouch_heard") == []
        assert (await _admit(system)).status == "rejected"
        await gates.told(news[0], LOTHAR, CITY)
        heard = chronicle.facts("gate_vouch_heard")
        assert len(heard) == 1
        assert (heard[0]["vouch"], heard[0]["from"], heard[0]["to"]) == (vouch["id"], LOTHAR, CITY)
        await gates.prepare(CITY)
        assert (await _admit(system)).status == "accepted"


@pytest.mark.parametrize("minute,accepted", [(29, True), (30, False)])
async def test_vouch_expires_at_exactly_two_game_days(minute, accepted):
    fake = GatesFake()
    fake.day, fake.hour, fake.minute = 4, 14, 30
    async with setup_gate(fake) as system:
        fact = await _vouch(system)
        system[2].record({"fact": "gate_vouch_heard", "vouch": fact["id"], "to": CITY})
        fake.day, fake.hour, fake.minute = 6, 14, minute
        await system[3].prepare(CITY)
        assert ("basis vouch" in (system[3].situation(CITY) or "")) is accepted
        assert (await _admit(system)).status == ("accepted" if accepted else "rejected")


@pytest.mark.parametrize("kind", ["gate_vouch", "contract", "quest_given", "quest", "deal", "meet"])
async def test_named_claim_truth_accepts_each_recorded_relationship(kind):
    async with setup_gate(GatesFake()) as system:
        fact = {"fact": kind, "id": "relationship-1", "giver" if kind == "quest" else "npc": HARAD}
        if kind == "gate_vouch":
            fact.update(at=DAY, expires=3 * DAY)
        system[2].record(fact)
        assert await system[3].claim_truth(HARAD) is True
        assert await system[3].claim_truth(BOSPER) is False


@pytest.mark.parametrize("master,npc", [(1, BOSPER), (2, HARAD), (3, CONSTANTINO)])
async def test_named_claim_truth_reads_apprenticeship_and_correct_master(master, npc):
    fake = GatesFake()
    fake.vars["PLAYER_ISAPPRENTICE"] = master
    async with setup_gate(fake) as system:
        assert await system[3].claim_truth(npc) is True
        for other in {BOSPER, HARAD, CONSTANTINO} - {npc}:
            assert await system[3].claim_truth(other) is False


async def test_named_claim_truth_reads_native_quest_state():
    fake = GatesFake()
    async with setup_gate(fake) as system:
        gates = system[3]
        quest, dialog = "MIS_HARAD_TASK", "DIA_HARAD_TASK"
        gates.graph.nodes[dialog] = Node(dialog, "dialog", HARAD, "Task", [], {quest})
        gates.graph.quests[quest] = Quest(quest, {}, {dialog}, set(), set(), set(), set())
        fake.vars[quest] = 0
        assert await gates.claim_truth(HARAD) is False
        fake.vars[quest] = 1
        assert await gates.claim_truth(HARAD) is True


@pytest.mark.parametrize("truth", [False, True])
async def test_named_npc_confirms_only_recorded_truth_when_asked(truth):
    fake = GatesFake()
    _reachable(fake)
    async with setup_gate(fake) as system:
        if truth:
            system[2].record({"fact": "deal", "id": "deal-harad", "npc": HARAD})
        assert (await _admit(system, basis="claim", about=HARAD)).status == "accepted"
        answer = await system[3].claim_heard(HARAD, CITY, "Меня ждёт Гарад.")
        assert answer
        assert system[2].facts("gate_claim_checked")[-1]["truth"] is truth
        assert bool(system[2].facts("caught")) is not truth
        assert _has_pass(fake, CITY) is truth and _has_pass(fake, PAIR) is truth


async def test_named_npc_hears_claim_when_its_listener_delivers_it():
    fake = GatesFake()
    _reachable(fake)
    async with setup_gate(fake) as system:
        assert (await _admit(system, basis="claim", about=HARAD)).status == "accepted"
        gates = system[3]
        news = gates.news(CITY, HARAD)
        assert news is not None and news[1] == "Меня ждёт Гарад."
        assert gates.news(PAIR, HARAD) is None
        await gates.told(news[0], CITY, HARAD)
        assert len(system[2].facts("caught")) == 1
        assert not _has_pass(fake, CITY) and not _has_pass(fake, PAIR)


async def test_next_morning_exposes_lie_revokes_pair_and_doubles_that_guards_minimum_bribe():
    fake = GatesFake()
    fake.day, fake.hour, fake.minute = 4, 17, 0
    _reachable(fake)
    fake.people.append({"npc": PAIR, "guild": 2, "npctype": 0, "routine": []})
    fake.people.append({"npc": CITY, "guild": 2, "npctype": 0, "routine": []})
    async with setup_gate(fake) as system:
        chronicle, gates = system[2], system[3]
        assert (await _admit(system, basis="claim", about=HARAD)).status == "accepted"
        claim = chronicle.facts("gate_claim")[0]
        assert claim["truth"] is False and _has_pass(fake, CITY) and _has_pass(fake, PAIR)
        fake.vars["MIL_333_SCHONMALREINGELASSEN"] = 1
        fake.day, fake.hour, fake.minute = 5, 5, 59
        await gates.check_claims()
        assert chronicle.facts("caught") == []
        fake.hour, fake.minute = 6, 0
        await gates.check_claims()
        assert not _has_pass(fake, CITY) and not _has_pass(fake, PAIR)
        assert fake.vars["MIL_310_SCHONMALREINGELASSEN"] == 0
        assert fake.vars["MIL_333_SCHONMALREINGELASSEN"] == 0
        caught = chronicle.facts("caught")
        assert len(caught) == 1
        assert caught[0]["npc"] == CITY and caught[0]["gate_claim"] == claim["id"]
        assert Trust(chronicle).of(CITY).lied
        assert not Trust(chronicle).of(CITY).heard
        assert not any(f.get("to") == f.get("from") == CITY for f in chronicle.facts("rumor"))
        assert any(f.get("to") == PAIR and f.get("kind") == "caught"
                   for f in chronicle.facts("rumor"))
        assert Trust(chronicle).of(PAIR).heard
        assert "120" in await gates.bribe_error(CITY, 119)
        assert not await gates.bribe_error(CITY, 120)
        assert not await gates.bribe_error(PAIR, 60)
        await gates.check_claims()
        assert len(chronicle.facts("caught")) == 1


@pytest.mark.parametrize("reason", ["truth", "unnamed", "not-citizen", "no-routine", "dead", "absent"])
async def test_morning_rule_does_not_expose_true_unnamed_or_unreachable_claims(monkeypatch, reason):
    fake = GatesFake()
    _reachable(fake, guild=8 if reason == "not-citizen" else 3,
               state=reason if reason in {"dead", "absent"} else "alive")
    if reason == "no-routine":
        fake.people[0]["routine"] = []
    async with setup_gate(fake) as system:
        people_calls = 0
        story_people = system[0].story_people

        async def read_people():
            nonlocal people_calls
            people_calls += 1
            return await story_people()

        monkeypatch.setattr(system[0], "story_people", read_people)
        if reason == "truth":
            system[2].record({"fact": "contract", "id": "C1", "npc": HARAD})
        about = "" if reason == "unnamed" else HARAD
        assert (await _admit(system, basis="claim", about=about)).status == "accepted"
        fake.day += 1
        fake.hour, fake.minute = 6, 0
        await system[3].check_claims()
        calls = people_calls
        await system[3].check_claims()
        assert people_calls == calls == (0 if reason == "unnamed" else 1)
        if reason not in {"truth", "unnamed"}:
            [checked] = system[2].facts("gate_claim_checked")
            assert checked["truth"] is None and checked["skipped"]
        assert system[2].facts("caught") == []
        assert _has_pass(fake, CITY) and _has_pass(fake, PAIR)


async def test_relationship_created_after_admission_does_not_rewrite_false_claim():
    fake = GatesFake()
    _reachable(fake)
    async with setup_gate(fake) as system:
        assert (await _admit(system, basis="claim", about=HARAD)).status == "accepted"
        system[2].record({"fact": "contract", "id": "C1", "npc": HARAD})
        fake.day += 1
        fake.hour, fake.minute = 6, 0
        await system[3].check_claims()
        assert len(system[2].facts("caught")) == 1


@pytest.mark.parametrize("offset,responsible", [(0, True), (2 * DAY, True), (5 * DAY - 1, True),
                                                (5 * DAY, False), (-1, False)])
async def test_voucher_responsibility_lasts_until_three_days_after_expiry(offset, responsible):
    async with setup_gate(GatesFake()) as system:
        chronicle = system[2]
        vouch = await _vouch(system)
        original = copy.deepcopy(vouch)
        chronicle.record({
            "fact": "crime", "id": "city-crime", "kind": "theft", "community": "city",
            "victim": HARAD, "owner": HARAD, "day": 1, "hour": 8, "minute": 0,
        })
        assert chronicle.facts("gate_vouch_broken") == []
        chronicle.record({
            "fact": "crime_known", "community": "city", "crime": "city-crime",
            "via": CITY, "how": "witness", "at": vouch["at"] + offset,
        })
        broken = chronicle.facts("gate_vouch_broken")
        assert bool(broken) is responsible
        assert bool(Trust(chronicle).of(LOTHAR).crimes) is responsible
        assert chronicle.facts("gate_vouch")[0] == original
        if responsible:
            assert (broken[0]["vouch"], broken[0]["crime"], broken[0]["npc"]) == (
                vouch["id"], "city-crime", LOTHAR,
            )


async def test_voucher_does_not_answer_for_unknown_crime_or_crime_outside_city():
    async with setup_gate(GatesFake()) as system:
        vouch = await _vouch(system)
        chronicle = system[2]
        for crime, community in [("secret-city", "city"), ("farm-crime", "farm")]:
            chronicle.record({
                "fact": "crime", "id": crime, "kind": "theft", "community": community,
            })
        chronicle.record({
            "fact": "crime_known", "community": "farm", "crime": "farm-crime",
            "via": "BAU_950_LOBART", "at": vouch["at"] + 1,
        })
        assert chronicle.facts("gate_vouch_broken") == []
        assert not Trust(chronicle).of(LOTHAR).crimes


async def test_step_one_claim_without_id_is_checked_only_once():
    fake = GatesFake()
    _reachable(fake)
    async with setup_gate(fake) as system:
        chronicle, gates = system[2], system[3]
        chronicle.record({
            "fact": "gate_claim", "npc": CITY, "about": HARAD, "claim": "Меня ждёт Гарад.",
            "believed": True, "day": 0, "hour": 14, "minute": 30,
        })
        await gates.check_claims()
        assert len(chronicle.facts("caught")) == len(chronicle.facts("gate_claim_checked")) == 1
        count = len(chronicle.facts())
        await gates.check_claims()
        assert len(chronicle.facts()) == count


async def test_guard_must_identify_the_gate_claim_when_asking_named_npc():
    fake = GatesFake()
    _reachable(fake)
    async with setup_gate(fake) as system:
        await _admit(system, basis="claim", about=HARAD)
        answer = await system[3].claim_heard(HARAD, CITY, "Гарад ждёт героя")
        assert answer is None and system[2].facts("caught") == []
        assert "Меня ждёт Гарад." in system[3].situation(CITY)
        answer = await system[3].claim_heard(HARAD, CITY, "Меня ждёт Гарад")
        assert answer and len(system[2].facts("caught")) == 1


@pytest.mark.parametrize("already_checked", [False, True])
async def test_old_gate_claim_does_not_replace_unrelated_witness_testimony(monkeypatch, already_checked):
    fake = GatesFake()
    async with setup_gate(fake) as system:
        world = World(system[0], fake, lobart())
        _reachable(fake)
        gates, chronicle = system[3], system[2]
        world.witnesses.gate_claim = gates.claim_heard
        await _admit(system, basis="claim", about=HARAD)
        if already_checked:
            assert await gates.claim_heard(HARAD, CITY, "Меня ждёт Гарад.")
        notes = len(gates.dialogue.conversations.get(HARAD).memory.entries) if already_checked else 0
        world.chronicle.record({
            "fact": "covered", "npc": HARAD, "deal": "cover-theft", "day": fake.day,
            "vouch": "Герой был со мной.",
        })
        testimony = await world.witnesses._testify(
            None, CITY, "Привратник", HARAD, "Гарад", "он украл у меня кошелёк", "near", 0,
        )
        assert testimony is not None and testimony.gate_claim == ""
        assert testimony.cover == "cover-theft" and testimony.vouch == "Герой был со мной."
        reactions = []
        monkeypatch.setattr(world.dialogue, "event_turn", lambda npc, cue, **kw: reactions.append(kw))
        world.witnesses._react(CITY, testimony)
        assert "catch_lie" in reactions[0]["tools"]
        assert len(chronicle.facts("gate_claim_checked")) == int(already_checked)
        if already_checked:
            assert await gates.claim_heard(HARAD, CITY, "Меня ждёт Гарад.") is None
            assert len(gates.dialogue.conversations[HARAD].memory.entries) == notes


async def test_unknown_claim_admission_keeps_claim_and_can_be_exposed_next_morning(monkeypatch):
    fake = GatesFake()
    _reachable(fake)
    async with setup_gate(fake) as system:
        link, dialogue, chronicle, gates, _, conv = system
        run = gate_code._run

        async def unconfirmed(*args):
            fake.fields[(CITY, "AIVAR", 14)] = fake.fields[(PAIR, "AIVAR", 14)] = 1
            raise gate_code.GateResultUnknown("lost result read")

        jobs = []
        monkeypatch.setattr(gate_code, "_run", unconfirmed)
        monkeypatch.setattr(dialogue, "after_speech", lambda conv, job: jobs.append(job))
        outcome = await gates.handle(dialogue, conv, ToolCall("let_pass", {
            "basis": "claim", "claim": "Меня ждёт Гарад.", "about": HARAD,
        }), link.snapshots[CITY])
        assert outcome.status == "accepted"
        note, ok = await jobs[0]()
        assert not ok and "unknown" in note and "refused" not in note.lower()
        [claim] = chronicle.facts("gate_claim")
        assert claim["admission"] == "unknown" and claim["truth"] is False
        assert _has_pass(fake, CITY) and _has_pass(fake, PAIR)
        monkeypatch.setattr(gate_code, "_run", run)
        fake.day += 1
        fake.hour, fake.minute = 6, 0
        await gates.check_claims()
        assert chronicle.facts("caught")[0]["gate_claim"] == claim["id"]
        assert not _has_pass(fake, CITY) and not _has_pass(fake, PAIR)


@pytest.mark.parametrize("error", [gate_code.GameError, gate_code.GateResultUnknown])
async def test_revocation_retries_remember_the_named_witness_denial_only_once(monkeypatch, error):
    fake = GatesFake()
    _reachable(fake)
    async with setup_gate(fake) as system:
        _, _, chronicle, gates, _, _ = system
        await _admit(system, basis="claim", about=HARAD)
        original_run = gate_code._run
        attempts = 0

        async def retry_revocation(link, npc, *args):
            nonlocal attempts
            if npc == "PC_HERO":
                attempts += 1
                if attempts <= 2:
                    raise error("Game unavailable")
            await original_run(link, npc, *args)

        monkeypatch.setattr(gate_code, "_run", retry_revocation)
        fake.day += 1
        fake.hour, fake.minute = 6, 0
        for _ in range(2):
            with pytest.raises(error):
                await gates.check_claims()
        assert not chronicle.facts("gate_claim_checked")
        assert not any("Тебе передали заявление героя" in e.content
                       for e in chronicle.memory.load(chronicle.branch, HARAD).entries)
        await gates.check_claims()
        await gates.check_claims()
        assert attempts == 3 and len(chronicle.facts("gate_claim_checked")) == 1
        notes = [e.content for e in chronicle.memory.load(chronicle.branch, HARAD).entries
                 if "Тебе передали заявление героя" in e.content]
        assert len(notes) == 1 and "Отрицаю" in notes[0]


@pytest.mark.parametrize("first", ["morning", "witness"])
async def test_concurrent_morning_check_and_witness_keep_the_code_verdict(monkeypatch, first):
    fake = GatesFake()
    async with setup_gate(fake) as system:
        world = World(system[0], fake, lobart())
        _reachable(fake)
        gates, chronicle = system[3], system[2]
        world.witnesses.gate_claim = gates.claim_heard
        # A step-one claim has no stored truth: checking it awaits the game's relationship state.
        chronicle.record({
            "fact": "gate_claim", "id": "concurrent-claim", "npc": CITY, "about": HARAD,
            "claim": "Меня ждёт Гарад.", "believed": True, "day": 0, "hour": 14, "minute": 30,
        })
        fake.fields[(CITY, "AIVAR", 14)] = fake.fields[(PAIR, "AIVAR", 14)] = 1
        truth_started, finish_truth, both_started = asyncio.Event(), asyncio.Event(), asyncio.Event()
        check = gates._check_claim
        starts = 0

        async def relationship_truth(about):
            truth_started.set()
            await finish_truth.wait()
            return False

        async def checking(*args):
            nonlocal starts
            starts += 1
            if starts == 2:
                both_started.set()
            return await check(*args)

        monkeypatch.setattr(gates, "claim_truth", relationship_truth)
        monkeypatch.setattr(gates, "_check_claim", checking)

        async def witness():
            return await world.witnesses._testify(
                "concurrent-answer", CITY, "Привратник", HARAD, "Гарад", "Меня ждёт Гарад", "far", 0,
            )

        calls = {"morning": gates.check_claims, "witness": witness}
        early = asyncio.create_task(calls[first]())
        await asyncio.wait_for(truth_started.wait(), 2)
        late = asyncio.create_task(calls["witness" if first == "morning" else "morning"]())
        await asyncio.wait_for(both_started.wait(), 2)
        finish_truth.set()
        await asyncio.gather(early, late)
        [testimony] = world.chronicle.facts("testimony")
        assert testimony["gate_claim"].startswith("Отрицаю")
        assert len(chronicle.facts("caught")) == len(chronicle.facts("gate_claim_checked")) == 1
        notes = [e.content for e in chronicle.memory.load(chronicle.branch, HARAD).entries
                 if "Тебе передали заявление героя" in e.content]
        assert len(notes) == 1
        assert not _has_pass(fake, CITY) and not _has_pass(fake, PAIR)


@pytest.mark.parametrize("during", ["vouch_hearing", "caught_clock"])
async def test_load_during_gate_check_does_not_write_old_knowledge_to_new_branch(monkeypatch, during):
    fake = GatesFake()
    _reachable(fake)
    fake.npc_states[LOTHAR] = {"state": "alive", "distance": 500}
    async with setup_gate(fake) as system:
        link, _, chronicle, gates, _, _ = system
        if during == "vouch_hearing":
            await _vouch(system)
        else:
            await _admit(system, basis="claim", about=HARAD)
        original = link.story_state

        async def load_while_reading(*args, **kwargs):
            state = await original(*args, **kwargs)
            if during == "caught_clock" or kwargs.get("near") == CITY:
                await fake.send(type="session", branch="loaded", parent_seq=0, reason="load")
                await _until(lambda: chronicle.branch == "loaded")
            return state

        monkeypatch.setattr(link, "story_state", load_while_reading)
        if during == "vouch_hearing":
            assert await gates._known_vouches(CITY) == []
        else:
            await gates.claim_heard(HARAD, CITY, "Меня ждёт Гарад.")
        assert chronicle.branch == "loaded" and chronicle.facts() == []
