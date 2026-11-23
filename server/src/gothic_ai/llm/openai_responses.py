"""OpenAI Responses API backend.

The same code serves an API key and a ChatGPT-plan token ("Sign in with ChatGPT"): only the bearer
token differs. Plan tokens require `store=False` and `stream=True` and reject sampling parameters
(temperature, max_output_tokens, ...), so none are sent.

With `store=False` nothing of a response stays on the server: to continue a turn after a tool call, the
response's own items go back in the next request. Reasoning comes encrypted for that
(`include=["reasoning.encrypted_content"]`); item ids are dropped, since unstored ids cannot be resolved.
Checked on a plan token on 2026-10-11 (`gothic-ai llm-test --probe-tools`): the continuation then needs
no new reasoning at all.

The tools of a request may stay the same while fewer may be called: `allowed` goes as an `allowed_tools`
tool choice, so the request's start (and the prompt cache) does not change with them; `cache_key` goes as
`prompt_cache_key`.

A request with a `cache_key` (an npc's) goes over a WebSocket of that key's own, as Codex sends its own
(openai/codex, codex-api `responses_websocket.rs`): a connection stays on one backend, whose prompt cache
then serves the key's next requests. Over HTTP the plan's gateway sends each request anywhere: measured
on 2026-10-14, a dialogue of 6 requests had 46% of its input cached over a WebSocket and none over HTTP,
and its first text came ~1 s sooner (2.1 s against 3.05 s, the median). A connection answers one request
at a time and lives up to an hour. A request that goes on from the connection's last one (the next step of
a turn: that response's items and the tool results added) sends only what was added, with
`previous_response_id`: the connection keeps its last response, `store=False` or not. An answer nobody
reads to its end any more is read in the background, so the connection stays. Where no connection opens,
the request goes over HTTP. The keys of rare requests (the director's, the gossip's) keep their
connection while unused: opening one takes the gateway 1.3-1.8 s.
"""

from __future__ import annotations

import asyncio
import json
import logging
import time
from collections.abc import AsyncIterator, Awaitable, Callable, Collection, Coroutine, Sequence
from contextlib import aclosing, suppress
from dataclasses import dataclass
from typing import Any

import openai
import websockets
from openai import AsyncOpenAI

from .base import (
    Done,
    Item,
    LLMError,
    LLMEvent,
    Message,
    Reasoning,
    TextDelta,
    Tool,
    ToolCall,
    ToolChoice,
    ToolResult,
    Usage,
)

log = logging.getLogger(__name__)

TokenProvider = Callable[[], Awaitable[str]]

WS_BETA = "responses_websockets=2026-02-06"  # Codex's header for the WebSocket transport
WS_MAX_AGE_S = 55 * 60.0  # the server closes a connection after an hour
WS_IDLE_S = 300.0  # a connection unused this long is closed (not one of a kept key)
WS_DRAIN_S = 30.0  # an answer nobody reads is read to its end within this, else its connection is closed
WS_OFF_S = 60.0  # no connection could be opened: requests go over HTTP this long
END_EVENTS = ("response.completed", "response.failed", "response.incomplete", "error")
PLAN_LIMIT_CODES = ("subscription_sharing_usage_limit_exceeded", "usage_limit_reached")


@dataclass(eq=False)
class _Socket:
    """A WebSocket of one cache key. `last`: its last response's id and the input that response goes on
    from (its request's input and its own items)."""

    key: str
    conn: Any  # openai's AsyncResponsesConnection
    opened: float
    used: float
    open_s: float
    busy: bool = True
    last: tuple[str, list[dict[str, Any]]] | None = None

    def request(self, body: dict[str, Any]) -> dict[str, Any]:
        """`body`, or only what was added to the last request, if it goes on from it."""
        if self.last is None:
            return body
        last_id, before = self.last
        whole = body["input"]
        if len(whole) <= len(before) or whole[: len(before)] != before:
            return body
        return {**body, "input": whole[len(before) :], "previous_response_id": last_id}


