"""Who speaks up in the conversation around the hero, by character (step B3 of docs/npc-tools.md).

The one the hero spoke to has answered him: now the others near him are asked, each by its own
character, whether it keeps quiet, puts in a word or steps in (the behavioral model's `interject`,
brain/reflex.py). Those likeliest to speak (by P(word) + P(step in), at least `SPEAK_P`) get the floor
(group.py), one or two, as many as voices are left of the three to a phrase of the hero's.

The model decides; the code only guards:
- the voices (the floor's limit) and the budget of words;
- nobody speaks up twice in a row: who did after the hero's last phrase is not asked after this one;
- whoever is fighting, speaking or waiting to already is not asked; nor is anyone once the hero has
  spoken again.

A free npc near the hero no longer speaks up on its own at his words (the fast loop's `player_speech`
escalation, incidents.py): that is this question now. Each answer goes to the trace: where the model does
not tell characters apart, it is trained on them (the teacher gpt-6.1-sol, as in step 7 of wave 2).
"""

from __future__ import annotations

import asyncio
import logging
from collections.abc import Callable, Coroutine
from typing import TYPE_CHECKING, Any

from .brain.reflex import Reaction, Reflexes
from .brain.situation import NpcContext, clock_text
from .budget import Budget
from .dialogue import DialogueManager
from .group import GROUP_CM, MAX_VOICES, GroupTalk
from .journal import Journal
from .talk import Addressee, TalkDesk, fights
from .trace import TraceWriter

if TYPE_CHECKING:
    from .protocol import RosterEntry

log = logging.getLogger(__name__)

ASK_MAX = 4  # the nearest this many are asked
# P(word) + P(step in) at least this: three to one in favour of speaking. The old 0.8 gate silenced
# Brago at 0.79 after Cavalorn's threat (2026-10-18). The bench's quiet characters average 0.55–0.74,
# Brago 0.86; keep the gate above those quiet averages and the existing limits on voices and repeats.
SPEAK_P = 0.75
INTERJECTORS_MAX = 2  # after the addressee: at most this many more (and only as many as voices are left)
WORD_CUE = (
    "[Рядом с тобой говорят, ты всё слышал. Вставь своё короткое слово — одну фразу, как сказал бы ты: "
    "герою или тому, кто говорил.]"
)
STEP_IN_CUE = (
    "[Рядом с тобой говорят, ты всё слышал. Ты вмешиваешься в разговор: скажи своё, как сказал бы ты.]"
)


def heard_lines(speaker: str, said: str, answer: str, *, to_all: bool) -> list[str]:
    """What the npc heard, for the behavioral model: the hero's words, the answer, and whose talk it is."""
    return [
        f'The hero said to {"all of you" if to_all else speaker}: "{said}"',
        f'{speaker} answered him: "{answer}"',
        f"He spoke to all of you, and {speaker} answered first." if to_all else "Nobody spoke to you.",
    ]


def speak_p(r: Reaction) -> float:
    """How likely the npc is to speak at all."""
    if r.probabilities:
        return r.probabilities.get("word", 0.0) + r.probabilities.get("step_in", 0.0)
    return r.confidence if r.choice != "silent" else 1.0 - r.confidence


