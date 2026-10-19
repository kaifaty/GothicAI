"""The director's decision: budget -> occasions -> proposal -> canon -> builders -> chronicle.

Only the queue and an in-flight decision live in memory. Lines, partial casting and both clocks belong
to the save branch, so loading restores the same budget without replaying a builder.
"""

from __future__ import annotations

import asyncio
import json
import logging
import time
from collections.abc import Callable
from dataclasses import dataclass, field
from typing import Any

from pydantic import BaseModel, ConfigDict, Field, ValidationError

from .attitudes import non_hostile
from .authoring import Author
from .casting import Casting
from .chains import ChainAuthor
from .chronicle import Fact
from .crime_law import CrimeLaw
from .crimes import Crimes
from .game import GameError
from .gamedata.storygraph import QUEST_STATES
from .llm import LLMBackend, LLMError, Message, Tool, ToolCall, open_ahead
from .occasions import DEEDS, NEAREST, OccasionFailed, Occasions
from .people_knowledge import rumor_subject
from .protocol import GameMessage, Loading, Session
from .questbook import Ground, _vanilla

log = logging.getLogger(__name__)

PROPOSE = "propose_line"
CACHE_KEY = "showrunner"
ATTEMPTS = 2
MAX_OPEN_LINES = 2
MAX_LINES_PER_CHAPTER = 4
MAX_PEOPLE_PER_CHAPTER = 8
MIN_GAME_MINUTES = 24 * 60
MIN_REAL_SECONDS = 15 * 60
NEAR_CM = 5000
MAX_PLACES = 6
RECENT_DEEDS = 10
CLOSED = {"done", "failed", "obsolete", "closed"}
OUTCOMES = {"line", "line_none", "line_skipped", "line_failed", "line_rejected"}

INSTRUCTIONS = """\
Ты — режиссёр-автор Gothic II. По поводу из сводки реши, нужна ли сейчас новая линия сюжета.
Пиши в духе оригинальной игры: простые человеческие беды, грубоватые люди, без пафоса и современных слов.
Не повторяй темы и людей прежних линий. Учитывай текущие квесты, главу и остаток бюджета.
Ответ — один вызов propose_line:
- kind: quest — небольшое поручение; chain — цепочка из 2–3 квестов; stranger — новый человек и его
  поручение; none — сейчас ничего не нужно.
- giver: id живого несюжетного заказчика из кандидатов для quest/chain, иначе пустая строка.
- place: id места из сводки для stranger, иначе пустая строка.
- new_people: для quest и chain нужны новые люди (включая бандитов); false — только существующие.
  Для stranger код создаёт одного заказчика, дополнительные люди не нужны: new_people=false.
- premise: 2–3 фразы, что случилось и почему именно сейчас. Это замысел для строителя квестов.
- follows: переменная квеста скриптов из сводки, на который отвечает линия; иначе пустая строка.
- crime: id дела из сводки, на которое отвечает линия; иначе пустая строка.
- requires: список условий открытия, иначе []. Условия: {kind: quest, quest: переменная, state:
  running|success|failed}, {kind: knows, node: id ванильного диалога}, {kind: chapter, chapter: 1–6}.
- reason: почему такая линия или почему сейчас не нужна.
На повод vanilla ответь на само событие: сосед заметил поступок героя и просит о своём. Если нужна линия,
укажи follows этого квеста и requires на его состояние. Заказчик — не тот, кто выдаёт или
закрывает этот квест скриптов: они перечислены в сводке. Не продолжай и не заканчивай работу скриптов,
не обещай их наград, не требуй их квестовых вещей. Опирайся на записи журнала, которые уже видел герой,
а не на будущие развязки. Предпочитай небольшое дело рядом, доступное при текущем ванильном состоянии.
Дела героя: ты знаешь правду, люди — только своё знание из сводки. Знание режиссёра не передавай людям.
Пострадавший может попросить вернуть вещь или найти вора, если знает о деле или заметил пропажу.
Пострадавший от кражи может заметить пропажу, не зная вора. Вором может быть сам герой: он вправе
«найти» свою добычу, соврать, вернуть или не вернуть. Ложь не запрещай. Возврат вещи сам по себе
не означает прощения: для этого нужна отдельная сделка. У линии про дело укажи crime; её заказчик
должен знать о деле или быть пострадавшим от кражи. Свидетель решает свою заботу в разговоре:
рассказать, промолчать или потребовать плату за молчание; ради этого отдельную линию не создавай.
Не каждому делу нужна линия: none с причиной — обычный ответ, если сейчас нечего добавить.
Людей и места выбирай только из сводки. При нехватке бюджета на людей выбери quest или chain без новых
людей или none. Отказ кода исправь, не повторяй его."""


