"""Speech through Codex's own realtime voice (ChatGPT plan): `codex app-server` runs as a child process
and does the sign-in and the calls itself; this provider never sees credentials.

Protocol (JSON-RPC over stdio, experimental methods): `initialize` with `experimentalApi`,
`thread/start` (ephemeral), `thread/realtime/start` with audio output and a voice, then per line
`thread/realtime/appendSpeech`.

Transport `webrtc` (default; the only one a ChatGPT sign-in allows): this provider is the WebRTC peer.
Codex posts its offer to the plan's realtime endpoint and returns the answer; the voice then arrives
as a WebRTC audio track, in real time (a line takes as long to receive as to say). It sends silence
as its microphone. Transport `websocket` needs an API key in Codex; audio then comes as
`thread/realtime/outputAudio/delta` notifications.

A line ends when the voice goes quiet for `idle_s` after it started.

An open realtime session keeps streaming (silence) and likely counts against the plan's voice
limits, so it is opened on demand and closed after `close_after_s` without lines.

EXPERIMENTAL, not reliable as TTS (2026-10-11, codex-cli 0.154): the voice is a conversational
model and appendSpeech reaches it as an agent handoff. It reads the text but often goes on talking
in its own words, or replaces the text with an invented one; nothing but stopping the session
interrupts it, so later lines queue behind that speech and line boundaries drift.
"""

from __future__ import annotations

import asyncio
import base64
import fractions
import itertools
import json
import logging
import math
import shlex
import struct
import tempfile
import time
from typing import Any

from .base import ChunkCallback, TtsError, Voice

log = logging.getLogger(__name__)

PROMPT = (
    "You are a text-to-speech engine, not a conversation partner. Every message you get is a script "
    "line. Read it aloud exactly as written, word for word, in its own language, with natural "
    "intonation. Then stop speaking immediately and stay silent until the next line arrives. "
    "Never answer, comment, continue, ask questions or add a single word of your own."
)


