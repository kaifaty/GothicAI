"""Wolves hear the hero (docs/beasts.md): not his words, the tone of them.

A wolf that growls at the hero (the scripts' ZS_MM_THREATENENEMY: four seconds, then it bites), attacks
him, or lets him pass hears what he says. The bridge lists such wolves twice a second (`beasts`, v21):

- **he starts to speak** (the talk key): the ones growling at him wait for his words, perked up
  (`listen`); their four seconds do not run out while he speaks;
- **his phrase is theirs**, not the people's about, when he looks at a wolf, or when one is at him and he
  calls nobody by name, looks at no one and no person fights him. An order to his companions stays
  theirs, «Стой!» excepted;
- **the tone** of it, in a word: the quick model (Gemma) tells it; the rules below when it is off, slow or
  unsure: soothing, a firm command, a threat to scare it off, hostility, or nothing of that;
- **the code rolls** for each wolf that hears it (`chances`): what it does to him now, its kind, the hero's
  weapon, its wounds and his blows, its pack, his earlier tries. It lets him pass (sits down if he soothed
  it or told it to), runs (a threat that worked), or goes for him: it did not believe him;
- a calm wolf that he hits, or that he comes right up to, attacks; one he leaves behind goes back to its
  routine and lets him be a while. The bridge does that itself and says so (`beast_event`).
"""

from __future__ import annotations

import asyncio
import logging
import random
import time
from collections.abc import Callable, Coroutine
from dataclasses import dataclass
from typing import TYPE_CHECKING, Any

from .commands import Order, words
from .game import GameError, GameLink
from .journal import Journal
from .llm import LLMBackend, LLMError, Message, TextDelta, with_deadlines
from .llm.base import QUICK_SLOT
from .protocol import (
    BeastDo,
    BeastEntry,
    BeastEvent,
    Beasts,
    GameMessage,
    Loading,
    PttCancel,
    PttStart,
    PttStop,
)
from .trace import TraceWriter

if TYPE_CHECKING:
    from .talk import Addressee

log = logging.getLogger(__name__)

HEAR_CM = 1500.0  # a wolf at the hero hears him this far (it loses him at 15 m)
LISTEN_MS = 8000  # it waits for his words while he holds the key, this long at most
AFTER_STOP_MS = 3000  # and once he let it go: the recognizer and the verdict
CALM_MS = 120_000
TONE_S = 1.5  # the quick model's word, or the rules
TONE_MAX_TOKENS = 6  # a word
TRIES_S = 120.0  # failed tries remembered this long
HIT_S = 300.0  # and his blows

# what a wolf does to him: those hear him
AT_HIM = ("threaten", "attack", "listen", "calm")
# orders to companions a wolf at him takes for its own
FOR_WOLVES = frozenset({"halt"})

TONES = ("calm", "command", "threat", "hostile", "other")
TONE_INSTRUCTIONS = """Человек говорит дикому волку, который рычит на него или нападает. Волк не понимает \
слов: он слышит только тон и намерение. Как прозвучала фраза для волка? Ответь одним словом:
calm — успокаивает, ласково, миролюбиво, обещает не трогать, просит пропустить («Тихо, тихо, хороший», \
«Я тебя не трону, я мимо пройду»);
command — короткая твёрдая команда, как собаке («Стой!», «Фу!», «Сидеть!», «Место!»);
threat — грозно прогоняет, пугает («Пошёл вон!», «Брысь!», «Убирайся отсюда!»);
hostile — злость, оскорбления, вызов, угроза убить («Иди сюда, тварь!», «Я тебя прикончу!»);
other — ничего из этого (вопрос, разговор не о волке).
Только одно слово из списка, без пояснений."""

# the rules: stems a word starts with, whole words, phrases
_HOSTILE = (
    "убью",
    "убить",
    "прикончу",
    "зарежу",
    "сдохн",
    "твар",
    "мраз",
    "урод",
    "сволоч",
    "порву",
    "выпотрош",
)
_THREAT_WORDS = frozenset(
    {"вон", "брысь", "прочь", "убирайся", "кыш", "отвали", "проваливай", "пшел", "пшол"}
)
_CALM = ("тих", "тиш", "спокойн", "хорош", "мальчик", "девочк", "умниц", "мимо", "пропуст", "друг", "успокой")
_CALM_WORDS = frozenset({"свой", "свои", "тсс", "тш", "шш", "ну"})
_CALM_PHRASES = ("не бойся", "не трону", "не обижу", "не трогаю", "все хорошо", "не злись")
_COMMAND_WORDS = frozenset({"стой", "стоять", "фу", "сидеть", "место", "лежать", "замри", "нельзя"})
_SIT_WORDS = frozenset({"сидеть", "место", "лежать"})