class Proposal(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True)

    kind: str
    giver: str = ""
    place: str = ""
    new_people: bool = False
    premise: str = ""
    reason: str = ""
    follows: str = ""
    crime: str = ""
    requires: list[dict[str, Any]] = Field(default_factory=list)

    @classmethod
    def of(cls, raw: dict[str, Any]) -> Proposal:
        return cls.model_validate(raw)


PROPOSE_TOOL = Tool(
    PROPOSE,
    "Решить, нужна ли новая линия, выбрать её вид, заказчика и замысел.",
    {
        "type": "object",
        "properties": {
            "kind": {"type": "string", "enum": ["quest", "chain", "stranger", "none"]},
            "giver": {"type": "string"},
            "place": {"type": "string"},
            "new_people": {
                "type": "boolean",
                "description": "Для quest и chain нужны новые люди, включая бандитов; для stranger false.",
            },
            "premise": {"type": "string"},
            "reason": {"type": "string"},
            "follows": {"type": "string"},
            "crime": {"type": "string"},
            "requires": {
                "type": "array",
                "items": {
                    "anyOf": [
                        {
                            "type": "object",
                            "properties": {
                                "kind": {"type": "string", "enum": ["quest"]},
                                "quest": {"type": "string"},
                                "state": {"type": "string", "enum": ["running", "success", "failed"]},
                            },
                            "required": ["kind", "quest", "state"],
                            "additionalProperties": False,
                        },
                        {
                            "type": "object",
                            "properties": {
                                "kind": {"type": "string", "enum": ["knows"]},
                                "node": {"type": "string"},
                            },
                            "required": ["kind", "node"],
                            "additionalProperties": False,
                        },
                        {
                            "type": "object",
                            "properties": {
                                "kind": {"type": "string", "enum": ["chapter"]},
                                "chapter": {"type": "integer"},
                            },
                            "required": ["kind", "chapter"],
                            "additionalProperties": False,
                        },
                    ]
                },
            },
        },
        "required": [
            "kind", "giver", "place", "new_people", "premise", "reason", "follows", "crime", "requires"
        ],
        "additionalProperties": False,
    },
)


def minutes(at: Fact) -> int:
    return int(at["day"]) * 1440 + int(at["hour"]) * 60 + int(at["minute"])


def occasion_members(occasion: Fact) -> list[Fact]:
    """The original facts, independent of which one led the decision."""
    return [{k: v for k, v in occasion.items() if k != "also"}, *occasion.get("also", [])]


@dataclass
class Budget:
    open_left: int
    lines_left: int
    people_left: int
    busy_givers: set[str]
    game_wait: int
    real_wait: float
    reasons: list[str]

    def text(self) -> str:
        return (
            f"Остаток бюджета: открытых линий {self.open_left}, линий за главу {self.lines_left}, "
            f"новых людей за главу {self.people_left} (каждый бандит отдельно). "
            f"Ждать: {self.game_wait} игровых минут, {self.real_wait:.0f} секунд по часам."
        )