class CodexRealtime:
    name = "codex"
    rate = 24000
    supports_cloning = False

    def __init__(
        self,
        voice: str = "spruce",
        model: str | None = None,
        codex: str = "codex",
        transport: str = "webrtc",
        version: str = "v3",
        timeout: str | float = 15.0,
        idle_s: str | float = 0.5,
        close_after_s: str | float = 120.0,
    ) -> None:
        if transport not in ("webrtc", "websocket"):
            raise ValueError(f"unknown transport {transport!r}")
        self.voice = voice
        self.model = model
        self.codex = codex
        self.transport = transport
        self.version = version
        self.timeout = float(timeout)
        self.idle_s = float(idle_s)
        self.close_after_s = float(close_after_s)
        self._idle_close: asyncio.TimerHandle | None = None
        self._peer: Any = None  # aiortc RTCPeerConnection (webrtc)
        self._audio: asyncio.Queue[bytes] = asyncio.Queue()  # webrtc voice, 24 kHz mono PCM16
        self._proc: asyncio.subprocess.Process | None = None
        self._reader: asyncio.Task[None] | None = None
        self._ids = itertools.count(1)
        self._pending: dict[int, asyncio.Future[Any]] = {}
        self._events: asyncio.Queue[dict] = asyncio.Queue()
        self._thread: str | None = None
        self._session_voice: str | None = None
        self._lock = asyncio.Lock()
        self._cwd = tempfile.mkdtemp(prefix="gothic-tts-codex-")

    async def warmup(self) -> None:
        async with self._lock:
            await self._session(self.voice)
        self._schedule_close()

    async def synthesize(self, text: str, voice: Voice, on_chunk: ChunkCallback) -> float:
        try:
            return await self._synthesize(text, voice, on_chunk)
        finally:
            self._schedule_close()

    def _schedule_close(self) -> None:
        if self._idle_close is not None:
            self._idle_close.cancel()
        self._idle_close = asyncio.get_running_loop().call_later(
            self.close_after_s, lambda: asyncio.ensure_future(self._close_session())
        )

    async def _close_session(self) -> None:
        async with self._lock:
            if self._thread is None:
                return
            log.info("codex realtime session idle, closing it")
            try:
                await self._call("thread/realtime/stop", {"threadId": self._thread})
            except (TtsError, OSError, TimeoutError, ConnectionError) as err:
                log.info("closing the realtime session: %s", err)
            self._thread = None
            await self._close_peer()

    def _preset(self, voice: Voice) -> str:
        if voice.preset:
            return voice.preset
        if any(w in voice.description.lower() for w in ("woman", "female", "girl")):
            return "juniper"
        return self.voice

    async def _synthesize(self, text: str, voice: Voice, on_chunk: ChunkCallback) -> float:
        async with self._lock:
            try:
                thread = await self._session(self._preset(voice))
                while not self._events.empty():  # leftovers of an earlier line
                    self._events.get_nowait()
                while not self._audio.empty():
                    self._audio.get_nowait()
                await self._call("thread/realtime/appendSpeech", {"threadId": thread, "text": text})
                if self.transport == "webrtc":
                    return await self._collect_track(on_chunk)
                return await self._collect(on_chunk)
            except (OSError, TimeoutError, ConnectionError) as err:
                await self._stop()
                raise TtsError(f"codex: {type(err).__name__}: {err}") from err

    async def _collect_track(self, on_chunk: ChunkCallback) -> float:
        """Voice from the WebRTC track: skip the silence before it (keeping a short lead-in, soft
        consonants start below the threshold), stop once it is quiet again."""
        samples, quiet = 0, 0.0
        lead: list[bytes] = []
        deadline = time.monotonic() + self.timeout
        while True:
            try:
                pcm = await asyncio.wait_for(self._audio.get(), max(0.1, deadline - time.monotonic()))
            except TimeoutError:
                if samples:
                    return samples / self.rate
                raise
            self._raise_events()
            loud = _rms(pcm) > SILENCE_RMS
            if not samples and not loud:
                lead.append(pcm)
                while sum(len(x) for x in lead) > LEAD_IN_S * self.rate * 2:
                    lead.pop(0)
                continue
            for part in [*lead, pcm]:
                samples += len(part) // 2
                await on_chunk(part)
            lead.clear()
            quiet = 0.0 if loud else quiet + len(pcm) / 2 / self.rate
            if quiet >= self.idle_s:
                return samples / self.rate
            deadline = time.monotonic() + self.timeout

    def _raise_events(self) -> None:
        while not self._events.empty():
            ev = self._events.get_nowait()
            params = ev.get("params") or {}
            if ev.get("method") == "thread/realtime/transcript/done":
                log.info("codex said (%s): %s", params.get("role"), params.get("text"))
            if ev.get("method") == "thread/realtime/error":
                raise TtsError(f"codex realtime: {params.get('message')}")
            if ev.get("method") == "thread/realtime/closed":
                self._thread = None
                raise TtsError(f"codex realtime closed: {params.get('reason')}")

    async def _collect(self, on_chunk: ChunkCallback) -> float:
        samples = 0
        while True:
            wait = self.idle_s if samples else self.timeout
            try:
                ev = await asyncio.wait_for(self._events.get(), wait)
            except TimeoutError:
                if samples:
                    return samples / self.rate  # the voice went quiet: the line is over
                raise
            method, params = ev.get("method", ""), ev.get("params") or {}
            match method:
                case "thread/realtime/outputAudio/delta":
                    audio = params.get("audio") or {}
                    rate = int(audio.get("sampleRate", self.rate))
                    if rate != self.rate or int(audio.get("numChannels", 1)) != 1:
                        raise TtsError(f"codex: unexpected audio format {rate} Hz")
                    pcm = base64.b64decode(audio.get("data", ""))
                    samples += len(pcm) // 2
                    await on_chunk(pcm)
                case "thread/realtime/transcript/done" if samples and params.get("role") == "assistant":
                    return samples / self.rate
                case "thread/realtime/error":
                    raise TtsError(f"codex realtime: {params.get('message')}")
                case "thread/realtime/closed":
                    self._thread = None
                    raise TtsError(f"codex realtime closed: {params.get('reason')}")

    # --- app-server ---------------------------------------------------------------------

    async def _session(self, voice: str) -> str:
        if self._thread is not None and self._session_voice == voice:
            return self._thread
        if self._proc is None or self._proc.returncode is not None:
            await self._start_server()
        if self._thread is not None:  # another voice: a new realtime session
            await self._call("thread/realtime/stop", {"threadId": self._thread})
        thread = (await self._call("thread/start", {"ephemeral": True, "cwd": self._cwd}))["thread"]["id"]
        params: dict[str, Any] = {
            "threadId": thread,
            "outputModality": "audio",
            "voice": voice,
            "includeStartupContext": False,
            "prompt": PROMPT,
            "version": self.version,
            "transport": {"type": "websocket"},
        }
        if self.model:
            params["model"] = self.model
        if self.transport == "webrtc":
            params["transport"] = {"type": "webrtc", "sdp": await self._offer()}
        await self._call("thread/realtime/start", params)
        wanted = {"thread/realtime/started"} | (
            {"thread/realtime/sdp"} if self.transport == "webrtc" else set()
        )
        events = await self._expect(wanted)  # they come in either order
        if self.transport == "webrtc":
            await self._answer((events["thread/realtime/sdp"].get("params") or {}).get("sdp", ""))
        self._thread, self._session_voice = thread, voice
        log.info("codex realtime session ready (voice %s)", voice)
        return thread

    async def _start_server(self) -> None:
        try:
            self._proc = await asyncio.create_subprocess_exec(
                *shlex.split(self.codex),
                "app-server",
                "--listen",
                "stdio://",
                stdin=asyncio.subprocess.PIPE,
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.DEVNULL,
                limit=2**24,
            )
        except FileNotFoundError as err:
            raise TtsError(f"codex CLI not found ({self.codex})") from err
        self._reader = asyncio.create_task(self._read())
        await self._call(
            "initialize",
            {
                "clientInfo": {"name": "gothic_ai_tts", "title": "Gothic AI TTS", "version": "0.1.0"},
                "capabilities": {"experimentalApi": True},
            },
        )
        await self._notify("initialized")

    async def _read(self) -> None:
        assert self._proc is not None and self._proc.stdout is not None
        try:
            while line := await self._proc.stdout.readline():
                try:
                    msg = json.loads(line)
                except ValueError:
                    continue
                if "id" in msg and ("result" in msg or "error" in msg):
                    fut = self._pending.pop(msg["id"], None)
                    if fut is not None and not fut.done():
                        if "error" in msg:
                            fut.set_exception(TtsError(f"codex: {msg['error'].get('message', msg['error'])}"))
                        else:
                            fut.set_result(msg["result"])
                elif "method" in msg and "id" in msg:
                    # a request from the server (approvals and the like): this client asks for nothing
                    await self._send({"id": msg["id"], "error": {"code": -32601, "message": "not supported"}})
                elif "method" in msg:
                    log.debug("codex event %s", msg["method"])
                    if msg["method"] == "thread/realtime/transcript/done":
                        log.info(
                            "codex voice said (%s): %s",
                            (msg.get("params") or {}).get("role"),
                            (msg.get("params") or {}).get("text"),
                        )
                    self._events.put_nowait(msg)
        finally:
            for fut in self._pending.values():
                if not fut.done():
                    fut.set_exception(ConnectionError("codex app-server exited"))
            self._pending.clear()
            self._thread = None

    async def _send(self, msg: dict) -> None:
        assert self._proc is not None and self._proc.stdin is not None
        self._proc.stdin.write(json.dumps(msg, ensure_ascii=False).encode() + b"\n")
        await self._proc.stdin.drain()

    async def _call(self, method: str, params: dict) -> Any:
        req = next(self._ids)
        fut: asyncio.Future[Any] = asyncio.get_running_loop().create_future()
        self._pending[req] = fut
        await self._send({"id": req, "method": method, "params": params})
        return await asyncio.wait_for(fut, self.timeout)

    async def _notify(self, method: str) -> None:
        await self._send({"method": method})

    async def _expect(self, methods: set[str]) -> dict[str, dict]:
        got: dict[str, dict] = {}
        while set(got) != methods:
            ev = await asyncio.wait_for(self._events.get(), self.timeout)
            if ev.get("method") in methods:
                got[ev["method"]] = ev
            elif ev.get("method") == "thread/realtime/error":
                raise TtsError(f"codex realtime: {(ev.get('params') or {}).get('message')}")
        return got

    # --- webrtc -------------------------------------------------------------------------

    async def _offer(self) -> str:
        try:
            from aiortc import RTCPeerConnection
        except ImportError as err:
            raise TtsError("aiortc is not installed: uv sync --extra codex") from err
        await self._close_peer()
        peer = RTCPeerConnection()
        peer.addTrack(_silence_track())
        peer.createDataChannel("oai-events")

        @peer.on("track")
        def on_track(track: Any) -> None:
            if track.kind == "audio":
                asyncio.ensure_future(self._receive(track))

        await peer.setLocalDescription(await peer.createOffer())
        self._peer = peer
        return peer.localDescription.sdp

    async def _answer(self, sdp: str) -> None:
        from aiortc import RTCSessionDescription

        if not sdp:
            raise TtsError("codex realtime: no SDP answer")
        await self._peer.setRemoteDescription(RTCSessionDescription(sdp=sdp, type="answer"))

    async def _receive(self, track: Any) -> None:
        from aiortc.mediastreams import MediaStreamError
        from av import AudioResampler

        resampler = AudioResampler(format="s16", layout="mono", rate=self.rate)
        try:
            while True:
                frame = await track.recv()
                for out in resampler.resample(frame):
                    self._audio.put_nowait(bytes(out.planes[0])[: out.samples * 2])
        except MediaStreamError:
            log.info("codex voice track ended")

    async def _close_peer(self) -> None:
        peer, self._peer = self._peer, None
        if peer is not None:
            await peer.close()

    async def _stop(self) -> None:
        await self._close_peer()
        proc, self._proc, self._thread = self._proc, None, None
        if proc is not None and proc.returncode is None:
            proc.terminate()
            try:
                await asyncio.wait_for(proc.wait(), 3)
            except TimeoutError:
                proc.kill()
        if self._reader is not None:
            self._reader.cancel()

    async def aclose(self) -> None:
        await self._stop()


