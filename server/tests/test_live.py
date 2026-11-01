"""Live outcomes are measurements; setup errors alone fail the scenario."""

import json

import pytest

from gothic_ai.scenarios.live import LiveSuite, estimate, selected_cases, summarize, write_report
from gothic_ai.trace import TraceWriter


def test_live_lies_selection_and_recorded_cases(tmp_path, monkeypatch):
    names = (
        "seen_alibi", "trace_age", "trace_control", "witness_alibi", "vcheck_claim",
        "crime_testimony", "frame_belief", "cover_offer",
    )
    assert tuple(spec.name for spec in selected_cases("live_lies")) == names
    assert tuple(spec.name for spec in selected_cases("live_lies", "cover_offer, seen_alibi")) == (
        "seen_alibi", "cover_offer",
    )
    with pytest.raises(ValueError, match="unknown CASES for live_lies: broken"):
        selected_cases("live_lies", "broken")
    monkeypatch.delenv("CASES", raising=False)
    trace = TraceWriter(tmp_path / "traces")
    suite = LiveSuite("live_lies", trace)
    outcomes = ("success", "failure", "success", "no_decision", "failure", "success", "observed", "observed")
    for name, outcome in zip(names, outcomes, strict=True):
        with suite.case(name) as case:
            trace.write({"type": "turn", "id": name, "steps": [{}], "events": []})
            case.finish(outcome, evidence={"decision": name})
    assert all(value.startswith("done") for value in suite.results().values())
    data = json.loads((tmp_path / "live.json").read_text())
    assert [case["name"] for case in data["cases"]] == list(names)
    assert [case["outcome"] for case in data["cases"]] == list(outcomes)
    assert all(case["requests"] == 1 for case in data["cases"])


@pytest.mark.parametrize(("name", "names", "outcomes"), [
    ("live_npc", (
        "wrath_tip", "calm_bandit", "cave_unnamed", "errand_tell", "spare_bandit",
        "initiative_debt", "knows_danger", "fame_recognized", "rescue_call",
    ), (
        "success", "observed", "failure", "observed", "observed", "failure",
        "observed", "observed", "observed",
    )),
    ("live_director", (
        "repair_handover", "authored_quest", "character_cast", "chain_built", "crime_line",
        "crime_return", "amends_storylet", "feat_award",
    ), (
        "success", "failure", "success", "failure", "no_decision", "success",
        "observed", "no_decision",
    )),
])
def test_npc_and_director_selection_and_recorded_outcomes(
    tmp_path, monkeypatch, name, names, outcomes,
):
    assert tuple(spec.name for spec in selected_cases(name)) == names
    assert tuple(spec.name for spec in selected_cases(name, f"{names[-1]}, {names[0]}")) == (
        names[0], names[-1],
    )
    with pytest.raises(ValueError, match=f"unknown CASES for {name}: broken"):
        selected_cases(name, "broken")
    monkeypatch.delenv("CASES", raising=False)
    trace = TraceWriter(tmp_path / "traces")
    suite = LiveSuite(name, trace)
    for case_name, outcome in zip(names, outcomes, strict=True):
        with suite.case(case_name) as case:
            trace.write({"type": "turn", "id": case_name, "steps": [{}], "events": []})
            case.finish(outcome, evidence={"decision": case_name})
    assert suite.results() == {
        case_name: f"done ({outcome})" for case_name, outcome in zip(names, outcomes, strict=True)
    }
    data = json.loads((tmp_path / "live.json").read_text())
    assert [case["name"] for case in data["cases"]] == list(names)
    assert [case["outcome"] for case in data["cases"]] == list(outcomes)
    assert all(case["requests"] == 1 for case in data["cases"])