class _Reader:
    """The events of one response as LLMEvents; `done` once it completed."""

    def __init__(self, name: str, started: float, timing: dict[str, Any]) -> None:
        self.name = name
        self.started_at = started
        self.timing = timing
        self.text = ""
        self.items: list[Item] = []
        self.done: Done | None = None
        self.response_id: str | None = None
        self.started = False  # text or a call went out: the request cannot be made again

    def feed(self, event: Any) -> list[LLMEvent]:
        if "first_event_s" not in self.timing:
            self.timing["first_event_s"] = round(time.monotonic() - self.started_at, 3)
        match event.type:
            case "response.output_text.delta":
                self.text += event.delta
                self.started = True
                return [TextDelta(event.delta)]
            case "response.output_item.done":
                item = _output_item(event.item)
                if item is None:
                    return []
                self.items.append(item)
                if isinstance(item, ToolCall):
                    self.started = True
                    return [item]
            case "response.completed":
                self.response_id = getattr(event.response, "id", None)
                self.done = Done(
                    self.text,
                    tuple(self.items),
                    _usage(getattr(event.response, "usage", None)),
                    self.timing,
                )
                return [self.done]
            case "response.failed" | "error":
                raise _event_error(self.name, event)
            case "response.incomplete":
                raise LLMError(f"{self.name}: response incomplete", retryable=True)
        return []


