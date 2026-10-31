"""The director's storylets: a lie or a crime comes home.

When someone the hero deals with learns that he lied — it was lied to and found out, it caught him, or a
rumor reached it (social.py) — the director may have it ask him to make amends, as a writer of the game
would. The model proposes; the code builds the change, the validator checks it, the game plays it:
- the talk: a dialog of the director's that the npc starts when the hero comes to it (important): what it
  heard and what it wants;
- what it closes until then: its dialogs the hero would come for — work, joining, teaching, trade —
  switched off;
- the amends: a dialog of the director's, at it or at the one lied to, offered once the talk is over:
  paying a fine or the reward back (gold taken from the hero), never more than MAX_GOLD;
- once he has made amends the dialogs open again, the npc (and the one lied to) remember it, and trust.py
  counts it. He may also make them in conversation: the npc offers a deal (deals.py) that forgives the lie,
  for gold or for nothing, and taking it settles the storylet the same way.

Its state is in the chronicle (`amends_*` facts) and in the patches of the save branch, so a load brings
back exactly where it was. A lie gets STORYLETS_PER_LIE of them at most, the one lied to first, then those
the rumor reached, in the order they heard it; the model may find that one does not care, and is asked
about LOOKS_PER_LIE people at most.

A victim who saw or heard of the hero's theft or assault can ask for compensation through the same
flow. Suspicion does not qualify. Paying records `forgiven{paid}` for that crime; a quest returning
property is separate and never forgives it automatically.
"""

from __future__ import annotations

import asyncio
import json
import logging
from dataclasses import asdict, dataclass, field
from typing import TYPE_CHECKING, Any

from .chronicle import Chronicle, Fact
from .game import GameError, GameLink
from .gamedata.storygraph import Node, StoryGraph
from .journal import Journal
from .llm import LLMBackend, LLMError, Message, Tool, ToolCall, open_ahead
from .people_knowledge import rumor_subject
from .protocol import DialogChosen, GameMessage, Loading, Session, Snapshot
from .story import StoryPatches
from .turn import ToolOutcome
from .validator import DIRECTOR_PREFIX, Rules, Story, names_in, validate

if TYPE_CHECKING:
    from .crimes import CrimeKnowledge, Crimes
    from .dialogue import Conversation, DialogueManager
    from .director import Director

log = logging.getLogger(__name__)

PROPOSE = "propose_amends"
STORYLETS_PER_LIE = 2  # opened
LOOKS_PER_LIE = 4  # people the model is asked about, for one lie
MAX_GOLD = 100  # the most a fine may be
MAX_CLOSE = 4
ATTEMPTS = 2
CACHE_KEY = (
    "amends"  # its requests' own connection (openai_responses.py): the prompt cache of its instructions
)
# what the hero comes to an npc for: what it may close until he makes amends
STAKES = {"join": "принимает в гильдию", "work": "даёт работу", "teach": "учит", "trade": "торгует"}
SKIP = ("PICKPOCKET", "EXIT", "PETZ", "PMSCHULDEN")  # the scripts' own crime and pickpocket dialogs

INSTRUCTIONS = """\
Ты — режиссёр сюжета Gothic II. Код следит за миром и зовёт тебя, когда о лжи героя узнал человек, с которым \
у героя есть дела, или пострадавший узнал, что герой обокрал или ударил его. Реши, как этот человек \
поступит, — как сделал бы сценарист оригинальной игры: в характере, бережно к миру и тону.

- amends: он требует загладить вину. Пока герой не заплатит, он закрывает для него свои дела — те из \
списка, которые ему естественно закрыть (close, по id; можно не все). Ты пишешь разговор, который он начнёт \
сам (lines: 2–6 реплик; npc — он, hero — герой): что он слышал и от кого, что требует. Расплата — золото \
(gold, не больше предела из сводки; 0 — достаточно повиниться) ему самому (to: self) или обманутому \
(to: victim), строка героя в меню (description, до 60 знаков, например «Вот штраф — 50 золотых»), разговор \
при расплате (amends_lines: 1–4 реплики), название темы журнала (title) и две записи от первого лица: \
journal_now — что от героя требуют, journal_done — что он заплатил.
- none: ему всё равно или это не его дело. Остальные поля пустые.

Для дела (crime) пострадавший требует возместить ущерб: gold от 1 до предела сводки, с учётом цены \
вещи или тяжести нападения. Если у него нет доступных дел, close может быть пустым; важный разговор \
и оплата остаются. Он знает только то, что видел или слышал по сводке. По подозрению виновным не объявляй.

Строго по сводке: не выдумывай, чего человек не знает. Тот, кто знает со слухов, может и сомневаться. \
Речь — по-русски, в духе Gothic: просто, грубовато, без пафоса и современных слов. Ответ — один вызов \
propose_amends."""