class Interjections:
    def __init__(
        self,
        dialogue: DialogueManager,
        talk: TalkDesk,
        group: GroupTalk,
        reflexes: Reflexes,
        budget: Budget,
        *,
        trace: TraceWriter | None = None,
        journal: Journal | None = None,
    ) -> None:
        self.dialogue = dialogue
        self.talk = talk
        self.group = group
        self.reflexes = reflexes
        self.budget = budget
        self.trace = trace
        self.journal = journal or dialogue.journal
        # what an npc perceives now, in English (perception.py), as the other questions have it
        self.view_of: Callable[[str], tuple[str, ...]] = lambda npc: ()
        self.spoke: set[str] = set()  # who spoke up after the hero's latest phrase
        # each time the others were asked: (who answered the hero, [(npc, P(speak))], who spoke up)
        self.asked: list[tuple[str, list[tuple[str, float]], list[str]]] = []
        self._before: set[str] = set()  # ...and after the one before it: not asked now
        self._tasks: set[asyncio.Task[Any]] = set()
        dialogue.on_said.append(self.on_said)
        talk.on_phrase.append(self.on_phrase)

    def on_phrase(self, to: Addressee | None, text: str) -> None:
        self._before, self.spoke = self.spoke, set()

    def on_said(self, npc: str, name: str, text: str, kind: str, to: str | None = None) -> None:
        """The one the hero spoke to has answered him: the others are asked whether they speak up."""
        phrase = self.talk.last
        if kind != "player" or phrase is None or phrase[0] is None or phrase[0].id != npc.upper():
            return
        self._spawn(self._ask(npc.upper(), name, text, phrase))

    def covers(self, npc: str) -> bool:
        """The npc is near the hero: whether it speaks up at his words is asked here."""
        e = self.talk.people.get(npc.upper())
        return e is not None and e.activity != "down" and e.earshot <= GROUP_CM

    def candidates(self, speaker: str) -> list[RosterEntry]:
        """Who is asked: the nearest near the hero, but not the one who answered, whoever is fighting,
        speaking or waiting to, who spoke up after his phrase before, and a witness being asked (it speaks
        from its testimony only)."""
        out = [
            e
            for npc, e in self.talk.people.items()
            if npc != speaker
            and self.covers(npc)
            and not fights(e)
            and npc not in self._before
            and not self.dialogue.speaking(npc)
            and not self.group.witness(npc)
        ]
        return sorted(out, key=lambda e: e.earshot)[:ASK_MAX]

    async def _ask(self, speaker: str, name: str, answer: str, phrase: tuple[Addressee | None, str]) -> None:
        to, said = phrase
        assert to is not None
        people = self.candidates(speaker)
        voices = MAX_VOICES - len([s for s in self.group.seats if not s.dropped])
        if not people or voices <= 0:
            return
        to_all = to.why == "group"
        heard = heard_lines(name, said, answer, to_all=to_all)
        clock = clock_text(*self.talk.game_time) if self.talk.game_time else None
        reactions = await asyncio.gather(*(self._one(e, heard, clock, to_all) for e in people))
        if self.talk.last is not phrase:
            return  # the hero has spoken again
        ranked = sorted(((speak_p(r), r) for r in reactions if r is not None), key=lambda pr: -pr[0])
        chosen = [r for p, r in ranked if p >= SPEAK_P][: min(INTERJECTORS_MAX, voices)]
        self.asked.append((speaker, [(r.npc, p) for p, r in ranked], [r.npc for r in chosen]))
        log.info(
            "after %s: %s",
            speaker,
            ", ".join(f"{r.npc} {p:.2f}{' speaks' if r in chosen else ''}" for p, r in ranked)
            or "nobody asked",
        )
        for r in chosen:
            if not self.budget.allow(r.npc):
                log.info("%s: no budget to speak up", r.npc)
                continue
            self.spoke.add(r.npc)
            self._speak_up(r, phrase)

    async def _one(
        self, e: RosterEntry, heard: list[str], clock: str | None, to_all: bool
    ) -> Reaction | None:
        npc = e.id.upper()
        conv = self.dialogue.conversations.get(npc)
        persona = conv.persona if conv is not None else self.dialogue.personas.get(npc, e.name)
        ctx = NpcContext(
            npc, e.name, e.guild, e.distance, e.in_view, traits=persona.traits, view=self.view_of(npc)
        )
        intent = conv.intent if conv is not None else None
        reaction = await self.reflexes.interject(ctx, heard, intent, clock, to_all=to_all)
        if reaction is not None and self.trace is not None:
            self.trace.write(reaction.record())
        return reaction

    def _speak_up(self, r: Reaction, phrase: tuple[Addressee | None, str]) -> None:
        e = self.talk.people.get(r.npc)
        name = e.name if e is not None else r.npc
        step_in = (
            r.probabilities.get("step_in", 0.0) >= r.probabilities.get("word", 0.0)
            if r.probabilities
            else (r.choice == "step_in")
        )
        self.journal.add(
            "interject", npc=r.npc, name=name, how="step_in" if step_in else "word", p=round(speak_p(r), 2)
        )
        self.dialogue.open(r.npc, name)
        self.dialogue.event_turn(
            r.npc,
            STEP_IN_CUE if step_in else WORD_CUE,
            relevant=lambda: self.talk.last is phrase,
            overlay=r.npc not in self.talk.attached,  # a free npc says it on the move, about its business
            tools=("set_intent",) if step_in else (),
            kind="interject",
        )

    def _spawn(self, coro: Coroutine[Any, Any, Any]) -> None:
        task = asyncio.get_running_loop().create_task(coro)
        self._tasks.add(task)
        task.add_done_callback(self._tasks.discard)