async def test_live_npc_skips_decider_cases_without_preparing_a_game(tmp_path, monkeypatch):
    from types import SimpleNamespace
    from unittest.mock import AsyncMock

    from gothic_ai.scenarios.live_npc import live_npc

    selected = "initiative_debt,rescue_call"
    monkeypatch.setenv("CASES", selected)
    monkeypatch.delenv("DECIDER_URL", raising=False)
    link = SimpleNamespace(wait_world=AsyncMock(side_effect=AssertionError("skip needs no game")))
    state = SimpleNamespace(dialogue=SimpleNamespace(
        llm=SimpleNamespace(name="chatgpt"), trace=TraceWriter(tmp_path / "traces"),
    ))
    assert await live_npc(link, state) == {
        "initiative_debt": "done (skipped)", "rescue_call": "done (skipped)",
    }
    data = json.loads((tmp_path / "live.json").read_text())
    assert all(case["outcome"] == "skipped" and not case["setup_error"] for case in data["cases"])
    assert all("DECIDER_URL" in case["evidence"]["skip_reason"] for case in data["cases"])
    assert all(case["requests"] == 0 for case in data["cases"])
    run = tmp_path / "run-1"
    run.mkdir()
    (run / "live.json").write_text(json.dumps(data))
    report = summarize(tmp_path, "live_npc", 1, [0], selected)
    assert report["setup_errors"] == 0
    assert all(case["attempts"] == 0 and case["skipped"] == 1 for case in report["cases"])
    write_report(tmp_path, report)
    assert "DECIDER_URL" in (tmp_path / "report.md").read_text()


@pytest.mark.parametrize(("evidence", "settled", "outcome"), [
    ([10, 11], True, "success"), ([12], True, "failure"),
    (None, True, "no_decision"), (None, False, "no_decision"),
])
async def test_live_feat_award_requires_evidence_of_the_actual_kill(
    tmp_path, monkeypatch, evidence, settled, outcome,
):
    from importlib import import_module
    from types import SimpleNamespace
    from unittest.mock import AsyncMock, Mock

    scenario = import_module("gothic_ai.scenarios.feats")
    monkeypatch.setenv("CASES", "feat_award")
    monkeypatch.setattr(scenario, "model_off", Mock(side_effect=AssertionError("live uses no givens")))
    monkeypatch.setattr(scenario, "_save_or_fail", AsyncMock(
        return_value=SimpleNamespace(branch="saved", seq=0),
    ))
    suite = LiveSuite("live_director", TraceWriter(tmp_path / "traces"))
    victim = "BDT_1014_BANDIT_L"
    entries = [
        (10, {"fact": "crime", "kind": "murder", "victim": victim}),
        (11, {"fact": "died", "npc": victim, "killer": "PLAYER"}),
        (12, {"fact": "died", "npc": "OTHER", "killer": "PLAYER"}),
    ]
    awards = [{"id": "award", "evidence": evidence}] if evidence is not None else []
    chronicle = SimpleNamespace(on_entry=[], entries=lambda: entries)
    chronicler = SimpleNamespace(
        artist=scenario.ScenarioArtist(tmp_path / "art"), chronicle=chronicle, feats=[],
        _work=None if settled else SimpleNamespace(done=lambda: False),
    )
    waits = []

    async def until(check, timeout):
        done = bool(check())
        waits.append((timeout, done))
        return done

    async def debug(command, **kwargs):
        if command == "kill":
            for seq, fact in entries:
                for callback in chronicle.on_entry:
                    callback(seq, fact)
            chronicler.feats.extend(awards)

    monkeypatch.setattr(scenario, "_until", until)
    link = SimpleNamespace(
        wait_world=AsyncMock(), session=object(), send=AsyncMock(), listeners=[],
        snapshot=AsyncMock(), story_nearby=AsyncMock(return_value=[]), debug=debug,
    )
    with suite.case("feat_award") as case:
        results = await scenario.feats(link, chronicler, SimpleNamespace(sink=None), object(), live=case)
    assert all(value.startswith("done") for value in results.values())
    assert waits == [(10, True), (120, settled)]
    assert suite.results() == {"feat_award": f"done ({outcome})"}
    [case] = json.loads((tmp_path / "live.json").read_text())["cases"]
    assert case["evidence"]["kill_sequences"] == [10, 11]
    assert case["evidence"]["awards"] == awards
    assert case["evidence"]["settled"] is settled