_LINES = {
    "type": "array",
    "items": {
        "type": "object",
        "properties": {"speaker": {"type": "string", "enum": ["hero", "npc"]}, "text": {"type": "string"}},
        "required": ["speaker", "text"],
        "additionalProperties": False,
    },
}
PROPOSE_TOOL = Tool(
    PROPOSE,
    "Предложить, как человек из сводки поступит с героем, узнав о его лжи или деле против него. "
    "Код проверит предложение и применит его или вернёт причины отказа.",
    {
        "type": "object",
        "properties": {
            "kind": {"type": "string", "enum": ["amends", "none"]},
            "close": {"type": "array", "items": {"type": "string"}, "description": "id его дел из сводки"},
            "lines": _LINES,
            "to": {"type": "string", "enum": ["self", "victim"]},
            "gold": {"type": "integer"},
            "description": {"type": "string"},
            "amends_lines": _LINES,
            "title": {"type": "string"},
            "journal_now": {"type": "string"},
            "journal_done": {"type": "string"},
            "reason": {"type": "string", "description": "почему так, одной фразой"},
        },
        "required": [
            "kind",
            "close",
            "lines",
            "to",
            "gold",
            "description",
            "amends_lines",
            "title",
            "journal_now",
            "journal_done",
            "reason",
        ],
        "additionalProperties": False,
    },
)


@dataclass
class Lie:
    """A lie of the hero's that came out, as the chronicle has it."""

    quest: str
    kind: str  # found_out: he took a reward for it; caught: he tried to
    victim: str
    victim_name: str
    title: str
    reward: str = ""
    said: bool = False  # the lie was his words (title), seen false (sightings.py), not a quest
    witness_name: str = ""  # who told the one lied to that his words were false (witnesses.py)

    @property
    def key(self) -> str:
        return f"{self.kind}:{self.victim}:{self.quest}"

    def words(self) -> str:
        who = f"обманул человека по имени {self.victim_name}"
        if self.kind == "found_out":
            return f"{who}: взял награду ({self.reward or 'какую-то'}) за «{self.title}», а дела не сделал"
        if self.said and self.witness_name:
            told = f"а тот узнал от человека по имени {self.witness_name}"
            return f"{who}: сказал ему «{self.title}», {told}, что это неправда"
        if self.said:
            return f"{who}: сказал ему «{self.title}», а тот своими глазами видел, что это неправда"
        return f"{who}: уверял, что сделал «{self.title}», а это неправда; награды не получил"


@dataclass
class CrimeCase:
    """A victim's proven grievance, using the same storylet flow as a revealed lie."""

    crime: str
    victim: str
    victim_name: str
    description: str
    kind: str

    @property
    def key(self) -> str:
        return f"crime:{self.crime}"

    def words(self) -> str:
        return self.description.removeprefix("герой ")


@dataclass
class Stake:
    """A dialog of the npc's the hero would come for."""

    id: str
    description: str
    what: str  # STAKES' words


@dataclass
class Brief:
    lie: Lie | CrimeCase
    npc: str
    name: str
    guild: str
    how: str  # how it knows: was lied to, caught him, heard from whom
    stakes: list[Stake]
    victim_alive: bool
    asked: list[str] = field(default_factory=list)  # what others already ask of the hero for this lie
    gold_limit: int = MAX_GOLD
    price: int | None = None
    required: bool = False

    def text(self) -> str:
        lines = [
            f"Человек: {self.name} ({self.npc}){f', {self.guild}' if self.guild else ''}.",
            f"Что он знает: герой {self.lie.words()}. Откуда: {self.how}.",
            f"{'Пострадавший' if isinstance(self.lie, CrimeCase) else 'Обманутый'}: "
            f"{self.lie.victim_name} ({self.lie.victim}), "
            f"{'жив' if self.victim_alive else 'мёртв'}.",
            "Его дела, ради которых герой к нему ходит (можно закрыть до расплаты):",
            *(f"- {s.id}: «{s.description}» — {s.what}" for s in self.stakes),
            f"Предел расплаты: {self.gold_limit} золотых.",
        ]
        if isinstance(self.lie, CrimeCase):
            lines.append(f"Дело: {self.lie.key}; пострадавший знает, что это сделал герой.")
            if self.required:
                lines.append(
                    "Правило встречи уже решило: пострадавший требует возмещения. "
                    "Напиши существующий разговор amends и оплату; kind: amends, не none."
                )
            if self.price is not None:
                lines.append(f"Стоимость взятого: {self.price} золотых.")
        if self.asked:
            lines.append(
                f"За {'это дело' if isinstance(self.lie, CrimeCase) else 'эту ложь'} уже требуют "
                "(если он заодно с ними, можно потребовать того же — тогда одна "
                "расплата закроет оба требования): " + "; ".join(self.asked)
            )
        return "\n".join(lines)


@dataclass
class Proposal:
    kind: str
    close: list[str] = field(default_factory=list)
    lines: list[dict[str, str]] = field(default_factory=list)
    to: str = "self"
    gold: int = 0
    description: str = ""
    amends_lines: list[dict[str, str]] = field(default_factory=list)
    title: str = ""
    journal_now: str = ""
    journal_done: str = ""
    reason: str = ""

    @classmethod
    def of(cls, args: dict[str, Any]) -> Proposal:
        fields = {k: args[k] for k in cls.__dataclass_fields__ if k in args}
        try:
            fields["gold"] = int(fields.get("gold") or 0)
        except (TypeError, ValueError):
            fields["gold"] = -1
        fields["close"] = [str(x).upper() for x in fields.get("close") or []]
        return cls(**fields)


