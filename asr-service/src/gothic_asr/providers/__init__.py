"""Provider registry. A provider is chosen by a spec string: `name` or `name:key=value,key=value`,
e.g. `inworld` (Inworld STT), `voxtral` (self-hosted Voxtral Realtime), `chatgpt` (ChatGPT plan) or
`openai:base_url=https://api.groq.com/openai/v1,model=whisper-large-v3-turbo,api_key_env=GROQ_API_KEY`.
Models are never run on this machine: self-hosted ones live on a GPU server.
"""

from __future__ import annotations

from collections.abc import Callable

from .base import AsrError, AsrProvider, Transcript

__all__ = ["PROVIDERS", "AsrError", "AsrProvider", "Transcript", "make_provider", "parse_spec"]


def _chatgpt(**opts: str) -> AsrProvider:
    from .chatgpt import ChatGPTRealtime

    return ChatGPTRealtime(**opts)


def _voxtral(**opts: str) -> AsrProvider:
    from .voxtral import VoxtralRealtime

    return VoxtralRealtime(**opts)


def _inworld(**opts: str) -> AsrProvider:
    from .inworld import InworldRealtime

    return InworldRealtime(**opts)


def _openai(**opts: str) -> AsrProvider:
    from .remote import OpenAICompatible

    timeout = float(opts.pop("timeout", 20.0))
    return OpenAICompatible(**opts, timeout=timeout)


def _fake(**opts: str) -> AsrProvider:
    from .fake import FakeProvider

    return FakeProvider(**opts)


PROVIDERS: dict[str, Callable[..., AsrProvider]] = {
    "inworld": _inworld,
    "voxtral": _voxtral,
    "chatgpt": _chatgpt,
    "openai": _openai,
    "fake": _fake,
}


def parse_spec(spec: str) -> tuple[str, dict[str, str]]:
    name, _, rest = spec.partition(":")
    opts: dict[str, str] = {}
    for part in filter(None, rest.split(",")):
        key, sep, value = part.partition("=")
        if not sep:
            raise ValueError(f"bad provider option {part!r} in {spec!r}, expected key=value")
        opts[key.strip()] = value.strip()
    return name.strip(), opts


def make_provider(spec: str) -> AsrProvider:
    name, opts = parse_spec(spec)
    if name not in PROVIDERS:
        raise ValueError(f"unknown provider {name!r}, known: {', '.join(PROVIDERS)}")
    try:
        return PROVIDERS[name](**opts)
    except TypeError as err:
        raise ValueError(f"bad options for {name}: {err}") from err
