"""Inworld Realtime TTS-2 by API key (`INWORLD_API_KEY`, the Base64 key from the Inworld portal).

An npc's voice is designed once from its description (Voice Design), published to the workspace's
voice library and remembered in a local file, so every line of that npc has the same voice. A voice
with a `sample` is cloned from that recording instead (Instant Voice Cloning), and remembered the same
way: the shorts' English dub speaks in the user's own voice, or in an npc's Russian voice. A library
voice can be used instead: `voice=Dmitry` for every npc, or a request's `preset`; its description then
steers the delivery. A line's `style` steers it too (an angry line), whichever the voice. Steering is
the request's `instruction`, in English; TTS-2 only, Flash has none.
"""

from __future__ import annotations

import asyncio
import base64
import hashlib
import json
import logging
import os
import re
import time
from email.utils import parsedate_to_datetime
from pathlib import Path
from typing import Any

import httpx

from .base import ChunkCallback, TtsError, Voice

log = logging.getLogger(__name__)

DEFAULT_CACHE = Path.home() / ".local/share/gothic-ai/inworld-voices.json"
KEEP_WARM_S = 60.0
CLONE_TIMEOUT = httpx.Timeout(120.0, connect=3.0)
# warmup's check: a line for a voice that does not exist, refused before anything is spoken or billed
CHECK_VOICE = "gothic-ai-no-such-voice"
CHECK_TEXT = "Да."
# Voice Design shapes the voice on a 1-30 s script in the voice's language (the shorts' English dub designs
# en-US voices)
PREVIEW_TEXTS = {
    "ru": "Ты снова здесь. Значит, Иннос ещё не отвернулся от тебя. Слушай внимательно, у нас мало времени.",
    "en": "You're back. So Innos hasn't turned away from you yet. Listen carefully, we don't have much time.",
}


class _UnknownVoice(TtsError):
    """The voice is not in the workspace (deleted, or the API key is of another workspace)."""


