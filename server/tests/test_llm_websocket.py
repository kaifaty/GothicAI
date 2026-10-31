"""The Responses backend over a WebSocket of each npc's own (openai_responses.py), against a local server."""

import asyncio
import json
import socket
import time
from types import SimpleNamespace

import pytest
from websockets.asyncio.server import ServerConnection, serve

from gothic_ai.llm import Done, LLMError, Message, TextDelta, ToolCall, ToolResult
from gothic_ai.llm.openai_responses import PLAN_LIMIT, WS_IDLE_S, OpenAIResponsesBackend
from gothic_ai.scenarios.responses_server import ResponsesServer, reply
from tests.test_dialogue import _until


def _reply(rid: str, text: str, *, pause: float = 0.0, call: bool = False) -> list:
    """The events of one response; a float in the list is a pause before the next event."""
    item = (
        {"type": "function_call", "id": "fc", "call_id": "c1", "name": "recall", "arguments": "{}"}
        if call
        else {
            "type": "message",
            "id": "m",
            "role": "assistant",
            "status": "completed",
            "content": [{"type": "output_text", "text": text, "annotations": []}],
        }
    )
    usage = {
        "input_tokens": 100,
        "input_tokens_details": {"cached_tokens": 64},
        "output_tokens": 5,
        "output_tokens_details": {"reasoning_tokens": 0},
        "total_tokens": 105,
    }
    delta = {"type": "response.output_text.delta", "delta": text, "item_id": "m", "output_index": 0}
    return [
        delta,
        pause,
        {"type": "response.output_item.done", "output_index": 0, "item": item},
        {"type": "response.completed", "response": {"id": rid, "status": "completed", "usage": usage}},
    ]


class Server:
    """Answers each response.create with `answer(request, n)`'s events; keeps what came, per connection."""

    def __init__(self, answer=None, close_after: int | None = None) -> None:
        self.answer = answer or (lambda request, n: _reply(f"resp_{n}", f"Ответ {n}."))
        self.close_after = close_after  # a connection closed after this many answers
        self.connections: list[list[dict]] = []
        self.headers: list[dict] = []
        self.count = 0

    async def handle(self, ws: ServerConnection) -> None:
        got: list[dict] = []
        self.connections.append(got)
        self.headers.append({k.lower(): v for k, v in ws.request.headers.raw_items()})
        async for raw in ws:
            request = json.loads(raw)
            got.append(request)
            self.count += 1
            for event in self.answer(request, self.count):
                if isinstance(event, float):
                    await asyncio.sleep(event)
                else:
                    await ws.send(json.dumps(event))
            if self.close_after is not None and len(got) >= self.close_after:
                return

    @property
    def requests(self) -> list[dict]:
        return [r for c in self.connections for r in c]


async def _start(server: Server):
    ws = await serve(server.handle, "127.0.0.1", 0)
    port = ws.sockets[0].getsockname()[1]

    async def token() -> str:
        return "t"

    backend = OpenAIResponsesBackend("m", token, base_url=f"http://127.0.0.1:{port}/v1", websocket=True)
    return ws, backend


async def _ask(backend, messages, key="NONE_100_XARDAS", **kw) -> list:
    return [e async for e in backend.stream(instructions="i", messages=messages, cache_key=key, **kw)]


async def test_an_npcs_requests_share_its_connection_and_a_step_sends_only_what_was_added():
    server = Server()
    ws, backend = await _start(server)
    try:
        first = [Message("user", "Привет")]
        out = await _ask(backend, first)
        done = out[-1]
        assert [type(e) for e in out] == [TextDelta, Done] and done.text == "Ответ 1."
        assert done.usage is not None and done.usage.cached_tokens == 64
        assert done.timing["transport"] == "websocket" and done.timing["connection"] == "new"
        assert done.timing["connection_open_s"] >= 0
        assert 0 <= done.timing["connection_wait_s"] <= done.timing["first_event_s"]
        # the next step of the turn: the response and what was added go on from it
        again = (await _ask(backend, [*first, *done.items, Message("user", "Ещё")]))[-1]
        assert again.timing["connection"] == "open"
        assert again.timing["connection_opened_at"] == done.timing["connection_opened_at"]
        # a new turn: another request, sent whole
        await _ask(backend, [Message("user", "Другое")])
    finally:
        await backend.aclose()
        ws.close()

    assert len(server.connections) == 1
    one, two, three = server.requests
    assert one["type"] == "response.create" and one["prompt_cache_key"] == "NONE_100_XARDAS"
    assert one["store"] is False and "previous_response_id" not in one
    assert two["previous_response_id"] == "resp_1" and two["input"] == [{"role": "user", "content": "Ещё"}]
    assert "previous_response_id" not in three and len(three["input"]) == 1
    headers = server.headers[0]
    assert headers["authorization"] == "Bearer t" and headers["openai-beta"].startswith(
        "responses_websockets="
    )


