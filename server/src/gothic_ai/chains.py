"""The director's author of linear quest chains: proposes once, checks, casts, then books atomically."""

from __future__ import annotations

import json
import logging
import re
from collections import Counter
from dataclasses import dataclass, field
from typing import TYPE_CHECKING, Any, Literal

from pydantic import BaseModel, ConfigDict, Field, ValidationError

from . import questbook
from .attitudes import non_hostile
from .authoring import ATTEMPTS, Author, Spot
from .authoring import Brief as AuthorBrief
from .casting import KINDS, Casting
from .game import GameError
from .llm import LLMBackend, LLMError, Message, Tool, ToolCall, open_ahead
from .people_knowledge import rumor_subject
from .places import Place
from .protocol import NpcSpec

if TYPE_CHECKING:
    from .chronicle import Fact
    from .crimes import Crimes

log = logging.getLogger(__name__)
PROPOSE = "propose_chain"
CACHE_KEY = "chain"
NEAR_CM = 20_000  # a chain can lead beyond the farmyard of a one-stage errand
ROLE = re.compile(r"[a-z][a-z0-9_]{0,31}")
NEW_PEOPLE = (
    "Этой цепочке нужен новый человек или лагерь бандитов: хотя бы одна роль в cast, и квесты на неё "
    "опираются (у кого вещь, с кем говорить, кого убить)."
)

INSTRUCTIONS = """\
Ты — сценарист Gothic II. Придумай для заказчика из сводки {assignment}.
Заказчик один. Квесты открываются по порядку после сдачи предыдущего; after и id ставит код.
Пиши просто, грубовато, в характере заказчика. Не повторяй его прежние поручения.
Цели квестов должны разыгрывать сам замысел (premise), а не подменять его пропажей вещи.
«Пропало — найди» подходит только тогда, когда замысел и есть пропажа.

Ответ — один вызов propose_chain, три части: cast, quests, reason.
cast — новые люди, нужные истории: role (короткий id латиницей), kind из списка, place из сводки,
count, about (кто он, одной фразой), carries (id вещей из сводки, по одному экземпляру).
count=1 — человек с именем: другую модель попросит код придумать его личность.
count=2..8 — только безымянные bandit, в месте с разрешением «бандиты: да», на разных точках.
Не поселяй bandit в доме, дворе, на улице или там, где стоит чей-либо распорядок.
Готовые роли повтора оставь с прежними kind/place/count/about/carries: второй раз их не ставят.

quests — описания книги: title, give (description, lines, remember), stages (journal, goals),
done_journal, knows (что заказчик знает), asked (что просит), gold, xp, days, late_journal.
Награда КАЖДОГО квеста в пределах сводки. Срок days=0, если он не нужен.
give.description и talk.description — строка героя в меню, до 80 знаков.
Реплика lines: speaker hero или npc, text. Записи журнала — от первого лица героя.
В каждой стадии 1–4 цели, всего 1–5 стадий. Последняя стадия заканчивается ровно одним bring или talk.
Цели: kind, npc, item, count, place, radius, npcs, days, talk, journal.
- reach: place из сводки с разрешением «дойти: да» (вейпоинт выберет код)
  ИЛИ npc роли/заказчика/жителя сводки;
  radius от 200 до 3000 см, обычно 500;
- have: вещь item, count; npc — её держатель (у новой роли она обязана быть в carries),
  или place — код положит вещь там при начале стадии;
- kill: npcs — список role, убить ВСЕХ персонажей каждой роли; чужих NPC убивать нельзя;
- bring: item и count принести npc после have, либо place, где код положит вещь;
- talk: разговор с npc;
- wait: days от 1 до 7.
npc — giver (заказчик), role человека с count=1 или id NPC из сводки. Не выдумывай других людей и вещей.
Роли bandit враждебны герою: к ним нельзя обращаться с talk или bring.
Для bring/talk нужен talk: description — строка героя в меню, lines — разговор, remember — память NPC.
Неиспользуемые строки заполняй "", списки [], count=1, radius=500, days=0, talk=null.
Цепочка должна проходиться по порядку; не отправляй разговаривать с уже убитым человеком.
"""


class Input(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True)


class Role(Input):
    role: str
    kind: str
    place: str
    count: int
    about: str
    carries: list[str]


