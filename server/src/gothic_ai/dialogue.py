"""Free-form conversation with an npc: player text in, npc lines (sentence by sentence) and actions out.

Whom the hero speaks to and when a conversation starts or ends is decided in talk.py (M5e: no chat
window); here are the turns of the conversational model, the npc's memory and its speech, and the lines
two npc's say to each other near the hero, in turn (`play_scene`: what they say is gossip.py's).

The quick model (a local one, `quick`) says what has to be said at once:
- the npc's first words to the hero's phrase, before the conversational model's answer, which goes on
  from them ("Что?! …Значит, Беллиар торопится"); nothing to a greeting;
- in a fight the whole line, and the conversational model only acts on it, its words unsaid: a bandit
  that shouts "Держи его!" attacks;
- a companion's answer to the hero's order ("Есть!", "Стою.", orders.py).
Its three prompt-cache slots hold the instructions and history of nearby npc's the hero looks at,
talks to, or that are in a fight (`prewarm`), so their words only read the situation and the trigger.
"""

from __future__ import annotations

import asyncio
import base64
import itertools
import logging
import time
import uuid
from collections.abc import Awaitable, Callable, Collection, Coroutine
from contextlib import aclosing
from dataclasses import replace
from functools import partial
from typing import TYPE_CHECKING, Any

from . import executor, npc_tools
from .budget import Budget
from .dialogue_body import DialogueBodyMixin
from .dialogue_constants import ACK_HINT as ACK_HINT
from .dialogue_constants import CALM_ATTITUDES as CALM_ATTITUDES
from .dialogue_constants import CHAT_TEXT_EVERY as CHAT_TEXT_EVERY
from .dialogue_constants import CLASSIFIER_SLOT as CLASSIFIER_SLOT
from .dialogue_constants import FIGHTS_HERO as FIGHTS_HERO
from .dialogue_constants import FIRST_REACTION_S as FIRST_REACTION_S
from .dialogue_constants import KEEP_RECENT as KEEP_RECENT
from .dialogue_constants import KILLERS as KILLERS
from .dialogue_constants import LINE_LOST_S as LINE_LOST_S
from .dialogue_constants import LLM_FIRST_S as LLM_FIRST_S
from .dialogue_constants import LLM_IDLE_S as LLM_IDLE_S
from .dialogue_constants import MAX_HISTORY as MAX_HISTORY
from .dialogue_constants import PARALLEL_ACT_HINT as PARALLEL_ACT_HINT
from .dialogue_constants import PREPARE_S as PREPARE_S
from .dialogue_constants import PREWARM_CM as PREWARM_CM
from .dialogue_constants import PREWARM_DELAY_S as PREWARM_DELAY_S
from .dialogue_constants import PREWARM_SLOTS as PREWARM_SLOTS
from .dialogue_constants import QUICK_FIRST_S as QUICK_FIRST_S
from .dialogue_constants import QUICK_HINT as QUICK_HINT
from .dialogue_constants import QUICK_IDLE_S as QUICK_IDLE_S
from .dialogue_constants import QUIET_STYLE as QUIET_STYLE
from .dialogue_constants import REACT_HINT as REACT_HINT
from .dialogue_constants import REACTION_S as REACTION_S
from .dialogue_constants import REACTION_WORDS_MAX as REACTION_WORDS_MAX
from .dialogue_constants import SCENE_GAP_S as SCENE_GAP_S
from .dialogue_constants import SCENE_START_S as SCENE_START_S
from .dialogue_constants import SOUND_SAID as SOUND_SAID
from .dialogue_constants import STEP_RETRIES as STEP_RETRIES
from .dialogue_constants import SUMMARY_FIRST_S as SUMMARY_FIRST_S
from .dialogue_constants import SUMMARY_INSTRUCTIONS as SUMMARY_INSTRUCTIONS
from .dialogue_constants import SURRENDER_CLASSIFIER as SURRENDER_CLASSIFIER
from .dialogue_constants import SVM_EVENTS as SVM_EVENTS
from .dialogue_constants import SVM_HINT as SVM_HINT
from .dialogue_constants import SVM_QUIET_S as SVM_QUIET_S
from .dialogue_constants import SVM_RECOVER_S as SVM_RECOVER_S
from .dialogue_constants import SVM_REPLY_S as SVM_REPLY_S
from .dialogue_constants import SVM_WORDS_MAX as SVM_WORDS_MAX
from .dialogue_constants import TALK_WARM_S as TALK_WARM_S
from .dialogue_constants import TTS_PAUSE as TTS_PAUSE
from .dialogue_quick import DialogueQuickMixin
from .dialogue_state import Conversation as Conversation
from .dialogue_state import FirstReaction as FirstReaction
from .dialogue_state import Floor as Floor
from .dialogue_state import QueuedLine as QueuedLine
from .dialogue_state import SceneLine as SceneLine
from .dialogue_state import ScenePlay as ScenePlay
from .dialogue_state import StaleTurn as StaleTurn
from .dialogue_state import TurnExtension as TurnExtension
from .dialogue_state import TurnState as TurnState
from .dialogue_state import Voice as Voice
from .dialogue_svm import DialogueSvmMixin
from .dialogue_text import _CLAUSE_END as _CLAUSE_END
from .dialogue_text import _FIRST_SENTENCE as _FIRST_SENTENCE
from .dialogue_text import _FOLLOW_ACCEPTED as _FOLLOW_ACCEPTED
from .dialogue_text import _FOLLOW_DELAY as _FOLLOW_DELAY
from .dialogue_text import _FOLLOW_LEAD as _FOLLOW_LEAD
from .dialogue_text import _FOLLOW_LEADING as _FOLLOW_LEADING
from .dialogue_text import _FOLLOW_REFUSAL as _FOLLOW_REFUSAL
from .dialogue_text import _FOLLOW_REPORTED as _FOLLOW_REPORTED
from .dialogue_text import _FOLLOW_SHORT as _FOLLOW_SHORT
from .dialogue_text import _FUNCTION_WORDS as _FUNCTION_WORDS
from .dialogue_text import _INTERJECTION as _INTERJECTION
from .dialogue_text import _LETTERS as _LETTERS
from .dialogue_text import _MARKDOWN as _MARKDOWN
from .dialogue_text import _MOOD_ALIASES as _MOOD_ALIASES
from .dialogue_text import _NOTHING as _NOTHING
from .dialogue_text import _PURE_SOUNDS as _PURE_SOUNDS
from .dialogue_text import _SENTENCE_END as _SENTENCE_END
from .dialogue_text import _SPACES as _SPACES
from .dialogue_text import _TOOL_SYNTAX as _TOOL_SYNTAX
from .dialogue_text import _WORD as _WORD
from .dialogue_text import FIRST_CLAUSE_MIN as FIRST_CLAUSE_MIN
from .dialogue_text import FIRST_WORDS_MAX as FIRST_WORDS_MAX
from .dialogue_text import MOOD_TAG_MAX as MOOD_TAG_MAX
from .dialogue_text import MoodTag as MoodTag
from .dialogue_text import MoodTags as MoodTags
from .dialogue_text import SentenceSplitter as SentenceSplitter
from .dialogue_text import first_words as first_words
from .dialogue_text import first_words_outcome as first_words_outcome
from .dialogue_text import follow_agreement as follow_agreement
from .dialogue_text import game_time as game_time
from .dialogue_text import is_pure_sound as is_pure_sound
from .dialogue_text import line_duration as line_duration
from .dialogue_text import mood_of as mood_of
from .dialogue_text import paragraph_end as paragraph_end
from .dialogue_text import strip_said as strip_said
from .dialogue_text import svm_hint as svm_hint
from .dialogue_text import svm_text as svm_text
from .dialogue_text import to_game_text as to_game_text
from .dialogue_text import tool_leak as tool_leak
from .dialogue_turns import DialogueTurnsMixin
from .dialogue_voice import DialogueVoiceMixin
from .executor import Intent
from .game import GameError, GameLink
from .journal import Journal
from .llm import (
    Done,
    Item,
    LLMBackend,
    LLMError,
    Message,
    Role,
    TextDelta,
    ToolCall,
    ToolChoice,
    with_deadlines,
)
from .llm.base import QUICK_SLOT
from .memory import Entry, MemoryStore
from .persona import MOOD_DELIVERY, PersonaProvider
from .protocol import (
    ChatStatus,
    ChatStatusCmd,
    ChatTextCmd,
    HushCmd,
    MemoryCmd,
    Mood,
    SayAudioCmd,
    SayCmd,
    SayEndCmd,
    Session,
    Snapshot,
)
from .trace import StepTrace, TraceWriter, TurnTrace
from .tts import TtsClient, TtsError

