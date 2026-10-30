"""Talk with several npc's at once, step B1 (docs/npc-tools.md «Беседа нескольких»): a shared transcript.

The npc's around a conversation used to be deaf to each other. The hero's words went into the memories of
everyone near him, but an npc's answer only into its own: on 2026-10-13 Brago shouted «Держи его!» not
knowing what his mate had just said to the hero. Now:

- **what an npc says is heard** by everyone within `SAID_CM` of it. It goes into their memories as the
  hero's words do (talk.py): «Ты слышал, как Бандит говорит герою: «…»». Only what was said counts: an
  answer the hero cut off is heard as far as it went. No model call;
- **the conversation around the hero has participants**: those he spoke to and those who spoke in it,
  while they are within `GROUP_CM` of him. Each of them is told who else is in it and who speaks now
  («В разговоре с героем ещё: Браго, Кавалорн. Сейчас говорит: Браго.»). It is over when nobody has said
  a word in it for `QUIET_S`.

Step B2: one word at a time. The conversation has one floor (`Floor`, dialogue.py):
- **the addressee speaks first**: the hero's phrase opens a round, its seat is the first;
- **whoever else speaks waits its turn**: a turn of an npc near the hero (an urgent shout of the fast
  loop, «Держи его!», an incident, one coming up to him) takes the next seat and thinks only once those
  before it have their text ready (not their sound): it answers with their words in mind, and each next
  voice adds a second or two, not four. Its lines are heard after theirs;
- **three voices to a phrase of the hero's** (`MAX_VOICES`): a fourth says nothing. The round ends with
  his next phrase, or once nobody has spoken for `ROUND_GAP_S` (a fight's shouts go on);
- **the hero's voice cuts all**: whoever speaks stops, whoever waits says nothing. Seats asked while he
  speaks wait for his phrase: the one he speaks to comes first.

Who interjects is step B3; a phrase to several («пацаны», «вы все») is the addressee rule of talk.py.

Step B4: npc's speak to each other in it. A line that calls one of those around the hero by name («Только
троньте его, Браго») is said to him (`say.to`, v15: the two look at each other), and so is the rest of the
turn. The one called answers first after the speaker, ahead of those still waiting (the last of them gives
up its voice if there is none left): a voice of the three too. Its lines go back to the one who spoke to it,
unless they call another. A companion may stand up for the hero, the bandits answer it.
"""

from __future__ import annotations

import asyncio
import itertools
import logging
import time
from collections.abc import Callable, Coroutine
from dataclasses import dataclass, field
from typing import TYPE_CHECKING, Any

from .dialogue import DialogueManager
from .game import GameError, GameLink
from .llm import Tool, ToolCall
from .protocol import GameMessage, Loading, PttCancel, PttStop, Snapshot
from .talk import calls, named
from .turn import ToolOutcome

if TYPE_CHECKING:
    from .dialogue import Conversation
    from .talk import Addressee, TalkDesk

log = logging.getLogger(__name__)

SAID_CM = 1500  # an npc's words are heard this far (talk.PARTNER_CM: the hero hears his partner as far)
GROUP_CM = 1500.0  # a participant farther than this from the hero is out of the conversation
QUIET_S = 60.0  # nobody has said a word in it this long: the conversation is over
TO_HERO = frozenset({"player", "initiative"})  # turns said to the hero, whoever else stands by
MAX_VOICES = 3  # to one phrase of the hero's: the addressee and two more (the user, 2026-10-13)
ROUND_GAP_S = 5.0  # nobody has spoken this long: the next voice opens a round of its own
THINK_WAIT_S = 30.0  # a seat waits this long at most for those before it to have their text ready
HEAR_WAIT_S = 30.0  # ...and its line this long at most for them to have been heard
HERO_TALK_S = 15.0  # the hero has started to speak: seats wait for his phrase this long at most
HEARD_S = 3.0  # ...and once he has stopped, this long for it to be recognized (it may come to nothing)
RESERVED_S = 5.0  # the addressee's seat waits this long for its turn (it may never come: he turned away)
POLL_S = 0.05
ANSWER = "answer"  # the turn of one another npc spoke to
# a witness's answer to the one who asked it (witnesses.py): that one reacts on a turn of its own, with
# catch_lie (2026-10-29: an answer turn, set_intent only, took Lobart's «соврал», and the catch never came)
TESTIMONY = "testimony"
ANSWER_CUE = (
    "[{name} обращается к тебе. Ответь тому, кто к тебе обращается, а не герою — коротко, как сказал бы ты.]"
)
BY_NAME = "Говоришь не герою, а кому-то из них — назови его по имени."


