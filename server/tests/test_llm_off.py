import logging
from types import SimpleNamespace

import pytest

from gothic_ai.llm import (
    Done,
    LLMError,
    Message,
    ModelOff,
    OffBackend,
    TextDelta,
    Tool,
    ToolCall,
    make_backend,
)
from gothic_ai.scenarios.common import model_off


def test_scenario_helper_requires_off_backend():
    llm = OffBackend()
    assert model_off(SimpleNamespace(llm=llm)) is llm
    with pytest.raises(RuntimeError, match="scenario needs LLM=off"):
        model_off(SimpleNamespace(llm=SimpleNamespace(name="fake")))


async def test_givens_match_who_and_last_message_in_declaration_order(caplog):
    llm = make_backend("off")
    assert isinstance(llm, OffBackend) and llm.name == "off"
    llm.silent("another npc", who="LOBART")
    llm.silent("another message", who="CAVALORN", when="old message")
    llm.silent("first match", who="CAVALORN", when="hello")
    llm.silent("next match", who="CAVALORN")
    with caplog.at_level(logging.INFO):
        events = [
            event
            async for event in llm.stream(
                instructions="",
                cache_key="CAVALORN",
                messages=[Message("user", "old message"), Message("user", "hello there")],
            )
        ]
    assert events == [Done("")]
    assert llm.used == ["first match"]
    assert llm.unused() == ["another npc", "another message", "next match"]
    assert "given: first match" in caplog.text
    assert not llm.attempts


async def test_line_calls_times_silent_and_unused():
    llm = OffBackend()
    llm.given(
        "a supplied decision",
        who="LOBART",
        line="A long enough answer.",
        calls=[ToolCall("accuse", {"crime": "theft"})],
        times=2,
    )
    llm.silent("nothing to say", who="LOBART")
    for _ in range(2):
        events = [
            event
            async for event in llm.stream(
                instructions="",
                messages=[],
                cache_key="LOBART",
                tools=[Tool("accuse", "Accuse")],
            )
        ]
        assert (
            "".join(event.text for event in events if isinstance(event, TextDelta)) == "A long enough answer."
        )
        assert len([event for event in events if isinstance(event, TextDelta)]) > 1
        call = next(event for event in events if isinstance(event, ToolCall))
        assert call.name == "accuse" and call.call_id and call.raw == '{"crime": "theft"}'
        assert events[-1] == Done(
            "A long enough answer.", (Message("assistant", "A long enough answer."), call)
        )
        assert "nothing to say" in llm.unused()
    assert llm.unused() == ["nothing to say"]
    assert [event async for event in llm.stream(instructions="", messages=[], cache_key="LOBART")] == [
        Done("")
    ]
    assert llm.used == ["a supplied decision", "a supplied decision", "nothing to say"]
    assert llm.unused() == []
    with pytest.raises(ModelOff):
        _ = [event async for event in llm.stream(instructions="", messages=[], cache_key="LOBART")]


async def test_unexpected_request_is_recorded_and_raises():
    llm = OffBackend()
    text = "x" * 120 + "truncated"
    with pytest.raises(LLMError) as error:
        _ = [
            event
            async for event in llm.stream(
                instructions="",
                messages=[Message("user", text)],
                cache_key="CAVALORN",
                tools=[Tool("accuse", "Accuse"), Tool("wait", "Wait")],
                allowed=["wait"],
            )
        ]
    assert isinstance(error.value, ModelOff)
    assert llm.attempts == [str(error.value)]
    assert "CAVALORN" in str(error.value) and "x" * 120 in str(error.value)
    assert "truncated" not in str(error.value)
    assert "tools=['wait']" in str(error.value)
    assert not llm.used


@pytest.mark.parametrize(
    "kwargs",
    [
        {},
        {"tools": [Tool("accuse", "Accuse")], "allowed": []},
        {"tools": [Tool("accuse", "Accuse")], "tool_choice": "none"},
    ],
)
async def test_unoffered_given_call_is_an_error(kwargs):
    llm = OffBackend()
    llm.given("accusation", who="LOBART", line="You stole it!", calls=[ToolCall("accuse", {})])
    with pytest.raises(ModelOff, match="given not applicable: accuse not offered to LOBART") as error:
        _ = [event async for event in llm.stream(instructions="", messages=[], cache_key="LOBART", **kwargs)]
    assert llm.attempts == [str(error.value)]
    assert llm.unused() == ["accusation"] and not llm.used


async def test_prewarm_and_open_ahead_do_not_attempt_a_request():
    llm = OffBackend()
    await llm.prewarm(instructions="", messages=[Message("user", "hello")])
    llm.open_ahead("CAVALORN", keep=True)
    await llm.aclose()
    assert llm.attempts == [] and llm.used == [] and llm.unused() == []