@dataclass
class Storylet:
    """An npc asking the hero to make amends, and where it stands."""

    id: str
    lie: str  # Lie.key or CrimeCase.key; persisted name kept for existing save branches
    npc: str
    name: str
    close: list[str]
    to: str  # npc symbol the amends are made at
    to_name: str
    gold: int
    title: str
    talk: str  # the director's dialogs
    pay: str
    pay_node: dict[str, Any]
    description: str
    stage: str = "open"  # open -> told (the talk is over) -> done (amends made)


class AmendsModel:
    """The cloud model that writes the storylets."""

    def __init__(self, llm: LLMBackend) -> None:
        self.llm = llm

    def warm(self) -> None:
        """Its connection opened at once: the request after the pacing does not wait for it."""
        open_ahead(self.llm, CACHE_KEY, keep=True)

    async def propose(
        self, brief: Brief, previous: Proposal | None = None, errors: list[str] | None = None
    ) -> Proposal | None:
        messages = [Message("user", brief.text())]
        if previous is not None and errors:
            sent = json.dumps(asdict(previous), ensure_ascii=False)
            reasons = "\n".join(f"- {e}" for e in errors)
            messages.append(Message("user", f"Твоё предложение: {sent}\nОтклонено:\n{reasons}\nИсправь его."))
        calls: list[ToolCall] = []
        try:
            async for ev in self.llm.stream(
                instructions=INSTRUCTIONS, messages=messages, tools=[PROPOSE_TOOL], cache_key=CACHE_KEY
            ):
                if isinstance(ev, ToolCall) and ev.name == PROPOSE:
                    calls.append(ev)
        except LLMError as err:
            log.warning("amends: the model failed: %s", err)
            return None
        return Proposal.of(calls[0].arguments) if calls else None


