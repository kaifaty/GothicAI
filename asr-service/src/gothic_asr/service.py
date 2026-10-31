"""WebSocket API of the service. It knows nothing about the game or microphones: audio in, text out.

    service -> client  {"type":"hello","service":"gothic-asr","providers":[...],"streaming":true}
    client  -> service {"type":"start","lang":"ru","sample_rate":16000,"format":"pcm16","hints":["Кавалорн"]}
    client  -> service binary frames: 16-bit little-endian mono PCM, sent while the player speaks
    service -> client  {"type":"partial","text":"..."}     (streaming providers: text so far)
    client  -> service {"type":"end"}            (or {"type":"cancel"} to drop the utterance)
    service -> client  {"type":"final","text":"...","provider":"...","audio_s":2.1,"latency_s":0.3}
    service -> client  {"type":"error","detail":"..."}

`latency_s` counts from `end`. Utterances on one connection are handled one after another.
`start.hints` is an optional list of names or phrases to help streaming providers recognize them.
Providers without hint support ignore it; clients can omit it.

`GET /health` on the same port answers {"ok":true,"service":"gothic-asr","providers":[{"name":"..."}]}:
the launcher waits for it before starting the game.
"""

from __future__ import annotations

import asyncio
import json
import logging
from http import HTTPStatus

from websockets.asyncio.server import ServerConnection, serve
from websockets.exceptions import ConnectionClosed
from websockets.http11 import Request, Response

from .audio import RATE, pcm16_to_float, resample
from .providers import AsrError
from .recognizer import Recognizer, Utterance

log = logging.getLogger(__name__)

MAX_SECONDS = 60


class AsrService:
    def __init__(self, recognizer: Recognizer, host: str = "127.0.0.1", port: int = 47819) -> None:
        self.recognizer = recognizer
        self.host = host
        self.port = port

    async def serve_forever(self, started=None) -> None:
        async with serve(
            self._handler, self.host, self.port, max_size=2**22, process_request=self._health
        ) as server:
            self.port = server.sockets[0].getsockname()[1]
            log.info("listening on ws://%s:%d", self.host, self.port)
            if started is not None:
                started.set()
            await server.serve_forever()

    def _health(self, conn: ServerConnection, request: Request) -> Response | None:
        if request.path != "/health":
            return None
        providers = [{"name": p.name} for p in self.recognizer.providers]
        return _json(conn, {"ok": True, "service": "gothic-asr", "providers": providers})

    async def _handler(self, conn: ServerConnection) -> None:
        log.info("client connected from %s", conn.remote_address)
        # one writer keeps partials (sent from provider callbacks) ordered before the final
        outbox: asyncio.Queue[dict] = asyncio.Queue()
        writer = asyncio.create_task(_write(conn, outbox))
        outbox.put_nowait(
            {
                "type": "hello",
                "service": "gothic-asr",
                "providers": [p.name for p in self.recognizer.providers],
                "streaming": self.recognizer.primary.streaming,
            }
        )
        utt: Utterance | None = None
        rate = RATE
        size = 0
        try:
            async for raw in conn:
                if isinstance(raw, bytes):
                    if utt is not None and size < MAX_SECONDS * rate * 2:
                        size += len(raw)
                        await utt.feed(resample(pcm16_to_float(raw), rate))
                    continue
                try:
                    msg = json.loads(raw)
                    kind = msg["type"]
                except (ValueError, KeyError, TypeError):
                    outbox.put_nowait({"type": "error", "detail": "malformed message"})
                    continue
                if kind == "start":
                    if utt is not None:
                        await utt.cancel()
                    if msg.get("format", "pcm16") != "pcm16":
                        outbox.put_nowait({"type": "error", "detail": "only pcm16 is supported"})
                        utt = None
                        continue
                    hints = msg.get("hints")
                    if "hints" in msg and (
                        not isinstance(hints, list) or any(not isinstance(h, str) for h in hints)
                    ):
                        outbox.put_nowait({"type": "error", "detail": "hints must be a list of strings"})
                        utt = None
                        continue
                    rate, size = int(msg.get("sample_rate", RATE)), 0
                    utt = self.recognizer.utterance(
                        str(msg.get("lang", "ru")),
                        lambda text: outbox.put_nowait({"type": "partial", "text": text}),
                        hints=hints,
                    )
                    await utt.begin()
                elif kind == "cancel":
                    if utt is not None:
                        await utt.cancel()
                    utt = None
                elif kind == "end":
                    if utt is None:
                        outbox.put_nowait({"type": "error", "detail": "end without start"})
                        continue
                    outbox.put_nowait(await _finish(utt))
                    utt = None
                else:
                    outbox.put_nowait({"type": "error", "detail": f"unknown message type {kind!r}"})
        except ConnectionClosed:
            pass
        finally:
            if utt is not None:
                await utt.cancel()
            writer.cancel()
        log.info("client disconnected")


async def _finish(utt: Utterance) -> dict:
    try:
        r = await utt.finish()
    except AsrError as err:
        return {"type": "error", "detail": str(err)}
    return {
        "type": "final",
        "text": r.text,
        "provider": r.provider,
        "audio_s": round(r.audio_s, 3),
        "latency_s": round(r.latency_s, 3),
    }


async def _write(conn: ServerConnection, outbox: asyncio.Queue[dict]) -> None:
    try:
        while True:
            await conn.send(json.dumps(await outbox.get(), ensure_ascii=False))
    except ConnectionClosed:
        pass


def _json(conn: ServerConnection, body: dict) -> Response:
    resp = conn.respond(HTTPStatus.OK, json.dumps(body, ensure_ascii=False))
    del resp.headers["Content-Type"]
    resp.headers["Content-Type"] = "application/json"
    return resp