async def test_each_npc_has_a_connection_of_its_own():
    server = Server(answer=lambda r, n: _reply(f"resp_{n}", "Да.", pause=0.05))
    ws, backend = await _start(server)
    try:
        a, b = await asyncio.gather(
            _ask(backend, [Message("user", "1")], key="A"), _ask(backend, [Message("user", "2")], key="B")
        )
        await _ask(backend, [Message("user", "3")], key="A")
    finally:
        await backend.aclose()
        ws.close()
    assert a[-1].text == b[-1].text == "Да."
    assert sorted(len(c) for c in server.connections) == [1, 2]


async def test_an_answer_left_unread_is_read_to_its_end_and_the_connection_kept():
    server = Server(answer=lambda r, n: _reply(f"resp_{n}", f"Ответ {n}.", pause=0.2))
    ws, backend = await _start(server)
    try:
        events = backend.stream(instructions="i", messages=[Message("user", "Привет")], cache_key="K")
        first = await anext(events)
        await events.aclose()  # the hero interrupted: nobody reads the rest
        assert first == TextDelta("Ответ 1.")
        await asyncio.sleep(0.4)
        out = await _ask(backend, [Message("user", "Снова")], key="K")
    finally:
        await backend.aclose()
        ws.close()
    assert out[-1].text == "Ответ 2."  # its own answer, not the rest of the first
    assert len(server.connections) == 1


async def test_a_lost_previous_response_sends_the_whole_request():
    def answer(request, n):
        if "previous_response_id" in request:
            return [
                {
                    "type": "error",
                    "status": 400,
                    "error": {
                        "type": "invalid_request_error",
                        "code": "previous_response_not_found",
                        "message": "x",
                    },
                }
            ]
        return _reply(f"resp_{n}", "Да.", call=n == 1)

    server = Server(answer=answer)
    ws, backend = await _start(server)
    try:
        first = [Message("user", "Вспомни")]
        done = (await _ask(backend, first))[-1]
        assert isinstance(done.items[-1], ToolCall)
        out = await _ask(backend, [*first, *done.items, ToolResult("c1", "ничего")])
    finally:
        await backend.aclose()
        ws.close()
    assert out[-1].text == "Да."
    assert [("previous_response_id" in r, len(r["input"])) for r in server.requests] == [
        (False, 1),
        (True, 1),
        (False, 3),
    ]


async def test_a_connection_closed_while_idle_is_replaced():
    server = Server(close_after=1)
    ws, backend = await _start(server)
    try:
        await _ask(backend, [Message("user", "1")])
        await asyncio.sleep(0.1)
        out = await _ask(backend, [Message("user", "2")])
    finally:
        await backend.aclose()
        ws.close()
    assert out[-1].text == "Ответ 2."
    assert len(server.connections) == 2


async def test_the_plan_limit_over_a_websocket_is_not_retried():
    def answer(request, n):
        return [
            {
                "type": "error",
                "status": 429,
                "error": {"type": "x", "code": "usage_limit_reached", "message": "m"},
            }
        ]

    ws, backend = await _start(Server(answer=answer))
    try:
        with pytest.raises(LLMError) as err:
            await _ask(backend, [Message("user", "1")])
    finally:
        await backend.aclose()
        ws.close()
    assert PLAN_LIMIT in str(err.value) and not err.value.retryable