@dataclass
class Brief:
    occasion: Fact
    chapter: int
    day: int
    hour: int
    minute: int
    places: dict[str, str]
    residents: list[str]
    candidates: dict[str, str]
    scripted: list[str]
    quests: list[str]
    deeds: list[Fact]
    vanilla: list[Fact]
    history: list[str]
    budget: Budget
    crimes: dict[str, Fact] = field(default_factory=dict)

    def text(self) -> str:
        leading, *others = occasion_members(self.occasion)
        at = minutes({"day": self.day, "hour": self.hour, "minute": self.minute})
        also = [
            {
                **f,
                "давность в игровых минутах": max(0, at - minutes(f)),
            }
            for f in others
        ]
        return "\n".join(
            [
                "Повод (вид, место, время; deed — поступок героя; vanilla — событие квеста скриптов): "
                + json.dumps(leading, ensure_ascii=False),
                *(["Заодно: " + json.dumps(also, ensure_ascii=False)] if also else []),
                f"Глава {self.chapter}, день {self.day}, {self.hour:02}:{self.minute:02}.",
                "Место повода и места рядом (id — название и вид):",
                *[f"- {p} — {s}" for p, s in self.places.items()],
                "Кто здесь живёт (карточки написаны самим людям):",
                *[f"- {s}" for s in self.residents],
                "Кандидаты в заказчики (id — имя и характер):",
                *[f"- {n} — {s}" for n, s in self.candidates.items()],
                "Квесты скриптов (running — открытые; также квесты последних событий):",
                *[f"- {s}" for s in self.scripted],
                "Открытые квесты режиссёра (тема, состояние, стадия):",
                *[f"- {s}" for s in self.quests],
                "Последние поступки героя: " + json.dumps(self.deeds, ensure_ascii=False),
                *(
                    [
                        "Дела героя (правда режиссёра отдельно от знания людей; unknown — не знает):",
                        json.dumps(self.crimes, ensure_ascii=False),
                    ]
                    if self.crimes else []
                ),
                "Последние ванильные события: " + json.dumps(self.vanilla, ensure_ascii=False),
                "Что режиссёр уже сделал на ветке, не повторяться:",
                *[f"- {s}" for s in self.history],
                self.budget.text(),
            ]
        )


@dataclass
class _Build:
    proposal: Proposal
    brief: Brief
    before_people: set[str]
    before_quests: set[str]
    giver: str
    outcome: Fact | None = None


class ShowrunnerModel:
    def __init__(self, llm: LLMBackend) -> None:
        self.llm = llm

    async def propose(
        self, brief: Brief, previous: dict[str, Any] | None = None, errors: list[str] | None = None
    ) -> dict[str, Any] | None:
        messages = [Message("user", brief.text())]
        if previous is not None:
            messages.append(
                Message(
                    "user",
                    "Твоё предложение: "
                    + json.dumps(previous, ensure_ascii=False)
                    + "\nОтклонено:\n"
                    + "\n".join(f"- {e}" for e in errors or [])
                    + "\nИсправь его.",
                )
            )
        calls = []
        try:
            async for ev in self.llm.stream(
                instructions=INSTRUCTIONS, messages=messages, tools=[PROPOSE_TOOL], cache_key=CACHE_KEY
            ):
                if isinstance(ev, ToolCall) and ev.name == PROPOSE:
                    calls.append(ev)
        except LLMError as error:
            log.warning("showrunner: model failed: %s", error)
            return None
        return calls[0].arguments if len(calls) == 1 else {}


