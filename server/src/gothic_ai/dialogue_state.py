"""Conversation, scene and turn state shared by dialogue methods."""

from __future__ import annotations

import asyncio
import logging
import re
from collections import deque
from collections.abc import Callable, Collection, Iterator
from dataclasses import dataclass, field
from typing import TYPE_CHECKING, Protocol

from .dialogue_text import (
    _SENTENCE_END,
    CueTag,
    FeltTag,
    MoodTag,
    MoodTags,
    SentenceSplitter,
    SpeechPiece,
    strip_said,
    to_game_text,
    tool_leak,
)
from .executor import Intent
from .llm import Message, Tool, ToolCall
from .memory import NpcMemory
from .persona import Persona
from .protocol import HushCmd, Mood, SayCue, SayLeak, Snapshot
from .turn import ToolOutcome

if TYPE_CHECKING:
    from .dialogue import DialogueManager
    from .perception import Sight


log = logging.getLogger("gothic_ai.dialogue")


@dataclass
class Conversation:
    npc: str
    persona: Persona
    branch: str  # memories of this conversation belong to this line of play
    memory: NpcMemory = field(default_factory=NpcMemory)
    reply: asyncio.Task[None] | None = None
    last_lines: list[str] = field(default_factory=list)  # line ids of the latest reply
    # lines of earlier replies still to be heard when the latest began (its next turn on the floor while
    # the voice of the one before is made): others wait for them too
    earlier_lines: list[str] = field(default_factory=list)
    actions: set[asyncio.Task[None]] = field(default_factory=set)
    new_meeting: bool = True  # the next reply starts a new meeting (noted in memory)
    compacting: asyncio.Task[None] | None = None
    # lines waiting for their audio, sent to the game strictly in order (with tts)
    voice: asyncio.Queue[QueuedLine] | None = None
    voicing: asyncio.Task[None] | None = None
    intent: Intent = field(default_factory=Intent)  # what the npc wants now (set by the model)
    pending: list[str] = field(default_factory=list)  # events that came while a turn was running
    calm: Intent | None = None  # the intent before an incident goal replaced it
    overlay_free_at: float = 0.0  # lines said on the move: when the last one is over
    overlay_reset: asyncio.Event = field(default_factory=asyncio.Event)
    reply_id: str = ""  # the latest turn's reply (its subtitle entry)
    hushed: set[str] = field(default_factory=set)  # lines cut off before they started: never sent
    cut: str | None = None  # the hero interrupted the running turn: what the npc had said of it
    trigger: Message | None = None  # what started the running turn (remembered when it ends)
    acting: bool = False  # the running turn's line is said (the quick model's): it only acts on it now
    talked_at: float = float("-inf")  # the hero last spoke to it (the quick model is kept warm for a while)
    first_words: str = ""  # the last it said at once to the hero's phrase: not the same twice in a row
    first_sounds: deque[bool] = field(default_factory=lambda: deque(maxlen=4))
    said_at: float = float("-inf")
    svm_phrases: deque[str] = field(default_factory=lambda: deque(maxlen=5))
    # queued or synthesizing, before its first sound: line -> finish its SVM trace
    svm_pending: dict[str, Callable[[str], None]] = field(default_factory=dict)
    bark_pending: dict[str, Callable[[str], None]] = field(default_factory=dict)

    @property
    def history(self) -> list[Message]:
        msgs = []
        if self.memory.summary:
            msgs.append(
                Message("developer", f"[Что ты помнишь о прежних встречах с героем: {self.memory.summary}]")
            )
        msgs.extend(e.message() for e in self.memory.entries)
        return msgs

    def tasks(self) -> list[asyncio.Task[None]]:
        return [t for t in (self.reply, self.compacting, self.voicing, *self.actions) if t is not None]


# A line waiting for its voice, including the face metadata already decided for its id.
QueuedLine = tuple[
    str, str, str | None, bool, Mood | None, bool, str | None, list[SayCue] | None, SayLeak | None
]