class OpenAIResponsesBackend:
    def __init__(
        self,
        model: str,
        token: TokenProvider,
        *,
        reasoning_effort: str | None = "low",
        base_url: str = "https://api.openai.com/v1",
        name: str = "openai",
        websocket: bool = False,
    ) -> None:
        self.name = name
        self.model = model
        self._token = token
        self._reasoning_effort = reasoning_effort
        # one client for the whole run: keeps the HTTPS connection warm between replies;
        # the (refreshable) bearer token is set per request
        self._client = AsyncOpenAI(
            api_key="per-request",
            base_url=base_url,
            organization=None,
            project=None,
            # plan quota errors (429) must surface immediately, not be retried
            max_retries=0,
        )
        self._websocket = websocket
        self._sockets: dict[str, list[_Socket]] = {}
        self._opening: dict[str, asyncio.Task[None]] = {}
        self._kept: set[str] = set()  # keys whose connections stay open while unused
        self._ws_off_until = 0.0
        self._tasks: set[asyncio.Task[None]] = set()  # connections being read to an answer's end or closed

    async def aclose(self) -> None:
        self._websocket = False  # no connection opens after this (an npc warmed up at the shutdown)
        for task in [*self._opening.values(), *self._tasks]:
            task.cancel()
        sockets = [s for socks in self._sockets.values() for s in socks]
        self._sockets.clear()
        for sock in sockets:
            await _close(sock)
        await self._client.close()

    async def stream(
        self,
        *,
        instructions: str,
        messages: Sequence[Item],
        tools: list[Tool] | None = None,
        tool_choice: ToolChoice | None = None,
        allowed: Collection[str] | None = None,
        cache_key: str | None = None,
    ) -> AsyncIterator[LLMEvent]:
        started = time.monotonic()
        body: dict[str, Any] = {
            "model": self.model,
            "instructions": instructions,
            "input": input_items(messages),
            "store": False,
            "stream": True,
        }
        if self._reasoning_effort:
            body["reasoning"] = {"effort": self._reasoning_effort}
            body["include"] = ["reasoning.encrypted_content"]
        if tools:
            body["tools"] = [_tool(t) for t in tools]
            if choice := _tool_choice(tools, tool_choice, allowed):
                body["tool_choice"] = choice
        if cache_key:
            body["prompt_cache_key"] = cache_key
        events = (
            self._over_ws(body, cache_key, started)
            if cache_key and self._ws_on()
            else self._over_http(body, started)
        )
        async with aclosing(events):  # an answer left unread: the connection reads it to its end
            async for event in events:
                yield event

    def open_ahead(self, key: str, *, keep: bool = False) -> None:
        """Open a connection for `key` now if it has no free one (an npc the hero is about to talk to):
        its next request does not wait for the handshake. Nothing is sent over it. `keep`: the key's
        connections are not closed while unused (its requests are rare)."""
        if keep:
            self._kept.add(key)
        if not self._ws_on() or key in self._opening or any(not s.busy for s in self._sockets.get(key, [])):
            return
        task = asyncio.create_task(self._open_free(key))
        self._opening[key] = task
        task.add_done_callback(lambda t: self._opening.pop(key) if self._opening.get(key) is t else None)

    def _ws_on(self) -> bool:
        return self._websocket and time.monotonic() >= self._ws_off_until

    async def _over_http(self, body: dict[str, Any], started: float) -> AsyncIterator[LLMEvent]:
        token = await self._token()
        reader = _Reader(self.name, started, {"transport": "http"})
        try:
            stream = await self._client.responses.create(
                **body, extra_headers={"Authorization": f"Bearer {token}"}
            )
            async with stream:  # always release the pooled connection
                async for event in stream:
                    for out in reader.feed(event):
                        yield out
                    if reader.done is not None:
                        return
                raise LLMError(f"{self.name}: stream ended before completion", retryable=True)
        except openai.APIError as err:
            raise api_error(self.name, err) from err

    async def _over_ws(self, body: dict[str, Any], key: str, started: float) -> AsyncIterator[LLMEvent]:
        """The request over `key`'s connection. A connection found closed before anything came back is
        replaced once (one left idle may have been closed by the server); none opening: over HTTP."""
        for attempt in (1, 2):
            try:
                sock = await self._socket(key)
            except (OSError, TimeoutError, websockets.WebSocketException) as err:
                if _status(err) == 401:
                    raise LLMError(f"{self.name}: authentication failed, sign in again") from err
                self._ws_off_until = time.monotonic() + WS_OFF_S
                log.warning("%s: no websocket (%r): over HTTP for %.0f s", self.name, err, WS_OFF_S)
                async for event in self._over_http(body, started):
                    yield event
                return
            now = time.monotonic()
            reader = _Reader(
                self.name,
                started,
                {
                    "transport": "websocket",
                    "connection": "open" if sock.opened <= started else "new",
                    "connection_opened_at": round(time.time() - (now - sock.opened), 3),
                    "connection_open_s": round(sock.open_s, 3),
                    "connection_wait_s": round(now - started, 3),
                },
            )
            request = sock.request(body)
            end = "drain"  # how the connection is left: drained, released or dropped
            try:
                await sock.conn.send_raw(json.dumps({"type": "response.create", **request}))
                reader.timing["request_sent_s"] = round(time.monotonic() - started, 3)
                while True:
                    event = await sock.conn.recv()
                    code = _error_code(event)
                    if code == "previous_response_not_found" and request is not body:
                        sock.last, request = None, body  # the connection lost it: the whole request
                        await sock.conn.send_raw(json.dumps({"type": "response.create", **request}))
                        continue
                    if code == "websocket_connection_limit_reached" and attempt == 1:
                        end = "drop"
                        break
                    if event.type in END_EVENTS:
                        end = "release"
                    for out in reader.feed(event):
                        yield out
                    if reader.done is not None:
                        sock.last = (
                            (reader.response_id, [*body["input"], *input_items(reader.done.items)])
                            if reader.response_id
                            else None
                        )
                        return
            except websockets.ConnectionClosed as err:
                end = "drop"
                if reader.started or attempt == 2:
                    raise LLMError(f"{self.name}: the connection was lost", retryable=True) from err
            finally:
                match end:
                    case "release":
                        sock.busy, sock.used = False, time.monotonic()
                    case "drop":
                        self._drop(sock)
                    case _:
                        self._spawn(self._drain(sock))
        raise LLMError(f"{self.name}: no connection", retryable=True)

    async def _socket(self, key: str) -> _Socket:
        """A free connection of `key` (it is busy then until the answer is over), or a new one."""
        self._sweep()
        if (opening := self._opening.get(key)) is not None:
            await asyncio.shield(opening)
        for sock in self._sockets.get(key, []):
            if not sock.busy:
                sock.busy = True
                return sock
        return await self._open(key)

    async def _open(self, key: str) -> _Socket:
        started = time.monotonic()
        token = await self._token()
        conn = await self._client.responses.connect(
            extra_headers={"Authorization": f"Bearer {token}", "OpenAI-Beta": WS_BETA}
        ).enter()
        now = time.monotonic()
        sock = _Socket(key, conn, now, now, now - started)
        self._sockets.setdefault(key, []).append(sock)
        log.info("%s: a websocket for %s opened in %.2f s", self.name, key, now - started)
        return sock

    async def _open_free(self, key: str) -> None:
        try:
            sock = await self._open(key)
        except (OSError, TimeoutError, websockets.WebSocketException, LLMError) as err:
            log.info("%s: no websocket for %s ahead (%r)", self.name, key, err)
            return
        sock.busy, sock.used = False, time.monotonic()

    async def _drain(self, sock: _Socket) -> None:
        """Read an answer nobody waits for to its end: the connection is free again after it."""
        try:
            async with asyncio.timeout(WS_DRAIN_S):
                while (await sock.conn.recv()).type not in END_EVENTS:
                    pass
        except (TimeoutError, websockets.ConnectionClosed):
            self._drop(sock)
            return
        sock.last = None
        sock.busy, sock.used = False, time.monotonic()

    def _sweep(self) -> None:
        """Close the connections unused for WS_IDLE_S (but a kept key's) and those near the server's limit
        of an hour."""
        now = time.monotonic()
        for key, socks in list(self._sockets.items()):
            for sock in [s for s in socks if not s.busy]:
                idle = now - sock.used > WS_IDLE_S and key not in self._kept
                if idle or now - sock.opened > WS_MAX_AGE_S:
                    self._drop(sock)

    def _drop(self, sock: _Socket) -> None:
        socks = self._sockets.get(sock.key, [])
        if sock in socks:
            socks.remove(sock)
        if not socks:
            self._sockets.pop(sock.key, None)
        self._spawn(_close(sock))

    def _spawn(self, coro: Coroutine[Any, Any, None]) -> None:
        task = asyncio.get_running_loop().create_task(coro)
        self._tasks.add(task)
        task.add_done_callback(self._tasks.discard)


