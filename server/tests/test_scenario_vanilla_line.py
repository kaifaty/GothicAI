import asyncio
import json
from contextlib import nullcontext
from types import SimpleNamespace
from unittest.mock import AsyncMock

import pytest

from gothic_ai import challenges, questbook
from gothic_ai.journal import Journal
from gothic_ai.scenarios import story as scenarios
from gothic_ai.scenarios import vanilla_line as live_scenario
from gothic_ai.scenarios.vanilla_line import (
    RUEBEN,
    Turnips,
    _giver_ok,
    vanilla_line_live,
)
from gothic_ai.showrunner import ShowrunnerModel
from gothic_ai.trace import TraceWriter


def test_event_matches_quest_state_and_current_occasion_window() -> None:
    def event(quest: str, state: str) -> dict:
        return {"kind": "vanilla", "vanilla": {"quest": quest, "to": state}}

    events = [event(RUEBEN, "running"), event("MIS_OTHER", "success"), event(RUEBEN, "success")]
    director = SimpleNamespace(chronicle=SimpleNamespace(facts=lambda kind: events))
    turnips = Turnips(None, director, None)
    assert turnips.event("running") == events[0]
    assert turnips.event("running", after=1) is None
    assert turnips.event("success", after=1) == events[2]


async def test_common_player_observes_vanilla_goal_without_writing_scripts() -> None:
    talk = questbook.Talk("Готово?", [{"speaker": "self", "text": "Да."}])
    quest = questbook.Quest(
        id="DIR_Q1",
        giver="BAU_952_VINO",
        title="После урожая",
        give=talk,
        stages=[
            questbook.Stage("Репа принята.", [questbook.Goal("vanilla", quest=RUEBEN, state="success")]),
            questbook.Stage("Сказать соседу.", [questbook.Goal("talk", npc="BAU_952_VINO", talk=talk)]),
        ],
        done_journal="Сосед доволен.",
    )

    class Link:
        status = stage = vanilla_reads = 0
        fired = False

        async def debug(self, *args, **kwargs):
            pass

        async def offered_dialogs(self, npc):
            return [questbook.give_node(quest), questbook.done_node(quest)]

        async def choose_dialog(self, npc, node):
            self.status, self.stage = (1, 1) if node == questbook.give_node(quest) else (2, 3)

        async def story_state(self, **kwargs):
            names = kwargs.get("vars", [])
            if RUEBEN in names:
                self.vanilla_reads += 1
                self.fired, self.stage = True, 2
            values = {quest.id: self.status, questbook.stage_var(quest): self.stage, RUEBEN: 2}
            return {"dialog": False, "vars": {name: values[name] for name in names}}

        async def triggers(self):
            return [{"id": questbook.goal_id(quest, 1, 1), "count": int(self.fired)}]

    link = Link()
    result = await scenarios.play_book_chain(link, None, [quest])
    assert all(value.startswith("done") for value in result.values())
    assert link.vanilla_reads == 1
    assert link.status == 2


async def test_live_line_must_follow_the_event_and_cannot_reuse_its_script_giver(monkeypatch) -> None:
    async def peaceful(link, people, **kwargs):
        return set(people)

    monkeypatch.setattr("gothic_ai.scenarios.vanilla_line.non_hostile", peaceful)
    graph = SimpleNamespace(
        quests={RUEBEN: SimpleNamespace(running={"TAKE"}, success={"END"}, failed=set(), obsolete=set())},
        nodes={"TAKE": SimpleNamespace(npc="LOBART"), "END": SimpleNamespace(npc="LOBART")},
    )
    director = SimpleNamespace(
        author=SimpleNamespace(graph=graph), rules=SimpleNamespace(plot_npc=lambda npc: False)
    )
    assert not await _giver_ok(None, director, {"giver": "LOBART", "follows": ""})
    assert not await _giver_ok(None, director, {"giver": "LOBART", "follows": RUEBEN})
    assert not await _giver_ok(None, director, {"giver": "HILDA", "follows": ""})
    assert await _giver_ok(None, director, {"giver": "HILDA", "follows": RUEBEN})


