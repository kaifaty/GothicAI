"""OpenAI-compatible Chat Completions backend: Ollama, mlx_lm.server, LM Studio, llama.cpp, OpenRouter.

`prewarm` fills a llama.cpp server's prompt cache ahead of a turn: the next request that starts with the
same messages only processes what was added after them (see dialogue.py, the quick model)."""

from __future__ import annotations

import json
import logging
from collections.abc import AsyncIterator, Collection, Sequence
from typing import Any

import openai
from openai import AsyncOpenAI

from .base import (
    QUICK_SLOT,
    Done,
    Item,
    LLMError,
    LLMEvent,
    Message,
    TextDelta,
    Tool,
    ToolCall,
    ToolChoice,
    ToolResult,
)

log = logging.getLogger(__name__)


class OpenAICompatBackend:
    def __init__(
        self,
        model: str,
        base_url: str,
        api_key: str = "local",
        *,
        name: str = "compat",
        max_tokens: int | None = None,  # a cap on every answer: a small model may loop until the context ends
    ) -> None:
        self.name = name
        self.model = model
        self.max_tokens = max_tokens
        self._client = AsyncOpenAI(api_key=api_key, base_url=base_url, max_retries=0)
        self._id_slot_supported = True
        self.timings: dict[int, dict[str, Any]] = {}

    async def aclose(self) -> None:
        await self._client.close()

    async def _create(self, **kwargs: Any) -> Any:
        slot = QUICK_SLOT.get()
        if slot is not None and self._id_slot_supported:
            kwargs["extra_body"] = {"id_slot": slot}
            if kwargs.get("stream"):
                kwargs["extra_body"]["timings_per_token"] = True
                kwargs["stream_options"] = {"include_usage": True}
        try:
            return await self._client.chat.completions.create(**kwargs)
        except openai.BadRequestError as err:
            if "extra_body" not in kwargs or "id_slot" not in str(err.body or err):
                raise
            if self._id_slot_supported:
                log.warning("%s: server rejected id_slot; continuing without slot pinning", self.name)
            self._id_slot_supported = False
            del kwargs["extra_body"]
            return await self._client.chat.completions.create(**kwargs)

    async def prewarm(self, *, instructions: str, messages: Sequence[Item]) -> None:
        """Process `instructions` and `messages` now (one token is generated and dropped). Cancelling it
        drops the connection, and llama.cpp stops the work."""
        chat = [{"role": "system", "content": instructions}, *chat_messages(messages)]
        try:
            await self._create(model=self.model, messages=chat, max_tokens=1)
        except openai.APIStatusError as err:
            raise LLMError(f"{self.name}: HTTP {err.status_code}: {err.message}") from err
        except openai.APIConnectionError as err:
            raise LLMError(f"{self.name}: cannot reach {self._client.base_url}", retryable=True) from err

    async def stream(
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
        chat = [{"role": "system", "content": instructions}, *chat_messages(messages)]
        kwargs: dict[str, Any] = {}
        if allowed is not None:  # no allowed_tools here: the others are left out
            tools = [t for t in tools or [] if t.name in allowed]
        if tools:
            kwargs["tools"] = [
                {
                    "type": "function",
                    "function": {"name": t.name, "description": t.description, "parameters": t.parameters},
                }
                for t in tools
            ]
            if tool_choice:
                kwargs["tool_choice"] = tool_choice
        if self.max_tokens is not None:
            kwargs["max_tokens"] = self.max_tokens
        if temperature is not None:
            kwargs["temperature"] = temperature
        text = ""
        calls: dict[int, dict[str, str]] = {}
        stream = None
        slot = QUICK_SLOT.get()
        if slot is not None:
            self.timings.pop(slot, None)
        try:
            stream = await self._create(
                model=self.model, messages=chat, stream=True, **kwargs
            )
            async for chunk in stream:
                if slot is not None and (timings := getattr(chunk, "timings", None)):
                    self.timings[slot] = dict(timings)
                if not chunk.choices:
                    continue
                delta = chunk.choices[0].delta
                if delta.content:
                    text += delta.content
                    yield TextDelta(delta.content)
                for tc in delta.tool_calls or []:
                    acc = calls.setdefault(tc.index, {"id": "", "name": "", "arguments": ""})
                    acc["id"] = tc.id or acc["id"]
                    if tc.function is not None:
                        acc["name"] += tc.function.name or ""
                        acc["arguments"] += tc.function.arguments or ""
        except openai.APIStatusError as err:
            raise LLMError(f"{self.name}: HTTP {err.status_code}: {err.message}") from err
        except openai.APIConnectionError as err:
            raise LLMError(f"{self.name}: cannot reach {self._client.base_url}", retryable=True) from err
        finally:
            if stream is not None:
                await stream.aclose()  # release the model slot when a short line is cut or yields priority
        items: list[Item] = [Message("assistant", text)] if text else []
        for acc in calls.values():
            try:
                args = json.loads(acc["arguments"] or "{}")
            except json.JSONDecodeError:
                log.warning("tool call %s with invalid arguments: %r", acc["name"], acc["arguments"])
                continue
            call = ToolCall(
                name=acc["name"],
                arguments=args if isinstance(args, dict) else {},
                call_id=acc["id"],
                raw=acc["arguments"],
            )
            items.append(call)
            yield call
        yield Done(text, tuple(items))


def chat_messages(messages: Sequence[Item]) -> list[dict[str, Any]]:
    """Request items as chat messages. Chat completions has no developer role everywhere: notes go in as
    system messages. Tool calls join the assistant message before them; reasoning is not carried."""
    out: list[dict[str, Any]] = []
    for m in messages:
        match m:
            case Message():
                out.append({"role": "system" if m.role == "developer" else m.role, "content": m.content})
            case ToolCall():
                call = {
                    "id": m.call_id,
                    "type": "function",
                    "function": {"name": m.name, "arguments": m.raw or json.dumps(m.arguments)},
                }
                if out and out[-1]["role"] == "assistant":
                    out[-1].setdefault("tool_calls", []).append(call)
                else:
                    out.append({"role": "assistant", "content": None, "tool_calls": [call]})
            case ToolResult():
                out.append({"role": "tool", "tool_call_id": m.call_id, "content": m.output})
    return out
