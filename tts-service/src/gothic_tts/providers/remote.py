"""External providers over HTTP."""

from __future__ import annotations

import os

import httpx

from .base import ChunkCallback, TtsError, Voice


class OpenAISpeech:
    """`POST {base_url}/audio/speech` with an API key (OpenAI and compatible services).
    Not available with a ChatGPT plan sign-in (the plan token is refused for /audio/speech)."""

    name = "openai"
    rate = 24000  # response_format=pcm
    supports_cloning = False

    def __init__(
        self,
        model: str = "gpt-4o-mini-tts",
        voice: str = "onyx",
        base_url: str = "https://api.openai.com/v1",
        api_key_env: str = "OPENAI_API_KEY",
        timeout: str | float = 20.0,
        transport: httpx.AsyncBaseTransport | None = None,
    ) -> None:
        self.model = model
        self.voice = voice
        self.base_url = base_url.rstrip("/")
        self.api_key_env = api_key_env
        self._client = httpx.AsyncClient(timeout=float(timeout), transport=transport)

    def _key(self) -> str:
        key = os.environ.get(self.api_key_env, "")
        if not key:
            raise TtsError(f"no API key: set {self.api_key_env}")
        return key

    async def warmup(self) -> None:
        self._key()

    async def synthesize(self, text: str, voice: Voice, on_chunk: ChunkCallback) -> float:
        body = {
            "model": self.model,
            "input": text,
            "voice": voice.preset or self.voice,
            "response_format": "pcm",
        }
        instructions = ". ".join(p.strip().rstrip(".") for p in (voice.description, voice.style) if p.strip())
        if instructions and "tts" in self.model:
            body["instructions"] = instructions
        size = 0
        try:
            async with self._client.stream(
                "POST",
                f"{self.base_url}/audio/speech",
                headers={"Authorization": f"Bearer {self._key()}"},
                json=body,
            ) as resp:
                if resp.status_code != 200:
                    detail = (await resp.aread()).decode(errors="replace")[:300]
                    raise TtsError(f"{self.base_url} returned {resp.status_code}: {detail}")
                carry = b""
                async for data in resp.aiter_bytes():
                    data = carry + data
                    cut = len(data) // 2 * 2  # whole samples only
                    carry = data[cut:]
                    if cut:
                        size += cut
                        await on_chunk(data[:cut])
        except httpx.HTTPError as err:
            raise TtsError(f"{self.base_url}: {type(err).__name__}: {err}") from err
        return size / 2 / self.rate

    async def aclose(self) -> None:
        await self._client.aclose()
