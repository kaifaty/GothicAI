"""WebSocket TTS API, with original game voices prepared in the background.

    client  -> service {"type":"speak","id":"7","text":"...",
                        "voice":{"id":"xardas","description":"...","preset":null},
                        "style":"say angrily with force"}
                       (`style`, optional: how to say this line, in English)
    service -> client  {"type":"start","id":"7","sample_rate":48000,"provider":"voxcpm"}
    service -> client  binary frames: 16-bit little-endian mono PCM, as it is generated
                       (at the loudness of the game's own dialogue, see loudness.py)
    service -> client  {"type":"done","id":"7","audio_s":2.1,"first_chunk_s":0.06,"latency_s":1.1}
    service -> client  {"type":"error","id":"7","detail":"..."}

Requests on one connection are handled one after another.

`GET /health` on the same port answers
    {"ok":true,"service":"gothic-tts","providers":[{"name":"...","ok":true}]}
(`ok` false: the provider failed and waits for its check in the background). The launcher waits for it
before starting the game.
"""

from __future__ import annotations

import asyncio
import hashlib
import json
import logging
import os
import re
import time
from dataclasses import replace
from http import HTTPStatus
from pathlib import Path

from websockets.asyncio.server import ServerConnection, serve
from websockets.exceptions import ConnectionClosed
from websockets.http11 import Request, Response

from . import originals
from .providers import TtsError, TtsProvider, Voice
from .providers.inworld import InworldTTS, _UnknownVoice
from .synthesizer import Synthesizer

log = logging.getLogger(__name__)

MAX_TEXT = 1000
MAX_STYLE = 300
DEFAULT_ORIGINAL_VOICES = Path.home() / ".local/share/gothic-ai/voices/original"
ORIGINAL_SAMPLES = json.loads(Path(__file__).with_name("original_samples.json").read_text())
ORIGINAL_LOOKUP_TIMEOUT_S = 15.0
LOCAL_ORIGINS = [None, re.compile(r"ws://(?:127\.0\.0\.1|localhost|\[::1\])(?::[0-9]+)?")]


def load_original_voices(directory: str | Path = DEFAULT_ORIGINAL_VOICES) -> dict[str, Voice]:
    """Index snapshots by instance ID; aliases share the canonical clone's cache key."""
    directory = Path(directory).expanduser()
    manifest = directory / "manifest.json"
    if not manifest.is_file():
        log.info("original voices disabled: no %s", manifest)
        return {}
    voices: dict[str, Voice] = {}
    data = json.loads(manifest.read_text())
    for entry in data.get("characters", []) if isinstance(data, dict) else data:
        cid = entry["id"]
        if entry.get("error"):
            log.warning("original voice %s skipped: %s", cid, entry["error"])
            continue
        sample = Path(entry.get("file") or f"{cid}.wav").expanduser()
        if not sample.is_absolute():
            sample = directory / sample
        if not sample.is_file():
            log.warning("original voice %s skipped: no sample %s", cid, sample)
            continue
        voice = Voice(f"{cid}@original", sample=str(sample))
        for alias in (cid, *entry.get("aliases", [])):
            voices[alias.casefold()] = voice
    return voices