@dataclass
class Member:
    name: str
    since: float  # when it joined
    last: float  # when it last spoke or was spoken to


@dataclass
class Seat:
    """A voice of the round: an npc's turn on the floor, in order."""

    npc: str
    at: float  # when it was taken (or reserved for the addressee)
    taken: bool = True  # its turn has come to it (the addressee's is reserved by the hero's phrase)
    ready: bool = False  # its turn is over: its text is ready
    dropped: bool = False  # it will not speak: the hero spoke first
    ready_at: float = 0.0
    id: int = field(default=0)
    kind: str = "player"  # whose turn a reserved seat waits for: the hero's addressee's, or ANSWER


class GroupTalk:
    """The participants of the conversation around the hero, and who hears whose words. A turn extension:
    the situation line; no tools."""

    def __init__(
        self,
        link: GameLink,
        dialogue: DialogueManager,
        talk: TalkDesk,
        *,
        clock: Callable[[], float] = time.monotonic,
    ) -> None:
        self.link = link
        self.dialogue = dialogue
        self.talk = talk
        self.clock = clock
        self.members: dict[str, Member] = {}
        self.last = float("-inf")  # when a word was last said in the conversation, by anyone
        self.seats: list[Seat] = []  # the round: the voices since the hero's phrase, in order
        self.hero_speaks_until = float("-inf")  # he has begun to speak: new seats wait for his phrase
        self.to_all: str | None = None  # his last phrase was to several: who answers first
        self._calls: dict[str, str] = {}  # whom an npc's turn has called by name, the last one
        # the npc waits for a witness's answer (witnesses.py): it reacts to it on a turn of its own, not
        # on an answer turn to the one who named it (2026-10-29: Hilda's «Лобарт, …» before her testimony)
        self.asks: Callable[[str], bool] = lambda npc: False
        self._ids = itertools.count(1)
        self._tasks: set[asyncio.Task[Any]] = set()
        link.listeners.append(self.on_message)
        dialogue.on_said.append(self.on_said)
        dialogue.floor = self
        talk.on_phrase.append(self.on_phrase)
        talk.on_hero_voice.append(self.cut)

    # --- who is in it --------------------------------------------------------------------------

    def on_message(self, msg: GameMessage) -> None:
        match msg:
            case Loading():
                self.members.clear()
                self._new_round()
            case PttStop():  # his words are being recognized
                self.hero_speaks_until = min(self.hero_speaks_until, self.clock() + HEARD_S)
            case PttCancel():  # a short tap: he said nothing
                self.hero_speaks_until = float("-inf")

    def on_phrase(self, to: Addressee | None, text: str) -> None:
        """The hero said something: whom he said it to joins the conversation and answers first; to several
        («пацаны»), everyone near him joins it."""
        now = self.clock()
        self.last = now
        self.hero_speaks_until = float("-inf")
        self._new_round()
        self.to_all = to.id.upper() if to is not None and to.why == "group" else None
        if to is None:
            return
        self._join(to.id, to.name, now)
        if to.why != "order":  # companions obey an order without a turn: nobody waits for one
            self.seats.append(Seat(to.id.upper(), now, taken=False, id=next(self._ids)))
        if to.why == "group":
            for npc, e in self.talk.people.items():
                if npc != to.id.upper() and self._near_hero(npc):
                    self._join(npc, e.name, now)

    def on_said(self, npc: str, name: str, text: str, kind: str, to: str | None = None) -> None:
        """An npc said its lines (`to`: to another npc): it takes part if it is near the hero, and whoever
        stands by hears it. Those in the conversation hear it at once: the next voice thinks with these words
        in mind. The one it called by name answers it next."""
        npc = npc.upper()
        to = to.upper() if to else None
        now = self.clock()
        called = self._calls.pop(npc, None)
        target = self.talk.people.get(to) if to else None
        if self._near_hero(npc):
            self.last = now
            self._join(npc, name, now)
            if target is not None and self._near_hero(to):
                self._join(to, target.name, now)
        to_hero = kind in TO_HERO or (self.talk.partner is not None and self.talk.partner.npc == npc)
        whom = (
            f"собеседнику ({target.name if target is not None else to})"
            if to
            else "герою"
            if to_hero
            else "вслух"
        )
        told = {npc}
        for other in [*self.participants(), *(s.npc for s in self.seats), *([to] if to else [])]:
            if other not in told:
                told.add(other)
                e = self.talk.people.get(other)
                heard = "тебе" if other == to else whom
                self.dialogue.heard(other, e.name if e is not None else other, name, text, heard)
        self._spawn(self._heard(npc, name, text, whom, told))
        if to is not None and called == to and kind != TESTIMONY and not self.asks(to):
            self._give_word(npc, name, to)

    def participants(self) -> dict[str, Member]:
        """Those in the conversation now: near the hero and on their feet, while it goes on."""
        if self.clock() - self.last > QUIET_S:
            self.members.clear()
        for npc in [n for n in self.members if not self._near_hero(n)]:
            log.info("%s is out of the conversation", npc)
            del self.members[npc]
        return self.members

    def _join(self, npc: str, name: str, now: float) -> None:
        npc = npc.upper()
        if npc == "PLAYER":
            return
        m = self.members.get(npc)
        if m is None:
            self.members[npc] = Member(name, now, now)
            if len(self.members) > 1:
                log.info("%s joins the conversation: %s", npc, ", ".join(self.members))
        else:
            m.last = now

    def _near_hero(self, npc: str) -> bool:
        e = self.talk.people.get(npc.upper())
        return e is not None and e.activity != "down" and e.earshot <= GROUP_CM

    # --- to each other ---------------------------------------------------------------------------

    def called(self, npc: str, line: str) -> str | None:
        """Whom of those near the hero the npc's line calls by name: «Браго, …», «…, Браго!» (talk.calls), the
        speaker near him too. Of two by one name («Бандит»), the one who was in the conversation last."""
        npc = npc.upper()
        by = calls(line)
        if not by or not self._near_hero(npc):
            return None
        found = [
            (d, e)
            for other, e in self.talk.people.items()
            if other != npc and self._near_hero(other) and (d := named(by, e.name)) is not None
        ]
        if not found:
            return None

        def last(other: str) -> float:
            m = self.members.get(other)
            return m.last if m is not None else float("-inf")

        _, e = min(found, key=lambda de: (de[0], -last(de[1].id.upper()), de[1].distance))
        who = self._calls[npc] = e.id.upper()
        return who

    def _give_word(self, speaker: str, name: str, to: str) -> None:
        """`speaker` called `to` by name: it answers first after `speaker`, its voice one of the round's
        three; with none left, the last of those still waiting gives up its own. Its lines go to `speaker`."""
        phrase = self.talk.last
        e = self.talk.people.get(to)
        # its seat: the one of the turn that said it (not ready yet: on_said comes before `done`)
        at = next(
            (
                i
                for i in reversed(range(len(self.seats)))
                if (s := self.seats[i]).npc == speaker and s.taken and not s.ready and not s.dropped
            ),
            None,
        )
        if e is None or at is None:
            return  # not in a round of the hero's (a cut turn: he has spoken again)
        if len([s for s in self.seats if not s.dropped]) >= MAX_VOICES:
            waiting = [s for s in self.seats[at + 1 :] if not s.dropped and not s.ready]
            if not waiting:
                log.info("%s: no voice left to answer %s", to, speaker)
                return
            waiting[-1].dropped = True
            log.info("%s gives up its voice: %s answers %s", waiting[-1].npc, to, speaker)
        seat = Seat(to, self.clock(), taken=False, id=next(self._ids), kind=ANSWER)
        self.seats.insert(at + 1, seat)
        log.info("%s answers %s next: %s", to, speaker, ", ".join(s.npc for s in self.seats if not s.dropped))
        self.dialogue.open(to, e.name)
        turn = self.dialogue.event_turn(
            to,
            ANSWER_CUE.format(name=name),
            relevant=lambda: self.talk.last is phrase,
            overlay=to not in self.talk.attached,  # a free npc answers on the move, as one who speaks up
            tools=("set_intent",),
            kind=ANSWER,
            to=speaker,
        )
        if turn is None:  # it is in a turn of its own: that one thinks with these words in mind
            seat.dropped = True

    # --- who hears it --------------------------------------------------------------------------

    async def _heard(self, npc: str, name: str, text: str, whom: str, told: set[str]) -> None:
        try:
            near = await self.link.story_nearby(npc, SAID_CM)
        except (GameError, TimeoutError) as err:
            log.info("who heard %s: %s", npc, err or type(err).__name__)
            return
        for p in near:
            hearer = str(p.get("npc", "")).upper()
            if not hearer or hearer in told or p.get("dead"):
                continue
            self.dialogue.heard(hearer, str(p.get("name") or hearer), name, text, whom)

    # --- the floor: one voice at a time ---------------------------------------------------------

    async def queue(self, npc: str, kind: str) -> bool:
        """A turn of the npc's takes its seat. The hero's words to it: the first, reserved by his phrase. Any
        other of an npc near the hero: the next one, if there is a voice left, once those before it have
        their text ready. An npc away from the conversation is not on its floor."""
        npc = npc.upper()
        self._calls.pop(npc, None)
        reserved = next(
            (s for s in self.seats if s.npc == npc and not s.taken and not s.dropped and s.kind == kind), None
        )
        if kind == "player":
            seat = reserved
            if seat is None:  # words to it not through talk.py (the console, a scenario): a round of its own
                self._new_round()
                seat = Seat(npc, self.clock(), id=next(self._ids))
                self.seats.append(seat)
            seat.taken = True
            return True
        if reserved is not None:  # another spoke to it: its seat is right after that one's
            reserved.taken = True
            return await self._wait_turn(reserved)
        if not self._near_hero(npc):
            return True
        # he speaks: his phrase opens the round, and first. Polled, as below: the floor's state is time (who
        # is still heard) and other desks'
        while self.clock() < self.hero_speaks_until:  # noqa: ASYNC110
            await asyncio.sleep(POLL_S)
        if self._quiet():
            self._new_round()
        if len([s for s in self.seats if not s.dropped]) >= MAX_VOICES:
            log.info("%s: no voice left for it in this round (%s)", npc, ", ".join(s.npc for s in self.seats))
            return False
        seat = Seat(npc, self.clock(), id=next(self._ids), kind=kind)
        self.seats.append(seat)
        if len(self.seats) > 1:
            log.info("%s waits its turn: %s", npc, ", ".join(s.npc for s in self.seats))
        return await self._wait_turn(seat)

    async def _wait_turn(self, seat: Seat) -> bool:
        """Until those before the seat have their text ready; False: it will not speak (the hero spoke)."""
        deadline = self.clock() + THINK_WAIT_S
        try:
            while not seat.dropped and not self._thought(seat) and self.clock() < deadline:  # noqa: ASYNC110
                await asyncio.sleep(POLL_S)
        except asyncio.CancelledError:
            self._give_up(seat)
            raise
        return not seat.dropped

    def done(self, npc: str) -> None:
        seat = next(
            (s for s in reversed(self.seats) if s.npc == npc.upper() and s.taken and not s.ready), None
        )
        if seat is not None:
            self._give_up(seat)

    def seat(self, npc: str) -> int | None:
        npc = npc.upper()
        return next((s.id for s in reversed(self.seats) if s.npc == npc and s.taken), None)

    async def hear(self, npc: str, seat: int | None) -> None:
        """A line of the npc's waits until those before its seat in the round have been heard: those before
        the turn that said it (2026-10-14: Cavalorn's «Браго, верни…» waited for Brago's answer to it, the
        seat of Cavalorn's next turn being behind Brago's)."""
        deadline = self.clock() + HEAR_WAIT_S
        while self.clock() < deadline:
            mine = next((s for s in self.seats if s.id == seat), None)
            if mine is None or not self._before(mine, heard=True):
                return  # no seat, or a round gone (the hero spoke: it is cut)
            await asyncio.sleep(POLL_S)

    def cut(self) -> bool:
        """The hero starts to speak: in the conversation whoever speaks stops and whoever waits says nothing
        (his partner talk.py stops itself). True if anyone did."""
        self.hero_speaks_until = self.clock() + HERO_TALK_S
        partner = self.talk.partner.npc if self.talk.partner is not None else None
        cut = False
        for seat in self.seats:
            if seat.npc != partner and not seat.ready and not self._thought(seat) and not seat.dropped:
                seat.dropped = True  # still waiting: it will not speak
                cut = True
        for npc in {s.npc for s in self.seats if not s.dropped} | set(self.members):
            # one who does not hear him goes on (2026-10-14 the hero's words to a bandit cut Cavalorn's «Они
            # уже обнажили оружие» 20 m from him, and the fight it was about to set itself on)
            if npc != partner and self._near_hero(npc) and self.dialogue.interrupt(npc):
                cut = True
        self._new_round()
        return cut

    def _thought(self, seat: Seat) -> bool:
        """Its turn may think: those before it have their text ready."""
        return not self._before(seat, heard=False)

    def _before(self, seat: Seat, *, heard: bool) -> bool:
        """Someone before the seat is still at it: thinking, or (`heard`) still being heard."""
        if seat not in self.seats:
            return False  # a round gone: the hero spoke, it was cut
        now = self.clock()
        for s in self.seats[: self.seats.index(seat)]:
            if s.dropped or s.npc == seat.npc:
                continue
            if not s.taken and now - s.at > RESERVED_S:
                continue  # his words went to it, but its turn never came
            # its own lines, not those of the npc's next turn: that one may wait for this seat (Cavalorn,
            # Brago, Cavalorn: 2026-10-14 the two waited for each other until Brago's line was given up)
            if not s.ready or (heard and self.dialogue.audible(s.npc, s.id)):
                return True
        return False

    def _give_up(self, seat: Seat) -> None:
        seat.ready, seat.ready_at = True, self.clock()

    def _quiet(self) -> bool:
        """The round is over: everyone in it has spoken and been heard a while ago."""
        if not self.seats:
            return True
        if any(
            not s.ready and not s.dropped and (s.taken or self.clock() - s.at <= RESERVED_S)
            for s in self.seats
        ):
            return False
        if any(self.dialogue.audible(s.npc, s.id) for s in self.seats):
            return False
        last = max((s.ready_at for s in self.seats), default=float("-inf"))
        return self.clock() - last >= ROUND_GAP_S

    def _new_round(self) -> None:
        for seat in self.seats:
            if not seat.ready and not self._thought(seat):
                seat.dropped = True  # the hero spoke: whoever still waited says nothing
        self.seats = []

    def waiting(self, npc: str) -> bool:
        """Its turn waits for the floor: it is not speaking yet."""
        npc = npc.upper()
        return any(s.npc == npc and s.taken and not s.ready and not self._thought(s) for s in self.seats)

    # --- the turn extension --------------------------------------------------------------------

    async def prepare(self, npc: str) -> None:
        return None

    def situation(self, npc: str) -> str | None:
        npc = npc.upper()
        members = self.participants()
        if npc not in members or len(members) < 2:
            return None
        others = [m.name for n, m in members.items() if n != npc]
        line = f"В разговоре с героем ещё: {', '.join(others)}."
        if self.to_all == npc and self.seats and self.seats[0].npc == npc and not self.seats[0].ready:
            line += " Последнюю фразу герой сказал всем вам, ты отвечаешь первым."
        speaking = [
            m.name
            for n, m in members.items()
            if n != npc and self.dialogue.speaking(n) and not self.waiting(n)
        ]
        if speaking:
            line += f" Сейчас говорит: {', '.join(speaking)}."
        return f"{line} {BY_NAME}"

    all_tools: tuple[Tool, ...] = ()

    def tools(self, npc: str) -> list[Tool]:
        return []

    async def handle(
        self, dialogue: DialogueManager, conv: Conversation, call: ToolCall, snap: Snapshot | None
    ) -> ToolOutcome | None:
        return None

    def _spawn(self, coro: Coroutine[Any, Any, Any]) -> None:
        task = asyncio.get_running_loop().create_task(coro)
        self._tasks.add(task)
        task.add_done_callback(self._tasks.discard)