async def _close(sock: _Socket) -> None:
    with suppress(Exception):
        await sock.conn.close()


def input_items(messages: Sequence[Item]) -> list[dict[str, Any]]:
    """Request items in Responses API form."""
    out: list[dict[str, Any]] = []
    for m in messages:
        match m:
            case Message():
                out.append({"role": m.role, "content": m.content})
            case ToolCall():
                out.append(
                    {
                        "type": "function_call",
                        "call_id": m.call_id,
                        "name": m.name,
                        "arguments": m.raw or json.dumps(m.arguments, ensure_ascii=False),
                    }
                )
            case ToolResult():
                out.append({"type": "function_call_output", "call_id": m.call_id, "output": m.output})
            case Reasoning():
                out.append(dict(m.data))
    return out


def _output_item(item: Any) -> Item | None:
    """A finished output item as a request item for the next step (None: not needed there)."""
    match getattr(item, "type", None):
        case "function_call":
            return _tool_call(item)
        case "reasoning":
            data = item.model_dump(exclude_none=True)
            data.pop("id", None)
            data.pop("status", None)
            return Reasoning(data) if data.get("encrypted_content") else None
        case "message":
            parts = [getattr(c, "text", "") for c in getattr(item, "content", None) or []]
            text = "".join(p for p in parts if p)
            return Message("assistant", text) if text else None
    return None


