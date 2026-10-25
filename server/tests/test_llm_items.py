from types import SimpleNamespace

import httpx
import openai
import pytest

from gothic_ai.llm import (
    QUICK_MAX_TOKENS,
    Done,
    FakeBackend,
    FakeStep,
    Message,
    Reasoning,
    TextDelta,
    Tool,
    ToolCall,
    ToolResult,
    Usage,
    make_quick,
    quick_url,
)
from gothic_ai.llm.base import QUICK_SLOT
from gothic_ai.llm.openai_compat import chat_messages
from gothic_ai.llm.openai_responses import OpenAIResponsesBackend, input_items

GIVE = Tool("give_item", "give")


def test_responses_input_items_carry_a_turn_back():
    items = input_items(
        [
            Message("user", "Дай зелье"),
            Reasoning({"type": "reasoning", "summary": [], "encrypted_content": "gAAA"}),
            Message("assistant", "Держи."),
            ToolCall("give_item", {"item": "ITPO_HEALTH_01"}, "call_1", '{"item":"ITPO_HEALTH_01"}'),
            ToolResult("call_1", "rejected"),
        ]
    )
    assert items == [
        {"role": "user", "content": "Дай зелье"},
        {"type": "reasoning", "summary": [], "encrypted_content": "gAAA"},
        {"role": "assistant", "content": "Держи."},
        {
            "type": "function_call",
            "call_id": "call_1",
            "name": "give_item",
            "arguments": '{"item":"ITPO_HEALTH_01"}',  # the model's own text, not re-serialized
        },
        {"type": "function_call_output", "call_id": "call_1", "output": "rejected"},
    ]


class _Item(SimpleNamespace):
    def model_dump(self, exclude_none: bool = False) -> dict:
        return {k: v for k, v in vars(self).items() if not (exclude_none and v is None)}


class _Stream:
    def __init__(self, events: list) -> None:
        self.events = events

    async def __aenter__(self) -> "_Stream":
        return self

    async def __aexit__(self, *exc: object) -> None:
        pass

    def __aiter__(self):
        async def gen():
            for e in self.events:
                yield e

        return gen()


async def test_responses_stream_returns_the_response_as_items_and_usage():
    events = [
        SimpleNamespace(
            type="response.output_item.done",
            item=_Item(type="reasoning", id="rs_1", status=None, summary=[], encrypted_content="gAAA"),
        ),
        SimpleNamespace(type="response.output_text.delta", delta="Нет у меня "),
        SimpleNamespace(type="response.output_text.delta", delta="свитка."),
        SimpleNamespace(
            type="response.output_item.done",
            item=_Item(type="message", id="msg_1", content=[SimpleNamespace(text="Нет у меня свитка.")]),
        ),
        SimpleNamespace(
            type="response.output_item.done",
            item=_Item(type="function_call", id="fc_1", name="give_item", call_id="call_9", arguments="{}"),
        ),
        SimpleNamespace(
            type="response.completed",
            response=SimpleNamespace(
                usage=SimpleNamespace(
                    input_tokens=1200,
                    input_tokens_details=SimpleNamespace(cached_tokens=1024),
                    output_tokens=40,
                    output_tokens_details=SimpleNamespace(reasoning_tokens=12),
                )
            ),
        ),
    ]
    sent = {}

    async def create(**kwargs):
        sent.update(kwargs)
        return _Stream(events)

    async def token() -> str:
        return "t"

    backend = OpenAIResponsesBackend("m", token)
    backend._client.responses.create = create  # type: ignore[method-assign]
    out = [e async for e in backend.stream(instructions="i", messages=[], tools=[GIVE], tool_choice="none")]
    await backend.aclose()

    assert sent["include"] == ["reasoning.encrypted_content"]
    assert sent["tool_choice"] == "none" and sent["store"] is False
    assert [type(e) for e in out] == [TextDelta, TextDelta, ToolCall, Done]
    done = out[-1]
    assert done.text == "Нет у меня свитка."
    assert done.items == (
        Reasoning({"type": "reasoning", "summary": [], "encrypted_content": "gAAA"}),  # no id: not stored
        Message("assistant", "Нет у меня свитка."),
        ToolCall("give_item", {}, "call_9", "{}"),
    )
    assert done.usage == Usage(input_tokens=1200, cached_tokens=1024, output_tokens=40, reasoning_tokens=12)