@pytest.mark.parametrize(("arrived", "outcome"), [(True, "success"), (False, "failure")])
async def test_live_cave_normalizes_the_call_and_waits_for_actual_arrival(
    tmp_path, monkeypatch, arrived, outcome,
):
    from types import SimpleNamespace
    from unittest.mock import AsyncMock, Mock

    from gothic_ai.scenarios import perception as scenario

    monkeypatch.setenv("CASES", "cave_unnamed")
    monkeypatch.setattr(scenario.asyncio, "sleep", AsyncMock())
    monkeypatch.setattr(scenario, "model_off", Mock(side_effect=AssertionError("live uses no givens")))
    trace = TraceWriter(tmp_path / "traces")
    suite = LiveSuite("live_npc", trace)
    npc = "BAU_4300_ADDON_CAVALORN"
    sent, positions, checks = [], [], []

    def hear(text, *_):
        if "зайди в пещеру" in text:
            sent.append(text)
            trace.write({
                "type": "turn", "id": "cave", "npc": npc, "status": "completed",
                "trigger": {"text": text}, "steps": [{"calls": [{
                    "name": "go_to", "outcome": "accepted",
                    "arguments": {"place": " xardas_bandits "},
                }]}],
            })

    async def snapshot(*_, **__):
        if sent:
            positions.append("CAVE" if arrived and positions else "STEPS")
        return SimpleNamespace(npc=SimpleNamespace(
            pos=(0, 0, 0), nearest_waypoint=positions[-1] if positions else "STEPS",
        ))

    async def until_async(poll, check, timeout):
        for _ in range(2):
            done = bool(check(await poll()))
            if sent:
                checks.append(done)
            if done:
                return True
        return False

    monkeypatch.setattr(scenario, "_until_async", until_async)
    book = SimpleNamespace(
        get=lambda _: SimpleNamespace(center=(0, 0, 0)),
        of=lambda point: SimpleNamespace(id="XARDAS_BANDITS" if point == "CAVE" else "XARDAS_STAIRS"),
    )
    link = SimpleNamespace(wait_world=AsyncMock(), debug=AsyncMock(), snapshot=snapshot)
    dialogue = SimpleNamespace(open=Mock(return_value=SimpleNamespace()), conversations={},
                               speaking=lambda _: False)
    talk = SimpleNamespace(people={npc: SimpleNamespace(distance=300)}, hear=hear, orders=SimpleNamespace(
        companion=lambda _: True,
        situation=lambda _: "Места рядом: пещера бандитов: пещера, перед героем, вход — на ступенях",
    ))
    whereabouts = SimpleNamespace(book=book, where=lambda *_: "XARDAS_BANDITS — вход — на ступенях. Рядом:")
    with suite.case("cave_unnamed") as case:
        results = await scenario.cave_scenario(link, dialogue, talk, whereabouts, live=case)
    assert all(value.startswith("done") for value in results.values())
    assert checks == ([False, True] if arrived else [False, False])
    assert suite.results() == {"cave_unnamed": f"done ({outcome})"}
    [case] = json.loads((tmp_path / "live.json").read_text())["cases"]
    assert case["evidence"]["inside"] is arrived