def _usage(usage: Any) -> Usage | None:
    if usage is None:
        return None
    details = getattr(usage, "input_tokens_details", None)
    out_details = getattr(usage, "output_tokens_details", None)
    return Usage(
        input_tokens=int(getattr(usage, "input_tokens", 0) or 0),
        cached_tokens=int(getattr(details, "cached_tokens", 0) or 0),
        output_tokens=int(getattr(usage, "output_tokens", 0) or 0),
        reasoning_tokens=int(getattr(out_details, "reasoning_tokens", 0) or 0),
    )


def _tool_choice(
    tools: list[Tool], choice: ToolChoice | None, allowed: Collection[str] | None
) -> ToolChoice | dict[str, Any] | None:
    """`choice`, or the tools of `allowed` when it leaves some out (none of them: no calls at all)."""
    if choice == "none" or allowed is None or {t.name for t in tools} <= set(allowed):
        return choice
    names = [t.name for t in tools if t.name in allowed]
    if not names:
        return "none"
    return {
        "type": "allowed_tools",
        "mode": "required" if choice == "required" else "auto",
        "tools": [{"type": "function", "name": n} for n in names],
    }


def _tool(tool: Tool) -> dict:
    return {
        "type": "function",
        "name": tool.name,
        "description": tool.description,
        "parameters": tool.parameters,
        "strict": True,
    }


def _tool_call(item: object) -> ToolCall | None:
    name = getattr(item, "name", "")
    raw = getattr(item, "arguments", "") or ""
    try:
        args = json.loads(raw or "{}")
    except json.JSONDecodeError:
        log.warning("tool call %s with invalid arguments: %r", name, raw)
        return None
    if not isinstance(args, dict):
        return None
    return ToolCall(name=name, arguments=args, call_id=getattr(item, "call_id", ""), raw=raw)


PLAN_LIMIT = "лимит подписки ChatGPT для сторонних приложений исчерпан, дождитесь сброса"


def api_error(name: str, err: openai.APIError) -> LLMError:
    """Map SDK errors (raised before or inside the stream) to LLMError; quota errors are not retryable."""
    if "usage limit" in str(err.message).lower() or getattr(err, "code", None) in (
        "subscription_sharing_usage_limit_exceeded",
        "usage_limit_reached",
    ):
        return LLMError(f"{name}: {PLAN_LIMIT}")
    match err:
        case openai.RateLimitError():
            return LLMError(f"{name}: rate limit reached")
        case openai.AuthenticationError():
            return LLMError(f"{name}: authentication failed, sign in again")
        case openai.APIStatusError():
            return LLMError(f"{name}: HTTP {err.status_code}: {err.message}")
        case openai.APIConnectionError():
            return LLMError(f"{name}: connection failed", retryable=True)
    return LLMError(f"{name}: {err.message}", retryable=True)


def _field(obj: Any, name: str) -> Any:
    return obj.get(name) if isinstance(obj, dict) else getattr(obj, name, None)


def _error_code(event: Any) -> str | None:
    """The code of an error event (a WebSocket's carries it in `error`), None for other events."""
    if getattr(event, "type", None) != "error":
        return None
    return _field(_field(event, "error"), "code") or _field(event, "code")


def _status(err: BaseException) -> int | None:
    """The HTTP status a WebSocket handshake was refused with."""
    return getattr(getattr(err, "response", None), "status_code", None)


def _event_error(name: str, event: Any) -> LLMError:
    """A failed response or an error event as LLMError; quota errors are not retryable."""
    error = _field(getattr(event, "response", None), "error") or _field(event, "error")
    code = _error_code(event) or _field(error, "code")
    message = _field(error, "message") or _field(event, "message") or error
    if code in PLAN_LIMIT_CODES or "usage limit" in str(message).lower():
        return LLMError(f"{name}: {PLAN_LIMIT}")
    if _field(event, "status") == 401:
        return LLMError(f"{name}: authentication failed, sign in again")
    return LLMError(f"{name}: response failed: {message or 'unknown error'}", retryable=True)