@pytest.mark.parametrize("force", [None, "0", "1"])
async def test_live_challenge_instructions_are_scoped_and_restored(monkeypatch, force) -> None:
    if force is None:
        monkeypatch.delenv("GOTHIC_FORCE_CHALLENGE", raising=False)
    else:
        monkeypatch.setenv("GOTHIC_FORCE_CHALLENGE", force)
    model = ShowrunnerModel(SimpleNamespace(name="chatgpt"))
    other_model = ShowrunnerModel(SimpleNamespace(name="chatgpt"))
    original = model.instructions

    def clock():
        return 10.0

    director = SimpleNamespace(model=model, clock=clock)
    entries = []
    journal = Journal(entries.append)
    sink = journal.sink

    def chance(*args):
        return 0.75

    monkeypatch.setattr(challenges, "_chance", chance)

    async def prepare(self):
        extra = (
            "\nПроверочный прогон: каждая линия, которую ты строишь, получает ровно одно скрытое условие "
            "challenge — выбери из четырёх самое уместное для её людей и сюжета."
        )
        assert model.instructions == original + (extra if force == "1" else "")
        assert other_model.instructions == original
        assert challenges._chance("challenge_hint", "1") == (0.0 if force == "1" else 0.75)
        raise RuntimeError("stop after setup")

    monkeypatch.setattr(Turnips, "prepare", prepare)
    with pytest.raises(RuntimeError, match="stop after setup"):
        await vanilla_line_live(None, director, None, journal=journal)
    assert model.instructions == other_model.instructions == original
    assert challenges._chance is chance
    assert director.clock is clock
    assert journal.sink is sink


@pytest.mark.parametrize(("missing", "expected"), [
    ("challenge", "failure"),
    ("hint", "failure"),
    ("outcome", "failure"),
    ("feat", "failure"),
    ("dropped", "success"),
    ("normal", "success"),
])
async def test_live_line_grade_includes_forced_challenge_checks(monkeypatch, tmp_path, missing, expected):
    monkeypatch.setenv("CASES", "running")
    monkeypatch.setenv("GOTHIC_FORCE_CHALLENGE", "0" if missing == "normal" else "1")
    occasion = {"fact": "occasion", "kind": "vanilla", "vanilla": {"quest": RUEBEN, "to": "running"}}
    line = {
        "fact": "line", "occasion": occasion, "giver": "HILDA", "quests": ["DIR_Q1"], "characters": [],
    }
    receipts = [(1, occasion), (2, line)]
    if missing == "dropped":
        receipts.append((3, {"fact": "challenge_dropped", "line": 2}))
    elif missing not in {"challenge", "normal"}:
        receipts.append((3, {"fact": "challenge", "line": 2, "kind": "mercy", "people": ["LOBART"]}))
        if missing != "hint":
            receipts.append((4, {"fact": "challenge_hint", "challenge": 3}))
        if missing != "outcome":
            receipts.append((5, {"fact": "challenge_met", "challenge": 3}))
        if missing != "feat":
            receipts.append((6, {"fact": "feat", "trigger": 5, "title": "Mercy"}))
    recorded = []

    async def take(self):
        recorded.extend(receipts)

    async def until(check, timeout):
        return bool(check())

    quest = SimpleNamespace(id="DIR_Q1", to_dict=lambda: {"id": "DIR_Q1"})
    chronicle = SimpleNamespace(
        on_fact=[],
        facts=lambda kind=None: [entry for _, entry in recorded if kind is None or entry["fact"] == kind],
        entries=lambda kind=None: [(seq, entry) for seq, entry in recorded
                                   if kind is None or entry["fact"] == kind],
    )
    director = SimpleNamespace(
        model=ShowrunnerModel(SimpleNamespace(name="chatgpt")), clock=lambda: 10.0,
        chronicle=chronicle, run=AsyncMock(), _work=None,
        occasions=SimpleNamespace(
            poll=AsyncMock(), _raw_place=None, pending=[], _events=[], _lock=asyncio.Lock(),
        ),
        book=SimpleNamespace(
            all=lambda: {quest.id: quest}, progress=lambda qid: SimpleNamespace(state="done"),
            settle=AsyncMock(),
        ),
        casting=SimpleNamespace(cast=SimpleNamespace(characters=lambda: [])),
    )
    passage = AsyncMock(return_value={"quest_1": "done"})
    monkeypatch.setattr(Turnips, "prepare", AsyncMock())
    monkeypatch.setattr(Turnips, "take", take)
    monkeypatch.setattr(live_scenario, "_until", until)
    monkeypatch.setattr(live_scenario, "_giver_ok", AsyncMock(return_value=True))
    monkeypatch.setattr(live_scenario, "play_book_chain", passage)
    monkeypatch.setattr(live_scenario, "trace_live_builders", lambda *args: nullcontext())

    results = await vanilla_line_live(
        SimpleNamespace(debug=AsyncMock()), director, None, Journal(), TraceWriter(tmp_path / "traces"),
    )

    passage.assert_awaited_once()
    [case] = json.loads((tmp_path / "live.json").read_text())["cases"]
    assert case["evidence"]["passage"] == {"quest_1": "done"}
    assert case["outcome"] == expected
    assert results == {"running": f"done ({expected})"}