@pytest.mark.parametrize("suite_case", ["witness_alibi", "crime_testimony"])
@pytest.mark.parametrize(("tool_outcome", "reply_status", "delivery", "outcome"), [
    ("accepted", "completed", "delayed", "success"),
    ("accepted", "completed", "missing", "failure"),
    ("accepted", "completed", "unrelated", "failure"),
    ("rejected", "completed", "missing", "failure"),
    (None, "completed", "missing", "failure"),
    ("accepted", "llm_error", "missing", "no_decision"),
])
async def test_live_testimony_waits_for_normalized_accusation_fact(
    tmp_path, monkeypatch, suite_case, tool_outcome, reply_status, delivery, outcome,
):
    from types import SimpleNamespace

    from gothic_ai.scenarios import crime_talk as crime_scenario
    from gothic_ai.scenarios import lies as witness_scenario

    scenario = witness_scenario if suite_case == "witness_alibi" else crime_scenario
    monkeypatch.setenv("CASES", suite_case)
    trace = TraceWriter(tmp_path / "traces")
    suite = LiveSuite("live_lies", trace)
    npc, witness, crime = scenario.LOBART, scenario.HILDA, "theft"
    said = "я всё утро был в городе" if suite_case == "witness_alibi" else scenario.DENIAL
    fact = {
        "npc": npc, "witness": witness, "crime": crime,
        "title": said, "how": "узнал со слов: Хильда видела героя",
    }
    facts = {"testimony": [{"npc": npc, "witness": witness}]}
    checks, deadlines = [], []

    async def until(check, timeout):
        deadlines.append(timeout)
        checks.append(bool(check()))
        if delivery != "missing":
            facts["caught"] = [fact if delivery == "delayed" else {**fact, "title": "other words"}]
            checks.append(bool(check()))
        return checks[-1]

    monkeypatch.setattr(scenario, "_until", until)
    with suite.case(suite_case) as case:
        trace.write({
            "type": "turn", "id": "question", "npc": npc, "status": "completed",
            "trigger": {"text": said}, "events": [],
            "steps": [{"calls": [{"name": "ask_witness", "outcome": "accepted"}]}],
        })
        trace.write({
            "type": "turn", "id": "reply", "npc": npc, "status": reply_status,
            "trigger": {"text": "Ответ: Хильда видела героя"}, "events": [],
            "steps": [{"calls": [{
                "name": "catch_lie", "outcome": tool_outcome,
                "arguments": {"seen": "C1", "said": f"«{said}.»"},
            }] if tool_outcome else []}],
        })
        chronicle = SimpleNamespace(facts=lambda kind: facts.get(kind, []))
        if suite_case == "witness_alibi":
            await scenario._record_live_witness(case, chronicle, npc, witness, set())
        else:
            await scenario._record_live_testimony(case, chronicle, crime, set())
    assert deadlines == [90]
    assert checks == ([False, True] if delivery == "delayed" else
                      [False, False] if delivery == "unrelated" else
                      [tool_outcome != "accepted"])
    assert suite.results() == {suite_case: f"done ({outcome})"}
    [record] = json.loads((tmp_path / "live.json").read_text())["cases"]
    assert record["outcome"] == outcome
    assert record["evidence"]["caught"] == ([fact] if delivery == "delayed" else [])


@pytest.mark.parametrize(("tool_outcome", "has_caught", "status", "outcome"), [
    ("accepted", True, "completed", "success"),
    ("accepted", False, "completed", "failure"),
    ("rejected", True, "completed", "failure"),
    ("accepted", False, "llm_error", "no_decision"),
])
async def test_live_vcheck_records_execution_instead_of_promised_check(
    tmp_path, monkeypatch, tool_outcome, has_caught, status, outcome,
):
    from types import SimpleNamespace
    from unittest.mock import AsyncMock, Mock

    from gothic_ai.scenarios import lies as scenario

    monkeypatch.setenv("CASES", "vcheck_claim")
    monkeypatch.setattr(scenario.asyncio, "sleep", AsyncMock())
    monkeypatch.setattr(scenario, "model_off", Mock(side_effect=AssertionError("live uses no givens")))

    async def until(check, timeout):
        return bool(check())

    monkeypatch.setattr(scenario, "_until", until)
    trace = TraceWriter(tmp_path / "traces")
    suite = LiveSuite("live_lies", trace)
    npc = scenario.LOBART
    caught = {"npc": npc, "quest": scenario.BUGS, "how": "сходил проверить: хищники живы"}
    facts = {}

    async def debug(command, **kwargs):
        if command == "type":
            trace.write({
                "type": "turn", "id": "claim", "npc": npc, "status": status,
                "trigger": {"text": kwargs["text"]}, "events": [],
                "steps": [{"text": "I will check.", "calls": [{
                    "name": "check_claim", "arguments": {"quest": scenario.BUGS},
                    "outcome": tool_outcome,
                }]}],
            })
            if has_caught:
                facts["caught"] = [caught]

    link = SimpleNamespace(
        session=object(), wait_world=AsyncMock(), debug=AsyncMock(side_effect=debug),
        story_effect=AsyncMock(), story_spawn=AsyncMock(), evaluate=AsyncMock(return_value=100),
        story_state=AsyncMock(return_value={"npcs": {"bug": {
            "distance": (scenario.SEE_CM + scenario.CHECK_CM) / 2,
        }}}),
    )
    dialogue = SimpleNamespace(
        conversations={}, open=Mock(return_value=object()),
        _situation=AsyncMock(return_value=("", None, None)),
        _offered=Mock(return_value=[SimpleNamespace(name="check_claim")]),
    )
    talk = SimpleNamespace(people={npc: SimpleNamespace(name="Lobart", distance=250)})
    vanilla = SimpleNamespace(
        checking=set(), chronicle=SimpleNamespace(facts=lambda kind: facts.get(kind, [])),
    )
    with suite.case("vcheck_claim") as case:
        result = await scenario.vanilla_check(link, dialogue, talk, vanilla, live=case)
    assert all(value.startswith("done") for value in result.values())
    assert suite.results() == {"vcheck_claim": f"done ({outcome})"}
    [case] = json.loads((tmp_path / "live.json").read_text())["cases"]
    assert case["outcome"] == outcome
    assert case["evidence"]["caught"] == ([caught] if has_caught else [])
    assert case["requests"] == 1