class InworldTTS:
    name = "inworld"
    supports_cloning = True

    def __init__(
        self,
        model: str = "inworld-tts-2",
        voice: str | None = None,
        language: str = "ru-RU",
        rate: str | int = 24000,
        delivery: str | None = None,
        cache: str | Path = DEFAULT_CACHE,
        base_url: str = "https://api.inworld.ai",
        api_key_env: str = "INWORLD_API_KEY",
        timeout: str | float = 10.0,
        keep_warm: str | float = KEEP_WARM_S,
        transport: httpx.AsyncBaseTransport | None = None,
    ) -> None:
        self.model = model
        self.voice = voice
        self.language = language
        self.rate = int(rate)
        self.delivery = delivery  # STABLE / BALANCED / CREATIVE (TTS-2 only)
        self.cache = Path(cache)
        self.base_url = base_url.rstrip("/")
        self.api_key_env = api_key_env
        # npc's are often silent for minutes, and a new connection adds its TCP+TLS handshake (~0.25 s) to
        # the next line's first audio. Inworld keeps an idle connection for 610 s and httpx for its
        # keepalive_expiry, so warmup's free check after `keep_warm` s without requests (0: never) keeps
        # it open
        self.keep_warm = float(keep_warm)
        self._client = httpx.AsyncClient(
            timeout=httpx.Timeout(float(timeout), connect=3.0),
            transport=transport,
            limits=httpx.Limits(keepalive_expiry=max(120.0, 2 * self.keep_warm)),
        )
        self._designing = asyncio.Lock()
        self._cloning = asyncio.Lock()
        self._used = 0.0  # time.monotonic() of the last request's start or end
        self._keeper: asyncio.Task[None] | None = None

    @property
    def steerable(self) -> bool:
        return "flash" not in self.model

    def _headers(self) -> dict[str, str]:
        key = os.environ.get(self.api_key_env, "")
        if not key:
            raise TtsError(f"no API key: set {self.api_key_env}")
        return {"Authorization": f"Basic {key}"}

    async def warmup(self) -> None:
        """Checks the key and the credits (and opens the connection) at no cost: Inworld refuses a line for
        a voice that does not exist before speaking, for missing credits first (402), then for the voice
        (404). The free voice list is no such check: an account without credits still gets it, and the
        next line paid for a 402 round trip before the fallback spoke."""
        self._start_keeper()
        check = self._body(CHECK_TEXT, CHECK_VOICE, "")
        resp = await self._request("POST", "/tts/v1/voice:stream", json=check)
        status, detail = resp.status_code, resp.text
        if status == 200 or (status == 404 and "Unknown voice" in detail):
            return  # refused for the voice alone: the key works and there are credits
        if status in (401, 402, 403) or status >= 500:
            _raise(status, detail)  # no credits, a wrong key, or Inworld itself is down
        # an answer this check does not know (Inworld changed?): the voice list decides, as before
        log.debug("inworld: the check line got %s: %s", status, detail[:300])
        lang = self.language.split("-")[0].lower()
        await self._call("GET", "/tts/v1/voices", params={"filter": f"language={lang}"})

    def _start_keeper(self) -> None:
        if self.keep_warm > 0 and self._keeper is None:
            self._keeper = asyncio.create_task(self._keep_warm())

    async def _keep_warm(self) -> None:
        while True:
            idle = time.monotonic() - self._used
            if idle < self.keep_warm:
                await asyncio.sleep(self.keep_warm - idle)
                continue
            try:
                await self.warmup()
            except TtsError as err:  # a line will find out too, and the synthesizer handles that
                log.debug("inworld: keep-warm request failed: %s", err)

    async def synthesize(self, text: str, voice: Voice, on_chunk: ChunkCallback) -> float:
        self._start_keeper()
        preset = voice.preset or self.voice
        if preset:
            return await self._stream(text, preset, self._instruction(voice, preset=True), on_chunk)
        voice_id = await self._library(voice)
        instruction = self._instruction(voice, preset=False)  # the description is in the voice itself
        try:
            return await self._stream(text, voice_id, instruction, on_chunk)
        except _UnknownVoice:
            log.warning("inworld: voice %s of %s is gone, making it again", voice_id, voice.id)
            self._forget(voice.id)
            return await self._stream(text, await self._library(voice), instruction, on_chunk)

    async def _library(self, voice: Voice) -> str:
        return await (self._cloned(voice) if voice.sample else self._designed(voice))

    def _instruction(self, voice: Voice, preset: bool) -> str:
        if not self.steerable:
            return ""
        parts = [voice.description] if preset else []
        return ". ".join(p.strip().rstrip(".") for p in (*parts, voice.style) if p.strip())

    def _body(self, text: str, voice_id: str, instruction: str) -> dict[str, Any]:
        body: dict[str, Any] = {
            "text": text,
            "voiceId": voice_id,
            "modelId": self.model,
            "language": self.language,
            "audioConfig": {"audioEncoding": "PCM", "sampleRateHertz": self.rate},
        }
        if instruction:
            body["instruction"] = instruction
        if self.delivery and self.steerable:
            body["deliveryMode"] = self.delivery.upper()
        return body

    async def _stream(self, text: str, voice_id: str, instruction: str, on_chunk: ChunkCallback) -> float:
        body = self._body(text, voice_id, instruction)
        size = 0
        self._used = time.monotonic()
        try:
            async with self._client.stream(
                "POST", f"{self.base_url}/tts/v1/voice:stream", headers=self._headers(), json=body
            ) as resp:
                if resp.status_code != 200:
                    _raise(resp.status_code, (await resp.aread()).decode(errors="replace"))
                carry = b""
                async for line in resp.aiter_lines():  # NDJSON, one result per line
                    if not line.strip():
                        continue
                    msg = json.loads(line)
                    if failure := msg.get("error"):
                        if isinstance(failure, dict):
                            failure = failure.get("message", failure)
                        raise TtsError(f"inworld: {failure}")
                    data = carry + base64.b64decode(msg.get("result", {}).get("audioContent", ""))
                    cut = len(data) // 2 * 2  # whole samples only
                    carry = data[cut:]
                    if cut:
                        size += cut
                        await on_chunk(data[:cut])
        except httpx.HTTPError as err:
            raise TtsError(f"inworld: {_describe(err)}") from err
        except ValueError as err:  # a line that is not JSON or not Base64
            raise TtsError(f"inworld: bad stream: {err}") from err
        finally:
            self._used = time.monotonic()
        return size / 2 / self.rate

    async def _designed(self, voice: Voice) -> str:
        """The library voice made for `voice` (designed and published on its first line)."""
        entry = self._load().get(voice.id, {})
        if entry.get("description") == voice.description and entry.get("language") == self.language:
            return str(entry["voice_id"])
        async with self._designing:
            entry = self._load().get(voice.id, {})
            if entry.get("description") == voice.description and entry.get("language") == self.language:
                return str(entry["voice_id"])
            if not voice.description:
                raise TtsError(f"inworld: voice {voice.id!r} has no description to design it from")
            design = await self._call(
                "POST",
                "/voices/v1/voices:design",
                json={
                    "languageCode": self.language,
                    "designPrompt": voice.description[:1000],
                    "previewText": preview_text(self.language),
                    "voiceDesignConfig": {"numberOfSamples": 1},
                },
            )
            try:
                preview = design["previewVoices"][0]["voiceId"]
            except (KeyError, IndexError, TypeError) as err:
                raise TtsError(f"inworld: voice design returned no voice: {design}") from err
            published = await self._call(
                "POST",
                f"/voices/v1/voices/{preview}:publish",
                json={
                    "displayName": f"gothic-ai {voice.id}",
                    "description": voice.description[:500],
                    "tags": ["gothic-ai"],
                },
            )
            voice_id = str(published.get("voiceId") or preview)
            log.info("inworld: designed voice %s for %s", voice_id, voice.id)
            entries = self._load()
            entries[voice.id] = {
                "voice_id": voice_id,
                "description": voice.description,
                "language": self.language,
            }
            self._save(entries)
            return voice_id

    async def clone(self, voice: Voice) -> str:
        """Prepare a cloned library voice without synthesizing a line."""
        return await self._cloned(voice)

    def cached_clone(self, voice: Voice) -> str | None:
        """Look up a matching sample without contacting Inworld; call from a worker thread."""
        entry = self._load().get(voice.id, {})
        if entry.get("voice_id") and entry.get("sample"):
            digest = hashlib.sha1(Path(voice.sample).read_bytes()).hexdigest()[:16]
            if entry["sample"] == digest:
                return str(entry["voice_id"])
        return None

    async def _cloned(self, voice: Voice) -> str:
        """The library voice cloned from `voice.sample` (made on its first line, again when the recording
        changes). Inworld detects the recording's language; the lines are said in the provider's
        `language`, so a Russian recording speaks English. Its best length is 10-30 s."""
        try:
            audio = await asyncio.to_thread(Path(voice.sample).read_bytes)
        except OSError as err:
            raise TtsError(f"inworld: no sample to clone {voice.id!r} from: {err}") from err
        digest = hashlib.sha1(audio).hexdigest()[:16]
        entry = self._load().get(voice.id, {})
        if entry.get("sample") == digest and entry.get("voice_id"):
            return str(entry["voice_id"])
        async with self._cloning:
            entry = self._load().get(voice.id, {})
            if entry.get("sample") == digest and entry.get("voice_id"):
                return str(entry["voice_id"])
            body: dict[str, Any] = {
                "displayName": f"gothic-ai {voice.id}",
                "voiceSamples": [{"audioData": base64.b64encode(audio).decode()}],
                "tags": ["gothic-ai"],
                "audioProcessingConfig": {"removeBackgroundNoise": False},  # the samples are clean
            }
            if voice.description:
                body["description"] = voice.description[:500]
            # cloning takes 10-20 s before the answer comes: longer than a line's timeout
            for attempt in range(6):
                resp = await self._request(
                    "POST", "/voices/v1/voices:clone", json=body, timeout=CLONE_TIMEOUT
                )
                if resp.status_code == 429 and attempt < 5:
                    delay = _clone_retry_delay(resp)
                    log.info("original voices: Inworld rate limit, waiting %g s", delay)
                    await asyncio.sleep(delay)
                    continue
                if resp.status_code != 200:
                    _raise(resp.status_code, resp.text)
                cloned = resp.json()
                break
            for checked in cloned.get("audioSamplesValidated", []):
                if errors := [e.get("text", e) for e in checked.get("errors", [])]:
                    raise TtsError(f"inworld: the sample of {voice.id!r} was refused: {errors}")
                for warning in checked.get("warnings", []):
                    log.warning("inworld: sample of %s: %s", voice.id, warning.get("text", warning))
            try:
                voice_id = cloned["voice"]["voiceId"]
                if not isinstance(voice_id, str) or not voice_id.strip():
                    raise ValueError("empty voice id")
            except (KeyError, TypeError, ValueError) as err:
                raise TtsError(f"inworld: voice cloning returned no voice: {str(cloned)[:300]}") from err
            log.info("inworld: cloned voice %s for %s", voice_id, voice.id)
            entries = self._load()
            entries[voice.id] = {
                "voice_id": voice_id,
                "sample": digest,
                "description": voice.description,
                "language": str(cloned["voice"].get("languageCode", "")),  # the sample's
            }
            self._save(entries)
            return voice_id

    async def _call(self, method: str, path: str, **kwargs: Any) -> dict[str, Any]:
        resp = await self._request(method, path, **kwargs)
        if resp.status_code != 200:
            _raise(resp.status_code, resp.text)
        return resp.json()

    async def _request(self, method: str, path: str, **kwargs: Any) -> httpx.Response:
        self._used = time.monotonic()
        try:
            return await self._client.request(
                method, f"{self.base_url}{path}", headers=self._headers(), **kwargs
            )
        except httpx.HTTPError as err:
            raise TtsError(f"inworld: {_describe(err)}") from err
        finally:
            self._used = time.monotonic()

    def _load(self) -> dict[str, dict[str, str]]:
        try:
            return json.loads(self.cache.read_text())
        except FileNotFoundError:
            return {}
        except ValueError:
            log.warning("inworld: %s is not JSON, starting over", self.cache)
            return {}

    def _save(self, entries: dict[str, dict[str, str]]) -> None:
        self.cache.parent.mkdir(parents=True, exist_ok=True)
        tmp = self.cache.with_suffix(".tmp")
        tmp.write_text(json.dumps(entries, ensure_ascii=False, indent=1))
        tmp.replace(self.cache)

    def _forget(self, key: str) -> None:
        entries = self._load()
        if entries.pop(key, None) is not None:
            self._save(entries)

    async def aclose(self) -> None:
        if self._keeper is not None:
            self._keeper.cancel()
            await asyncio.gather(self._keeper, return_exceptions=True)
            self._keeper = None
        await self._client.aclose()


