"""Provider-agnostic streaming interface. The orchestrator only talks to `LLMBackend`.

A request is a list of items: role messages, plus, within one turn, what the model returned on the
previous step (`Done.items`: its reasoning, text and tool calls) followed by the results of those
calls (`ToolResult`). That lets the model continue after a tool without repeating itself.
"""

from __future__ import annotations

import asyncio
from collections.abc import AsyncIterator, Collection, Sequence
from contextvars import ContextVar
from dataclasses import dataclass, field
from typing import Any, Literal, Protocol

Role = Literal["user", "assistant", "developer"]
ToolChoice = Literal["auto", "none"]

# A quick-model request's llama.cpp slot; scoped to its coroutine, including test backends.
QUICK_SLOT: ContextVar[int | None] = ContextVar("quick_slot", default=None)


@dataclass(frozen=True)
class Message:
    role: Role
    content: str


@dataclass(frozen=True)
class Tool:
    """A function the model may call. `parameters` is a JSON schema object (strict: all fields required)."""

    name: str
    description: str
    parameters: dict[str, Any] = field(
        default_factory=lambda: {
            "type": "object",
            "properties": {},
            "required": [],
            "additionalProperties": False,
        }
    )


@dataclass(frozen=True)
class TextDelta:
    text: str


@dataclass(frozen=True)
class ToolCall:
    name: str
    arguments: dict[str, Any]
    call_id: str = ""
    raw: str = ""  # the arguments exactly as the model wrote them; sent back as is


@dataclass(frozen=True)
class ToolResult:
    """What came of a tool call, for the next step of the same turn."""

    call_id: str
    output: str


@dataclass(frozen=True)
class Reasoning:
    """The model's reasoning on a step, opaque and provider-specific; sent back unchanged."""

    data: dict[str, Any]


Item = Message | ToolCall | ToolResult | Reasoning


@dataclass(frozen=True)
class Usage:
    input_tokens: int = 0
    cached_tokens: int = 0  # part of input_tokens served from the provider's prompt cache
    output_tokens: int = 0
    reasoning_tokens: int = 0


@dataclass(frozen=True)
class Done:
    text: str
    items: tuple[Item, ...] = ()  # the response as request items, in order: reasoning, text, calls
    usage: Usage | None = None


LLMEvent = TextDelta | ToolCall | Done


class LLMError(RuntimeError):
    """A backend failed. `retryable` is False for errors like quota exhaustion or bad credentials."""

    def __init__(self, message: str, *, retryable: bool = False) -> None:
        super().__init__(message)
        self.retryable = retryable


async def with_deadlines(
    events: AsyncIterator[LLMEvent], *, first_s: float, idle_s: float, name: str
) -> AsyncIterator[LLMEvent]:
    """A backend's events, or LLMError when the model goes quiet: nothing for `first_s` seconds from the
    request, or for `idle_s` between events. A stalled stream would otherwise hang as long as the HTTP read
    timeout (10 minutes in the OpenAI SDK); the backend's stream is closed and its connection released."""
    wait, waited = first_s, "first answer"
    try:
        while True:
            try:
                async with asyncio.timeout(wait):
                    event = await anext(events)
            except StopAsyncIteration:
                return
            except TimeoutError:
                raise LLMError(f"{name}: no {waited} in {wait:g} s", retryable=True) from None
            yield event
            wait, waited = idle_s, "more of the answer"
    finally:
        close = getattr(events, "aclose", None)
        if close is not None:
            await close()


def open_ahead(llm: object, key: str, *, keep: bool = False) -> None:
    """Open `llm`'s connection for the requests of `key` before they come, if it keeps connections
    (openai_responses.py); `keep`: it stays open while unused (rare requests: the director's)."""
    if (open_ := getattr(llm, "open_ahead", None)) is not None:
        open_(key, keep=keep)


class LLMBackend(Protocol):
    name: str

    async def aclose(self) -> None:
        """Release connections (call once at shutdown)."""
        ...

    def stream(
        self,
        *,
        instructions: str,
        messages: Sequence[Item],
        tools: list[Tool] | None = None,
        tool_choice: ToolChoice | None = None,
        allowed: Collection[str] | None = None,
        cache_key: str | None = None,
        temperature: float | None = None,
    ) -> AsyncIterator[LLMEvent]:
        """Yield `TextDelta`s and `ToolCall`s as they arrive, then exactly one `Done` with the full text.
        `tool_choice="none"` keeps the tools in the request (and the prompt cache) but forbids calls;
        `allowed` likewise lets only the named ones be called (None: all of them). `cache_key`: requests
        that share their start (an npc's) and the provider's prompt cache with it.
        `temperature`: override the quick model's sampling temperature; None keeps its default."""
        ...