def test_case_outcomes_requests_and_live_json(tmp_path, monkeypatch):
    selected = "A_lobart,A_bandit,B_lobart,B_bandit,B_guard,C_bandit"
    monkeypatch.setenv("CASES", selected)
    trace = TraceWriter(tmp_path / "traces")
    trace.write({"type": "turn", "id": "before", "steps": [{}, {}, {}], "events": [{"kind": "retry"}]})
    suite = LiveSuite("surrender_live", trace)
    outcomes = ("success", "failure", "no_decision", "observed")
    for index, outcome in enumerate(outcomes):
        with suite.case(suite.selected[index]) as case:
            trace.write({"type": "instructions", "hash": "prompt", "text": "prompt"})
            trace.write({"type": "combat_bark", "outcome": "said"})
            trace.write({
                "type": "turn", "id": f"turn-{index}", "steps": [{}, {}],
                "events": [{"kind": "retry"}, {"kind": "tool_start"}] if index == 0 else [],
            })
            assert case.requests == (3 if index == 0 else 2)
            case.finish(outcome, evidence={"decision": "fight"})
    with suite.case("B_guard"):
        raise RuntimeError("game lost connection")
    with suite.case("C_bandit") as case:
        case.finish("observed", evidence={"decision": "surrender"})
    trace.write({"type": "turn", "id": "after", "steps": [{}], "events": [{"kind": "retry"}]})
    result = suite.results()
    assert all(result[name].startswith("done") for name in suite.selected if name != "B_guard")
    assert result["B_guard"] == "failed (RuntimeError: game lost connection)"
    data = json.loads((tmp_path / "live.json").read_text())
    assert data["suite"] == "surrender_live"
    assert [case["outcome"] for case in data["cases"]] == [*outcomes, None, "observed"]
    assert [case["requests"] for case in data["cases"]] == [3, 2, 2, 2, 0, 0]
    assert data["cases"][0]["transcripts"] == [{"path": str(trace.path()), "turns": ["turn-0"]}]
    assert data["cases"][3]["evidence"] == {"decision": "fight"}

    # Reporting keeps observations and setup failures separate from successes and model refusals.
    run = tmp_path / "run-1"
    run.mkdir()
    (run / "live.json").write_text(json.dumps(data))
    report = summarize(tmp_path, "surrender_live", 1, [1], selected)
    assert [row["successes"] for row in report["cases"]] == [1, 0, 0, 0, 0, 0]
    assert report["cases"][1]["failures"] == 1
    assert report["cases"][2]["no_decision"] == 1
    assert report["cases"][3]["success_rate"] is None
    assert report["cases"][3]["decisions"] == {"fight": {"count": 1, "share": 1.0}}
    assert report["cases"][4]["setup_errors"] == 1
    assert report["requests"] == 9
    write_report(tmp_path, report)
    assert estimate("surrender_live", 2, tmp_path, "A_lobart") == 6