class Floor(Protocol):
    """One voice at a time in the conversation around the hero (group.py, step B2 of docs/npc-tools.md).
    A turn takes its seat before it thinks and gives it up when it is over; each line waits to be heard
    until those before it have been."""

    async def queue(self, npc: str, kind: str) -> bool:
        """Before a turn: wait for those before it to have their text ready; False: no voice for it."""
        ...

    def reserve_after(self, npc: str, after: str) -> None:
        """Reserve a hero turn immediately after the current speaker, in the same round."""

    def done(self, npc: str) -> None:
        """Its turn is over: its text is ready (or it said nothing)."""

    def seat(self, npc: str) -> int | None:
        """The npc's seat now, None: none. A line said now is heard by it, though the npc may have taken
        another by the time the line's voice is ready."""

    async def hear(self, npc: str, seat: int | None) -> None:
        """Before a line of the npc's is heard: those before its seat have been."""

    def called(self, npc: str, line: str) -> str | None:
        """Whom of those around the hero the npc's line calls by name («Браго, …»): its id, None: nobody."""


class TurnExtension(Protocol):
    """More of the world for the conversational model (the director's quests, quests.py; the scripts'
    quests, vanilla.py; what the npc knows, knowledge.py): lines for the situation, tools to offer, and the
    handling of their calls. One may also answer recall: `recall(npc, query) -> str | None`."""

    all_tools: tuple[Tool, ...]  # every tool `tools` may offer: all are in each request, see `_request_tools`

    async def prepare(self, npc: str) -> None:
        """Before a turn: what the situation and the tools need from the game."""
        ...

    def situation(self, npc: str) -> str | None: ...

    def tools(self, npc: str) -> list[Tool]: ...

    async def handle(
        self, dialogue: DialogueManager, conv: Conversation, call: ToolCall, snap: Snapshot | None
    ) -> ToolOutcome | None:
        """The outcome of a call this extension knows, None for other calls."""
        ...


Voice = tuple[int, list[bytes], float]  # a line's whole voice: sample rate, 16-bit mono PCM chunks, seconds


@dataclass
class SceneLine:
    """A line one npc says to another in a scene (play_scene)."""

    line: str  # its id, as the game knows it
    npc: str
    name: str
    to: str  # the npc it is said to
    text: str
    mood: Mood | None
    started: float | None = None  # when the game started it (time.monotonic())
    audio_s: float | None = None  # its voice's length; None: said without a voice


class ScenePlay:
    """Lines of a scene being said in turn (DialogueManager.play_scene). `task` ends when the last one has
    been heard, or early: cut, or a line the game did not start (`why`)."""

    task: asyncio.Task[None]

    def __init__(self, dialogue: DialogueManager, lines: list[SceneLine], quiet: bool) -> None:
        self.lines = lines
        self.quiet = quiet
        self.sent = 0  # lines sent to the game so far
        self.why = ""  # why it ended early: "cut", "not_started" (the game never started a line), "gone"
        self.next_voice: asyncio.Task[Voice | None] | None = None  # made while the line before plays
        self._dialogue = dialogue

    def said(self) -> list[SceneLine]:
        """The lines the game started (heard, at least in part)."""
        for line in self.lines[: self.sent]:
            line.started = line.started or self._dialogue.started_at(line.line)
        return [line for line in self.lines[: self.sent] if line.started is not None]

    def cut(self, *, hush: bool = True) -> list[SceneLine]:
        """Stop at once: lines not sent yet are never heard. `hush`: both voices stop too, the line being
        said keeps its subtitle as said; without it nothing more goes to the game (it is loading). Nothing
        after the scene has ended. The lines said."""
        if self.task.done() or self.why == "cut":
            return self.said()
        self.why = "cut"
        self.task.cancel()
        if self.next_voice is not None:
            self.next_voice.cancel()
        said = self.said()
        if hush and self.sent:
            last = self.lines[self.sent - 1]
            if last.started is not None:
                self._dialogue.hush(HushCmd(npc=last.npc, line=last.line, said=last.text))
            else:  # sent, not started: if it starts later the game cuts it at once
                self._dialogue.hush(HushCmd(npc=last.npc))
            for npc in dict.fromkeys(line.npc for line in self.lines):
                if npc != last.npc:
                    self._dialogue.hush(HushCmd(npc=npc))
        return said


