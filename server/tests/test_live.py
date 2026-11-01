"""Live outcomes are measurements; setup errors alone fail the scenario."""

import json

from gothic_ai.scenarios.live import LiveSuite, summarize
from gothic_ai.trace import TraceWriter


def test_case_outcomes_requests_and_live_json(tmp_path, monkeypatch):
    selected = "A_lobart,A_bandit,B_lobart,B_bandit,B_guard,C_bandit"
    monkeypatch.setenv("CASES", selected)
    trace = TraceWriter(tmp_path / "traces")
    trace.write({"type": "turn", "id": "before", "steps": [{}, {}, {}]})
    suite = LiveSuite("surrender_live", trace)
    outcomes = ("success", "failure", "no_decision", "observed")
    for index, outcome in enumerate(outcomes):
        with suite.case(suite.selected[index]) as case:
            trace.write({"type": "instructions", "hash": "prompt", "text": "prompt"})
            trace.write({"type": "combat_bark", "outcome": "said"})
            trace.write({"type": "turn", "id": f"turn-{index}", "steps": [{}, {}]})
            case.finish(outcome, evidence={"decision": "fight"})
    with suite.case("B_guard"):
        raise RuntimeError("game lost connection")
    with suite.case("C_bandit") as case:
        case.finish("observed", evidence={"decision": "surrender"})
    trace.write({"type": "turn", "id": "after", "steps": [{}]})
    result = suite.results()
    assert all(result[name].startswith("done") for name in suite.selected if name != "B_guard")
    assert result["B_guard"] == "failed (RuntimeError: game lost connection)"
    data = json.loads((tmp_path / "live.json").read_text())
    assert data["suite"] == "surrender_live"
    assert [case["outcome"] for case in data["cases"]] == [*outcomes, None, "observed"]
    assert [case["requests"] for case in data["cases"]] == [2, 2, 2, 2, 0, 0]
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
    assert report["requests"] == 8


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
