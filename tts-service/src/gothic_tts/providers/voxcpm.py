"""VoxCPM2 on the GPU server (tts-service/deploy/voxcpm/server.py; never on this machine)."""

from __future__ import annotations

import asyncio
import itertools
import json
import logging
import os

from websockets.asyncio.client import ClientConnection, connect
from websockets.exceptions import WebSocketException

from .base import ChunkCallback, TtsError, Voice

log = logging.getLogger(__name__)

DEFAULT_URL = "ws://127.0.0.1:47831"  # GOTHIC_VOXCPM_URL: the GPU server running it


class VoxCPM:
    name = "voxcpm"
    rate = 48000
    supports_cloning = False

    def __init__(
        self, url: str = "", timeout: str | float = 10.0, connect_timeout: str | float = 3.0
    ) -> None:
        self.url = url or os.environ.get("GOTHIC_VOXCPM_URL", DEFAULT_URL)
        self.timeout = float(timeout)  # also the limit for the first chunk
        self.connect_timeout = float(connect_timeout)  # an offline server (Tailscale) hangs the connect
        self._ws: ClientConnection | None = None
        self._lock = asyncio.Lock()
        self._ids = itertools.count(1)

    async def warmup(self) -> None:
        async with self._lock:
            await self._connection()

    async def synthesize(self, text: str, voice: Voice, on_chunk: ChunkCallback) -> float:
        if voice.sample:
            raise TtsError("voxcpm: the bridge designs voices, it clones none from a recording")
        async with self._lock:
            try:
                ws = await self._connection()
                req = str(next(self._ids))
                request = {
                    "type": "speak",
                    "id": req,
                    "text": text,
                    "voice": {"id": voice.id, "description": voice.description},
                }
                if voice.style:
                    request["style"] = voice.style
                await ws.send(json.dumps(request, ensure_ascii=False))
                while True:
                    msg = await asyncio.wait_for(ws.recv(), self.timeout)
                    if isinstance(msg, bytes):
                        await on_chunk(msg)
                        continue
                    ev = json.loads(msg)
                    if ev.get("id") not in (req, None):
                        continue
                    if ev.get("type") == "done":
                        if int(ev.get("sample_rate", self.rate)) != self.rate:
                            raise TtsError(f"voxcpm: unexpected sample rate {ev.get('sample_rate')}")
                        return float(ev.get("audio_s", 0.0))
                    if ev.get("type") == "error":
                        raise TtsError(f"voxcpm: {ev.get('detail')}")
            except (WebSocketException, OSError, TimeoutError) as err:
                await self._drop()  # a half-read answer must not leak into the next request
                raise TtsError(f"voxcpm {self.url}: {type(err).__name__}: {err}") from err
            except BaseException:
                await self._drop()
                raise

    async def _connection(self) -> ClientConnection:
        if self._ws is None:
            try:
                self._ws = await connect(self.url, open_timeout=self.connect_timeout, max_size=2**22)
            except (WebSocketException, OSError, TimeoutError) as err:
                raise TtsError(f"voxcpm {self.url}: {type(err).__name__}: {err}") from err
            log.info("voxcpm connected (%s)", self.url)
        return self._ws

    async def _drop(self) -> None:
        ws, self._ws = self._ws, None
        if ws is not None:
            await ws.close()

    async def aclose(self) -> None:
        await self._drop()