# the chance a wolf lets him pass: from what it does to him, the tone, then factors
BASE = {"threaten": 0.8, "listen": 0.8, "attack": 0.4, "calm": 0.95}
TONE = {"calm": 1.0, "command": 0.85, "other": 0.4}
KINDS = (("YWOLF", 1.15), ("WARG", 0.6), ("ICEWOLF", 0.8), ("BLACKWOLF", 0.8))  # by its symbol; WOLF 1
WEAPON = {"none": 1.0, "fists": 0.8}  # anything sharper: ARMED
ARMED = 0.5
HURT = 0.7  # below half its health
HIT = 0.25  # he hit it lately
PACK = 0.9  # each other wolf at him
TRY = 0.6  # each failed try lately
MAX_CHANCE = 0.95
# a threat scares it off: young, hurt, a blade in his hand help; a warg hardly runs
FLEE_BASE, FLEE_HURT, FLEE_YOUNG, FLEE_ARMED = 0.3, 0.35, 0.25, 0.15
FLEE_WARG = 0.5
MAX_FLEE = 0.9


def rules_tone(text: str) -> str:
    """The tone of the phrase by its words: hostility over a threat, over soothing, over a command."""
    said = words(text)
    joined = " ".join(said)
    if any(w.startswith(_HOSTILE) for w in said):
        return "hostile"
    if any(w in _THREAT_WORDS for w in said) or "пошел вон" in joined:
        return "threat"
    if any(w.startswith(_CALM) or w in _CALM_WORDS for w in said) or any(p in joined for p in _CALM_PHRASES):
        return "calm"
    if any(w in _COMMAND_WORDS for w in said):
        return "command"
    return "other"


def kind_of(wolf: str) -> float:
    symbol = wolf.split("#", 1)[0].upper()
    return next((f for k, f in KINDS if k in symbol), 1.0)


def young(wolf: str) -> bool:
    return "YWOLF" in wolf.split("#", 1)[0].upper()


@dataclass(frozen=True)
class Chances:
    calm: float  # it lets him pass
    flee: float  # it runs (a threat)


def chances(wolf: BeastEntry, tone: str, weapon: str, pack: int, tries: int) -> Chances:
    """How likely `wolf` is to let him pass, or to run, at his words of `tone`; `pack`: other wolves at
    him, `tries`: his failed tries with it lately."""
    hurt = wolf.hp_max > 0 and wolf.hp * 2 < wolf.hp_max
    hit = wolf.hit_ms is not None and wolf.hit_ms < HIT_S * 1000
    armed = weapon not in WEAPON
    if tone == "hostile":
        return Chances(0.0, 0.0)
    if tone == "threat":
        flee = FLEE_BASE + (FLEE_HURT if hurt else 0) + (FLEE_YOUNG if young(wolf.id) else 0)
        flee += FLEE_ARMED if armed else 0
        if "WARG" in wolf.id.upper():
            flee *= FLEE_WARG
        return Chances(0.0, min(MAX_FLEE, flee * TRY**tries))
    p = BASE.get(wolf.state, 0.0) * TONE.get(tone, 0.0) * kind_of(wolf.id) * WEAPON.get(weapon, ARMED)
    p *= (HURT if hurt else 1.0) * (HIT if hit else 1.0) * PACK**pack * TRY**tries
    return Chances(min(MAX_CHANCE, p), 0.0)


@dataclass(frozen=True)
class Verdict:
    wolf: str
    name: str
    state: str  # what it did to him before
    chance: float
    roll: float
    outcome: str  # calm, flee, attack, keep (it goes on as it was)
    sit: bool = False