class Line(Input):
    speaker: Literal["hero", "npc", "self", "other"]
    text: str


class Talk(Input):
    description: str
    lines: list[Line]
    remember: str = ""


class Goal(Input):
    kind: str
    npc: str = ""
    item: str = ""
    count: int = 1
    place: str = ""
    radius: int = questbook.REACH_CM
    npcs: list[str] = Field(default_factory=list)
    days: int = 0
    talk: Talk | None = None
    journal: str = ""


class Stage(Input):
    journal: str
    goals: list[Goal]


class Quest(Input):
    title: str
    give: Talk
    stages: list[Stage]
    done_journal: str
    knows: str
    asked: str
    gold: int
    xp: int
    days: int = 0
    late_journal: str = ""


class Proposal(Input):
    cast: list[Role]
    quests: list[Quest]
    reason: str

    @classmethod
    def of(cls, args: dict[str, Any]) -> Proposal:
        return cls.model_validate(args)


def _schema() -> dict[str, Any]:
    schema = Proposal.model_json_schema()

    def required(node: Any) -> None:
        if isinstance(node, dict):
            node.pop("default", None)
            if "properties" in node:
                node["required"] = list(node["properties"])
            for value in node.values():
                required(value)
        elif isinstance(node, list):
            for value in node:
                required(value)

    required(schema)
    return schema


PROPOSE_TOOL = Tool(
    PROPOSE, "Предложить поручение или связанную цепочку и нужных ей новых персонажей.", _schema()
)


@dataclass
class Brief:
    base: AuthorBrief
    places: dict[str, Place]
    people: dict[str, str]
    occupied: set[str]
    roles: dict[str, list[NpcSpec]] = field(default_factory=dict)
    fixed: dict[str, Role] = field(default_factory=dict)
    new_people: bool = False  # the chain must bring in someone new: a person or a camp
    max_people: int | None = None  # the whole chain, including already introduced roles on a retry
    quest_count: int | None = None  # None keeps the usual 2–3 quests; otherwise an exact count
    crime: Fact | None = None
    crime_context: str = ""
    crime_items: dict[str, str] = field(default_factory=dict)  # only this deed's actual stolen items

    def bandit_ok(self, place: str) -> bool:
        p = self.places.get(place)
        return p is not None and p.kind not in ("house", "yard", "street") and place not in self.occupied

    def text(self) -> str:
        out = [self.base.text(errand=False), "Места для поручения (точки выбирает код):"]
        if self.quest_count is not None:
            out.append(f"Число квестов: ровно {self.quest_count}.")
        if self.crime_context:
            out.append(self.crime_context)
            if self.crime_items:
                out.append("Взятые вещи этого дела, дополнительно разрешённые в целях:")
                out += [f"- {item} — {name}" for item, name in self.crime_items.items()]
                out.append(
                    "Добыча может уже быть у героя: для её возврата перед bring поставь have с npc=PC_HERO "
                    "и item этой вещи. Не создавай её снова в месте или у новой роли. "
                    "Герой может принести свою же добычу и соврать, что нашёл её: это законный исход. "
                    "Возврат сам по себе не означает прощения; прощают только отдельной сделкой."
                )
        out += [
            f"- {p.id} — {p.where_ru}; вид {p.kind}; "
            f"бандиты: {'да' if self.bandit_ok(p.id) else 'нет'}; "
            f"дойти: {'да' if p.waypoints else 'нет'}; точек {len(p.points)}"
            for p in self.places.values()
        ]
        out.append("Люди для разговора и передачи вещей (npc — имя; заказчик также giver):")
        out += [f"- {n} — {name}" for n, name in self.people.items()]
        out.append("Виды новых людей: " + ", ".join(KINDS))
        if self.new_people:
            out.append(NEW_PEOPLE)
        if self.max_people is not None:
            out.append(
                f"Новых персонажей в этой цепочке — не больше {self.max_people}; каждый бандит считается "
                "отдельно. Готовые роли повтора входят в этот предел один раз."
            )
        if self.roles:
            out.append("Готовые роли, уже в мире (сохрани описание роли, не ставь их снова):")
            for role, specs in self.roles.items():
                out.append(
                    f"- {role}: {json.dumps(self.fixed[role].model_dump(), ensure_ascii=False)}; "
                    + ", ".join(f"{s.npc} — {s.name}" for s in specs)
                )
        return "\n".join(out)