class Showrunner:
    def __init__(
        self,
        occasions: Occasions,
        author: Author,
        chains: ChainAuthor,
        casting: Casting,
        llm: LLMBackend,
        *,
        clock: Callable[[], float] = time.time,
        crimes: Crimes | None = None,
        law: CrimeLaw | None = None,
    ) -> None:
        self.occasions, self.author, self.chains, self.casting = occasions, author, chains, casting
        self.link, self.chronicle, self.places = occasions.link, occasions.chronicle, occasions.places
        self.book, self.rules = author.book, author.rules
        self.model, self.clock = ShowrunnerModel(llm), clock
        self.crimes, self.law = crimes, law
        self._lock = asyncio.Lock()
        self._generation = 0
        self._work: asyncio.Task[Fact | None] | None = None
        self._building: _Build | None = None
        self._budget_state: Fact | None = None
        self._budget_checked = float("-inf")
        self._budget_reason: str | None = None
        self.link.listeners.append(self._on_message)

    def _on_message(self, message: GameMessage) -> None:
        if isinstance(message, (Session, Loading)):
            if isinstance(message, Loading) and self._building is not None:
                # Loading still belongs to the old branch. Cancellation runs asynchronously and may
                # only be delivered after Session has switched the chronicle and the builders.
                self._finish_build(self._building, ["постройка отменена загрузкой"])
            self._generation += 1
            self._budget_state = None
            self._budget_checked = float("-inf")
            self._budget_reason = None
            if self._work is not None:
                self._work.cancel()

    @property
    def lines(self) -> list[Fact]:
        return self.chronicle.facts("line")

    def is_open(self, line: Fact) -> bool:
        return any(self.book.progress(q).state not in CLOSED for q in line.get("quests", []))

    def budget(self, chapter: int, at: Fact) -> Budget:
        # A failed builder may already have cast people or booked quests. Count those receipts too.
        attempts = [f for f in self.chronicle.facts() if f.get("fact") in {"line", "line_failed"}]
        lines = [f for f in attempts if f.get("fact") == "line" or f.get("quests")]
        opened = [f for f in lines if self.is_open(f)]
        chapter_lines = [f for f in lines if f.get("chapter") == chapter]
        people = {n for f in attempts if f.get("chapter") == chapter for n in f.get("characters", [])}
        last = lines[-1] if lines else None
        game_wait = max(0, MIN_GAME_MINUTES - (minutes(at) - minutes(last))) if last else 0
        real_wait = max(0, MIN_REAL_SECONDS - (self.clock() - last["wall_time"])) if last else 0
        reasons = []
        if len(opened) >= MAX_OPEN_LINES:
            reasons.append(f"открытых линий уже {len(opened)}, предел {MAX_OPEN_LINES}")
        if len(chapter_lines) >= MAX_LINES_PER_CHAPTER:
            reasons.append(f"линий за главу уже {len(chapter_lines)}, предел {MAX_LINES_PER_CHAPTER}")
        if game_wait:
            reasons.append(f"рано: между линиями нужны игровые сутки, ещё {game_wait} минут")
        if real_wait:
            reasons.append(f"рано: между линиями нужны 15 минут по часам, ещё {real_wait:.0f} секунд")
        return Budget(
            max(0, MAX_OPEN_LINES - len(opened)),
            max(0, MAX_LINES_PER_CHAPTER - len(chapter_lines)),
            max(0, MAX_PEOPLE_PER_CHAPTER - len(people)),
            {f["giver"] for f in opened},
            game_wait,
            real_wait,
            reasons,
        )

    async def _state(self) -> Fact:
        return await self.link.story_state(vars=["KAPITEL", *self.author.graph.quests])

    async def _ready(self) -> bool:
        """Keep queued occasions until the budget opens, sampling game time once per poll."""
        if self._lock.locked():
            return False
        now = self.occasions.clock()
        if now - self._budget_checked >= self.occasions.poll_s:
            generation = self._generation
            self._budget_checked = now
            self._budget_state = None
            state = await self._state()
            if generation != self._generation:
                return False
            self._budget_state = state
        if self._budget_state is None:
            return False
        state = self._budget_state
        budget = self.budget(int(state.get("vars", {}).get("KAPITEL") or 1), state)
        # Real time and open quest progress can change without another bridge request.
        reason = "; ".join(budget.reasons)
        if reason and not self._budget_reason:
            log.info("showrunner waiting for budget: %s", reason)
        self._budget_reason = reason
        return not budget.reasons

    async def brief(self, occasion: Fact, budget: Budget | None = None) -> Brief:
        state = await self._state()
        chapter = int(state.get("vars", {}).get("KAPITEL") or 1)
        return await self._brief(occasion, budget or self.budget(chapter, state), state)

    async def _brief(self, occasion: Fact, budget: Budget, state: Fact) -> Brief:
        place = self.places.get(occasion.get("place") or "")
        nearby = {place.id: place} if place is not None else {}
        if place is not None and place.center is not None:
            nearby.update({p.id: p for p, _ in self.places.near(place.center, NEAR_CM, MAX_PLACES)})
        world = {p["npc"]: p for p in await self.link.story_people()}
        # Own characters are residents too, including a newly introduced person before the next poll.
        own = self.casting.cast.characters()
        for spec in own:
            world.setdefault(
                spec.npc,
                {
                    "npc": spec.npc,
                    "name": spec.name,
                    "routine": [e.model_dump() for e in spec.routine] or [{"waypoint": spec.at}],
                },
            )
        residents = {
            n
            for n, person in world.items()
            if any(
                (p := self.places.of(e.get("waypoint"))) is not None and p.id in nearby
                for e in person.get("routine", [])
            )
        }
        names = sorted(residents | set(occasion.get("people", [])) | {s.npc for s in own})
        context = next(iter(world), "PC_HERO")
        [hero] = await self.link.story_eval(context, [["inst", "HERO"]])
        live = (await self.link.story_state(npcs=names, near=hero)).get("npcs", {})
        own_cards = self.casting.cast.cards()

        def description(npc: str) -> str:
            name = live.get(npc, {}).get("name") or world.get(npc, {}).get("name") or npc
            card = self.author.cards.get(npc) or own_cards.get(npc)
            return str(name) + (f"; {card.ru}" if card else "")

        candidates = {
            n: description(n)
            for n in names
            if live.get(n, {}).get("state") == "alive"
            and live[n].get("distance", float("inf")) <= NEAR_CM
            and not self.rules.plot_npc(n)
            and n not in budget.busy_givers
        }
        peaceful = await non_hostile(self.link, candidates, context=context)
        candidates = {n: description for n, description in candidates.items() if n in peaceful}
        quests = []
        for q in self.book.all().values():
            progress = self.book.progress(q.id)
            if progress.state not in CLOSED:
                quests.append(f"{q.id}: «{q.title}», {progress.state}, стадия {progress.stage}")
        facts = self.chronicle.facts()
        vanilla = [f for f in facts if f.get("fact") == "vanilla_quest"][-RECENT_DEEDS:]
        event_quests = {
            f.get("quest") for f in [*vanilla, *(o.get("vanilla", {}) for o in occasion_members(occasion))]
        }
        scripted = []
        for var, q in self.author.graph.quests.items():
            value = state.get("vars", {}).get(var)
            if value != 1 and var not in event_quests:
                continue
            givers = {
                self.author.graph.nodes[n].npc
                for n in q.running
                if n in self.author.graph.nodes and self.author.graph.nodes[n].npc
            }
            giver = description(next(iter(givers))) if len(givers) == 1 else "не единственный"
            people = self._script_people(var)
            scripted.append(
                f"{var}: «{q.title}», {QUEST_STATES.get(value, 'open')}; заказчик: {giver}; "
                f"выдают или закрывают (не могут быть заказчиками ответа): {', '.join(sorted(people))}; "
                "известный герою журнал: " + json.dumps(self._told(var), ensure_ascii=False)
            )
        deeds = [
            f
            for f in facts
            if (
                f.get("fact") in DEEDS
                and (f.get("fact") != "crime" or self.crimes is not None)
            )
            or (
                f.get("fact") == "died"
                and f.get("killer") == "PLAYER"
                and not (
                    self.crimes is not None
                    and any(
                        c.get("kind") == "murder" and c.get("victim") == f.get("npc")
                        for c in self.crimes.facts()
                    )
                )
            )
        ][-RECENT_DEEDS:]
        crimes = self._crime_briefs(occasion, facts)
        history = []
        line_quests = set()
        for f in facts:
            if f.get("fact") in {"line", "line_failed"}:
                line_quests.update(f.get("quests", []))
                titles = [self.book.all()[q].title for q in f.get("quests", []) if q in self.book.all()]
                history.append(
                    f"{f.get('kind')}, заказчик {f.get('giver')}: {', '.join(titles)}; "
                    f"замысел {f.get('premise', '')}; персонажи {', '.join(f.get('characters', []))}"
                )
        history += [
            f"Квест {q.id}: «{q.title}», заказчик {q.giver}"
            for q in self.book.all().values()
            if q.id not in line_quests
        ]
        history += [f"Прежнее поручение «{q.title}», заказчик {q.npc}" for q in self.author.old.values()]
        history += [f"Персонаж {s.npc}: {s.name}" for s in self.casting.cast.characters()]
        return Brief(
            occasion,
            int(state.get("vars", {}).get("KAPITEL") or 1),
            state["day"],
            state["hour"],
            state["minute"],
            {p.id: f"{p.ru}; {p.kind} ({p.kind_ru})" for p in nearby.values()},
            [f"{n} — {description(n)}" for n in sorted(residents)],
            candidates,
            list(dict.fromkeys(scripted)),
            quests,
            deeds,
            vanilla,
            history,
            budget,
            crimes,
        )

    def _crime_briefs(self, occasion: Fact, facts: list[Fact]) -> dict[str, Fact]:
        """A read-only view: omniscient truth, npc beliefs and the existing law's projection."""
        if self.crimes is None:
            return {}
        ids = {c["id"] for c in self.crimes.facts()[-RECENT_DEEDS:]}
        ids.update(o["crime"] for o in occasion_members(occasion) if o.get("crime"))
        deliveries = self.law.deliveries() if self.law is not None else []
        out = {}
        for crime in self.crimes.facts():
            cid = crime["id"]
            if cid not in ids:
                continue
            victim = crime.get("victim") or crime.get("owner") or ""
            witnesses = set(crime.get("witnesses", []))
            people = {victim, *witnesses} - {""}
            for fact in facts:
                if fact.get("fact") == "rumor" and rumor_subject(fact) != "PLAYER":
                    continue
                if fact.get("crime") == cid:
                    people.update(n for n in (fact.get("npc"), fact.get("to")) if n)
                elif cid in self.crimes.testimony_crimes(fact):
                    if fact.get("npc"):
                        people.add(fact["npc"])
            knowledge = []
            for npc in sorted(people):
                role = "victim" if npc == victim else "witness" if npc in witnesses else "informed"
                known = [k for k in self.crimes.known(npc) if k.crime["id"] == cid]
                if not known:
                    knowledge.append({"npc": npc, "role": role, "degree": "unknown"})
                for k in known:
                    knowledge.append(
                        {
                            "npc": npc, "role": role, "degree": k.degree, "source": k.source,
                            "hops": k.hops, "suspect": k.suspect,
                            "forgiven": k.forgiven, "silenced": k.silenced,
                        }
                    )
            community = crime.get("community")
            out[cid] = {
                "truth": crime,
                "description": self.crimes.describe(crime),
                "knowledge": knowledge,
                "victim_can_notice_missing": crime.get("kind") == "theft" and bool(victim),
                "law": {
                    "community": community,
                    "known": any(
                        f.get("crime") == cid and f.get("community") == community
                        for f in facts if f.get("fact") == "crime_known"
                    ),
                    "paid": any(
                        cid in f.get("crimes", []) and f.get("community") == community
                        for f in facts if f.get("fact") == "absolved"
                    ),
                    "deliveries": [
                        {"via": d.via, "due": d.due, "status": d.status, "reason": d.reason}
                        for d in deliveries if d.crime["id"] == cid
                    ],
                    "settlements": [
                        f for f in facts if f.get("fact") == "forgiven" and f.get("crime") == cid
                    ],
                },
            }
        return out

    def _told(self, var: str) -> list[str]:
        # As in the repair director: the running quest's journal, without its future endings.
        q = self.author.graph.quests[var]
        told = []
        for node in sorted(q.running):
            for effect in self.author.graph.nodes[node].effects:
                if (
                    effect["do"] == "journal_entry"
                    and effect.get("topic") in q.topics
                    and effect.get("text") not in told
                ):
                    told.append(effect["text"])
        return told[:4]

    def _script_people(self, var: str) -> set[str]:
        q = self.author.graph.quests[var]
        return {
            self.author.graph.nodes[n].npc
            for n in q.running | q.success | q.failed | q.obsolete
            if n in self.author.graph.nodes and self.author.graph.nodes[n].npc
        }

    def canon(self, p: Proposal, brief: Brief) -> list[str]:
        errors = []
        if p.kind not in {"quest", "chain", "stranger", "none"}:
            errors.append(f"неизвестный вид линии {p.kind!r}")
        if p.kind in {"quest", "chain"}:
            if p.giver not in brief.candidates:
                errors.append(f"заказчика {p.giver!r} нет в кандидатах сводки")
            if self.rules.plot_npc(p.giver):
                errors.append(f"{p.giver} — сюжетный NPC, его не трогаем")
        if p.kind == "stranger" and p.place not in brief.places:
            errors.append(f"места {p.place!r} нет в сводке")
        if p.follows:
            if p.follows not in self.author.graph.quests:
                errors.append(f"follows {p.follows!r} — не квест ванильного графа")
            elif p.giver.upper() in self._script_people(p.follows):
                errors.append(
                    f"{p.giver} выдаёт или закрывает квест скриптов {p.follows}: "
                    "выбери соседа, линия отвечает рядом"
                )
        if p.crime:
            crime = brief.crimes.get(p.crime)
            if crime is None:
                errors.append(f"crime {p.crime!r} — нет такого дела в сводке")
            elif p.kind != "none":
                victim = crime["truth"].get("victim") or crime["truth"].get("owner")
                knows = any(
                    k["npc"] == p.giver.upper() and k["degree"] != "unknown"
                    for k in crime["knowledge"]
                )
                notices = crime["truth"].get("kind") == "theft" and victim == p.giver.upper()
                if p.kind not in {"quest", "chain"} or not (knows or notices):
                    errors.append(
                        f"{p.giver or 'новый человек'} не знает о деле {p.crime}: заказчик должен "
                        "знать о деле или быть пострадавшим от кражи, который заметит пропажу"
                    )
        elif p.kind != "none" and brief.occasion.get("crime") and self.crimes is not None:
            errors.append("линия по поводу дела должна указать его id в crime")
        if p.kind != "none":
            errors += brief.budget.reasons
            if not p.premise.strip():
                errors.append("нужен замысел режиссёра: premise")
            needs_people = p.kind == "stranger" or (p.kind in {"quest", "chain"} and p.new_people)
            if needs_people and not brief.budget.people_left:
                errors.append("исчерпан бюджет новых людей за главу")
        return errors

    async def _requirements(self, p: Proposal) -> list[str]:
        if not p.requires:
            return []
        # The book remains the authority, including reachability in the current vanilla graph.
        ground = Ground({}, {}, self.rules.plot_npc, lambda _: False, 0, 0)
        ground.story = await self.author.director.story()
        return [
            f"условие открытия {i}: {error}"
            for i, requirement in enumerate(p.requires, 1)
            for error in _vanilla(requirement, ground)
        ]

    async def run(self) -> None:
        while True:
            occasion = None
            generation = self._generation
            try:
                occasion = await self.occasions.next(ready=self._ready)
                generation = self._generation
                await self.decide(occasion, wait_budget=True)
            except asyncio.CancelledError:
                raise
            except Exception as error:
                log.exception("showrunner: unexpected failure")
                if isinstance(error, OccasionFailed):
                    occasion, error = error.occasion, error.error
                if (
                    occasion is not None
                    and generation == self._generation
                    and not any(f.get("occasion") == occasion for f in self.chronicle.facts("line_failed"))
                ):
                    self._record("line_failed", occasion, why=[self._unexpected(error)])
                # A failure with no queued occasion can happen before handoff. Avoid spinning while
                # waiting for a usable world; an associated occasion has already been consumed.
                await asyncio.sleep(self.occasions.busy_poll_s)

    @staticmethod
    def _unexpected(error: Exception) -> str:
        return f"неожиданная ошибка {type(error).__name__}: {error}"

    async def pitch(self) -> Fact | None:
        """An occasion now at the hero's actual place, independently of the queue."""
        generation = self._generation
        state = await self._state()
        people = await self.link.story_people()
        context = people[0]["npc"] if people else "PC_HERO"
        waypoint, hero = await self.link.story_eval(context, [NEAREST, ["inst", "HERO"]])
        live = await self.link.story_state(npcs=[p["npc"] for p in people], near=hero)
        if generation != self._generation:
            return None
        place = self.places.of(waypoint)
        occasion = {
            "fact": "occasion",
            "kind": "now",
            "place": place.id if place else None,
            "waypoint": waypoint,
            "wall_time": self.clock(),
            "people": [
                n
                for n, s in live.get("npcs", {}).items()
                if s.get("state") == "alive" and s.get("distance", float("inf")) <= NEAR_CM
            ],
            **{k: state[k] for k in ("day", "hour", "minute")},
        }
        self.chronicle.record(occasion)
        return await self.decide(occasion)

    async def decide(self, occasion: Fact, *, wait_budget: bool = False) -> Fact | None:
        generation = self._generation
        async with self._lock:
            if generation != self._generation:
                return None
            self._work = asyncio.create_task(self._decide(occasion, wait_budget=wait_budget))
            try:
                result = await self._work
                return result if generation == self._generation else None
            except asyncio.CancelledError:
                caller = asyncio.current_task()
                if generation == self._generation or (caller is not None and caller.cancelling()):
                    raise
                return None  # load revoked this decision, including its late model response
            finally:
                self._work = None

    def _record(self, fact_kind: str, occasion: Fact, **details: Any) -> Fact:
        fact = {"fact": fact_kind, "occasion": occasion, **details}
        self.chronicle.record(fact)
        return fact

    async def _decide(self, occasion: Fact, *, wait_budget: bool = False) -> Fact | None:
        # A failed or refused occasion is used, too. Explicitly re-submitting it never replays a write.
        members = occasion_members(occasion)
        used = next(
            (
                f
                for f in reversed(self.chronicle.facts())
                if f.get("fact") in OUTCOMES
                and f.get("occasion") is not None
                and any(o in members for o in occasion_members(f["occasion"]))
            ),
            None,
        )
        if used is not None:
            return used
        try:
            state = await self._state()
            budget = self.budget(int(state.get("vars", {}).get("KAPITEL") or 1), state)
            if budget.reasons:
                if wait_budget:
                    # A console pitch can spend the budget after next() checked readiness but before
                    # this decision acquired the lock. Keep the whole group for the next opening.
                    self.occasions.put_back(occasion)
                    return None
                return self._record("line_skipped", occasion, why=budget.reasons)
            brief = await self._brief(occasion, budget, state)
            if not brief.candidates and not budget.people_left:
                return self._record(
                    "line_skipped",
                    occasion,
                    why=[
                        "нет свободных живых заказчиков: у заказчика одна открытая линия; "
                        "бюджет новых людей за главу исчерпан"
                    ],
                )
        except (GameError, TimeoutError) as error:
            return self._record("line_failed", occasion, why=[f"не удалось прочитать мир: {error}"])
        open_ahead(self.model.llm, CACHE_KEY, keep=True)
        previous = None
        errors: list[str] = []
        for attempt in range(1, ATTEMPTS + 1):
            raw = await self.model.propose(brief, previous, errors)
            if raw is None:
                return self._record("line_failed", occasion, why=["модель не ответила"])
            try:
                proposal = Proposal.of(raw)
                errors = self.canon(proposal, brief)
                if not errors:
                    errors = await self._requirements(proposal)
            except ValidationError as error:
                proposal = None
                errors = [f"структура предложения: {e['loc']}: {e['msg']}" for e in error.errors()]
            if errors:
                rejected = self._record("line_rejected", occasion, proposal=raw, attempt=attempt, why=errors)
                previous = raw
                continue
            assert proposal is not None
            if proposal.kind == "none":
                return self._record("line_none", occasion, reason=proposal.reason, proposal=raw)
            return await self._build(proposal, brief)
        return rejected

    async def _build(self, p: Proposal, brief: Brief) -> Fact:
        build = _Build(
            p, brief, {s.npc for s in self.casting.cast.characters()}, set(self.book.all()), p.giver
        )
        self._building = build
        made = None
        errors = []
        try:
            try:
                if p.kind in {"quest", "chain"}:
                    made = await self.chains.chain(
                        build.giver,
                        new_people=p.new_people,
                        premise=p.premise,
                        max_people=brief.budget.people_left if p.new_people else 0,
                        follows=p.follows,
                        requires=p.requires,
                        **({"crime": p.crime} if p.crime else {}),
                        **({"quest_count": 1} if p.kind == "quest" else {}),
                    )
                elif p.kind == "stranger":
                    person = await self.casting.character(p.place, about=p.premise, premise=p.premise)
                    if person is not None:
                        build.giver = person.npc
                        made = await self.chains.chain(
                            build.giver,
                            premise=p.premise,
                            follows=p.follows,
                            requires=p.requires,
                            quest_count=1,
                            max_people=0,
                        )
                if not made:
                    errors.append("строитель не справился")
            except (GameError, TimeoutError) as error:
                errors.append(f"строитель не справился: {error}")
            at = None
            try:
                current = await self.link.story_state()
                at = {k: current[k] for k in ("day", "hour", "minute")}
            except (GameError, TimeoutError):
                pass
            # Keep the builder's receipts, including all quests of a completed chain.
            quests = ([q.id for q in made] if isinstance(made, list) else [made.id]) if made else None
            return self._finish_build(build, errors, quests=quests, at=at)
        except Exception as error:
            self._finish_build(build, [self._unexpected(error)])
            raise  # run() logs the traceback and moves on to the next occasion
        finally:
            self._building = None

    def _finish_build(
        self,
        build: _Build,
        errors: list[str],
        *,
        quests: list[str] | None = None,
        at: Fact | None = None,
    ) -> Fact:
        if build.outcome is not None:
            return build.outcome
        p, brief = build.proposal, build.brief
        characters = sorted({s.npc for s in self.casting.cast.characters()} - build.before_people)
        if quests is None:
            quests = [q for q in self.book.all() if q not in build.before_quests]
        # A stranger may have been recorded just before Loading interrupted character()'s return.
        giver = build.giver or (characters[0] if p.kind == "stranger" and len(characters) == 1 else "")
        build.outcome = self._record(
            "line_failed" if errors else "line",
            brief.occasion,
            kind=p.kind,
            giver=giver,
            premise=p.premise,
            follows=p.follows,
            **({"crime": p.crime} if p.crime else {}),
            requires=p.requires,
            reason=p.reason,
            quests=quests,
            characters=characters,
            chapter=brief.chapter,
            **(at or {k: getattr(brief, k) for k in ("day", "hour", "minute")}),
            wall_time=self.clock(),
            **({"why": errors} if errors else {}),
        )
        return build.outcome