async def test_responses_keep_every_tool_and_allow_some_of_them():
    """All the tools stay in the request, so its start (and the prompt cache) is the same whatever is
    offered; `allowed` narrows what may be called."""
    sent: list[dict] = []
    done = SimpleNamespace(type="response.completed", response=SimpleNamespace(usage=None))

    async def create(**kwargs):
        sent.append(kwargs)
        return _Stream([done])

    async def token() -> str:
        return "t"

    backend = OpenAIResponsesBackend("m", token)
    backend._client.responses.create = create  # type: ignore[method-assign]
    intent = Tool("set_intent", "intent")
    for allowed, choice in (
        (["set_intent"], None),
        (["set_intent", "give_item"], None),
        ([], None),
        (None, None),
        (["set_intent"], "none"),
    ):
        async for _ in backend.stream(
            instructions="i",
            messages=[],
            tools=[GIVE, intent],
            tool_choice=choice,
            allowed=allowed,
            cache_key="NONE_100_XARDAS",
        ):
            pass
    await backend.aclose()

    assert all([t["name"] for t in s["tools"]] == ["give_item", "set_intent"] for s in sent)
    assert all(s["prompt_cache_key"] == "NONE_100_XARDAS" for s in sent)
    assert [s.get("tool_choice") for s in sent] == [
        {"type": "allowed_tools", "mode": "auto", "tools": [{"type": "function", "name": "set_intent"}]},
        None,  # all of them
        "none",  # none of them
        None,
        "none",
    ]


def test_chat_messages_attach_tool_calls_to_the_assistant_message():
    chat = chat_messages(
        [
            Message("developer", "[обстановка]"),
            Message("user", "Дай зелье"),
            Reasoning({"type": "reasoning"}),
            Message("assistant", "Держи."),
            ToolCall("give_item", {"item": "X"}, "c1", '{"item":"X"}'),
            ToolResult("c1", "ok"),
        ]
    )
    assert chat == [
        {"role": "system", "content": "[обстановка]"},
        {"role": "user", "content": "Дай зелье"},
        {
            "role": "assistant",
            "content": "Держи.",
            "tool_calls": [
                {
                    "id": "c1",
                    "type": "function",
                    "function": {"name": "give_item", "arguments": '{"item":"X"}'},
                }
            ],
        },
        {"role": "tool", "tool_call_id": "c1", "content": "ok"},
    ]


async def test_fake_backend_follows_its_script_one_step_per_call():
    llm = FakeBackend(
        script=[
            FakeStep("Держи.", [ToolCall("give_item", {"item": "X", "count": 1})]),
            FakeStep("Хотя нет, такого у меня нет."),
        ],
        delay=0,
    )
    first = [e async for e in llm.stream(instructions="", messages=[], tools=[GIVE])]
    done = first[-1]
    assert isinstance(done, Done)
    call = done.items[-1]
    assert isinstance(call, ToolCall) and call.call_id and call.raw == '{"item": "X", "count": 1}'

    second = [e async for e in llm.stream(instructions="", messages=[], tools=[GIVE])]
    assert second[-1].text == "Хотя нет, такого у меня нет."
    third = [e async for e in llm.stream(instructions="", messages=[], tools=[GIVE])]
    assert third[-1].text == "Хотя нет, такого у меня нет."  # the last step repeats


async def test_fake_backend_makes_no_calls_with_tool_choice_none():
    llm = FakeBackend("Держи.", tool_calls=[ToolCall("give_item", {})], delay=0)
    events = [e async for e in llm.stream(instructions="", messages=[], tools=[GIVE], tool_choice="none")]
    assert not any(isinstance(e, ToolCall) for e in events)
    assert llm.tool_choices == ["none"]