async def test_no_websocket_the_request_goes_over_http():
    with socket.socket() as s:  # a port nobody listens on
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]

    async def token() -> str:
        return "t"

    backend = OpenAIResponsesBackend("m", token, base_url=f"http://127.0.0.1:{port}/v1", websocket=True)
    sent = {}

    class _Stream:
        async def __aenter__(self):
            return self

        async def __aexit__(self, *exc):
            pass

        async def __aiter__(self):
            yield SimpleNamespace(type="response.output_text.delta", delta="По HTTP.")
            yield SimpleNamespace(type="response.completed", response=SimpleNamespace(id="r", usage=None))

    async def create(**kwargs):
        sent.update(kwargs)
        return _Stream()

    backend._client.responses.create = create  # type: ignore[method-assign]
    try:
        out = await _ask(backend, [Message("user", "1")])
    finally:
        await backend.aclose()
    assert out[-1].text == "По HTTP." and sent["prompt_cache_key"] == "NONE_100_XARDAS"
    assert out[-1].timing["transport"] == "http" and out[-1].timing["first_event_s"] >= 0
    assert not backend._ws_on()  # HTTP for a while, not a failed connect each time


@pytest.mark.parametrize("websocket,refuse", [(True, False), (True, True), (False, True)])
async def test_scenario_server_serves_the_real_sdk_over_ws_and_http(websocket, refuse, caplog):
    """SSE reaches the SDK both after a refused handshake and with WebSocket explicitly disabled."""
    async def token() -> str:
        return "fake-token"

    async with ResponsesServer(lambda request, n: reply(f"response_{n}", f"Реплика {n}.")) as server:
        server.refuse_websocket = refuse
        backend = OpenAIResponsesBackend("fake-model", token, base_url=server.base_url, websocket=websocket)
        try:
            out = await _ask(backend, [Message("user", "Привет")])
            again = await _ask(backend, [Message("user", "Ещё")])
            assert out[-1].text == "Реплика 1." and again[-1].text == "Реплика 2."
            assert out[-1].usage.input_tokens == 10
        finally:
            await backend.aclose()

    assert server.handshakes == int(websocket)
    assert server.refused == int(websocket and refuse)
    if websocket and not refuse:
        assert len(server.connections) == 1 and len(server.requests) == 2
        assert all(request["type"] == "response.create" for request in server.requests)
        assert not server.http_requests
    else:
        assert not server.connections and len(server.http_requests) == 2
        assert all(request["prompt_cache_key"] == "NONE_100_XARDAS" for request in server.http_requests)
    warnings = [record for record in caplog.records if "over HTTP for" in record.message]
    assert len(warnings) == int(websocket and refuse)


async def test_a_kept_key_stays_open_while_unused():
    """The director's requests are rare: its connection is not closed with an npc's unused one."""
    server = Server()
    ws, backend = await _start(server)
    try:
        backend.open_ahead("director", keep=True)
        await _ask(backend, [Message("user", "1")], key="director")
        await _ask(backend, [Message("user", "2")], key="NONE_100_XARDAS")
        for socks in backend._sockets.values():
            for sock in socks:
                sock.used -= WS_IDLE_S + 1  # unused for longer than an npc's may be
        backend._sweep()
        assert list(backend._sockets) == ["director"]
    finally:
        await backend.aclose()
        ws.close()


async def test_a_connection_is_opened_ahead_of_the_turn():
    server = Server(
        answer=lambda request, n: [
            {"type": "response.created", "response": {"id": f"resp_{n}", "status": "in_progress"}},
            0.05,
            *_reply(f"resp_{n}", "Да."),
        ]
    )
    ws, backend = await _start(server)
    try:
        backend.open_ahead("K")
        backend.open_ahead("K")  # once
        await _until(lambda: any(not s.busy for s in backend._sockets.get("K", [])))
        started = time.monotonic()
        text_at = None
        out = []
        async for event in backend.stream(instructions="i", messages=[Message("user", "1")], cache_key="K"):
            out.append(event)
            if isinstance(event, TextDelta):
                text_at = time.monotonic() - started
        timing = out[-1].timing
        assert timing["connection"] == "open" and timing["connection_opened_at"] <= time.time()
        assert timing["connection_wait_s"] <= timing["request_sent_s"] <= timing["first_event_s"]
        # The first raw event precedes text: response.created is not exposed as an LLMEvent.
        assert text_at is not None and text_at - timing["first_event_s"] >= 0.04
    finally:
        await backend.aclose()
        backend.open_ahead("L")  # closed: nothing opens
        await asyncio.sleep(0.1)
        ws.close()
    assert len(server.connections) == 1 and len(server.requests) == 1