class ChainModel:
    def __init__(self, llm: LLMBackend) -> None:
        self.llm = llm

    async def propose(
        self, brief: Brief, previous: dict[str, Any] | None, errors: list[str]
    ) -> dict[str, Any] | None:
        messages = [Message("user", brief.text())]
        if previous is not None:
            messages.append(
                Message(
                    "user",
                    f"Твоё предложение: {json.dumps(previous, ensure_ascii=False)}\nОтклонено:\n"
                    + "\n".join(f"- {e}" for e in errors)
                    + "\nИсправь его.",
                )
            )
        calls = []
        assignment = (
            "одно поручение, ровно один квест"
            if brief.quest_count == 1
            else f"одну связанную цепочку из {brief.quest_count or '2–3'} квестов"
        )
        try:
            async for event in self.llm.stream(
                instructions=INSTRUCTIONS.format(assignment=assignment),
                messages=messages,
                tools=[PROPOSE_TOOL],
                cache_key=CACHE_KEY,
            ):
                if isinstance(event, ToolCall) and event.name == PROPOSE:
                    calls.append(event)
        except LLMError as err:
            log.warning("chain: model failed: %s", err)
            return None
        return calls[0].arguments if len(calls) == 1 else {}


class ChainAuthor:
    def __init__(self, author: Author, casting: Casting, *, crimes: Crimes | None = None) -> None:
        self.author = author
        self.casting = casting
        self.book = author.book
        self.chronicle = author.chronicle
        self.link = author.link
        self.model = ChainModel(author.model.llm)
        self.crimes = crimes

    async def brief(
        self,
        npc: str,
        *,
        premise: str = "",
        requires: list[dict[str, Any]] | None = None,
        follows: str = "",
        crime: str = "",
    ) -> Brief | None:
        deed = self.crimes.crime(crime) if self.crimes is not None and crime else None
        if crime and deed is None:
            log.info("chain: unavailable crime %s", crime)
            return None
        base = await self.author.brief(npc.upper(), premise, requires=requires, follows=follows)
        places = self.author.places
        if base is None or places is None:
            return None
        nearby = {p.id: p for s in base.spots if (p := places.get(s.id)) is not None}
        occupied: set[str] = set()
        people = {base.npc: base.name}
        residents = await self.link.story_people()
        for person in residents:
            homes = {
                p.id for e in person.get("routine", []) if (p := places.of(e.get("waypoint"))) is not None
            }
            occupied.update(homes)
        routine = await self.link.routine(base.npc)
        homes = Counter(p.id for e in routine.entries if (p := places.of(e.waypoint)) is not None)
        home = places.get(homes.most_common(1)[0][0]) if homes else None
        if home is not None and home.center is not None:
            # Keep the author's nearby places, and add a few uninhabited sites for the camp. The short
            # errand's radius often contains only the house and the people working in its yard.
            for place, distance in places.near(home.center, NEAR_CM, len(places.places)):
                if len(nearby) >= 10:
                    break
                if place.id in nearby or place.id in occupied or place.kind in ("house", "yard", "street"):
                    continue
                if len(place.points) < 3:
                    continue
                nearby[place.id] = place
                base.spots.append(Spot(place.id, place.where_ru, round(distance / 100), place.points[0]))
        for person in residents:
            if any(
                (p := places.of(e.get("waypoint"))) is not None and p.id in nearby
                for e in person.get("routine", [])
            ):
                people[person["npc"]] = str(person.get("name") or person["npc"])
        safe = await non_hostile(self.link, people, context=base.npc)
        if base.npc not in safe:
            log.info("chain: %s is hostile to the hero", base.npc)
            return None
        people = {npc: name for npc, name in people.items() if npc in safe}
        brief = Brief(base, nearby, people, occupied, crime=deed)
        if deed is not None:
            brief.crime_context = self._crime_context(deed, people)
            if deed.get("kind") == "theft":
                taken = [
                    {"item": deed.get("item"), "count": deed.get("count", 0)},
                    *deed.get("items", []),
                ]
                brief.crime_items = {
                    entry["item"]: str(deed.get("item_name") or entry["item"])
                    for entry in taken
                    if entry.get("item") and int(entry.get("count", 0)) > 0
                }
        return brief

    def _crime_context(self, deed: Fact, nearby: dict[str, str]) -> str:
        assert self.crimes is not None
        crime = deed["id"]
        out = [
            f"Дело {crime}. Правда режиссёра: {self.crimes.describe(deed)}.",
            f"Место дела: {deed.get('waypoint') or deed.get('place')}.",
            "Люди знают только своё. Правда режиссёра не становится знанием заказчика; "
            "подозрение не доказывает вину героя. Реплики и knows опирай на знание говорящего.",
            "Кто что знает:",
        ]
        victim = deed.get("victim") or deed.get("owner") or ""
        candidates = set(nearby) | set(deed.get("witnesses", []))
        if victim:
            candidates.add(victim)
        for fact in self.chronicle.facts():
            if fact.get("fact") == "rumor" and rumor_subject(fact) != "PLAYER":
                continue
            if fact.get("crime") == crime or crime in self.crimes.testimony_crimes(fact):
                candidates.update(npc for key in ("npc", "to") if (npc := fact.get(key)))
        for npc in sorted(candidates):
            known = [k for k in self.crimes.known(npc) if k.crime["id"] == crime]
            if not known and npc != victim and npc not in deed.get("witnesses", []):
                continue
            role = (
                "пострадавший"
                if npc == victim
                else "свидетель"
                if npc in deed.get("witnesses", [])
                else "узнал о деле"
            )
            prefix = f"- {npc} — {self.crimes.names(npc)}; {role}; "
            if not known:
                out.append(prefix + "о деле и виновнике не знает.")
                if npc == victim and deed.get("kind") == "theft":
                    out.append("Пострадавший может заметить пропажу, не узнав, кто взял вещь.")
            for knowledge in known:
                degree = {"seen": "видел", "heard": "слышал", "suspects": "подозревает"}[knowledge.degree]
                out.append(
                    prefix + f"{degree}; кого: {knowledge.suspect}; "
                    f"источник: {knowledge.source}; переходов: {knowledge.hops}; "
                    f"простил: {'да' if knowledge.forgiven else 'нет'}; "
                    f"обещал молчать: {'да' if knowledge.silenced else 'нет'}."
                )
        return "\n".join(out)

    async def chain(
        self,
        npc: str,
        *,
        new_people: bool = False,
        premise: str = "",
        max_people: int | None = None,
        requires: list[dict[str, Any]] | None = None,
        follows: str = "",
        quest_count: int | None = None,
        crime: str = "",
    ) -> list[questbook.Quest] | None:
        """Build 2–3 quests by default, or exactly `quest_count`; `new_people` requires a nonempty cast."""
        if quest_count is not None and quest_count not in (1, 2, 3):
            raise ValueError("quest_count must be 1, 2, 3 or None")
        open_ahead(self.model.llm, CACHE_KEY, keep=True)
        brief = await self.brief(npc, premise=premise, requires=requires, follows=follows, crime=crime)
        if brief is None:
            return None
        brief.new_people = new_people
        brief.max_people = max_people
        brief.quest_count = quest_count
        previous = None
        errors: list[str] = []
        for attempt in range(1, ATTEMPTS + 1):
            proposal = await self.model.propose(brief, previous, errors)
            if proposal is None:
                break
            log.info("chain: %s proposal %d: %s", npc, attempt, json.dumps(proposal, ensure_ascii=False))
            try:
                made, errors = await self._try(proposal, brief)
            except (GameError, TimeoutError) as err:
                made, errors = None, [f"игра не ответила: {err}"]
            if made is not None:
                self.chronicle.record(
                    {
                        "fact": "chain",
                        "npc": brief.base.npc,
                        "roles": {r: [s.npc for s in specs] for r, specs in brief.roles.items()},
                        "quests": [q.id for q in made],
                        "reason": proposal["reason"],
                        **({"crime": crime} if crime else {}),
                    }
                )
                return made
            self.chronicle.record(
                {"fact": "chain_rejected", "npc": brief.base.npc, "attempt": attempt, "why": errors}
            )
            log.info("chain: rejected %d: %s", attempt, "; ".join(errors))
            previous = proposal
        return None

    async def _try(self, raw: dict[str, Any], brief: Brief) -> tuple[list[questbook.Quest] | None, list[str]]:
        try:
            p = Proposal.of(raw)
        except ValidationError as err:
            return None, [f"структура предложения: {e['loc']}: {e['msg']}" for e in err.errors()]
        errors = self._roles(p, brief)
        if errors:
            return None, errors
        roles = {
            r.role: [
                *brief.roles.get(r.role, []),
                *[
                    NpcSpec(npc=f"DIR_NPC_CHAIN_{r.role.upper()}_{n}", like="", at="", name=r.about)
                    for n in range(len(brief.roles.get(r.role, [])), r.count)
                ],
            ]
            for r in p.cast
        }
        quests, errors = await self._quests(p, brief, roles, placeholders=True)
        if errors:
            return None, errors
        # No mutation before every description has passed the book with living role placeholders.
        for role in p.cast:
            specs = brief.roles.get(role.role, [])
            if len(specs) == role.count:
                continue
            if role.count == 1:
                spec = await self.casting.character(
                    role.place,
                    kind=role.kind,
                    about=role.about,
                    carries=role.carries,
                    premise=brief.base.premise,
                )
                if spec is None:
                    return None, [f"кастинг не сделал роль {role.role}"]
                specs.append(spec)
            else:
                added, errors = await self.casting.bandits(
                    role.place,
                    role.count - len(specs),
                    carries=role.carries,
                    occupied_points=[s.at for s in specs],
                )
                specs.extend(added)
                if specs:
                    brief.roles[role.role], brief.fixed[role.role] = specs, role
                if errors:
                    return None, errors
            brief.roles[role.role], brief.fixed[role.role] = specs, role
        quests, errors = await self._quests(p, brief, brief.roles, placeholders=False)
        if errors:
            return None, errors
        return await self.book.make_chain(quests, gold=brief.base.gold, xp=brief.base.xp)

    def _roles(self, p: Proposal, brief: Brief) -> list[str]:
        errors = []
        if brief.quest_count is not None and len(p.quests) != brief.quest_count:
            errors.append(f"нужно ровно {brief.quest_count} квест, предложено {len(p.quests)}")
        elif brief.quest_count is None and not 2 <= len(p.quests) <= 3:
            errors.append("в цепочке должно быть 2–3 квеста")
        if brief.new_people and not p.cast:
            errors.append("cast пуст, а цепочке нужен новый человек или лагерь бандитов")
        people = sum(r.count for r in p.cast)
        if brief.max_people is not None and people > brief.max_people:
            errors.append(f"новых персонажей {people}, а бюджет цепочки — не больше {brief.max_people}")
        seen = set()
        for r in p.cast:
            if not ROLE.fullmatch(r.role) or r.role == "giver" or r.role in seen:
                errors.append(f"роль {r.role!r}: нужен уникальный короткий id латиницей, не giver")
            seen.add(r.role)
            if r.kind not in KINDS:
                errors.append(f"роль {r.role}: неизвестный вид {r.kind}")
            place = brief.places.get(r.place)
            if place is None:
                errors.append(f"места {r.place!r} нет в сводке")
            elif r.kind == "bandit" and not brief.bandit_ok(r.place):
                errors.append(f"бандит: место {r.place} жилое (вид {place.kind} или чей-то распорядок)")
            if not 1 <= r.count <= questbook.MAX_KILL or (r.count != 1 and r.kind != "bandit"):
                errors.append(f"роль {r.role}: count=1, для группы bandit — 2..{questbook.MAX_KILL}")
            if place is not None and r.count > len(place.points):
                errors.append(f"роль {r.role}: в месте {r.place} не хватает разных точек")
            if not r.about.strip() or len(r.about) > 300:
                errors.append(f"роль {r.role}: about — одна непустая фраза до 300 знаков")
            for item in r.carries:
                if item not in brief.base.items:
                    errors.append(f"вещи {item!r} нет в сводке")
            if len(set(r.carries)) != len(r.carries):
                errors.append(f"роль {r.role}: carries содержит повторяющиеся вещи")
            if r.role in brief.fixed and r != brief.fixed[r.role]:
                errors.append(f"роль {r.role} уже поставлена: сохрани её описание")
        for role in brief.fixed.keys() - seen:
            errors.append(f"роль {role} уже поставлена: сохрани её в cast")
        return errors

    async def _quests(
        self, p: Proposal, brief: Brief, roles: dict[str, list[NpcSpec]], *, placeholders: bool
    ) -> tuple[list[questbook.Quest], list[str]]:
        errors: list[str] = []
        out: list[questbook.Quest] = []
        taken = set(self.author.old)
        descriptions = {r.role: r for r in p.cast}
        killed: set[str] = set()
        for description in p.quests:
            if not description.knows.strip() or not description.asked.strip():
                errors.append(f"«{description.title}»: нужны knows и asked заказчика")
            data = description.model_dump()
            for stage in data["stages"]:
                for goal in stage["goals"]:
                    place = goal.pop("place")
                    if place:
                        if place not in brief.places:
                            errors.append(f"места {place!r} нет в сводке")
                        elif goal["kind"] not in (questbook.REACH, questbook.HAVE, questbook.BRING):
                            errors.append(f"цель {goal['kind']}: место не используется")
                        else:
                            site = brief.places[place]
                            points = site.waypoints if goal["kind"] == questbook.REACH else site.points
                            if not points:
                                errors.append(f"дойти до места {place} нельзя: в нём нет вейпоинтов")
                            else:
                                goal["point"] = points[len(points) // 2]
                    ref = goal["npc"]
                    if ref:
                        if ref in roles:
                            if len(roles[ref]) != 1:
                                errors.append(f"npc роли {ref}: для разговора/вещи/дойти нужен один человек")
                            if (
                                goal["kind"] in (questbook.TALK, questbook.BRING)
                                and descriptions[ref].kind == "bandit"
                            ):
                                errors.append(f"роль {ref}: бандит враждебен герою, talk/bring невозможны")
                            goal["npc"] = roles[ref][0].npc
                        elif (
                            ref == "PC_HERO"
                            and goal["kind"] == questbook.HAVE
                            and goal["item"] in brief.crime_items
                        ):
                            pass  # The actual loot can already be in the thief's inventory.
                        elif ref == "giver" or ref == brief.base.npc:
                            goal["npc"] = brief.base.npc
                        elif ref not in brief.people:
                            errors.append(f"NPC {ref!r} нет в сводке и ролях")
                        if goal["npc"] in killed:
                            errors.append(f"NPC {ref} уже убит предыдущей целью")
                    if goal["item"] and goal["item"] not in brief.base.items | brief.crime_items:
                        errors.append(f"вещи {goal['item']!r} нет в сводке")
                    if goal["kind"] == questbook.HAVE and ref and not place:
                        if ref in descriptions:
                            if Counter(descriptions[ref].carries)[goal["item"]] < goal["count"]:
                                errors.append(f"{goal['item']} неоткуда взять: у роли {ref} её нет в carries")
                        elif ref in brief.people or ref == "giver":
                            snapshot = await self.link.snapshot(goal["npc"], fresh=True)
                            count = sum(i.count for i in snapshot.npc.inventory if i.item == goal["item"])
                            if count < goal["count"]:
                                errors.append(f"{goal['item']} неоткуда взять: у NPC {ref} её нет")
                    targets = []
                    for role in goal["npcs"]:
                        if role not in roles:
                            errors.append(f"убить: неизвестная роль {role!r}")
                        else:
                            targets.extend(s.npc for s in roles[role])
                    goal["npcs"] = targets
                killed.update(n for g in stage["goals"] if g["kind"] == questbook.KILL for n in g["npcs"])
            q = questbook.Quest.of(
                {
                    **data,
                    "id": self.book.next_id(taken),
                    "giver": brief.base.npc,
                    "after": out[-1].id if out else "",
                    "requires": [] if out else brief.base.requires,
                    "knows": brief.base.knowledge(description.knows),
                    "reason": p.reason,
                    "names": {**brief.people, **{s.npc: s.name for specs in roles.values() for s in specs}},
                }
            )
            taken.add(q.id)
            ground = await self.book.ground(q)
            ground.gold, ground.xp = brief.base.gold, brief.base.xp
            ground.quests = {**ground.quests, **{previous.id: previous for previous in out}}
            if placeholders:
                for specs in roles.values():
                    for spec in specs:
                        ground.symbols[spec.npc] = "npc"
                        ground.states[spec.npc] = "alive"
            errors.extend(f"«{q.title}»: {e}" for e in questbook.problems(q, ground))
            out.append(q)
        return out, errors