async def test_haggle_persists_actual_deal_terms(tmp_path, monkeypatch):
    from types import SimpleNamespace
    from unittest.mock import AsyncMock

    from gothic_ai.contracts import Give
    from gothic_ai.scenarios import deals as scenario

    monkeypatch.delenv("CASES", raising=False)
    monkeypatch.setattr(scenario.asyncio, "sleep", AsyncMock())

    async def until(check, timeout):
        return bool(check())

    monkeypatch.setattr(scenario, "_until", until)
    link = SimpleNamespace(
        session=object(), wait_world=AsyncMock(), attach=AsyncMock(), act=AsyncMock(),
        detach=AsyncMock(), story_effect=AsyncMock(), debug=AsyncMock(), decline_deal=AsyncMock(),
    )
    dialogue = SimpleNamespace(trace=TraceWriter(tmp_path / "traces"), conversations={})
    people = {npc: SimpleNamespace(distance=250, name=npc) for npc in (scenario.CANTHAR, scenario.LUTERO)}
    talk = SimpleNamespace(people=people)
    deals = SimpleNamespace(
        open={
            npc: SimpleNamespace(
                version=1, hero_gives=(Give("ITMI_SILVERCUP", 1, "Silver cup"),),
                npc_gives=(Give(scenario.GOLD, price, "Gold"),),
            )
            for npc, price in ((scenario.CANTHAR, 10), (scenario.LUTERO, 20))
        },
        chronicle=SimpleNamespace(facts=lambda kind: []),
    )
    assert await scenario.haggle(link, dialogue, talk, deals) == {"greedy_price": "done (success)"}
    [case] = json.loads((tmp_path / "live.json").read_text())["cases"]
    assert case["outcome"] == "success"
    assert case["evidence"]["offers"] == {scenario.CANTHAR: [10], scenario.LUTERO: [20]}
    assert case["evidence"]["terms"][scenario.CANTHAR][0]["hero_gives"][0]["count"] == 1


@pytest.mark.parametrize(("reply", "outcome"), [
    ("Не продам.", "failure"),
    ("Нет. Одежда мне самому нужна.", "failure"),
    ("Нет проблем. Принесу.", "no_decision"),
    ("Почему бы и нет. Продам.", "no_decision"),
])
async def test_live_chest_refusals_are_distinct_from_unfulfilled_promises(
    tmp_path, monkeypatch, reply, outcome,
):
    from types import SimpleNamespace
    from unittest.mock import AsyncMock, Mock

    from gothic_ai.journal import Journal
    from gothic_ai.scenarios import npc_life as scenario

    monkeypatch.setenv("CASES", "fetch_clothes")
    monkeypatch.setattr(scenario.asyncio, "sleep", AsyncMock())
    ticks = iter((0, 0, 241))
    monkeypatch.setattr(scenario, "time", SimpleNamespace(monotonic=lambda: next(ticks)))

    async def until(check, timeout):
        return bool(check())

    monkeypatch.setattr(scenario, "_until", until)
    journal = Journal()
    npc, item = scenario.LOBART, "ITAR_BAU_L"
    chest = SimpleNamespace(id="chest", items=[SimpleNamespace(item=item, count=1)])
    link = SimpleNamespace(
        wait_world=AsyncMock(), debug=AsyncMock(), npc_chests=AsyncMock(return_value=[chest]),
        evaluate=AsyncMock(return_value=0), story_effect=AsyncMock(), listeners=[],
        snapshot=AsyncMock(return_value=SimpleNamespace(npc=SimpleNamespace(pos=(0, 0, 0)))),
        deal_window=AsyncMock(return_value=None),
    )
    dialogue = SimpleNamespace(
        trace=TraceWriter(tmp_path / "traces"), journal=journal, speaking=lambda who: False,
    )
    talk = SimpleNamespace(
        people={npc: SimpleNamespace(distance=250)},
        hear=Mock(side_effect=lambda *args: journal.add("npc", npc=npc, text=reply)),
    )
    result = await scenario._chests_lobart(
        link, dialogue, talk, SimpleNamespace(journal=journal, _going=set()), npc, item, "Sell me clothes.",
    )
    assert result == {"fetch_clothes": f"done ({outcome})"}
    [case] = json.loads((tmp_path / "live.json").read_text())["cases"]
    assert case["outcome"] == outcome
    assert case["evidence"]["refusal_words"] == ([reply] if outcome == "failure" else [])