def preview_text(language: str) -> str:
    """The script a voice is designed on, in its language (ru-RU, en-US); a language without one gets the
    English."""
    return PREVIEW_TEXTS.get(language.split("-")[0].lower(), PREVIEW_TEXTS["en"])


def _describe(err: Exception) -> str:
    return f"{type(err).__name__}: {err}" if str(err) else type(err).__name__  # timeouts have no text


def _clone_retry_delay(resp: httpx.Response) -> float:
    retry_after = resp.headers.get("Retry-After", "")
    try:
        delay = float(retry_after)
    except ValueError:
        try:
            delay = parsedate_to_datetime(retry_after).timestamp() - time.time()
        except (ValueError, TypeError, OverflowError):
            match = re.search(r"try again in (\d+(?:\.\d+)?) seconds?", resp.text, re.IGNORECASE)
            delay = float(match[1]) if match else 30.0
    return max(0.0, delay) + 1.0  # leave a second for Inworld's rate-limit window to reset


def _raise(status: int, detail: str) -> None:
    detail = detail.strip()[:300]
    if status in (401, 403):
        raise TtsError(f"inworld rejected the API key ({status}): {detail}")
    if status == 404 and "Unknown voice" in detail:
        raise _UnknownVoice(f"inworld: {detail}")
    raise TtsError(f"inworld returned {status}: {detail}")