class StaleTurn(Exception):
    """The incident a turn was about is over before the npc said a word."""


@dataclass
class FirstReaction:
    """The behavioral model's first reaction to the hero's phrase (incidents.py)."""

    gesture: Callable[[], None] | None = None  # makes its gesture, if any


@dataclass
class TurnState:
    reply: str  # chat entry id of this turn's text
    splitter: SentenceSplitter
    started: float
    moods: MoodTags = field(default_factory=MoodTags)
    mood: Mood | None = None  # of the lines being said (the model's last tag)
    cues: list[SayCue] = field(default_factory=list)
    felt: tuple[Mood, int] | None = None
    face_text: str = ""  # unsent text, including the last completed line until its trailing tags arrive
    face_tags: list[tuple[int, CueTag | FeltTag]] = field(default_factory=list)
    ready_lines: deque[str] = field(default_factory=deque)
    text: str = ""  # everything said in the turn, all steps
    pending_text: str = ""  # only the last unfinished word, held across stream chunks
    discarded_text: str = ""  # a tool leak, skipped through its sentence's end or the end of the step
    shown: str = ""
    shown_at: float = 0.0
    first_delta: float | None = None
    first_line: float | None = None
    end_requested: bool = False
    given: set[tuple[str, str]] = field(default_factory=set)  # (item, reply) handed over in this turn
    taken: set[str] = field(default_factory=set)  # things picked up off the ground in this turn
    sight: Sight | None = None  # the same ground items shown in this turn's situation
    relevant: Callable[[], bool] | None = None  # an event turn: is it still worth saying?
    recalled: bool = False  # recall is used once per turn
    follow_set: bool = False  # the model already accepted following this turn; snapshots may lag
    filler: str = ""  # said at once (the quick model's "Хм...", "Что?!"): the answer does not repeat it
    overlay: bool = False  # said on the move (an urgent incident): the body does not stop for it
    tools: list[Tool] = field(default_factory=list)  # offered to the model in this turn
    dynamic_tools: bool = False  # no explicit tools: amendments refresh the offered list
    to: str | None = None  # the npc its lines are said to now, None: the hero
    act: bool = False  # its first step must call a tool (event_turn)
    refused: list[str] = field(default_factory=list)  # its rejected calls: remembered after its words

    def spoken(
        self,
        pieces: list[SpeechPiece],
        names: Collection[str],
        npc: str,
        *,
        flush: bool = False,
    ) -> list[SpeechPiece]:
        """Hold only unfinished words; a tool marker discards the unsaid rest of its sentence."""
        out: list[SpeechPiece] = []

        def accept(word: str) -> None:
            if not self.discarded_text:
                # Include this chunk's words, which have not reached the splitter yet.
                pending = self.splitter._buf + "".join(p for p in out if isinstance(p, str))
                start = 0
                for end in _SENTENCE_END.finditer(pending):
                    start = end.end()
                prefix = pending[max(start, pending.rfind("\n") + 1) :]
                if tool_leak(prefix + word, names):
                    # Keep completed sentences waiting in the splitter, and any clause already said.
                    remove = len(prefix)
                    if remove:
                        self.text = self.text[:-remove]
                    for i in range(len(out) - 1, -1, -1):
                        if isinstance(p := out[i], str):
                            cut = min(remove, len(p))
                            out[i] = p[: len(p) - cut]
                            remove -= cut
                    if remove:
                        self.splitter._buf = self.splitter._buf[:-remove]
                        self.face_text = self.face_text[:-remove]
                        self.face_tags = [(at, tag) for at, tag in self.face_tags if at < len(self.face_text)]
                    self.discarded_text = prefix + word
                else:
                    if not self.text or self.text[-1].isspace():
                        word = word.lstrip()  # no double space after a tag or discarded text
                    if word:
                        self.text += word
                        if out and isinstance(out[-1], str):
                            out[-1] += word
                        else:
                            out.append(word)
                    return
            else:
                self.discarded_text += word
            balanced = self.discarded_text.count('"') % 2 == 0 and all(
                self.discarded_text.count(opening) <= self.discarded_text.count(closing)
                for opening, closing in ("()", "{}", "[]")
            )
            if balanced and (
                "\n" in self.discarded_text or _SENTENCE_END.search(self.discarded_text)
            ):
                log.warning("%s: discarded tool text: %s", npc, self.discarded_text.strip())
                self.discarded_text = ""

        def finish_word() -> None:
            if self.pending_text:
                accept(self.pending_text)
                self.pending_text = ""

        for p in pieces:
            if not isinstance(p, str):
                finish_word()
                if isinstance(p, MoodTag) or not self.discarded_text:
                    out.append(p)
                continue
            self.pending_text += p
            end = 0
            for word in re.finditer(r"\S*\s+", self.pending_text):
                accept(word.group())
                end = word.end()
            self.pending_text = self.pending_text[end:]
        if flush:
            finish_word()
            if self.discarded_text:
                log.warning("%s: discarded tool text: %s", npc, self.discarded_text.strip())
                self.discarded_text = ""
        return [p for p in out if p != ""]

    def lines(self, pieces: list[SpeechPiece], *, flush: bool = False) -> Iterator[str]:
        """Split speech into lines, flushing the previous mood before changing it."""
        for p in pieces:
            if isinstance(p, MoodTag):
                if p.mood != self.mood:
                    self.ready_lines.extend(self.splitter.flush())
                    yield from self._ready_lines(final=True)
                    self.mood = p.mood
                continue
            if isinstance(p, (CueTag, FeltTag)):
                self.face_tags.append((len(self.face_text), p))
                continue
            self.face_text += p
            self.ready_lines.extend(self.splitter.feed(p))
            yield from self._ready_lines()
        if flush:
            self.ready_lines.extend(self.splitter.flush())
            yield from self._ready_lines(final=True)

    def _ready_lines(self, *, final: bool = False) -> Iterator[str]:
        while self.ready_lines:
            line = self.ready_lines[0]
            start = self.face_text.index(line)
            end = start + len(line)
            last = len(self.ready_lines) == 1 and not self.face_text[end:].strip()
            if last and not final and not self.pending_text.strip():
                break  # a trailing tag must still be able to anchor to this line's last word
            self.ready_lines.popleft()
            self.cues = []
            self.felt = None
            rest = []
            words = len(to_game_text(line).split())
            for at, tag in self.face_tags:
                if at < end or (last and final):
                    word = min(len(to_game_text(line[: max(0, at - start)]).split()), words - 1)
                    if words and isinstance(tag, CueTag):
                        if len(self.cues) < 2 and all(c.cue != tag.cue for c in self.cues):
                            self.cues.append(SayCue(cue=tag.cue, word=word))
                    elif words and self.felt is None:
                        self.felt = (tag.felt, word)
                else:
                    rest.append((at - end, tag))
            self.face_tags = rest
            self.face_text = self.face_text[end:]
            yield line
        if final:
            self.face_text = ""
            self.face_tags.clear()

    def strip_line(self, line: str) -> str:
        """Remove the first words until a nonempty answer line is ready to be said."""
        if self.filler:
            line = strip_said(line, self.filler)
            if line:
                self.filler = ""
        return line