async def test_fake_backend_records_the_temperature_of_each_request():
    llm = FakeBackend("Держись!", delay=0)
    for kwargs in ({"temperature": 0.6}, {}):
        async for _ in llm.stream(instructions="", messages=[], **kwargs):
            pass
    assert llm.temperatures == [0.6, None]


async def test_the_quick_backend_caps_its_answers_and_warms_the_servers_cache():
    backend = make_quick("http://127.0.0.1:9/v1")
    sent: list[dict] = []

    async def create(**kwargs):
        sent.append(kwargs)
        if not kwargs.get("stream"):
            return SimpleNamespace()

        async def chunks():
            delta = SimpleNamespace(content="Держись!", tool_calls=None)
            yield SimpleNamespace(choices=[SimpleNamespace(delta=delta)])

        return chunks()

    backend._client.chat.completions.create = create  # type: ignore[attr-defined]
    messages = [Message("user", "Волк!")]
    await backend.prewarm(instructions="Ты Кавалорн.", messages=messages)
    events = [e async for e in backend.stream(instructions="Ты Кавалорн.", messages=messages)]
    async for _ in backend.stream(instructions="Ты Кавалорн.", messages=messages, temperature=0.6):
        pass
    await backend.aclose()

    chat = [{"role": "system", "content": "Ты Кавалорн."}, {"role": "user", "content": "Волк!"}]
    assert sent[0]["messages"] == chat and sent[0]["max_tokens"] == 1  # the same prefix, nothing said
    assert sent[1]["messages"] == chat and sent[1]["max_tokens"] == QUICK_MAX_TOKENS and sent[1]["stream"]
    assert "temperature" not in sent[0] and "temperature" not in sent[1]
    assert sent[2]["temperature"] == 0.6
    assert isinstance(events[-1], Done) and events[-1].text == "Держись!"


@pytest.mark.parametrize("reject", [None, "prewarm", "stream"])
async def test_quick_slot_transport_and_one_time_unsupported_field_fallback(reject, caplog):
    backend = make_quick("http://127.0.0.1:9/v1")
    sent = []

    async def create(**kwargs):
        sent.append(kwargs)
        if reject and "extra_body" in kwargs:
            response = httpx.Response(400, request=httpx.Request("POST", "http://fake/v1/chat/completions"))
            raise openai.BadRequestError("Unknown field: id_slot", response=response,
                                         body={"param": "id_slot"})
        if not kwargs.get("stream"):
            return SimpleNamespace()

        async def chunks():
            yield SimpleNamespace(choices=[SimpleNamespace(
                delta=SimpleNamespace(content="Есть!", tool_calls=None),
            )], timings={"cache_n": 100, "prompt_n": 20})

        return chunks()

    backend._client.chat.completions.create = create
    token = QUICK_SLOT.set(2)
    try:
        if reject == "stream":
            async for _ in backend.stream(instructions="", messages=[]):
                pass
        await backend.prewarm(instructions="", messages=[])
        async for _ in backend.stream(instructions="", messages=[]):
            pass
    finally:
        QUICK_SLOT.reset(token)
        await backend.aclose()
    if reject:
        assert sent[0]["extra_body"]["id_slot"] == 2
        assert all("extra_body" not in request for request in sent[1:])
        assert len([r for r in caplog.records if "rejected id_slot" in r.message]) == 1
    else:
        assert [r["extra_body"]["id_slot"] for r in sent] == [2, 2]
        assert backend.timings[2] == {"cache_n": 100, "prompt_n": 20}


def test_the_quick_model_is_on_wherever_the_decider_is():
    assert quick_url(None, "ws://gpu-host:47832") == "http://gpu-host:47834/v1"  # the same GPU server
    assert quick_url("http://other:9000/v1", "ws://gpu-host:47832") == "http://other:9000/v1"
    assert quick_url(None, None) is None