class Amends:
    """Storylets of amends for revealed lies and crimes known to their victims."""

    def __init__(
        self,
        link: GameLink,
        graph: StoryGraph,
        patches: StoryPatches,
        chronicle: Chronicle,
        director: Director,
        llm: LLMBackend,
        *,
        dialogue: DialogueManager | None = None,
        journal: Journal | None = None,
        crimes: Crimes | None = None,
    ) -> None:
        self.link = link
        self.graph = graph
        self.patches = patches
        self.chronicle = chronicle
        self.director = director
        self.model = AmendsModel(llm)
        self.dialogue = dialogue
        self.journal = journal or Journal()
        self.crimes = crimes
        self.rules = Rules.of(graph)
        self.lies: dict[str, Lie | CrimeCase] = {}
        self.storylets: dict[str, Storylet] = {}  # by id
        self.looked: dict[str, list[str]] = {}  # lie -> npc's looked at for it
        self._queue: asyncio.Queue[tuple[str, str, str] | None] = asyncio.Queue()
        self._background: set[asyncio.Task[Any]] = set()
        self._world = 0
        self._loading = False
        self._consider_lock = asyncio.Lock()
        chronicle.on_fact.append(self._on_fact)
        link.listeners.append(self._on_message)
        if dialogue is not None:
            dialogue.extensions.append(self)
        self._load()

    # --- watching -----------------------------------------------------------------------------

    def _on_fact(self, f: Fact) -> None:
        if f.get("fact") == "rumor" and rumor_subject(f) != "PLAYER":
            return
        match f.get("fact"):
            case "found_out" | "caught":
                lie = Lie(
                    f.get("quest", ""),
                    f["fact"],
                    f.get("npc", ""),
                    f.get("name") or f.get("npc", ""),
                    f.get("title") or f.get("quest", ""),
                    self._reward(f.get("quest", "")),
                    said=bool(f.get("said")),
                    witness_name=str(f.get("witness_name", "")),
                )
                self.lies.setdefault(lie.key, lie)
                self._queue.put_nowait((lie.key, lie.victim, "lied"))
            case "rumor":
                if f.get("crime"):
                    self._queue_crime(f.get("to", ""), f["crime"])
                    return
                key = (
                    f"{f.get('kind', 'found_out')}:{f.get('about') or f.get('from', '')}:{f.get('quest', '')}"
                )
                self._queue.put_nowait((key, f.get("to", ""), f.get("from_name") or ""))
            case "crime_seen":
                self._queue_crime(f.get("npc", ""), f.get("crime", ""))
            case "testimony" if self.crimes is not None:
                for crime in self.crimes.testimony_crimes(f):
                    self._queue_crime(f.get("npc", ""), crime)
            case "deal" if f.get("forgive"):
                self._spawn(self._forgiven(f))
            case "forgiven" if self.crimes is not None:
                self._spawn(self._forgiven(f))
            case "silenced" | "died" | "blamed" if self.crimes is not None:
                self._spawn(self._withdraw_crimes(self._world))

    def _queue_crime(self, npc: str, crime_id: str) -> None:
        knowledge = self._crime_knowledge(npc, crime_id)
        if knowledge is None or self.crimes is None:
            return
        case = self._crime_case(knowledge.crime)
        self.lies.setdefault(case.key, case)
        how = (
            "видел сам" if knowledge.degree == "seen" else f"слышал от {self.crimes.names(knowledge.source)}"
        )
        self._queue.put_nowait((case.key, case.victim, how))

    def _crime_case(self, crime: Fact) -> CrimeCase:
        assert self.crimes is not None
        victim = crime.get("victim") or crime.get("owner", "")
        return CrimeCase(
            crime["id"], victim, self.crimes.names(victim), self.crimes.describe(crime), crime["kind"]
        )

    def _crime_knowledge(self, npc: str, crime_id: str) -> CrimeKnowledge | None:
        if self.crimes is None:
            return None
        npc = npc.upper()
        if self.crimes.blamed(npc, crime_id):
            return None
        if any(f.get("npc", "").upper() == npc for f in self.chronicle.facts("died")):
            return None
        return next(
            (
                k
                for k in self.crimes.known(npc)
                if k.crime["id"] == crime_id
                and (k.crime.get("victim") or k.crime.get("owner")) == npc
                and k.crime.get("kind") in {"theft", "attack"}
                and k.degree in {"seen", "heard"}
                and k.suspect == "PLAYER"
                and not k.forgiven
                and not k.silenced
            ),
            None,
        )

    def _on_message(self, msg: GameMessage) -> None:
        if isinstance(msg, (Loading, Session)):
            # StoryPatches records acknowledgements in the current branch. Stop old
            # dialog callbacks before an acknowledgement can cross a load boundary.
            for task in tuple(self._background):
                if not task.done() and not task.cancelling():
                    task.cancel()
        match msg:
            case Loading():
                self._world += 1
                self._loading = True
            case Session():
                self._world += 1
                self._loading = False
                self._load()
            case DialogChosen(made=True):
                for s in list(self.storylets.values()):
                    if msg.node == s.talk and s.stage == "open":
                        self._spawn(self._told(s))
                    elif msg.node == s.pay and s.stage != "done":
                        self._spawn(self._paid(s))

    async def run(self) -> None:
        while True:
            item = await self._queue.get()
            if item is None:
                continue
            key, npc, how = item
            try:
                await self.consider(key, npc.upper(), how)
            except (GameError, TimeoutError) as err:
                log.warning("amends: %s", err)

    # --- a storylet ---------------------------------------------------------------------------

    async def consider(self, key: str, npc: str, how: str) -> Storylet | None:
        """Whether `npc`, who has learnt of the grievance `key`, asks the hero to make amends."""
        world = self._world
        async with self._consider_lock:
            if world != self._world or self._loading:
                return None
            return await self._consider(key, npc, how, world=world)

    def active_crime(self, npc: str, crime_id: str) -> bool:
        """The victim still knows an unresolved grievance against the hero."""
        return self._crime_knowledge(npc, crime_id) is not None

    async def demand(self, npc: str, crime_id: str) -> Storylet | None:
        """A meeting rule requires compensation; reuse the usual validated Amends dialog and payment."""
        npc = npc.upper()
        world = self._world
        async with self._consider_lock:
            if world != self._world or self._loading:
                return None
            if (knowledge := self._crime_knowledge(npc, crime_id)) is None:
                return None
            case = self._crime_case(knowledge.crime)
            self.lies.setdefault(case.key, case)
            existing = next(
                (
                    s for s in self.storylets.values()
                    if s.npc == npc and s.lie == case.key and s.stage != "done"
                ),
                None,
            )
            if existing is not None:
                return existing
            how = (
                "видел сам" if knowledge.degree == "seen"
                else f"слышал от {self.crimes.names(knowledge.source)}"
            )
            return await self._consider(case.key, npc, how, world=world, required=True)

    async def _consider(
        self, key: str, npc: str, how: str, *, world: int, required: bool = False
    ) -> Storylet | None:
        lie = self.lies.get(key)
        looked = self.looked.setdefault(key, [])
        opened = sum(1 for s in self.storylets.values() if s.lie == key)
        if lie is None or not npc or (npc in looked and not required):
            return None
        if isinstance(lie, CrimeCase) and self._crime_knowledge(npc, lie.crime) is None:
            return None
        if (len(looked) >= LOOKS_PER_LIE and not required) or opened >= STORYLETS_PER_LIE:
            return None
        if any(s.npc == npc and s.stage != "done" for s in self.storylets.values()):
            return None
        if self.rules.plot_npc(npc):
            return None
        stakes = self.stakes(npc)
        if not stakes and isinstance(lie, Lie):
            return None  # nothing he comes to it for: no reason to write a storylet
        self.model.warm()
        await self.director.pacing.wait(self.director.busy)
        try:
            story = await self.director.story()
            if world != self._world or not story.state.alive(npc):
                return None
            if isinstance(lie, CrimeCase) and self._crime_knowledge(npc, lie.crime) is None:
                return None
            stakes = [s for s in stakes if story.can_offer(s.id)]
            if not stakes and isinstance(lie, Lie):
                return None
            looked.append(npc)
            price = await self._crime_price(lie) if isinstance(lie, CrimeCase) else None
            limit = MAX_GOLD
            if isinstance(lie, CrimeCase):
                limit = min(MAX_GOLD, price) if price else (50 if lie.kind == "theft" else MAX_GOLD)
            brief = Brief(
                lie,
                npc,
                story.state.name(npc),
                "",
                _how(how, lie),
                stakes,
                story.state.alive(lie.victim),
                [
                    f"{x.name} — {_price(x)}"
                    for x in self.storylets.values()
                    if x.lie == key and x.stage != "done"
                ],
                gold_limit=limit,
                price=price,
                required=required,
            )
            previous: Proposal | None = None
            errors: list[str] = []
            for attempt in range(1, ATTEMPTS + 1):
                proposal = await self.model.propose(brief, previous, errors)
                if not self._current(brief, world):
                    return None
                log.info("amends: %s for %s, proposal %d: %s", npc, key, attempt, proposal)
                if proposal is None:
                    errors, previous = ["нет предложения: ответ — вызов propose_amends"], Proposal(kind="")
                    continue
                if proposal.kind == "none":
                    if required:
                        errors, previous = ["правило встречи требует kind: amends"], proposal
                        continue
                    self.chronicle.record(
                        {"fact": "amends_none", "lie": key, "npc": npc, "reason": proposal.reason}
                    )
                    return None
                made, errors = await self._try(proposal, brief, story, world=world)
                if made is not None:
                    return made
                previous = proposal
                self.chronicle.record({"fact": "amends_rejected", "lie": key, "npc": npc, "why": errors})
            if required and self._current(brief, world):
                made, errors = await self._try(self._fallback(brief), brief, story, world=world)
                if made is None and self._current(brief, world):
                    self.chronicle.record({"fact": "amends_rejected", "lie": key, "npc": npc, "why": errors})
                return made
            return None
        finally:
            self.director.pacing.stepped()

    def _current(self, brief: Brief, world: int) -> bool:
        return world == self._world and not self._loading and (
            not isinstance(brief.lie, CrimeCase) or self.active_crime(brief.npc, brief.lie.crime)
        )

    @staticmethod
    def _fallback(brief: Brief) -> Proposal:
        gold = min(brief.gold_limit, brief.price or (50 if brief.lie.kind == "attack" else 10))
        deed = "за взятое" if brief.lie.kind == "theft" else "за побои"
        return Proposal(
            kind="amends", gold=gold,
            lines=[
                {"speaker": "npc", "text": f"Я знаю, что ты сделал. Теперь отвечай {deed}."},
                {"speaker": "npc", "text": f"Заплати {gold} золотых, тогда сочтёмся."},
            ],
            description=f"Вот {gold} золотых в возмещение.",
            amends_lines=[{"speaker": "npc", "text": "Ладно. За это дело я тебя простил."}],
            title=f"Возмещение: {brief.name}",
            journal_now=f"{brief.name} требует {gold} золотых {deed}.",
            journal_done=f"Я заплатил {brief.name}: {gold} золотых {deed}.",
            reason="пострадавший требует возмещения по правилу встречи",
        )

    def stakes(self, npc: str) -> list[Stake]:
        """Its dialogs the hero would come for, as the graph has them."""
        out = []
        for node in self.graph.dialogs_of(npc):
            if any(s in node.id for s in SKIP) or node.id in self.rules.plot_nodes:
                continue
            what = _stake(node, self.graph)
            if what:
                out.append(Stake(node.id, node.description, STAKES[what]))
        return out

    async def _crime_price(self, case: CrimeCase) -> int | None:
        """Read available script item values, without creating items or adding protocol fields."""
        crime = self.crimes.crime(case.crime) if self.crimes is not None else None
        if crime is None or case.kind != "theft":
            return None
        items = crime.get("items") or [{"item": crime.get("item", ""), "count": crime.get("count", 0)}]
        items = [item for item in items if item.get("item") and int(item.get("count", 0)) > 0]
        if not items:
            return None
        try:
            values = await self.link.story_eval(
                case.victim,
                [
                    ["int", 1] if item["item"] == "ITMI_GOLD" else ["field", ["inst", item["item"]], "VALUE"]
                    for item in items
                ],
            )
        except (GameError, TimeoutError):
            return None
        if len(values) != len(items) or not all(isinstance(v, int) and v > 0 for v in values):
            return None  # an absent VM instance has no price; do not invent one
        return sum(int(item["count"]) * value for item, value in zip(items, values, strict=True))

    async def _try(
        self, p: Proposal, brief: Brief, story: Story, *, world: int | None = None
    ) -> tuple[Storylet | None, list[str]]:
        world = self._world if world is None else world
        errors = []
        stakes = {s.id for s in brief.stakes}
        if p.kind != "amends":
            return None, [f"вид: amends или none, а не {p.kind!r}"]
        minimum = 0 if isinstance(brief.lie, CrimeCase) else 1
        if len(p.close) < minimum or len(p.close) > MAX_CLOSE or not set(p.close) <= stakes:
            errors.append(f"close: от {minimum} до {MAX_CLOSE} id из списка его дел")
        if p.to not in ("self", "victim"):
            errors.append("to: self или victim")
        elif p.to == "victim" and not brief.victim_alive:
            errors.append(f"{brief.lie.victim_name} мёртв: расплата — ему самому (to: self)")
        minimum_gold = 1 if isinstance(brief.lie, CrimeCase) else 0
        if not minimum_gold <= p.gold <= brief.gold_limit:
            errors.append(f"gold: от {minimum_gold} до {brief.gold_limit}")
        if not 2 <= len(p.lines) <= 6 or not 1 <= len(p.amends_lines) <= 4:
            errors.append("lines: 2–6 реплик, amends_lines: 1–4")
        if not (
            p.title.strip() and p.journal_now.strip() and p.journal_done.strip() and p.description.strip()
        ):
            errors.append("нужны title, description, journal_now и journal_done")
        if errors:
            return None, errors
        n = 1 + sum(1 for s in self.storylets.values() if s.npc == brief.npc)
        base = f"{DIRECTOR_PREFIX}AMENDS_{brief.npc}_{n}"
        to = brief.npc if p.to == "self" else brief.lie.victim
        talk = {
            "id": f"{base}_TALK",
            "npc": brief.npc,
            "description": "",
            "important": True,
            "nr": 1,
            "condition": None,
            "lines": _lines(p.lines),
            "effects": [{"do": "journal", "topic": p.title, "text": p.journal_now}],
        }
        pay_effects: list[dict[str, Any]] = []
        if p.gold > 0:
            pay_effects.append({"do": "give", "item": "ITMI_GOLD", "count": p.gold, "to": "self"})
        pay_effects.append(
            {"do": "journal", "topic": p.title, "text": p.journal_done, "status": "LOG_SUCCESS"}
        )
        has_gold = [">=", ["call", "NPC_HASITEMS", ["inst", "OTHER"], ["sym", "ITMI_GOLD"]], ["int", p.gold]]
        pay = {
            "id": f"{base}_PAY",
            "npc": to,
            "description": p.description,
            "nr": 1,
            "condition": has_gold if p.gold > 0 else None,
            "lines": _lines(p.amends_lines),
            "effects": pay_effects,
        }
        ops = [{"op": "add_node", "node": talk}, *({"op": "disable", "node": c} for c in p.close)]
        story = await self.director.with_symbols(
            story, names_in([*ops, {"op": "add_node", "node": pay}]) | {to}
        )
        errors = validate([*ops, {"op": "add_node", "node": pay}], story, self.rules)
        if errors:
            return None, errors
        if not self._current(brief, world):
            return None, ["знание или ветка изменились"]
        sent: list[dict[str, Any]] = []

        async def rollback() -> None:
            undo = [{"op": "remove_node", "node": talk["id"]}]
            undo.extend({"op": "enable", "node": op["node"]} for op in sent if op["op"] == "disable")
            for op in undo:
                if world != self._world:
                    return
                try:
                    await self._apply(op)
                except (TimeoutError, ConnectionError) as error:
                    log.warning("amends rollback %s: %s", talk["id"], error)

        try:
            for op in ops:
                if not self._current(brief, world):
                    await rollback()
                    return None, ["знание или ветка изменились"]
                # Sending may change the native graph before its acknowledgement arrives.
                # Track attempted writes so cancellation also undoes that in-flight operation.
                sent.append(op)
                await self.patches.apply(op)
            if not self._current(brief, world):
                await rollback()
                return None, ["знание или ветка изменились"]
        except asyncio.CancelledError:
            if sent:
                await asyncio.shield(rollback())
            raise
        s = Storylet(
            base,
            brief.lie.key,
            brief.npc,
            brief.name,
            p.close,
            to,
            story.state.name(to),
            p.gold,
            p.title,
            talk["id"],
            pay["id"],
            pay,
            p.description,
        )
        self.storylets[s.id] = s
        self.chronicle.record({"fact": "amends_open", **asdict(s), "reason": p.reason})
        self.journal.add("amends_open", npc=s.npc, name=s.name, lie=s.lie, close=s.close, gold=s.gold)
        if self.dialogue is not None:
            what = "дело" if isinstance(brief.lie, CrimeCase) else "ложь"
            closed = " Твои дела для него закрыты до расплаты." if s.close else ""
            self.dialogue.remember(
                s.npc,
                s.name,
                f"Ты требуешь загладить вину за {what} ({brief.lie.words()}). Расплата: {_price(s)}.{closed}",
            )
        log.info("amends: %s opened (%s): %s", s.id, ", ".join(s.close), p.reason)
        return s, []

    async def _told(self, s: Storylet) -> None:
        """The talk is over: the amends are offered."""
        world = self._world
        if self._loading:
            return
        if s.lie.startswith("crime:") and self.crimes is not None and not self.active_crime(s.npc, s.lie[6:]):
            await self._withdraw_crimes(world)
            return
        s.stage = "told"
        try:
            await self.patches.apply({"op": "add_node", "node": s.pay_node})
        except GameError as err:
            log.warning("amends: %s: %s", s.id, err)
            return
        if world == self._world and not self._loading:
            self.chronicle.record({"fact": "amends_told", "id": s.id})

    async def _withdraw_crimes(self, world: int) -> None:
        """A silenced or dead victim no longer has a native important demand waiting."""
        for s in list(self.storylets.values()):
            if world != self._world:
                return
            if not s.lie.startswith("crime:") or s.stage == "done" or self.active_crime(s.npc, s.lie[6:]):
                continue
            s.stage = "done"
            for node in (s.talk, s.pay):
                if world != self._world:
                    return
                if node in self.patches_nodes():
                    await self._apply({"op": "remove_node", "node": node})
            for closed in s.close:
                if world != self._world:
                    return
                await self._apply({"op": "enable", "node": closed})
            if world == self._world:
                self.chronicle.record({"fact": "amends_withdrawn", "id": s.id})

    async def _paid(self, s: Storylet) -> None:
        """The hero made amends: the npc's dialogs open again; so do those of the others asking the same of
        him for the same lie (one confession to Lobart is enough for Hilda too)."""
        world = self._world
        await self._settle(s, paid=True, world=world)
        for other in list(self.storylets.values()):
            if world != self._world or self._loading:
                return
            if other.lie == s.lie and other.to == s.to and other.stage != "done" and other.gold <= s.gold:
                for node in (other.talk, other.pay):
                    if world != self._world or self._loading:
                        return
                    if node in self.patches_nodes():
                        await self._apply({"op": "remove_node", "node": node})
                await self._settle(other, world=world)

    async def _forgiven(self, f: Fact) -> None:
        """The npc forgave a grievance in a deal: the storylets that it asked for, or
        that were to be made at it, are settled; their dialogs of the talk and the amends leave the menu."""
        world = self._world
        npc = f.get("npc", "")
        keys = [f"crime:{f['crime']}"] if f.get("fact") == "forgiven" else f.get("forgive", [])
        for key in keys:
            if key.startswith("crime:") and f.get("fact") != "forgiven":
                continue  # wait for the deal's actual transfer and its canonical forgiven fact
            for s in list(self.storylets.values()):
                if world != self._world or self._loading:
                    return
                if s.lie != key or s.stage == "done" or npc not in (s.npc, s.to):
                    continue
                for node in (s.talk, s.pay):
                    if world != self._world or self._loading:
                        return
                    if node in self.patches_nodes():
                        await self._apply({"op": "remove_node", "node": node})
                if world != self._world or self._loading:
                    return
                s.gold = int(f.get("gold", 0))  # what was agreed on in the end
                if f.get("fact") == "forgiven":
                    s.gold = sum(
                        int(item.get("count", 0))
                        for item in f.get("hero_gives", [])
                        if item.get("item") == "ITMI_GOLD"
                    )
                await self._settle(s, world=world)

    def patches_nodes(self) -> set[str]:
        return {op["node"]["id"] for op in self.patches.ops() if op.get("op") == "add_node"}

    async def _apply(self, op: dict[str, Any]) -> None:
        try:
            await self.patches.apply(op)
        except GameError as err:
            log.warning("amends: %s: %s", op, err)

    async def _settle(self, s: Storylet, *, paid: bool = False, world: int | None = None) -> None:
        world = self._world if world is None else world
        if world != self._world or self._loading:
            return
        s.stage = "done"
        for c in s.close:
            if world != self._world or self._loading:
                return
            await self._apply({"op": "enable", "node": c})
        if world != self._world or self._loading:
            return
        lie = self.lies.get(s.lie)
        fact = {"fact": "amends", "id": s.id, "npc": s.npc, "to": s.to, "gold": s.gold, "lie": s.lie}
        # Saved storylets remain payable when --no-crimes disables new crime triggers.
        crime = s.lie.removeprefix("crime:") if s.lie.startswith("crime:") else ""
        if crime:
            fact["crime"] = crime
            if paid:
                self.chronicle.record(
                    {
                        "fact": "forgiven",
                        "npc": lie.victim if isinstance(lie, CrimeCase) else s.npc,
                        "crime": crime,
                        "paid": True,
                        "gold": s.gold,
                        "amends": s.id,
                    }
                )
        elif lie is not None:
            fact.update(quest=lie.quest, about=lie.victim)
        self.chronicle.record(fact)
        self.journal.add("amends", npc=s.npc, name=s.name, to=s.to, gold=s.gold)
        if self.dialogue is not None:
            self.dialogue.remember(
                s.npc, s.name, f"Герой загладил вину ({_price(s)}). Твои дела для него снова открыты"
            )
            if s.to != s.npc:
                paid = f" и заплатил тебе {s.gold} золотых" if s.gold else ""
                self.dialogue.remember(s.to, s.to_name, f"Герой пришёл к тебе повиниться за ложь{paid}")

    # --- the save branch --------------------------------------------------------------------

    def _load(self) -> None:
        self.lies.clear()
        self.storylets.clear()
        self.looked.clear()
        while not self._queue.empty():
            self._queue.get_nowait()
        for f in self.chronicle.facts():
            match f.get("fact"):
                case "crime" if self.crimes is not None:
                    case = self._crime_case(f)
                    self.lies.setdefault(case.key, case)
                case "found_out" | "caught":
                    lie = Lie(
                        f.get("quest", ""),
                        f["fact"],
                        f.get("npc", ""),
                        f.get("name") or f.get("npc", ""),
                        f.get("title") or f.get("quest", ""),
                        self._reward(f.get("quest", "")),
                        said=bool(f.get("said")),
                        witness_name=str(f.get("witness_name", "")),
                    )
                    self.lies.setdefault(lie.key, lie)
                case "amends_open":
                    fields = {k: f[k] for k in Storylet.__dataclass_fields__ if k in f}
                    s = Storylet(**fields)
                    self.storylets[s.id] = s
                    self.looked.setdefault(s.lie, []).append(s.npc)
                case "amends_none" | "amends_rejected":
                    looked = self.looked.setdefault(f.get("lie", ""), [])
                    if f.get("npc") not in looked:
                        looked.append(f.get("npc", ""))
                case "amends_told" if f.get("id") in self.storylets:
                    self.storylets[f["id"]].stage = "told"
                case "amends" | "amends_withdrawn" if f.get("id") in self.storylets:
                    self.storylets[f["id"]].stage = "done"
        if self.crimes is not None:
            for crime in self.crimes.facts():
                self._queue_crime(crime.get("victim") or crime.get("owner", ""), crime["id"])

    def _reward(self, quest: str) -> str:
        """What the hero took for the lie, from the chronicle."""
        for f in reversed(self.chronicle.facts("claim")):
            if f.get("quest") == quest:
                return str(f.get("reward", ""))
        return ""

    def _spawn(self, coro: Any) -> None:
        task = asyncio.create_task(coro)
        self._background.add(task)
        task.add_done_callback(self._background.discard)

    # --- the conversation (TurnExtension) ------------------------------------------------------

    async def prepare(self, npc: str) -> None:
        pass

    def situation(self, npc: str) -> str | None:
        npc = npc.upper()
        lines = []
        for s in self.storylets.values():
            names = [self.graph.nodes[c].description for c in s.close if c in self.graph.nodes]
            what = "дело" if s.lie.startswith("crime:") else "ложь"
            if s.npc == npc and s.stage != "done":
                closed = (
                    f" Пока не заплатит, для него закрыто: {'; '.join(f'«{n}»' for n in names)}."
                    if names
                    else ""
                )
                lines.append(
                    f"Ты требуешь от героя загладить вину за {what} ({s.lie}): {_price(s)}.{closed} "
                    "Договоритесь — простишь в сделке (forgive)."
                )
            elif s.npc == npc:
                lines.append(f"Герой загладил перед тобой вину за {what} ({_price(s)}).")
            elif s.to == npc and s.stage != "done":
                lines.append(
                    f"{s.name} требует, чтобы герой повинился перед тобой и заплатил тебе {s.gold} золотых. "
                    f"Повинится — простишь ему эту ложь ({s.lie}) в сделке (forgive)."
                )
        return "\n".join(lines) or None

    all_tools: tuple[Tool, ...] = ()

    def tools(self, npc: str) -> list[Tool]:
        return []

    async def handle(
        self, dialogue: DialogueManager, conv: Conversation, call: ToolCall, snap: Snapshot | None
    ) -> ToolOutcome | None:
        return None