if TYPE_CHECKING:
    from .perception import Sight

log = logging.getLogger(__name__)


class DialogueManager(
    DialogueSvmMixin,
    DialogueQuickMixin,
    DialogueVoiceMixin,
    DialogueTurnsMixin,
    DialogueBodyMixin,
):
    """Turns of the npc's conversational model. One reply in flight per npc; new player text interrupts it.

    Every message is stored in `memory` under the game's current branch (see memory.py), so npc's
    remember earlier conversations of the same save game.
    """

    def __init__(
        self,
        link: GameLink,
        llm: LLMBackend,
        personas: PersonaProvider,
        *,
        memory: MemoryStore | None = None,
        tts: TtsClient | None = None,
        tools: bool = True,
        journal: Journal | None = None,
        trace: TraceWriter | None = None,
        budget: Budget | None = None,
        quick: LLMBackend | None = None,
        svm_lines: bool = True,
    ) -> None:
        self.link = link
        self.journal = journal or Journal()  # lines and actions for the launcher (see journal.py)
        self.trace = trace  # every turn as a JSON line (see trace.py)
        self.budget = budget  # calls nobody asked for (summaries); the player's turns are only counted
        # the npc's intent changed (e.g. for the planner: what it wants from the hero after the chat)
        self.on_intent: Callable[[str, Intent], None] | None = None
        self.on_fight: Callable[[str, str], None] | None = None
        # the hero said something to the npc (npc, words): the gesture its body makes in the pause before
        # the answer (incidents.py asks the behavioral model)
        self.first_reaction: Callable[[str, str], Awaitable[FirstReaction]] | None = None
        # something the npc's body was to do did not work (npc, what)
        self.on_body_failed: Callable[[str, str], None] | None = None
        # whether the npc is under our control (attached): only then do intents move its body directly
        self.controls: Callable[[str], bool] | None = None
        # the intent of an npc we do not control changed (npc, old, new): who runs its body carries it out
        self.on_free_intent: Callable[[str, Intent, Intent], None] | None = None
        # a free npc's body is needed for a commitment (give an item, follow): hold it (attach) for `why`,
        # and let it go again; without them only controlled npc's act on commitments (talk.py)
        self.take_body: Callable[[str, str], Awaitable[bool]] | None = None
        self.free_body: Callable[[str, str], None] | None = None
        # a companion's fight (npc, target): carried out by orders.py, which brings it back to the hero
        # after; False if it is not the companions' business
        self.companion_fight: Callable[[str, str | None], bool] | None = None
        self.companion_fight_reason: Callable[[str, str | None], str | None] | None = None
        # the npc is in such a fight: a goal of going with the hero waits for it to end
        self.fighting: Callable[[str], bool] = lambda npc: False
        # the npc's guild (the roster's, talk.py), None if not known: whom it attacks to kill (`KILLERS`)
        self.guild_of: Callable[[str], int | None] = lambda npc: None
        # A bandit's reason to attack the hero's side (npc, snapshot, target; parley.py).
        self.fight_reason: Callable[[str, Snapshot | None, str | None], str | None] | None = None
        # Parley's balance of forces, read synchronously from snapshots and perception in memory.
        self.balance: Callable[[str], str | None] | None = None
        self.on_player_said: list[Callable[[str], None]] = []
        # the npc fights the hero right now (the scripts' attack, talk.py; companions are not asked): talked
        # to, it may call it off (`_calm`)
        self.fights_hero: Callable[[str], bool] = lambda npc: False
        self.surrender_hold: Callable[[Conversation, list[Item]], Awaitable[bool | None]] | None = None
        self.surrender_context: Callable[[str], str | None] = lambda npc: None
        self.hero_fight_held: Callable[[str], bool] = lambda npc: False
        # an npc called off its fight with the hero (npc): a companion fighting it lets it be (orders.py)
        self.on_calm: list[Callable[[str], None]] = []
        # an npc turned on the hero by its own decision (npc): his companions stand up for him (orders.py)
        self.on_attack: list[Callable[[str], None]] = []
        # the npc ended the conversation (its goodbye has been said)
        self.on_end: Callable[[str], None] | None = None
        # an npc's turn is over and it said something (npc, name, the text said, the turn's kind, the npc it
        # said it to: None for the hero or aloud): whoever stood by heard it (group.py)
        self.on_said: list[Callable[[str, str, str, str, str | None], None]] = []
        # one voice at a time around the hero (group.py): a seat before a turn thinks, before a line is heard
        self.floor: Floor | None = None
        self._line_seats: dict[str, int | None] = {}  # a line -> the seat of the turn that said it
        self.llm_first_s, self.llm_idle_s = LLM_FIRST_S, LLM_IDLE_S
        # the npc is in a fight (incidents.py): its lines come from the quick model, if there is one
        self.in_fight: Callable[[str], bool] = lambda npc: False
        self.quick = quick
        self.svm_lines = svm_lines
        self._svm_available = True
        self._svm_errors = 0
        self._svm_tasks: dict[str, asyncio.Task[str | None]] = {}
        self._svm_probe: asyncio.Task[None] | None = None
        self._spoke_at: dict[str, float] = {}  # includes scenes before a conversation is opened
        self.quick_first_s, self.quick_idle_s = QUICK_FIRST_S, QUICK_IDLE_S
        self._prewarms: dict[str, asyncio.Task[None]] = {}  # npc -> filling the quick model's cache
        self._prewarm_history: dict[str, list[Message]] = {}  # pending or warmed histories
        self._quick_slots: dict[str, int] = {}
        self._quick_used: dict[str, int] = {}
        self._quick_clock = itertools.count()
        self._quick_busy = [0] * (CLASSIFIER_SLOT + 1)
        self._quick_locks = [asyncio.Lock() for _ in range(CLASSIFIER_SLOT + 1)]
        self._classifier_slot: int | None = None  # CLASSIFIER_SLOT once the quick server is known to have it
        self._slot_check: asyncio.Task[None] | None = None
        self._quick_free = asyncio.Event()
        self._surrender_probes = 0
        self._nearby: dict[str, tuple[str, float]] = {}  # human npc -> name, distance to the hero in cm
        self._svm_down: dict[str, bool] = {}  # latest known deaths, roster and snapshot health
        self._focus: str | None = None
        # what the npc sees (perception.py), in place of the snapshot's omniscient lines
        self.sight: Callable[[str, Snapshot | None], Sight | None] | None = None
        self.extensions: list[TurnExtension] = []
        self.llm = llm
        self.personas = personas
        self.memory = memory or MemoryStore(":memory:")
        self.tts = tts
        self._tts_paused_until = 0.0
        self.tools = npc_tools.TOOLS if tools else []
        self.branch: str | None = None
        self.conversations: dict[str, Conversation] = {}
        self._line_ids = itertools.count(1)
        self._reply_ids = itertools.count(1)
        self._lines: dict[str, tuple[str, float | None]] = {}  # line id -> (text, started at)
        self._line_replies: dict[str, str] = {}  # line id -> the reply whose subtitle it belongs to
        self._sent: dict[str, float] = {}  # line id -> when it was sent (a line that never starts is lost)
        self._audio: dict[str, float] = {}  # line id -> how long its voice is, once it is known
        # scenes (play_scene): how long a line lasts without a voice (tests make it short), the lines
        # waiting for the game to start them, and a TTS connection of their own (made on first use): a
        # scene line does not wait behind an answer to the hero, nor the other way round
        self.scene_pace: Callable[[str], float] = line_duration
        self._starts: dict[str, asyncio.Event] = {}
        self._scene_tts: TtsClient | None = None
        self._story_tts: TtsClient | None = None  # the director's lines (v26): not behind talk or scenes
        self._story_voices: set[asyncio.Task[float | None]] = set()
        self._background: set[asyncio.Task[Any]] = set()
        link.listeners.append(self._on_message)
        if getattr(link, "hello", None) is not None:
            self._svm_mode()
        if link.session is not None:
            self._switch_branch(link.session)


    async def _recover_svm(self) -> None:
        assert self.quick is not None
        while not self._svm_available:
            try:
                await asyncio.wait_for(self.link.closed.wait(), SVM_RECOVER_S)
                return
            except TimeoutError:
                pass
            try:
                async with asyncio.timeout(SVM_REPLY_S):
                    warm = getattr(self.quick, "prewarm", None)
                    if warm is not None:
                        await warm(instructions="Ответь одним словом.", messages=[])
                    else:
                        async with aclosing(self.quick.stream(
                            instructions="Ответь одним словом.", messages=[], tools=[],
                        )) as events:
                            async for event in events:
                                if isinstance(event, TextDelta) and event.text.strip():
                                    break
                self._svm_errors = 0
                self._svm_available = True
                self._svm_mode()
            except (LLMError, TimeoutError):
                continue

    # --- memory -------------------------------------------------------------------------

    def _switch_branch(self, session: Session) -> None:
        if session.branch != self.branch:
            # another line of play (new game or a load): conversations are read again from memory
            for conv in self.conversations.values():
                for task in conv.tasks():
                    task.cancel()
            self.conversations.clear()
            for task in self._svm_tasks.values():
                task.cancel()
            self._svm_tasks.clear()
            self._spoke_at.clear()
            for task in self._prewarms.values():
                task.cancel()
            self._prewarms.clear()
            self._prewarm_history.clear()
            self._quick_slots.clear()
            self._quick_used.clear()
            self._nearby.clear()
            self._svm_down.clear()
            self._focus = None
            self.branch = session.branch
        head = self.memory.ensure_branch(session.branch, session.parent, session.parent_seq)
        log.info(
            "memory: branch %s (%s%s), head %d",
            session.branch,
            session.reason,
            f" from {session.parent}@{session.parent_seq}" if session.parent else "",
            head,
        )
        self._send_head(session.branch, head)

    def _current_branch(self) -> str:
        if self.branch is None:
            self.branch = f"unsaved-{uuid.uuid4().hex[:12]}"
            self.memory.ensure_branch(self.branch)
            log.warning("the game did not report a session; memories go to %s", self.branch)
        return self.branch

    def _remember(self, conv: Conversation, role: Role, content: str) -> None:
        seq = self.memory.append(
            conv.branch, conv.npc, role, content, game_time(self.link.snapshots.get(conv.npc))
        )
        conv.memory.entries.append(Entry(seq, role, content))
        self._send_head(conv.branch, seq)
        self.prewarm(conv.npc)

    def _send_head(self, branch: str, seq: int) -> None:
        self._spawn(self.link.send(MemoryCmd(branch=branch, seq=seq)))

    def _spawn(self, coro: Coroutine[Any, Any, Any]) -> None:
        async def run() -> None:
            try:
                await coro
            except GameError:
                pass  # the game is gone; it learns the head again on reconnect

        task = asyncio.create_task(run())
        self._background.add(task)
        task.add_done_callback(self._background.discard)

    def _maybe_compact(self, conv: Conversation) -> None:
        if len(conv.memory.entries) <= MAX_HISTORY or (conv.compacting and not conv.compacting.done()):
            return
        if self.budget is not None and not self.budget.allow(conv.npc):
            return  # later: the next message tries again
        conv.compacting = asyncio.create_task(self._compact(conv))

    async def _compact(self, conv: Conversation) -> None:
        """Fold older messages into the summary (one extra LLM call per ~25 messages)."""
        old = conv.memory.entries[:-KEEP_RECENT]
        speaker = {"user": "Герой", "assistant": conv.persona.name, "developer": ""}
        records = "\n".join(f"{speaker[e.role]}: {e.content}" if speaker[e.role] else e.content for e in old)
        prompt = f"Прежняя сводка: {conv.memory.summary or 'нет'}\n\nНовые записи:\n{records}"
        text = ""
        try:
            events = self.llm.stream(
                instructions=SUMMARY_INSTRUCTIONS.format(name=conv.persona.name),
                messages=[Message("user", prompt)],
            )
            async for event in with_deadlines(
                events, first_s=SUMMARY_FIRST_S, idle_s=SUMMARY_FIRST_S, name=self.llm.name
            ):
                if isinstance(event, TextDelta):
                    text += event.text
        except LLMError as err:
            log.warning("%s: summary failed, will retry later: %s", conv.npc, err)
            return
        text = text.strip()
        if not text:
            return
        covers = old[-1].seq
        seq = self.memory.add_summary(conv.branch, conv.npc, text, covers)
        conv.memory.summary, conv.memory.summary_seq = text, covers
        conv.memory.entries = [e for e in conv.memory.entries if e.seq > covers]
        log.info("%s: %d messages folded into the summary (%d chars)", conv.npc, len(old), len(text))
        self._send_head(conv.branch, seq)
        self.prewarm(conv.npc)

    # --- the quick model of a fight ----------------------------------------------------------

    def _fighting(self, conv: Conversation) -> bool:
        """The npc's lines are the quick model's: there is one, and the npc is in a fight."""
        return self.quick is not None and (
            self.in_fight(conv.npc) or self.fights_hero(conv.npc)
            or conv.intent.goal in executor.INCIDENT_GOALS
        )

    def _listening(self, conv: Conversation) -> bool:
        """The quick model is likely to speak for the npc soon: it is in a fight, or the hero talked to it
        a moment ago, or looks at it within eight meters."""
        return (
            self._fighting(conv)
            or time.monotonic() - conv.talked_at < TALK_WARM_S
            or (conv.npc == self._focus and self._distance(conv.npc) <= PREWARM_CM)
        )

    def _distance(self, npc: str) -> float:
        if npc in self._nearby:
            return self._nearby[npc][1]
        snap = self.link.snapshots.get(npc)
        return snap.player.distance if snap is not None and snap.player is not None else float("inf")


    async def _prewarm(self, conv: Conversation, warm: Callable[..., Awaitable[None]], slot: int) -> None:
        await asyncio.sleep(PREWARM_DELAY_S)
        if ((conv.reply is not None and not conv.reply.done()) or self._quick_busy[slot]
            or self._quick_slots.get(conv.npc) != slot):
            return  # a turn started during the debounce: warming waits for its reply
        started = time.monotonic()
        token = QUICK_SLOT.set(slot)
        try:
            await warm(instructions=conv.persona.instructions, messages=conv.history)
        except LLMError as err:
            self._prewarm_history.pop(conv.npc, None)
            log.info("%s: the quick model was not warmed: %s", conv.npc, err)
            return
        finally:
            QUICK_SLOT.reset(token)
        log.debug("%s: the quick model warmed in %.2fs", conv.npc, time.monotonic() - started)


    async def _quick_words(
        self,
        conv: Conversation,
        request: list[Item],
        *,
        temperature: float | None = None,
        attempt: dict[str, Any] | None = None,
    ) -> tuple[str, Mood | None]:
        """A few words from the quick model, at once: its first sentence, within REACTION_S; ("", None) if
        it has nothing to say, fails or is late."""
        assert self.quick is not None
        self._yield_to(conv.npc)
        text = ""
        started = time.monotonic()
        attempt = attempt if attempt is not None else {}
        attempt["outcome"] = "late"
        options = {} if temperature is None else {"temperature": temperature}
        try:
            async with (
                asyncio.timeout(REACTION_S),
                aclosing(
                    self._quick_stream(
                        conv.npc, instructions=conv.persona.instructions, messages=request,
                        tools=[], **options
                    )
                ) as events,
            ):
                async for event in events:
                    if isinstance(event, TextDelta):
                        text += event.text
                        if _FIRST_SENTENCE.search(text) or paragraph_end(text) is not None:
                            break  # the stream is closed: the server stops writing
        except (LLMError, TimeoutError) as err:
            attempt["outcome"] = "late" if isinstance(err, TimeoutError) else "error"
            log.info("%s: no quick words (%s)", conv.npc, err or "late")
            return "", None
        finally:
            words, mood = first_words(text)
            attempt.update(text=text, words=words, elapsed_ms=round((time.monotonic() - started) * 1000, 1))
        attempt["outcome"] = first_words_outcome(words)
        log.info("%s: quick words in %.2fs: %r", conv.npc, time.monotonic() - started, words or "—")
        return words, mood

    def _reply(self, conv: Conversation, turn: Coroutine[Any, Any, None]) -> asyncio.Task[None]:
        """The npc's turn; once it is over, its history (grown by it) warms the quick model."""
        if svm := self._svm_tasks.get(conv.npc):
            svm.cancel()
        conv.hushed.update(conv.bark_pending)
        for record in conv.bark_pending.values():
            record("busy")
        conv.bark_pending.clear()
        conv.hushed.update(conv.svm_pending)
        for record in conv.svm_pending.values():
            record("busy")
        conv.svm_pending.clear()
        conv.reply = asyncio.create_task(turn)
        conv.reply.add_done_callback(lambda _: self.prewarm(conv.npc))
        return conv.reply

    # --- conversation -------------------------------------------------------------------

    def open(self, npc: str, name: str, *, warm: bool = True) -> Conversation:
        """The npc's conversation, read from memory the first time. `warm`: it is about to talk to the hero,
        the quick model reads its instructions and history now (not for a note: a rumor or a witness would
        warm it for every npc around)."""
        npc = npc.upper()
        conv = self.conversations.get(npc)
        if conv is None:
            branch = self._current_branch()
            conv = Conversation(npc, self.personas.get(npc, name), branch, self.memory.load(branch, npc))
            if conv.memory.intent:
                conv.intent = Intent.from_json(conv.memory.intent)
            self.conversations[npc] = conv
            self.memory.queue_embeddings(branch, npc)
            if conv.memory.entries or conv.memory.summary:
                log.info(
                    "%s remembers %d message(s)%s",
                    npc,
                    len(conv.memory.entries),
                    " and a summary" if conv.memory.summary else "",
                )
        if warm:
            conv.talked_at = time.monotonic()
            self.prewarm(npc)
        return conv

    async def situation(self, npc: str, name: str) -> str:
        """Prepare the same situation as a full turn, without asking a model to respond."""
        text, _, _ = await self._situation(self.open(npc, name, warm=False), None)
        return text

    def say_as_player(
        self, npc: str, text: str, *, overlay: bool = False, body: str | None = None
    ) -> asyncio.Task[None]:
        """The hero says `text` to the npc (talk.py, console, scenarios). `overlay`: the npc answers on the
        move (it walks or works); `body`: what its body does meanwhile, for the situation."""
        conv = self.conversations.get(npc.upper())
        if conv is None:
            raise KeyError(f"no conversation with {npc}")
        # square brackets are remarks about what really happens: the hero's words can't pass for one
        text = text.replace("[", "(").replace("]", ")")
        return self._start_reply(conv, text, overlay=overlay, body=body)

    def _cancel(self, conv: Conversation) -> None:
        if conv.reply is not None and not conv.reply.done():
            conv.reply.cancel()
        conv.reply = None

    def _drop_unvoiced(self, conv: Conversation, lines: Collection[str] | None = None) -> None:
        for line in tuple(conv.bark_pending):
            if lines is None or line in lines:
                conv.hushed.add(line)
                conv.bark_pending.pop(line)("busy")
        for line in tuple(conv.svm_pending):
            if lines is None or line in lines:
                conv.hushed.add(line)
                conv.svm_pending.pop(line)("dropped")
        if conv.voice is not None:
            for _ in range(conv.voice.qsize()):
                item = conv.voice.get_nowait()
                if lines is not None and item[0] not in lines:
                    conv.voice.put_nowait(item)
                conv.voice.task_done()

    def _start_reply(
        self, conv: Conversation, user_text: str, *, overlay: bool = False, body: str | None = None
    ) -> asyncio.Task[None]:
        for listener in self.on_player_said:
            listener(conv.npc)
        self._cancel(conv)
        self._drop_unvoiced(conv)
        if self.budget is not None:
            self.budget.note(conv.npc)
        conv.talked_at = time.monotonic()
        return self._reply(
            conv, self._turn(conv, Message("user", user_text), "player", overlay=overlay, body=body)
        )

    def _said(self, conv: Conversation) -> tuple[list[str], bool, bool]:
        """The latest reply: the texts of its lines that started, whether one is being said now and
        whether some were not said yet."""
        now = time.monotonic()
        said, speaking, unsaid = [], False, False
        for line in conv.last_lines:
            text, at = self._lines.get(line, ("", None))
            if line in conv.hushed:
                continue
            if at is None:
                # still to come, unless it never will: the game does not start a line of an npc far from
                # the hero, or one the bridge refused
                unsaid = unsaid or now - self._sent.get(line, now) < LINE_LOST_S
                continue
            said.append(text)
            speaking = speaking or at + self._audio.get(line, line_duration(text)) > now
        return said, speaking, unsaid

    def speaking(self, npc: str) -> bool:
        """The npc is answering: thinking of it, saying it or about to (not acting on a line it has said)."""
        conv = self.conversations.get(npc.upper())
        if conv is None:
            return False
        thinking = conv.reply is not None and not conv.reply.done() and not conv.acting
        return thinking or self.audible(npc)

    def audible(self, npc: str, seat: int | None = None) -> bool:
        """The npc's lines are being heard, or ones it has said are still to come (not its thinking); with
        `seat`, those said in that seat of the floor's (not those of its next turn, waiting for others)."""
        conv = self.conversations.get(npc.upper())
        if conv is None:
            return False
        now = time.monotonic()
        return any(
            self._to_hear(conv, line, now)
            for line in (*conv.earlier_lines, *conv.last_lines)
            if seat is None or self._line_seats.get(line) == seat
        )

    def _to_hear(self, conv: Conversation, line: str, now: float) -> bool:
        """The line is being heard, or is still to come."""
        text, at = self._lines.get(line, ("", None))
        if line in conv.hushed:
            return False
        if at is None:
            return now - self._sent.get(line, now) < LINE_LOST_S
        return at + self._audio.get(line, line_duration(text)) > now

    def interrupt(self, npc: str) -> bool:
        """The hero speaks while the npc answers: it stops at once (its voice, the lines it did not say
        yet, the turn if it is still thinking) and remembers being interrupted. False if it was silent."""
        conv = self.conversations.get(npc.upper())
        if conv is None or not (self.speaking(npc) or conv.svm_pending):
            return False
        running = conv.reply is not None and not conv.reply.done()
        replying = running or any(
            self._line_replies.get(line) != line and self._to_hear(conv, line, time.monotonic())
            for line in (*conv.earlier_lines, *conv.last_lines)
        )
        said, _, _ = self._said(conv) if replying else ([], False, False)
        spoken = " ".join(said)
        conv.hushed.update(
            line
            for line in (*conv.earlier_lines, *conv.last_lines)
            if self._lines.get(line, ("", None))[1] is None
        )
        conv.earlier_lines = []
        if running:
            conv.cut = spoken  # the turn remembers only this (see _turn)
        self._cancel(conv)
        self._drop_unvoiced(conv)
        self._reset_overlay(conv)
        log.info("%s interrupted after %d line(s)", conv.npc, len(said))
        self._spawn(self.link.send(HushCmd(
            npc=conv.npc, reply=(conv.reply_id or None) if replying else None, said=spoken or None,
        )))
        if not running and replying:
            self._note(conv, f"Герой перебил тебя{f'; ты успел сказать: «{spoken}»' if spoken else ''}")
        return True

    def _reset_overlay(self, conv: Conversation) -> None:
        conv.overlay_free_at = time.monotonic()
        conv.overlay_reset.set()

    async def _wait_overlay(self, conv: Conversation) -> None:
        while (delay := conv.overlay_free_at - time.monotonic()) > 0:
            conv.overlay_reset.clear()
            try:
                await asyncio.wait_for(conv.overlay_reset.wait(), delay)
            except TimeoutError:
                pass

    async def _hush_earlier(self, conv: Conversation, reply: str | None) -> None:
        """An overlay cuts off unheard earlier turns just before its first sound reaches the game."""
        now = time.monotonic()
        lines = [
            line
            for line in conv.earlier_lines
            if self._line_replies.get(line) != reply and self._to_hear(conv, line, now)
        ]
        if not lines:
            return
        replies = dict.fromkeys(self._line_replies[line] for line in lines)
        conv.hushed.update(line for line in lines if self._lines[line][1] is None)
        self._drop_unvoiced(conv, lines)
        conv.earlier_lines = [
            line for line in conv.earlier_lines if self._line_replies.get(line) not in replies
        ]
        self._reset_overlay(conv)
        for earlier in replies:
            said = " ".join(
                self._lines[line][0]
                for line, owner in self._line_replies.items()
                if owner == earlier and self._lines[line][1] is not None
            )
            await self.link.send(HushCmd(npc=conv.npc, reply=earlier, said=said or None))
            self._note(conv, f"Ты оборвал себя на полуслове{f'; успел сказать: «{said}»' if said else ''}")

    def remember(self, npc: str, name: str, text: str) -> None:
        """A remark in the memory of any npc, also one nobody talks to (a rumor, something it saw)."""
        self._note(self.open(npc, name, warm=False), text)

    def change_intent(self, npc: str, name: str, *, body: bool = True, **fields: Any) -> None:
        """Change what the npc wants or how it sees the hero, from outside a turn (a lie found out).
        `body` False: the body has done its part already (an order, orders.py)."""
        conv = self.open(npc, name)
        self._change_intent(
            conv, replace(conv.intent, **fields), self.link.snapshots.get(conv.npc), body=body
        )

    def after_speech(
        self,
        conv: Conversation,
        job: Callable[[], Awaitable[tuple[str, bool]]],
        *,
        then: str | None = None,
        kind: str = "incident",
    ) -> None:
        """For extensions: a body job once the npc has said its lines; its note goes into the history. `then`:
        once it is done, a turn of its own with this cue (it came back from its chest: it goes on talking)."""
        self._after_speech(conv, job, then=then, kind=kind)

    async def give(self, conv: Conversation, call: ToolCall, snap: Snapshot | None) -> tuple[str, bool]:
        """For extensions: hand an item over (give_item's checks are the caller's)."""
        return await self._give(conv, call, snap)

    async def borrow_body(self, conv: Conversation, why: str) -> bool:
        """For extensions: the npc's body for a job of theirs (going somewhere); give it back with
        return_body."""
        return await self._hold(conv, why)

    def return_body(self, conv: Conversation, why: str) -> None:
        self._free(conv, why)

    def overheard(self, npc: str, name: str, text: str, to_name: str | None) -> None:
        """A witness: the npc heard the hero say something to someone else (no model call, only memory)."""
        conv = self.open(npc, name, warm=False)
        whom = f" собеседнику ({to_name})" if to_name else " вслух"
        self._note(conv, f"Ты слышал, как герой сказал{whom}: «{text}»")

    def told_all(self, npc: str, name: str, text: str, first: str) -> None:
        """The hero said something to several, the npc among them; `first` answers first (talk.py)."""
        conv = self.open(npc, name, warm=False)
        for listener in self.on_player_said:
            listener(conv.npc)
        self._note(conv, f"Герой сказал всем вам, и тебе тоже: «{text}» (первым отвечает {first})")

    def heard(self, npc: str, name: str, speaker: str, text: str, whom: str) -> None:
        """A witness: the npc heard another npc (`speaker`, its name) say something, `whom`: "герою",
        "вслух". No model call, only memory."""
        self._note(self.open(npc, name, warm=False), f"Ты слышал, как {speaker} говорит {whom}: «{text}»")

    def overheard_talk(
        self, npc: str, name: str, names: tuple[str, str], said: list[tuple[str, str]]
    ) -> None:
        """A witness: the npc heard two others talk about the hero (a scene, gossip.py): `said` is
        (speaker's name, line). No model call, only memory."""
        if not said:
            return
        lines = " — ".join(f"{who}: «{text}»" for who, text in said)
        self._note(
            self.open(npc, name, warm=False),
            f"Ты слышал, как {names[0]} и {names[1]} говорили о герое: {lines}",
        )

    def event_turn(
        self,
        npc: str,
        cue: str,
        relevant: Callable[[], bool] | None = None,
        *,
        overlay: bool = False,
        tools: Collection[str] | None = None,
        kind: str = "incident",
        to: str | None = None,
        act: bool = False,
    ) -> asyncio.Task[None] | None:
        """A turn about something that happened (an incident): the npc may speak and change its intent.
        While a turn is running the event waits for the next one's situation instead of interrupting.
        `tools` limits what the model is offered (an npc nobody controls cannot hand over items); the names
        may be of the extensions' tools too (deals.py: an offer declined, the npc may offer again). `kind`
        names the trigger in the turn's trace (an npc that comes up to the hero: "initiative"); `to`: the
        npc its lines are said to (another spoke to it, group.py), unless one of them calls someone else.
        `act`: its first step must call one of `tools` (a witness's answer: catch_lie, or set_intent if it
        believes him), so words alone do not stand for a decision of code's."""
        conv = self.conversations.get(npc.upper())
        if conv is None:
            return None
        if conv.reply is not None and not conv.reply.done():
            conv.pending.append(cue)
            return None
        offered = self._offered(npc)
        if tools is None:
            offered = [t for t in offered if t in self.tools]  # its own, not the extensions'
        else:
            offered = [t for t in offered if t.name in tools]
        turn = self._turn(
            conv, Message("developer", cue), kind, relevant, overlay=overlay, tools=offered, to=to, act=act
        )
        return self._reply(conv, turn)

    def note(self, npc: str, text: str) -> None:
        """A remark in the npc's history (what happened to it)."""
        if conv := self.conversations.get(npc.upper()):
            self._note(conv, text)

    def has_spoken(self, npc: str) -> bool:
        """The npc's current turn has sent a line already."""
        conv = self.conversations.get(npc.upper())
        return conv is not None and bool(conv.last_lines)

    def body_busy(self, npc: str) -> bool:
        """A commitment is being carried out (a gesture now would cancel it)."""
        conv = self.conversations.get(npc.upper())
        return self.link.body_owned(npc) or (conv is not None and bool(conv.actions))

    async def _first_reaction(
        self, conv: Conversation, phrase: str, request: list[Item] | None, trace: TurnTrace | None = None
    ) -> str:
        """The npc's first reaction to the hero's phrase, before its answer: with `request`, the quick
        model's first words ("Хм...", "Что?!", "Тьфу, ну и наглец!"), and the behavioral model's gesture
        (within FIRST_REACTION_S). The words go first: the game's queue would hold them until the gesture
        is over. What was said, "" for nothing."""
        gesture: asyncio.Task[FirstReaction] | None = None
        if self.first_reaction is not None:
            gesture = asyncio.create_task(
                asyncio.wait_for(self.first_reaction(conv.npc, phrase), FIRST_REACTION_S)
            )
        said = ""
        sound_said = False
        attempt: dict[str, Any] = {}
        try:
            if request is not None:
                words, mood = await self._quick_words(conv, request, temperature=0.6, attempt=attempt)
                outcome = attempt["outcome"]
                if outcome not in {"late", "error"}:
                    outcome = first_words_outcome(words, conv.first_words)
                if outcome == "too_long":
                    log.info("%s: first words too long, not said: %r", conv.npc, words)
                elif outcome == "same":
                    log.info("%s: the same first words again, not said: %r", conv.npc, words)
                elif outcome == "said" and is_pure_sound(words) and any(conv.first_sounds):
                    outcome = "sound"
                    log.info("%s: first words pure sound too soon, not said: %r", conv.npc, words)
                attempt["outcome"] = "late" if outcome == "said" else outcome
                if outcome == "said":
                    await self._say(conv, words, None, mood=mood, filler=True)  # it goes on thinking
                    said = conv.first_words = words
                    sound_said = is_pure_sound(words)
                    attempt["outcome"] = "said"
            reaction = await gesture if gesture is not None else FirstReaction()
        except TimeoutError:
            log.info("%s: no gesture within %.2fs", conv.npc, FIRST_REACTION_S)
            return said
        except asyncio.CancelledError:
            if gesture is not None:
                gesture.cancel()
            raise
        finally:
            conv.first_sounds.append(sound_said)
            if trace is not None and attempt:
                situation = next(
                    item.content for item in reversed(request or [])
                    if isinstance(item, Message) and item.role == "developer"
                ).removesuffix("\n" + REACT_HINT)
                trace.event("first_words", **attempt, situation=situation, hint=REACT_HINT)
        if reaction.gesture is not None:
            reaction.gesture()
        return said

    async def acknowledge(self, npc: str, name: str) -> None:
        """The npc's short answer to the hero's order (orders.py, the order is in its history already):
        "Есть!", "Стою.", from the quick model, on the move (queued, it would wait while the npc walks and
        hold up the order's actions). Nothing without the quick model."""
        if self.quick is None:
            return
        conv = self.open(npc, name)
        request: list[Item] = [*conv.history, Message("developer", f"{self._brief(conv)}\n{ACK_HINT}")]
        words, mood = await self._quick_words(conv, request)
        if words:
            await self._say(conv, words, None, overlay=True, mood=mood)

    async def _status(self, conv: Conversation, status: ChatStatus, detail: str | None = None) -> None:
        await self.link.send(ChatStatusCmd(npc=conv.npc, status=status, detail=detail))

    async def _say(
        self,
        conv: Conversation,
        text: str,
        reply: str | None,
        overlay: bool = False,
        mood: Mood | None = None,
        *,
        filler: bool = False,
        to: str | None = None,
        bark: Callable[[str], None] | None = None,
    ) -> str | None:
        """A line of the npc's, voiced if there is TTS. `filler`: its first words before the answer (the
        npc goes on thinking after them, protocol v14); `reply` None: not a line of a reply's text; `to`:
        the npc it is said to (v15: the two look at each other), None: the hero."""
        text = to_game_text(text)
        if not text:
            return
        conv.said_at = time.monotonic()
        line = f"AIB_{next(self._line_ids)}"
        if reply is not None:  # first words and answers to orders are not the reply's: no gesture is too late
            conv.last_lines.append(line)
            self._line_replies[line] = reply
        self._lines[line] = (text, None)
        self._sent[line] = time.monotonic()
        if bark is not None:
            conv.bark_pending[line] = bark
        if self.floor is not None:
            self._line_seats[line] = self.floor.seat(conv.npc)
        log.info("%s: %s", conv.persona.name, text)
        if self.tts is None:
            await self._wait_floor(conv, line)
            await self._wait_overlay(conv)
            if line in conv.hushed:
                return  # the hero spoke while it waited to be heard
            if overlay:  # the game does not order lines said on the move
                await self._hush_earlier(conv, reply)
                if line in conv.hushed:
                    return
                conv.overlay_free_at = time.monotonic() + line_duration(text)
            await self.link.send(
                SayCmd(
                    npc=conv.npc,
                    line=line,
                    text=text,
                    reply=reply,
                    overlay=overlay or None,
                    mood=mood,
                    filler=filler or None,
                    to=to,
                )
            )
            self._bark_started(conv, line)
            return line
        if conv.voice is None:
            conv.voice = asyncio.Queue()
            conv.voicing = asyncio.create_task(self._voice(conv, conv.voice))

            def finished(_done: asyncio.Task[None]) -> None:
                while conv.bark_pending:
                    _, record = conv.bark_pending.popitem()
                    record("busy")
                while conv.svm_pending:
                    _, record = conv.svm_pending.popitem()
                    record("busy")

            conv.voicing.add_done_callback(finished)
        conv.voice.put_nowait((line, text, reply, overlay, mood, filler, to))
        return line

    async def _voice(self, conv: Conversation, queue: asyncio.Queue[QueuedLine]) -> None:
        """Voices the lines one by one and streams them to the game: `say` goes out with the first
        audio, chunks follow as they are generated, `say_end` gives the length. A line that gets no
        audio still goes out, silent. The game does not order overlays: every line waits for the
        preceding overlay, and an overlay cuts off unheard earlier turns before its first sound."""
        assert self.tts is not None
        while True:
            line, text, reply, overlay, mood, filler, to = await queue.get()
            started = False
            spoken_at = 0.0
            await self._wait_floor(conv, line)
            await self._wait_overlay(conv)
            try:
                if line in conv.hushed:
                    continue  # the hero interrupted the npc before this line
                if time.monotonic() >= self._tts_paused_until:

                    async def start(
                        rate: int,
                        _provider: str,
                        line: str = line,
                        text: str = text,
                        reply: str | None = reply,
                        overlay: bool = overlay,
                        mood: Mood | None = mood,
                        filler: bool = filler,
                        to: str | None = to,
                    ) -> None:
                        nonlocal started, spoken_at
                        if line in conv.hushed:
                            return  # interrupted while its voice was being made
                        if overlay:
                            await self._hush_earlier(conv, reply)
                            if line in conv.hushed:
                                return
                        started = True
                        spoken_at = time.monotonic()
                        conv.said_at = spoken_at
                        await self.link.send(
                            SayCmd(
                                npc=conv.npc,
                                line=line,
                                text=text,
                                stream=True,
                                rate=rate,
                                reply=reply,
                                overlay=overlay or None,
                                mood=mood,
                                filler=filler or None,
                                to=to,
                            )
                        )
                        self._svm_started(conv, line)
                        self._bark_started(conv, line)

                    async def chunk(pcm: bytes, line: str = line) -> None:
                        if line in conv.hushed:
                            return
                        await self.link.send(
                            SayAudioCmd(npc=conv.npc, line=line, data=base64.b64encode(pcm).decode())
                        )

                    try:
                        speech = await self.tts.stream(
                            text,
                            conv.persona.voice_id,
                            conv.persona.voice,
                            start,
                            chunk,
                            MOOD_DELIVERY[mood] if mood else "",
                        )
                        self._audio[line] = speech.audio_s
                        await self.link.send(
                            SayEndCmd(npc=conv.npc, line=line, audio_s=round(speech.audio_s, 3))
                        )
                        # An interruption while TTS streamed reset the clock: do not restore it.
                        if overlay and conv.overlay_free_at <= spoken_at:
                            conv.overlay_free_at = spoken_at + speech.audio_s + 0.25
                        log.info(
                            "%s voice %s: first audio after %.2fs, %.2fs of audio in %.2fs (%s)",
                            conv.npc,
                            line,
                            speech.first_chunk_s,
                            speech.audio_s,
                            speech.latency_s,
                            speech.provider,
                        )
                    except TtsError as err:
                        self._tts_off(conv.npc, conv.persona.name, err)
                        if started:
                            await self.link.send(SayEndCmd(npc=conv.npc, line=line, error=str(err)))
                if not started and line not in conv.hushed:
                    if overlay:
                        await self._hush_earlier(conv, reply)
                        if line in conv.hushed:
                            continue
                        conv.overlay_free_at = time.monotonic() + line_duration(text)
                    await self.link.send(
                        SayCmd(
                            npc=conv.npc,
                            line=line,
                            text=text,
                            reply=reply,
                            overlay=overlay or None,
                            mood=mood,
                            filler=filler or None,
                            to=to,
                        )
                    )
                    self._svm_started(conv, line)
                    self._bark_started(conv, line)
            except GameError:
                pass
            finally:
                if record := conv.bark_pending.pop(line, None):
                    record("busy" if line in conv.hushed else "error")
                if record := conv.svm_pending.pop(line, None):
                    record("busy" if line in conv.hushed else "error")
                queue.task_done()


    # --- scenes: two npc's talk to each other -------------------------------------------


    async def _play(self, play: ScenePlay) -> None:
        lines = play.lines
        try:
            if lines:
                play.next_voice = asyncio.create_task(self._scene_voice(lines[0], play.quiet))
            free_at = 0.0
            for i, line in enumerate(lines):
                assert play.next_voice is not None
                voice = await play.next_voice
                play.next_voice = (
                    asyncio.create_task(self._scene_voice(lines[i + 1], play.quiet))
                    if i + 1 < len(lines)
                    else None
                )
                await asyncio.sleep(max(0.0, free_at - time.monotonic()))
                started = self._starts[line.line] = asyncio.Event()
                self._lines[line.line] = (line.text, None)
                self._sent[line.line] = time.monotonic()
                play.sent = i + 1
                log.info("%s -> %s: %s", line.name, line.to, line.text)
                await self._send_scene_line(line, voice)
                try:
                    await asyncio.wait_for(started.wait(), SCENE_START_S)
                except TimeoutError:
                    # the game dropped it (the npc cannot speak) or holds it: the scene is over, and the
                    # line is cut if it starts later
                    log.info(
                        "%s: line %s not started in %.0fs, the scene ends", line.npc, line.line, SCENE_START_S
                    )
                    play.why = "not_started"
                    await self.link.send(HushCmd(npc=line.npc))
                    return
                line.started = self.started_at(line.line) or time.monotonic()
                free_at = line.started + (line.audio_s or self.scene_pace(line.text)) + SCENE_GAP_S
            await asyncio.sleep(max(0.0, free_at - time.monotonic()))
        except GameError:
            play.why = "gone"
        finally:
            if play.next_voice is not None:
                play.next_voice.cancel()
            for line in lines:
                self._starts.pop(line.line, None)


    def _commit(
        self, conv: Conversation, trigger: Message, kind: str, state: TurnState, said: str | None = None
    ) -> None:
        """Remember the turn: its trigger, its words (`said`: only those the hero heard before he cut in),
        then what the code refused it."""
        # one synchronous step: an interrupted reply and its replacement cannot interleave here.
        # What started the turn is remembered even when the npc said nothing (it may have acted).
        self._remember(conv, trigger.role, trigger.content)
        if text := (state.text if said is None else said).strip():
            self._remember(conv, "assistant", text)
            for listener in self.on_said:
                listener(conv.npc, conv.persona.name, to_game_text(text), kind, state.to)
        for note in state.refused:  # the next turn knows the code said no, and why: not the same call again
            self._note(conv, note)
        self._maybe_compact(conv)

    def _note(self, conv: Conversation, note: str) -> None:
        if note:
            log.info("%s note: %s", conv.npc, note)
            self._remember(conv, "developer", f"[{note}]")

    def _after_speech(
        self,
        conv: Conversation,
        job: Callable[[], Awaitable[tuple[str, bool]]],
        *,
        then: str | None = None,
        kind: str = "incident",
    ) -> None:
        """Run a body job once the npc has said its queued lines; the note it returns goes into the
        history after the current turn. A failure is reported (`on_body_failed`); a success with `then`
        starts a turn with that cue."""
        reply = conv.reply

        async def run() -> None:
            if conv.voice is not None:
                await conv.voice.join()  # the npc says what it said before acting on it
            note, ok = await job()
            if reply is not None and not reply.done() and reply is not asyncio.current_task():
                await asyncio.wait({reply})  # the note goes after this turn's reply in the history
            self._note(conv, note)
            if not ok and self.on_body_failed is not None:
                self.on_body_failed(conv.npc, note)
            if ok and then is not None:  # a turn as one with the hero: the extensions' tools too (a deal)
                self.event_turn(conv.npc, then, kind=kind, tools=[t.name for t in self._offered(conv.npc)])

        task = asyncio.create_task(run())
        conv.actions.add(task)
        task.add_done_callback(conv.actions.discard)

    def _journal_call(self, conv: Conversation, name: str, arguments: dict[str, Any]) -> None:
        log.info("%s calls %s %s", conv.npc, name, arguments)
        self.journal.add("action", npc=conv.npc, name=conv.persona.name, action=name, arguments=arguments)

    async def _end_after_speech(self, conv: Conversation, lines: list[str]) -> None:
        """The conversation ends once the npc has said its goodbye."""
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            started = [self._lines.get(line, ("", None)) for line in lines]
            if all(at is not None for _, at in started):
                last_text, last_at = started[-1] if started else ("", time.monotonic())
                await asyncio.sleep(max(0.0, (last_at or 0) + line_duration(last_text) - time.monotonic()))
                break
            await asyncio.sleep(0.2)
        self._note(conv, "Ты закончил разговор")
        if self.on_end is not None:
            self.on_end(conv.npc)


    async def _step(
        self,
        conv: Conversation,
        state: TurnState,
        request: list[Item],
        choice: ToolChoice | None,
        st: StepTrace,
        *,
        quick: bool = False,
        speak: bool = True,
    ) -> Done:
        """One model call. Its text continues the turn's reply in the chat as words complete; voice and
        animation follow line by line. What is left of the last sentence is said at the end. `quick`:
        the quick model's line, without tools, up to its first blank line (after it the model may go on
        with a tool call written out, seen in the history, or loop); `speak` False: the text is not said
        (the quick model's was)."""
        llm = self.quick if quick else self.llm
        assert llm is not None
        first_s, idle_s = (
            (self.quick_first_s, self.quick_idle_s) if quick else (self.llm_first_s, self.llm_idle_s)
        )
        started = time.monotonic()
        first: float | None = None
        done: Done | None = None
        written = ""  # the quick model's line so far
        if speak and state.text and not state.text.endswith((" ", "\n")):
            state.text += " "
        try:
            try:
                stream = partial(self._quick_stream, conv.npc) if quick else llm.stream
                events = stream(
                    instructions=conv.persona.instructions,
                    messages=request,
                    tools=[] if quick else self._request_tools(state.tools),
                    tool_choice=choice,
                    allowed=None if quick else [t.name for t in state.tools],
                    cache_key=conv.npc,
                )
                # closed here, in the turn's task: a `break` would leave the quick model's slot to the loop's
                # finalizer, in another context
                async with aclosing(
                    with_deadlines(events, first_s=first_s, idle_s=idle_s, name=llm.name)
                ) as answer:
                    async for event in answer:
                        if isinstance(event, Done):
                            done = event
                        if not isinstance(event, TextDelta):
                            continue
                        text = event.text
                        if quick:
                            written += text
                            if (end := paragraph_end(written)) is not None:
                                text = text[: max(0, end - (len(written) - len(text)))]  # this chunk's part
                                done = Done(written[:end], (Message("assistant", written[:end]),))
                        now = time.monotonic()
                        if not speak:
                            first = first if first is not None else now - started
                            continue  # the quick model said the npc's line: these words stay unsaid
                        if first is None and state.relevant is not None and not state.relevant():
                            raise StaleTurn
                        first = first if first is not None else now - started
                        if state.first_delta is None:
                            state.first_delta = now - state.started
                        pieces = state.spoken(
                            state.moods.feed(text),
                            {t.name for t in self._request_tools([*npc_tools.TOOLS, *state.tools])},
                            conv.npc,
                        )
                        # The chat shows checked words as they arrive, before their subtitle lines.
                        if (
                            now - state.shown_at >= CHAT_TEXT_EVERY
                            and (shown := to_game_text(state.text)) != state.shown
                        ):
                            state.shown, state.shown_at = shown, now
                            await self.link.send(ChatTextCmd(npc=conv.npc, reply=state.reply, text=shown))
                        await self._feed_lines(conv, state, pieces)
                        if quick and done is not None:
                            break  # the line is over: the stream is closed, the server stops writing
            except LLMError:
                if (
                    speak and first is not None
                ):  # it went quiet mid-answer: what it wrote is said, as remembered
                    await self._say_rest(conv, state)
                raise
            if speak:
                await self._say_rest(conv, state)
        finally:
            TurnTrace.finish_step(st, done, first, time.monotonic() - started)
        if done is None:
            raise LLMError(f"{llm.name}: the stream ended without a result", retryable=True)
        return done