@pytest.mark.parametrize(("name", "seen", "delivery", "outcome"), [
    ("seen_alibi", " b2 ", "delayed", "success"),
    ("trace_age", "2", "delayed", "success"),
    ("seen_alibi", "В2", "missing", "failure"),
    ("trace_age", "В2", "unrelated", "failure"),
])
async def test_live_sighting_waits_for_executed_normalized_catch(
    tmp_path, monkeypatch, name, seen, delivery, outcome,
):
    from types import SimpleNamespace

    from gothic_ai.scenarios import common, perception

    monkeypatch.setenv("CASES", name)
    trace = TraceWriter(tmp_path / "traces")
    suite = LiveSuite("live_lies", trace)
    facts, clock = [], [0.0]
    npc, denial = perception.LOBART, "I was in town"
    fact = {"npc": npc, "said": True, "title": denial}

    async def sleep(delay):
        clock[0] += 45
        if delivery == "delayed":
            facts.append(fact)
        elif delivery == "unrelated":
            facts.append({**fact, "npc": "OTHER"})

    monkeypatch.setattr(common, "time", SimpleNamespace(monotonic=lambda: clock[0]))
    monkeypatch.setattr(common.asyncio, "sleep", sleep)
    with suite.case(name) as case:
        trace.write({
            "type": "turn", "id": "alibi", "npc": npc, "status": "completed",
            "trigger": {"text": denial}, "steps": [{"calls": [{
                "name": "catch_lie", "outcome": "accepted",
                "arguments": {"seen": seen, "said": denial},
            }]}],
        })
        await perception._record_live_sighting(
            case, SimpleNamespace(facts=lambda kind: facts), npc, "В2", "situation", denial,
        )
    assert clock[0] == (45 if delivery == "delayed" else 90)
    assert suite.results() == {name: f"done ({outcome})"}
    [record] = json.loads((tmp_path / "live.json").read_text())["cases"]
    assert record["evidence"]["caught"] == ([fact] if delivery == "delayed" else [])


@pytest.mark.parametrize(("call", "fact", "outcome"), [
    (None, None, "success"),
    ("catch_lie", None, "failure"),
    ("blame", None, "failure"),
    (None, "caught", "failure"),
    (None, "blamed", "failure"),
])
async def test_live_trace_control_scores_actions_and_facts_not_words(
    tmp_path, monkeypatch, call, fact, outcome,
):
    from types import SimpleNamespace

    from gothic_ai.scenarios import perception

    monkeypatch.setenv("CASES", "trace_control")
    trace = TraceWriter(tmp_path / "traces")
    suite = LiveSuite("live_lies", trace)
    npc, denial, words = perception.LOBART, "I did not shoot", "Ты стрелял. Это твоя стрела!"
    facts = {fact: [{"npc": npc}]} if fact else {}
    with suite.case("trace_control") as case:
        trace.write({
            "type": "turn", "id": "denial", "npc": npc, "status": "completed",
            "trigger": {"text": denial}, "steps": [{
                "text": words,
                "calls": [{"name": call, "outcome": "rejected"}] if call else [],
            }],
        })
        await perception._record_live_trace_control(
            case, SimpleNamespace(facts=lambda kind: facts.get(kind, [])), npc, "situation", denial,
        )
    assert suite.results() == {"trace_control": f"done ({outcome})"}
    [record] = json.loads((tmp_path / "live.json").read_text())["cases"]
    assert record["evidence"]["words"] == [words]
    assert record["evidence"]["caught"] == facts.get("caught", [])
    assert record["evidence"]["blamed"] == facts.get("blamed", [])