def _stake(node: Node, graph: StoryGraph, seen: set[str] | None = None) -> str | None:
    """What of STAKES the dialog (or a choice of it) gives the hero."""
    seen = seen if seen is not None else set()
    if node.id in seen:
        return None
    seen.add(node.id)
    if node.trade:
        return "trade"
    found = None
    for e in node.effects:
        if e["do"] == "set_guild":
            return "join"
        if e["do"] == "call" and str(e.get("fn", "")).startswith("B_TEACH"):
            found = found or "teach"
        if e["do"] == "set" and str(e.get("var", "")).startswith("MIS_") and _value(e.get("value")) == 1:
            found = found or "work"
        if e["do"] == "choice" and (c := graph.nodes.get(e.get("node", ""))) is not None:
            found = _stake(c, graph, seen) or found
    return found


def _value(v: Any) -> int | None:
    if isinstance(v, int):
        return v
    return int(v[1]) if isinstance(v, list) and v[:1] == ["int"] else None


def _lines(lines: list[dict[str, str]]) -> list[dict[str, str]]:
    return [
        {"speaker": "other" if x.get("speaker") == "hero" else "self", "text": x.get("text", "")}
        for x in lines
    ]


def _price(s: Storylet) -> str:
    to = "" if s.to == s.npc else f" — {s.to_name}"
    return f"{s.gold} золотых{to}" if s.gold else f"повиниться{to}"


def _how(how: str, lie: Lie | CrimeCase) -> str:
    if isinstance(lie, CrimeCase):
        return how
    if how == "lied":
        return "его самого обманули" if lie.kind == "found_out" else "он сам уличил героя"
    return f"слух, рассказал {how}" if how else "слух"
