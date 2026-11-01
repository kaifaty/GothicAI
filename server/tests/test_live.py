"""Live outcomes are measurements; setup errors alone fail the scenario."""

import json

import pytest

from gothic_ai.scenarios.live import LiveSuite, estimate, summarize, write_report
from gothic_ai.trace import TraceWriter


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
