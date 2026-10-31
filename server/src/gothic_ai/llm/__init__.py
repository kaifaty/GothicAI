"""LLM adapter: one streaming interface, swappable backends (plan subscription, API key, local, fake)."""

from __future__ import annotations

import os
from urllib.parse import urlsplit

from .base import (
    Done,
    Item,
    LLMBackend,
    LLMError,
    LLMEvent,
    Message,
    Reasoning,
    Role,
    TextDelta,
    Tool,
    ToolCall,
    ToolChoice,
    ToolResult,
    Usage,
    open_ahead,
    with_deadlines,
)
from .fake import FakeBackend, FakeStep
from .off import ModelOff, OffBackend
from .openai_compat import OpenAICompatBackend
from .openai_responses import OpenAIResponsesBackend
from .siwc import ChatGPTAuth

BACKENDS = ("chatgpt", "openai", "local", "fake", "off")
DEFAULT_MODELS = {
    "chatgpt": "gpt-6.1-sol", "openai": "gpt-6.1-sol", "local": "qwen3:8b", "fake": "fake", "off": "off"
}
# the quick model of a fight (dialogue.py): Gemma 4 12B on llama.cpp, as server/deploy/gemma names it. A line
# is a sentence or two; the cap keeps a small model from looping until its context is full
QUICK_MODEL = "gemma-4-12b"
QUICK_MAX_TOKENS = 120
QUICK_PORT = 47834  # on the GPU server, next to the decider (server/deploy/gemma)

__all__ = [
    "BACKENDS",
    "QUICK_MAX_TOKENS",
    "QUICK_MODEL",
    "QUICK_PORT",
    "Done",
    "FakeBackend",
    "FakeStep",
    "Item",
    "LLMBackend",
    "LLMError",
    "LLMEvent",
    "Message",
    "ModelOff",
    "OffBackend",
    "Reasoning",
    "Role",
    "TextDelta",
    "Tool",
    "ToolCall",
    "ToolChoice",
    "ToolResult",
    "Usage",
    "make_backend",
    "make_quick",
    "open_ahead",
    "quick_url",
    "with_deadlines",
]


def quick_url(url: str | None, decider_url: str | None) -> str | None:
    """Where the quick model is: `url` if given, else on the decider's host (the GPU server runs both,
    so it is on wherever the decider is); None without either."""
    if url:
        return url
    host = urlsplit(decider_url).hostname if decider_url else None
    return f"http://{host}:{QUICK_PORT}/v1" if host else None


def make_quick(base_url: str, model: str = QUICK_MODEL, max_tokens: int = QUICK_MAX_TOKENS) -> LLMBackend:
    """The quick model of a fight: an OpenAI-compatible server (llama.cpp), its answers capped."""
    return OpenAICompatBackend(model, base_url, name="quick", max_tokens=max_tokens)


def make_backend(
    kind: str,
    model: str | None = None,
    *,
    base_url: str | None = None,
    reasoning_effort: str | None = "low",
    websocket: bool = True,
) -> LLMBackend:
    """`websocket`: an npc's requests go over a WebSocket of its own (openai_responses.py)."""
    model = model or DEFAULT_MODELS[kind]
    match kind:
        case "off":
            return OffBackend()
        case "chatgpt":
            return OpenAIResponsesBackend(
                model, ChatGPTAuth(), reasoning_effort=reasoning_effort, name="chatgpt", websocket=websocket
            )
        case "openai":
            key = os.environ.get("OPENAI_API_KEY")
            if not key:
                raise LLMError("OPENAI_API_KEY is not set")

            async def api_key() -> str:
                return key

            return OpenAIResponsesBackend(
                model,
                api_key,
                reasoning_effort=reasoning_effort,
                base_url=base_url or "https://api.openai.com/v1",
                websocket=websocket,
            )
        case "local":
            return OpenAICompatBackend(model, base_url or "http://127.0.0.1:11434/v1", name="local")
        case "fake":
            # a slow model (to see the npc's first sounds): GOTHIC_AI_FAKE_FIRST seconds before it writes,
            # GOTHIC_AI_FAKE_DELAY seconds a chunk of the canned reply; GOTHIC_AI_FAKE_REPLIES: replies
            # in turn instead, separated by " | " (a scripted scene: answers that fit its phrases)
            replies = [
                r.strip() for r in os.environ.get("GOTHIC_AI_FAKE_REPLIES", "").split(" | ") if r.strip()
            ]
            return FakeBackend(
                delay=float(os.environ.get("GOTHIC_AI_FAKE_DELAY", "0.02")),
                first=float(os.environ.get("GOTHIC_AI_FAKE_FIRST", "0")),
                script=[FakeStep(r) for r in replies] or None,
            )
    raise LLMError(f"unknown LLM backend: {kind} (expected one of {', '.join(BACKENDS)})")