class BeastDesk:
    """The wolves near the hero, what he says to them and what they make of it."""

    def __init__(
        self,
        link: GameLink,
        *,
        quick: LLMBackend | None = None,
        journal: Journal | None = None,
        trace: TraceWriter | None = None,
        rng: random.Random | None = None,
        clock: Callable[[], float] = time.monotonic,
    ) -> None:
        self.link = link
        self.quick = quick
        self.journal = journal or Journal()
        self.trace = trace
        self.rng = rng or random.Random()
        self.clock = clock
        self.wolves: dict[str, BeastEntry] = {}
        self.hero_weapon = "none"
        self.listening: set[str] = set()
        self.failed: dict[str, list[float]] = {}  # wolf -> when his tries failed
        self.verdicts: list[Verdict] = []  # the latest phrase's (scenarios, tests)
        self.last_tone: tuple[str, str, int] | None = None  # and its tone: the word, who said so, ms
        self._tasks: set[asyncio.Task[Any]] = set()
        link.listeners.append(self.on_message)

    # --- events -------------------------------------------------------------------------------

    def on_message(self, msg: GameMessage) -> None:
        match msg:
            case Beasts():
                self.wolves = {w.id: w for w in msg.wolves}
                self.hero_weapon = msg.hero_weapon
                self.listening = {w for w in self.listening if w in self.wolves}
            case BeastEvent():
                self._event(msg)
            case PttStart():
                self.listen()
            case PttStop():
                for wolf in self.listening:
                    self._spawn(self.link.beast(wolf, "listen", ms=AFTER_STOP_MS))
            case PttCancel():
                self._resume_listeners()
            case Loading():
                self.wolves.clear()
                self.listening.clear()
                self.failed.clear()

    def listen(self) -> list[str]:
        """The hero starts to speak: the wolves growling at him wait for his words; which do."""
        at = [w.id for w in self.wolves.values() if w.state == "threaten" and w.distance <= HEAR_CM]
        for wolf in at:
            self.listening.add(wolf)
            self._spawn(self.link.beast(wolf, "listen", ms=LISTEN_MS))
        return at

    def _event(self, e: BeastEvent) -> None:
        self.listening.discard(e.npc)
        wolf = self.wolves.get(e.npc)
        name = wolf.name if wolf else e.npc
        log.info("%s (%s): %s %s", e.npc, name, e.kind, e.detail)
        self.journal.add("beast_event", npc=e.npc, name=name, event=e.kind, detail=e.detail)
        if self.trace is not None:
            self.trace.write(
                {"type": "beast_event", "t": round(time.time(), 3), "npc": e.npc, **e.model_dump()}
            )

    # --- his words ----------------------------------------------------------------------------

    def at_him(self) -> list[BeastEntry]:
        return [w for w in self.wolves.values() if w.state in AT_HIM and w.distance <= HEAR_CM]

    def selected(self, focus: str | None) -> BeastEntry | None:
        """The wolf his words would go to, without judging their tone or making it hear them."""
        return self._looked_at(focus) or min(self.at_him(), key=lambda w: w.distance, default=None)

    def resume(self) -> None:
        """The phrase went to a person: stop waiting for words meant for the wolves."""
        self._resume_listeners()

    def hears(
        self, text: str, focus: str | None, to: Addressee | None, order: Order | None
    ) -> Addressee | None:
        """Whether the hero's phrase is for the wolves (then it is judged in the background, and the wolf
        it is said to is returned) or for the people about (None)."""
        from .talk import Addressee

        looked = self._looked_at(focus)
        hearers = self.at_him()
        if to is not None and to.why in ("name", "focus", "group"):
            mine = False  # he calls someone by name, or looks at a person
        elif looked is not None:
            mine = True
        elif to is not None and to.why == "attacker":
            mine = False  # a person fighting him: the words are his
        else:
            mine = bool(hearers) and (order is None or order.kind in FOR_WOLVES)
        if not mine:
            self._resume_listeners()
            return None
        main = self.selected(focus)
        assert main is not None
        # the wolves at him hear it; one he looks at that is not at him pays no heed
        self._spawn(self.judge(text, hearers))
        return Addressee(main.id, main.name, "beast")

    def _looked_at(self, focus: str | None) -> BeastEntry | None:
        if not focus:
            return None
        focus = focus.upper()
        seen = [w for w in self.wolves.values() if w.id.split("#", 1)[0].upper() == focus]
        return next((w for w in seen if w.focus), min(seen, key=lambda w: w.distance) if seen else None)

    def spares(self, symbol: str) -> bool:
        """A wolf of `symbol` waits for the hero's words or lets him pass: his companions leave it be."""
        symbol = symbol.upper()
        return any(
            w.id.split("#", 1)[0].upper() == symbol and (w.state in ("listen", "calm") or w.tolerant)
            for w in self.wolves.values()
        )

    async def tone(self, text: str) -> tuple[str, str]:
        """How the phrase sounds to a wolf, and who said so (quick, rules)."""
        if self.quick is not None:
            try:
                word = await asyncio.wait_for(self._quick_tone(text), TONE_S)
            except (LLMError, TimeoutError) as err:
                log.warning("tone of %r: the quick model failed, the rules decide: %s", text, err)
            else:
                if word in TONES:
                    return word, "quick"
                log.info("tone of %r: the quick model said %r, the rules decide", text, word)
        return rules_tone(text), "rules"

    async def _quick_tone(self, text: str) -> str:
        assert self.quick is not None
        said = ""
        token = QUICK_SLOT.set(None)  # tone has no NPC persona or pinned slot
        try:
            events = self.quick.stream(instructions=TONE_INSTRUCTIONS, messages=[Message("user", text)])
            async for event in with_deadlines(events, first_s=TONE_S, idle_s=TONE_S, name=self.quick.name):
                if isinstance(event, TextDelta):
                    said += event.text
        finally:
            QUICK_SLOT.reset(token)
        found = words(said)
        return next((w for w in found if w in TONES), said.strip()[:40])

    async def judge(self, text: str, hearers: list[BeastEntry]) -> list[Verdict]:
        """What each wolf that heard the phrase makes of it; carried out at once."""
        if not hearers:
            self.verdicts = []
            return []
        t0 = self.clock()
        tone, via = await self.tone(text)
        tone_ms = round((self.clock() - t0) * 1000)
        self.last_tone = (tone, via, tone_ms)
        now = self.clock()
        said = set(words(text))
        sit = tone == "calm" or bool(said & _SIT_WORDS)
        verdicts = []
        for w in hearers:
            wolf = self.wolves.get(w.id, w)  # its latest state: it may have bitten meanwhile
            pack = sum(1 for o in self.at_him() if o.id != wolf.id)
            tries = len([t for t in self.failed.get(wolf.id, []) if now - t < TRIES_S])
            ch = chances(wolf, tone, self.hero_weapon, pack, tries)
            roll = self.rng.random()
            if roll < ch.calm:
                outcome = "keep" if wolf.state == "calm" else "calm"
            elif roll < ch.calm + ch.flee:
                outcome = "flee"
            elif wolf.state == "calm" and tone != "hostile":
                outcome = "keep"  # it let him pass already: only hostility undoes that
            else:
                outcome = "keep" if wolf.state == "attack" else "attack"
                self.failed.setdefault(wolf.id, []).append(now)
            v = Verdict(
                wolf.id,
                wolf.name,
                wolf.state,
                max(ch.calm, ch.flee),
                roll,
                outcome,
                sit and outcome == "calm",
            )
            verdicts.append(v)
            self.listening.discard(wolf.id)
            self._record(v, text, tone, via, tone_ms)
            await self._carry_out(v)
        self.verdicts = verdicts
        return verdicts

    async def _carry_out(self, v: Verdict) -> None:
        do: BeastDo | None = {"calm": "calm", "flee": "flee", "attack": "attack"}.get(v.outcome)  # type: ignore[assignment]
        if do is None:
            return
        try:
            await self.link.beast(v.wolf, do, ms=CALM_MS if do == "calm" else None, sit=v.sit or None)
        except GameError as err:
            log.warning("%s: %s not carried out: %s", v.wolf, do, err)

    def _record(self, v: Verdict, text: str, tone: str, via: str, tone_ms: int) -> None:
        log.info(
            "%s (%s, %s) hears %r: %s (%s, %d ms), chance %.2f, roll %.2f -> %s",
            v.wolf,
            v.name,
            v.state,
            text,
            tone,
            via,
            tone_ms,
            v.chance,
            v.roll,
            v.outcome,
        )
        self.journal.add(
            "beast",
            npc=v.wolf,
            name=v.name,
            text=text,
            tone=tone,
            via=via,
            chance=round(v.chance, 2),
            roll=round(v.roll, 2),
            outcome=v.outcome,
        )
        if self.trace is not None:
            self.trace.write(
                {
                    "type": "beast",
                    "t": round(time.time(), 3),
                    "npc": v.wolf,
                    "state": v.state,
                    "text": text,
                    "tone": tone,
                    "via": via,
                    "tone_ms": tone_ms,
                    "weapon": self.hero_weapon,
                    "chance": round(v.chance, 3),
                    "roll": round(v.roll, 3),
                    "outcome": v.outcome,
                    "sit": v.sit,
                }
            )

    def _resume_listeners(self) -> None:
        for wolf in self.listening:
            self._spawn(self.link.beast(wolf, "resume"))
        self.listening.clear()

    def _spawn(self, coro: Coroutine[Any, Any, Any]) -> None:
        async def run() -> None:
            try:
                await coro
            except GameError as err:
                log.debug("beast command failed: %s", err)

        task = asyncio.get_running_loop().create_task(run())
        self._tasks.add(task)
        task.add_done_callback(self._tasks.discard)