class TtsService:
    def __init__(
        self,
        synthesizer: Synthesizer,
        host: str = "127.0.0.1",
        port: int = 47820,
        *,
        original_voices_dir: str | Path | None = DEFAULT_ORIGINAL_VOICES,
        game_dir: str | Path | None = None,
    ) -> None:
        self.synthesizer = synthesizer
        self.host = host
        self.port = port
        self.original_voices_dir = (
            Path(original_voices_dir).expanduser() if original_voices_dir is not None else None
        )
        self.game_dir = Path(game_dir).expanduser() if game_dir is not None else None
        self.original_voices: dict[str, Voice] = {}

    async def serve_forever(self, started=None) -> None:
        await self._publish_originals()
        async with serve(
            self._handler, self.host, self.port, max_size=2**20, process_request=self._health,
            origins=LOCAL_ORIGINS,
        ) as server:
            self.port = server.sockets[0].getsockname()[1]
            log.info("listening on ws://%s:%d", self.host, self.port)
            preparation = (
                asyncio.create_task(self._prepare_originals())
                if self.original_voices_dir is not None else None
            )
            try:
                if started is not None:
                    started.set()
                await server.serve_forever()
            finally:
                if preparation is not None:
                    preparation.cancel()
                    await asyncio.gather(preparation, return_exceptions=True)

    async def _publish_originals(self) -> None:
        """Resolve known snapshots before accepting connections, without needing their WAVs."""
        provider = self.synthesizer.primary
        if (
            self.original_voices_dir is None or not isinstance(provider, InworldTTS)
            or not os.environ.get(provider.api_key_env)
        ):
            return
        try:
            async with asyncio.timeout(ORIGINAL_LOOKUP_TIMEOUT_S):
                for cid, (_, _, aliases) in originals.CHARACTERS.items():
                    voice = Voice(f"{cid}@original")
                    digest = ORIGINAL_SAMPLES[cid]
                    voice_id = provider.cached_clone(voice, digest)
                    if voice_id is None:
                        voice_id = await provider.workspace_clone(voice, digest)
                    if voice_id is not None:
                        prepared = replace(voice, preset=voice_id, sample="")
                        self.original_voices.update(
                            (alias.casefold(), prepared) for alias in (cid, *aliases)
                        )
        except Exception as err:
            log.warning(
                "original voice startup lookup unavailable: %s",
                " ".join(str(err).split()) or type(err).__name__,
            )

    async def _prepare_originals(self) -> None:
        """Publish each cloned voice only after it is ready; game requests never create clones."""
        if self.original_voices_dir is None:
            return
        started = time.monotonic()
        provider = self.synthesizer.primary
        manifest = self.original_voices_dir / "manifest.json"
        if isinstance(provider, InworldTTS) and os.environ.get(provider.api_key_env) and manifest.is_file():
            try:
                snapshots = load_original_voices(self.original_voices_dir)
                for voice in {voice.id: voice for voice in snapshots.values()}.values():
                    if any(ready.id == voice.id for ready in self.original_voices.values()):
                        continue  # keep the expected snapshot's clone until preparation verifies it
                    if voice_id := await asyncio.to_thread(provider.cached_clone, voice):
                        prepared = replace(voice, preset=voice_id, sample="")
                        self.original_voices.update(
                            (alias, prepared) for alias, sample in snapshots.items() if sample.id == voice.id
                        )
            except (OSError, ValueError, KeyError) as err:
                log.warning("original voice cache unavailable: %s", " ".join(str(err).split()))
        try:
            if self.game_dir is None and not manifest.is_file():
                raise ValueError("no game directory (--game-dir) or prepared original voices")

            def progress(done: int, total: int) -> None:
                log.info("original voices: snapshots %d/%d", done, total)

            if self.game_dir is not None:
                await asyncio.to_thread(
                    originals.prepare, self.game_dir, self.original_voices_dir, progress=progress
                )
            snapshots = load_original_voices(self.original_voices_dir)
        except Exception as err:
            log.warning(
                "original voice preparation unavailable: %s", " ".join(str(err).split())
            )
            return
        if not isinstance(provider, InworldTTS) or not os.environ.get(provider.api_key_env):
            log.info(
                "original voices: snapshots ready in %.1fs; Inworld cloning unavailable",
                time.monotonic() - started,
            )
            return
        voices = {voice.id: voice for voice in snapshots.values()}
        self.original_voices = {
            alias: ready for alias, ready in self.original_voices.items() if ready.id in voices
        }
        ready = 0
        for done, voice in enumerate(voices.values(), 1):
            cid = voice.id.removesuffix("@original")
            try:
                audio = await asyncio.to_thread(Path(voice.sample).read_bytes)
                digest = hashlib.sha1(audio).hexdigest()[:16]
                expected = ORIGINAL_SAMPLES.get(cid)
                if expected is not None and digest != expected:
                    log.warning(
                        "original voices: %s: sample hash %s differs from expected %s; "
                        "update original_samples.json", cid, digest, expected,
                    )
                voice_id = provider.cached_clone(voice, digest)
                if voice_id is None:
                    voice_id = await provider.clone(voice)
            except Exception as err:
                self.original_voices = {
                    alias: ready for alias, ready in self.original_voices.items() if ready.id != voice.id
                }
                log.warning("original voices: %s: %s", cid, " ".join(str(err).split()))
            else:
                # A preset bypasses the provider's sample/clone path, including its retry on a deleted
                # library voice. A request can only use a clone which this background task prepared.
                prepared = replace(voice, preset=voice_id, sample="")
                aliases = {alias for alias, ready in self.original_voices.items() if ready.id == voice.id}
                aliases.update(alias for alias, sample in snapshots.items() if sample.id == voice.id)
                if cid in originals.CHARACTERS:
                    aliases.update(alias.casefold() for alias in (cid, *originals.CHARACTERS[cid][2]))
                self.original_voices.update(
                    (alias, prepared) for alias in aliases
                )
                ready += 1
            log.info("original voices: clones %d/%d", done, len(voices))
        log.info(
            "original voices: %d/%d ready, %d failed in %.1fs",
            ready, len(voices), len(voices) - ready, time.monotonic() - started,
        )

    def _health(self, conn: ServerConnection, request: Request) -> Response | None:
        if request.path != "/health":
            return None
        up = {id(p) for p in self.synthesizer.available()}
        providers = [{"name": p.name, "ok": id(p) in up} for p in self.synthesizer.providers]
        return _json(conn, {"ok": True, "service": "gothic-tts", "providers": providers})

    async def _handler(self, conn: ServerConnection) -> None:
        log.info("client connected from %s", conn.remote_address)
        try:
            async for raw in conn:
                if isinstance(raw, bytes):
                    continue
                try:
                    msg = json.loads(raw)
                    kind = msg["type"]
                except (ValueError, KeyError, TypeError):
                    await _send(conn, {"type": "error", "detail": "malformed message"})
                    continue
                if kind != "speak":
                    await _send(
                        conn, {"type": "error", "id": msg.get("id"), "detail": f"unknown type {kind!r}"}
                    )
                    continue
                await self._speak(conn, msg)
        except ConnectionClosed:
            pass
        log.info("client disconnected")

    async def _speak(self, conn: ServerConnection, msg: dict) -> None:
        req = msg.get("id")
        text = str(msg.get("text", "")).strip()[:MAX_TEXT]
        v = msg.get("voice") or {}
        voice = Voice(
            str(v.get("id", "default")),
            str(v.get("description") or ""),
            v.get("preset"),
            str(msg.get("style") or "")[:MAX_STYLE],
        )
        original = self.original_voices.get(voice.id.casefold()) if not voice.preset else None
        if original is not None:
            original = replace(original, description="", style=voice.style)
        if not text:
            await _send(conn, {"type": "error", "id": req, "detail": "empty text"})
            return
        current: list[TtsProvider] = []

        async def chunk(pcm: bytes, p: TtsProvider) -> None:
            if not current:
                current.append(p)
                await _send(conn, {"type": "start", "id": req, "sample_rate": p.rate, "provider": p.name})
            await conn.send(pcm)

        try:
            try:
                s = await self.synthesizer.speak(text, voice, chunk, original_voice=original)
            except _UnknownVoice:
                if original is None or current:
                    raise
                self.original_voices = {
                    alias: ready for alias, ready in self.original_voices.items() if ready.id != original.id
                }
                log.warning("original voice %s is gone; using designed voice until restart", original.id)
                s = await self.synthesizer.speak(text, voice, chunk)
        except TtsError as err:
            await _send(conn, {"type": "error", "id": req, "detail": str(err)})
            return
        await _send(
            conn,
            {
                "type": "done",
                "id": req,
                "audio_s": round(s.audio_s, 3),
                "first_chunk_s": round(s.first_chunk_s, 3),
                "latency_s": round(s.latency_s, 3),
            },
        )


async def _send(conn: ServerConnection, msg: dict) -> None:
    await conn.send(json.dumps(msg, ensure_ascii=False))


def _json(conn: ServerConnection, body: dict) -> Response:
    resp = conn.respond(HTTPStatus.OK, json.dumps(body, ensure_ascii=False))
    del resp.headers["Content-Type"]
    resp.headers["Content-Type"] = "application/json"
    return resp