SILENCE_RMS = 200.0  # 16-bit PCM; quieter frames count as a pause
LEAD_IN_S = 0.3


def _rms(pcm: bytes) -> float:
    n = len(pcm) // 2
    if n == 0:
        return 0.0
    samples = struct.unpack(f"<{n}h", pcm[: n * 2])
    return math.sqrt(sum(x * x for x in samples) / n)


def _silence_track() -> Any:
    """The provider's "microphone": 20 ms frames of silence, paced in real time."""
    from aiortc import MediaStreamTrack
    from av import AudioFrame

    class SilenceTrack(MediaStreamTrack):
        kind = "audio"

        def __init__(self) -> None:
            super().__init__()
            self._pts = 0
            self._start: float | None = None

        async def recv(self) -> AudioFrame:
            if self._start is None:
                self._start = time.monotonic()
            wait = self._start + self._pts / 48000 - time.monotonic()
            if wait > 0:
                await asyncio.sleep(wait)
            frame = AudioFrame(format="s16", layout="mono", samples=960)
            for plane in frame.planes:
                plane.update(bytes(plane.buffer_size))
            frame.sample_rate = 48000
            frame.pts = self._pts
            frame.time_base = fractions.Fraction(1, 48000)
            self._pts += 960
            return frame

    return SilenceTrack()
