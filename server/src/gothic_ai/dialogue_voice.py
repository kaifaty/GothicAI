"""Scene and story voices and speech-floor coordination."""

from __future__ import annotations

import asyncio
import base64
import logging
import time

from .dialogue_constants import QUIET_STYLE, TTS_PAUSE
from .dialogue_state import Conversation, SceneLine, ScenePlay, Voice
from .dialogue_text import to_game_text
from .game import GameError
from .persona import MOOD_DELIVERY
from .protocol import HushCmd, LineVoice, Mood, SayAudioCmd, SayCmd, SayEndCmd
from .tts import TtsClient, TtsError, WordMatcher, WordTiming

log = logging.getLogger("gothic_ai.dialogue")


class DialogueVoiceMixin:
    async def _wait_floor(self, conv: Conversation, line: str) -> None:
        """A line waits until those before the npc in the conversation have been heard (group.py); its clock
        of a line that never starts begins after."""
        seat = self._line_seats.get(line)
        if self.floor is None or line in conv.hushed:
            return
        try:
            await self.floor.hear(conv.npc, seat)
        finally:
            self._sent[line] = time.monotonic()

    def _tts_off(self, npc: str, name: str, err: TtsError) -> None:
        self._tts_paused_until = time.monotonic() + TTS_PAUSE
        log.warning("%s: no voice, subtitles only for %.0fs: %s", npc, TTS_PAUSE, err)
        self.journal.add("tts_off", npc=npc, name=name, seconds=TTS_PAUSE, detail=str(err))

    def play_scene(
        self, lines: list[tuple[str, str, str, str, Mood | None]], *, quiet: bool = False
    ) -> ScenePlay:
        """Two npc's talk near the hero (gossip.py): lines (npc, name, to, text, mood) said in turn, each
        to the npc `to`. The game plays two npc's voices over each other, so a line is sent only once the
        one before has been heard; it goes out voiced whole (the next one is voiced while one plays).
        `quiet`: they lower their voices. Nothing here goes into anyone's memory."""
        scene = [
            SceneLine(f"AIB_{next(self._line_ids)}", npc.upper(), name, to.upper(), said, mood)
            for npc, name, to, text, mood in lines
            if (said := to_game_text(text))
        ]
        play = ScenePlay(self, scene, quiet)
        play.task = asyncio.create_task(self._play(play))
        return play

    def started_at(self, line: str) -> float | None:
        """When the game started a line sent by this server; None if it has not."""
        return self._lines.get(line, ("", None))[1]

    def hush(self, cmd: HushCmd) -> None:
        """Send a hush without waiting for it (a scene is cut synchronously)."""
        self._spawn(self.link.send(cmd))

    async def _scene_voice(self, line: SceneLine, quiet: bool) -> Voice | None:
        """A scene line's whole voice, None to say it without one (no TTS, or it failed)."""
        if self.tts is None or time.monotonic() < self._tts_paused_until:
            return None
        if self._scene_tts is None:
            self._scene_tts = TtsClient(self.tts.url, self.tts.timeout)
        rate = 0
        chunks: list[tuple[bytes, list[tuple[int, float, float]] | None]] = []
        matcher = WordMatcher(line.text)
        pending_words: list[tuple[int, float, float]] = []

        async def words(timings: list[WordTiming]) -> None:
            pending_words.extend(matcher.match(timings))

        # nothing else is awaited here: the client's lock is held while these run
        async def start(r: int, _provider: str) -> None:
            nonlocal rate
            rate = r

        async def chunk(pcm: bytes) -> None:
            chunks.append((pcm, pending_words.copy() or None))
            pending_words.clear()

        mood = MOOD_DELIVERY[line.mood] if line.mood else ""
        style = "; ".join(s for s in (mood, QUIET_STYLE if quiet else "") if s)
        persona = self.personas.get(line.npc, line.name)
        try:
            speech = await self._scene_tts.stream(
                line.text, persona.voice_id, persona.voice, start, chunk, style, on_words=words
            )
            if pending_words:
                await chunk(b"")
        except TtsError as err:
            self._tts_off(line.npc, line.name, err)
            return None
        log.info(
            "%s voice %s: %.2fs of audio in %.2fs (%s)",
            line.npc,
            line.line,
            speech.audio_s,
            speech.latency_s,
            speech.provider,
        )
        return rate, chunks, speech.audio_s

    def _spawn_story_voice(self, msg: LineVoice) -> None:
        task = asyncio.create_task(self.voice_story_line(msg.npc, msg.name or msg.npc, msg.line, msg.text))
        self._story_voices.add(task)
        task.add_done_callback(self._story_voices.discard)

    async def voice_story_line(self, npc: str, name: str, line: str, text: str) -> float | None:
        """A line of the director's (v26) in the npc's voice, streamed as it comes for the line the game has
        queued; in the order they are asked for (one client, one line at a time). Its audio length, None
        without a voice: the line stays a subtitle."""
        if self.tts is None or time.monotonic() < self._tts_paused_until:
            return None
        if self._story_tts is None:
            self._story_tts = TtsClient(self.tts.url, self.tts.timeout)
        matcher = WordMatcher(text)
        pending_words: list[tuple[int, float, float]] = []

        async def words(timings: list[WordTiming]) -> None:
            pending_words.extend(matcher.match(timings))

        async def start(rate: int, _provider: str) -> None:
            await self.link.send(SayCmd(npc=npc, line=line, text=text, stream=True, rate=rate))

        async def chunk(pcm: bytes) -> None:
            await self.link.send(SayAudioCmd(
                npc=npc, line=line, data=base64.b64encode(pcm).decode(), words=pending_words.copy() or None,
            ))
            pending_words.clear()

        persona = self.personas.get(npc, name)
        try:
            speech = await self._story_tts.stream(
                text, persona.voice_id, persona.voice, start, chunk, on_words=words,
            )
            if pending_words:
                await chunk(b"")
        except TtsError as err:
            self._tts_off(npc, name, err)
            return None
        except GameError:
            return None
        await self.link.send(SayEndCmd(npc=npc, line=line, audio_s=round(speech.audio_s, 3)))
        self._audio[line] = speech.audio_s
        log.info(
            "%s director's line %s: %.2fs of audio in %.2fs (%s)",
            npc,
            line,
            speech.audio_s,
            speech.latency_s,
            speech.provider,
        )
        return speech.audio_s

    def story_voiced(self, line: str) -> float | None:
        """How long a director's line voiced here is, None if it was not."""
        return self._audio.get(line)

    async def _send_scene_line(self, line: SceneLine, voice: Voice | None) -> None:
        """Say, all of its audio and its end at once: the game drains them in one tick, so the line starts
        with its whole voice and its length known (never a WAV `audio`: a hush stops only streamed voices)."""
        if voice is None:
            await self.link.send(
                SayCmd(npc=line.npc, line=line.line, text=line.text, mood=line.mood, to=line.to)
            )
            return
        rate, chunks, audio_s = voice
        line.audio_s = audio_s
        await self.link.send(
            SayCmd(
                npc=line.npc,
                line=line.line,
                text=line.text,
                stream=True,
                rate=rate,
                mood=line.mood,
                to=line.to,
            )
        )
        for pcm, words in chunks:
            await self.link.send(
                SayAudioCmd(npc=line.npc, line=line.line, data=base64.b64encode(pcm).decode(), words=words)
            )
        await self.link.send(SayEndCmd(npc=line.npc, line=line.line, audio_s=round(audio_s, 3)))
