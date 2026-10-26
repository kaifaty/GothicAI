"""The director brings in a character of its own (docs/director.md, stage 2, layer 4).

Its model is told of a place of the world — its points, the people who live and work there, the chapter —
and of what a stranger there may be: a farmer, a townsman or townswoman, a mercenary, the heads, faces and
clothes of nobody, what one does by day and by night, the things one carries. It answers with one call,
propose_character: who he is, how he looks, where he stands by day and by night, what he carries, his card,
voice, past, what he knows and whom of the people there he knows. The code builds the character from it
(cast.py: the body for the game, the person for the server; the template, the skin by the face and the hours
of the day are the code's), Cast checks it and brings it in; a refusal goes back to the model with its
reasons, twice at most. Then a quest of the author's (authoring.py) is his to give.
"""

from __future__ import annotations

import json
import logging
import re
from collections.abc import Collection
from dataclasses import asdict, dataclass, field
from typing import Any

from .cast import FEMALE_FACES, HEADS, Cast, Person, PersonTie
from .chronicle import Chronicle
from .game import GameLink
from .journal import Journal
from .llm import LLMBackend, LLMError, Message, Tool, ToolCall, open_ahead
from .places import PlaceBook
from .protocol import NpcLook, NpcSpec, NpcThing, RoutineEntry

log = logging.getLogger(__name__)

PROPOSE = "propose_character"
CACHE_KEY = "casting"  # its requests' own connection: the prompt cache of its instructions
ATTEMPTS = 2
DAY = ("06:00", "21:00")  # by day from, to; the night is the rest
MAX_POINTS = 10
MAX_PEOPLE = 8
MAX_GOLD = 100

# what a stranger may be: the template's kind (cast.TEMPLATES), its guild, a man or a woman, its clothes
KINDS = {
    "farmer": ("крестьянин", re.compile(r"BAU_\d+_BAUER"), "GIL_BAU", "MALE", ("ITAR_BAU_L", "ITAR_BAU_M")),
    "townsman": (
        "горожанин",
        re.compile(r"VLK_\d+_BUERGER"),
        "GIL_VLK",
        "MALE",
        ("ITAR_VLK_L", "ITAR_VLK_M"),
    ),
    "townswoman": (
        "горожанка",
        re.compile(r"VLK_\d+_BUERGERIN"),
        "GIL_VLK",
        "FEMALE",
        ("ITAR_VLKBABE_L", "ITAR_VLKBABE_M"),
    ),
    "mercenary": ("наёмник", re.compile(r"SLD_\d+_SOELDNER"), "GIL_SLD", "MALE", ("ITAR_SLD_L",)),
    "bandit": (
        "бандит",
        re.compile(r"BDT_\d+_BANDIT_[LMH]"),
        "GIL_BDT",
        "MALE",
        ("ITAR_BDT_M",),
    ),
}
STATES = {
    "ZS_STAND_ARMSCROSSED": "стоит, скрестив руки",
    "ZS_STAND_GUARDING": "стоит и поглядывает по сторонам",
    "ZS_STAND_WP": "просто стоит",
    "ZS_STAND_EATING": "стоит и ест",
    "ZS_STAND_DRINKING": "стоит и пьёт",
}
# what one carries besides gold: food and drink, nothing a quest of the scripts asks for
THINGS = ("ITFO_APPLE", "ITFO_BREAD", "ITFO_CHEESE", "ITFO_SAUSAGE", "ITFO_FISH", "ITFO_BEER", "ITFO_WINE")
MALE_FACE_LIST = tuple(
    [f"FACE_N_NORMAL0{i}" for i in range(1, 9)]
    + [f"FACE_N_NORMALBART0{i}" for i in range(1, 7)]
    + ["FACE_L_NORMALBART01", "FACE_L_NORMALBART02", "FACE_B_NORMAL01", "FACE_P_NORMAL01", "FACE_P_NORMAL02"]
)

INSTRUCTIONS = """\
Ты — сценарист Gothic II. Придумай нового жителя мира для места из сводки: человека, который здесь живёт или \
пришёл недавно, — со своим делом, нравом и прошлым. Он не занимает ничью роль в сюжете игры, не родня и не \
двойник известных героев. Пиши, как написал бы сценарист оригинальной игры: просто, грубовато, без пафоса и \
современных слов.

Выбери из сводки и напиши:
- kind — кто он; name — имя, одно-два слова, не как у людей вокруг;
- head, face, armor — голову, лицо и одежду из списков для его вида;
- day_point и day_state — где он днём и чем занят, night_point и night_state — где и как ночью (точки этого \
места, состояния из списка);
- items — что при нём из списка вещей (до 3, по 1–5 штук), gold — сколько золота (до 100);
- ru — 2–4 предложения о нём ему самому, «Ты — …»: кто он, его нрав и манера; en — одна строка по-английски;
- greed, honesty, courage, talk, pushy, strangers — его нрав по шкале 1–5: жадность, честность, смелость, \
разговорчивость, назойливость, отношение к чужакам (1 — подозрителен, 5 — открыт);
- voice — его голос по-английски для синтеза речи: возраст, тембр, манера, и прямо сказано man или woman;
- past — откуда он и зачем здесь, ему самому («Ты пришёл…»), до 3 предложений;
- knows — до 4 вещей, которые он знает о мире вокруг (без выдумок о сюжете игры);
- ties — с кем из людей вокруг он знаком и как (how — с именем этого человека), до 3.
Если задана роль автора, сохрани её kind, место и суть about; придумай человеку личность вокруг неё. \
Не выдумывай людей, мест, вещей и состояний, которых нет в сводке. Ответ — один вызов propose_character."""

PROPOSE_TOOL = Tool(
    PROPOSE,
    "Предложить нового жителя: кто он, как выглядит, где и как живёт, что при нём и кто он как человек. Код "
    "проверит его и введёт в мир или вернёт причины отказа.",
    {
        "type": "object",
        "properties": {
            "kind": {"type": "string", "enum": list(KINDS)},
            "name": {"type": "string"},
            "head": {"type": "string"},
            "face": {"type": "string"},
            "armor": {"type": "string"},
            "day_point": {"type": "string"},
            "day_state": {"type": "string"},
            "night_point": {"type": "string"},
            "night_state": {"type": "string"},
            "items": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {"item": {"type": "string"}, "count": {"type": "integer"}},
                    "required": ["item", "count"],
                    "additionalProperties": False,
                },
            },
            "gold": {"type": "integer"},
            "ru": {"type": "string"},
            "en": {"type": "string"},
            **{k: {"type": "integer"} for k in ("greed", "honesty", "courage", "talk", "pushy", "strangers")},
            "voice": {"type": "string"},
            "past": {"type": "string"},
            "knows": {"type": "array", "items": {"type": "string"}},
            "ties": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {"npc": {"type": "string"}, "how": {"type": "string"}},
                    "required": ["npc", "how"],
                    "additionalProperties": False,
                },
            },
            "reason": {"type": "string", "description": "зачем он здесь в истории, одной фразой"},
        },
        "required": [
            "kind",
            "name",
            "head",
            "face",
            "armor",
            "day_point",
            "day_state",
            "night_point",
            "night_state",
            "items",
            "gold",
            "ru",
            "en",
            "greed",
            "honesty",
            "courage",
            "talk",
            "pushy",
            "strangers",
            "voice",
            "past",
            "knows",
            "ties",
            "reason",
        ],
        "additionalProperties": False,
    },
)


@dataclass
class Brief:
    """What the model is told of the place a character is for."""

    place: str  # its id
    where: str  # «во дворе перед домом Лобарта»
    points: list[str]
    chapter: int
    templates: dict[str, str]  # kind -> the scripts' npc it is made like (the kinds the world has one of)
    things: dict[str, str]  # instance -> name in the game
    people: dict[str, str]  # npc -> name: who lives and works there
    cast: list[str] = field(default_factory=list)  # names of the director's characters so far
    kind: str = ""  # the author's fixed role, if this is a character of a chain
    about: str = ""
    carries: list[str] = field(default_factory=list)  # put in its inventory by code, besides its own things
    premise: str = ""

    def text(self) -> str:
        out = [f"Место: {self.where} ({self.place}).", f"Глава {self.chapter}."]
        if self.premise:
            out.append(f"Замысел режиссёра: {self.premise}")
        if self.kind or self.about:
            out.append(f"Роль автора задана: kind — {self.kind}; about — {self.about}.")
            out.append("Место, вид и дело заданы; придумай ему имя и личность, сохранив эту роль.")
        if self.carries:
            out.append(
                "Код положит ему заданные вещи (в items их повторять не нужно): " + ", ".join(self.carries)
            )
        out.append("Точки этого места, для его дня и ночи:")
        out += [f"- {p}" for p in self.points]
        out.append("Кем он может быть (kind — кто; одежда):")
        out += [f"- {k} — {KINDS[k][0]}; {', '.join(KINDS[k][4])}" for k in self.templates]
        out.append(f"Головы мужчин: {', '.join(sorted(HEADS['MALE']))}.")
        out.append(f"Головы женщин: {', '.join(sorted(HEADS['FEMALE']))}.")
        out.append(f"Лица мужчин: {', '.join(MALE_FACE_LIST)}.")
        out.append(f"Лица женщин: {', '.join(sorted(FEMALE_FACES))}.")
        out.append("Чем он занят (состояние — как это выглядит):")
        out += [f"- {s} — {how}" for s, how in STATES.items()]
        out.append(f"Вещи при нём (id — название), и золота до {MAX_GOLD}:")
        out += [f"- {i} — {n}" for i, n in self.things.items()]
        if self.people:
            out.append("Люди этого места (id — имя), с кем он может быть знаком:")
            out += [f"- {n} — {name}" for n, name in self.people.items()]
        if self.cast:
            out.append("Режиссёр уже ввёл, не повторяйся: " + ", ".join(self.cast))
        return "\n".join(out)


@dataclass
class Proposal:
    kind: str = ""
    name: str = ""
    head: str = ""
    face: str = ""
    armor: str = ""
    day_point: str = ""
    day_state: str = ""
    night_point: str = ""
    night_state: str = ""
    items: list[dict[str, Any]] = field(default_factory=list)
    gold: int = 0
    ru: str = ""
    en: str = ""
    greed: int = 0
    honesty: int = 0
    courage: int = 0
    talk: int = 0
    pushy: int = 0
    strangers: int = 0
    voice: str = ""
    past: str = ""
    knows: list[str] = field(default_factory=list)
    ties: list[dict[str, str]] = field(default_factory=list)
    reason: str = ""

    @classmethod
    def of(cls, args: dict[str, Any]) -> Proposal:
        return cls(**{k: args[k] for k in cls.__dataclass_fields__ if k in args})


class CastingModel:
    """The cloud model that makes up the characters."""

    def __init__(self, llm: LLMBackend) -> None:
        self.llm = llm

    def warm(self) -> None:
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
            log.warning("casting: the model failed: %s", err)
            return None
        return Proposal.of(calls[0].arguments) if calls else None


def build(p: Proposal, brief: Brief, npc: str) -> tuple[NpcSpec, Person] | list[str]:
    """The character from a proposal: its description for the game and who it is; or what is wrong with it
    against the brief (Cast checks the rest)."""
    errors: list[str] = []
    kind = KINDS.get(p.kind.strip().lower())
    if kind is None or p.kind.strip().lower() not in brief.templates:
        errors.append(f"вида {p.kind!r} нет в сводке")
    if brief.kind and p.kind.strip().lower() != brief.kind:
        errors.append(f"вид задан автором: {brief.kind}, а не {p.kind!r}")
    points = {pt.upper() for pt in brief.points}
    for what, point in (("днём", p.day_point), ("ночью", p.night_point)):
        if point.strip().upper() not in points:
            errors.append(f"точки {point!r} ({what}) нет в сводке")
    for what, state in (("днём", p.day_state), ("ночью", p.night_state)):
        if state.strip().upper() not in STATES:
            errors.append(f"состояния {state!r} ({what}) нет в сводке")
    things: list[NpcThing] = []
    for x in p.items[:3]:
        item = str(x.get("item", "")).strip().upper()
        if item not in brief.things:
            errors.append(f"вещи {x.get('item')!r} нет в сводке")
        else:
            things.append(NpcThing(item=item, count=max(1, min(5, int(x.get("count", 1))))))
    if not 0 <= p.gold <= MAX_GOLD:
        errors.append(f"золота {p.gold}, а можно от 0 до {MAX_GOLD}")
    names = {n.lower() for n in [*brief.people.values(), *brief.cast]}
    if not p.name.strip() or p.name.strip().lower() in names:
        errors.append(f"имя {p.name!r} пустое или уже есть у кого-то здесь")
    if kind is not None:
        gender, clothes = kind[3], kind[4]
        if p.armor.strip().upper() not in clothes:
            errors.append(f"одежда {p.armor!r} не для этого вида: {', '.join(clothes)}")
        if p.head.strip().upper() not in HEADS[gender]:
            errors.append(
                f"голова {p.head!r} не из списка для {'женщин' if gender == 'FEMALE' else 'мужчин'}"
            )
        faces = MALE_FACE_LIST if gender == "MALE" else tuple(sorted(FEMALE_FACES))
        if p.face.strip().upper() not in faces:
            errors.append(f"лицо {p.face!r} не из списка для {'женщин' if gender == 'FEMALE' else 'мужчин'}")
    if errors or kind is None:
        return errors
    face = p.face.strip().upper()
    if p.gold:
        things.insert(0, NpcThing(item="ITMI_GOLD", count=p.gold))
    day_point, night_point = p.day_point.strip().upper(), p.night_point.strip().upper()
    spec = NpcSpec(
        npc=npc,
        like=brief.templates[p.kind.strip().lower()],
        at=day_point,
        name=p.name.strip(),
        guild=kind[2],
        look=NpcLook(
            gender=kind[3],
            head=p.head.strip(),
            face=face,
            # the body's skin is the face's: FACE_N_… on BODYTEX_N
            skin=f"BODYTEX_{face.split('_')[1]}",
            armor=p.armor.strip().upper(),
        ),
        items=things,
        routine=[
            RoutineEntry(start=DAY[0], end=DAY[1], state=p.day_state.strip().upper(), waypoint=day_point),
            RoutineEntry(start=DAY[1], end=DAY[0], state=p.night_state.strip().upper(), waypoint=night_point),
        ],
    )
    person = Person(
        ru=p.ru.strip(),
        en=p.en.strip(),
        greed=p.greed,
        honesty=p.honesty,
        courage=p.courage,
        talk=p.talk,
        pushy=p.pushy,
        strangers=p.strangers,
        voice=p.voice.strip(),
        past=p.past.strip(),
        knows=tuple(k.strip() for k in p.knows[:4] if k.strip()),
        ties=tuple(
            PersonTie(npc=t.get("npc", "").strip().upper(), how=t.get("how", "").strip()) for t in p.ties[:3]
        ),
    )
    return spec, person


def _carrying(spec: NpcSpec, carries: list[str]) -> NpcSpec:
    """The author's things are the code's, even when the character model did not ask for them."""
    items = {thing.item: thing for thing in spec.items}
    for item in carries:
        item = item.strip().upper()
        items.setdefault(item, NpcThing(item=item))
    return spec.model_copy(update={"items": list(items.values())})


class Casting:
    """The director's characters, made up by its model for a place of the world."""

    def __init__(
        self,
        link: GameLink,
        cast: Cast,
        llm: LLMBackend,
        places: PlaceBook,
        chronicle: Chronicle,
        *,
        journal: Journal | None = None,
    ) -> None:
        self.link = link
        self.cast = cast
        self.model = CastingModel(llm)
        self.places = places
        self.chronicle = chronicle
        self.journal = journal or Journal()

    async def character(
        self,
        place: str,
        *,
        kind: str = "",
        about: str = "",
        carries: list[str] | None = None,
        premise: str = "",
    ) -> NpcSpec | None:
        """A new character for the place, in the world; None if the model has none that passes."""
        self.model.warm()
        brief = await self.brief(place, premise=premise)
        if brief is None:
            return None
        kind = kind.strip().lower()
        if kind and kind not in brief.templates:
            self.journal.add("cast_none", place=place, why=[f"вида {kind!r} нет в сводке"])
            return None
        brief.kind, brief.about = kind, about.strip()
        if kind:
            brief.templates = {kind: brief.templates[kind]}
        brief.carries = list(dict.fromkeys(i.strip().upper() for i in carries or []))
        proposal: Proposal | None = None
        errors: list[str] = []
        for attempt in range(ATTEMPTS):
            proposal = await self.model.propose(brief, proposal, errors)
            if proposal is None:
                break
            log.info("casting: %s, proposal %d: %s", place, attempt + 1, proposal)
            built = build(proposal, brief, await self.cast.next_id())
            if isinstance(built, list):
                errors = built
            else:
                spec, person = built
                spec = _carrying(spec, brief.carries)
                made, errors = await self.cast.introduce(spec, person)
                if made is not None:
                    await self.chronicle.record(
                        {"fact": "cast_reason", "npc": spec.npc, "reason": proposal.reason}
                    )
                    return spec
            log.info("casting: %s, rejected: %s", place, "; ".join(errors))
            await self.chronicle.record({"fact": "cast_rejected", "place": brief.place, "why": errors})
        self.journal.add("cast_none", place=place, why=errors)
        return None

    async def bandits(
        self,
        place: str,
        count: int,
        carries: list[str] | None = None,
        *,
        occupied_points: Collection[str] = (),
    ) -> tuple[list[NpcSpec], list[str]]:
        """A nameless camp without a model call. On a refusal, keep and return those already introduced.

        The chain's author checks that the place is uninhabited before calling this.
        """
        brief = await self.brief(place)
        if brief is None:
            return [], [f"места {place!r} нет или в нём нет точек"]
        if "bandit" not in brief.templates:
            return [], ["в мире нет безымянного шаблона бандита"]
        occupied = {point.upper() for point in occupied_points}
        points = [point for point in brief.points if point.upper() not in occupied]
        if type(count) is not int or count < 1 or count > len(points):
            return [], [f"бандитов — от 1 до числа свободных точек места ({len(points)})"]
        made: list[NpcSpec] = []
        for i, point in enumerate(points[:count]):
            face = MALE_FACE_LIST[i % len(MALE_FACE_LIST)]
            spec = NpcSpec(
                npc=await self.cast.next_id(),
                like=brief.templates["bandit"],
                at=point,
                name="Бандит",
                guild="GIL_BDT",
                look=NpcLook(
                    gender="MALE",
                    head="HUM_HEAD_FIGHTER",
                    face=face,
                    skin=f"BODYTEX_{face.split('_')[1]}",
                    armor="ITAR_BDT_M",
                ),
                routine=[
                    RoutineEntry(start=DAY[0], end=DAY[1], state="ZS_STAND_GUARDING", waypoint=point),
                    RoutineEntry(start=DAY[1], end=DAY[0], state="ZS_STAND_GUARDING", waypoint=point),
                ],
            )
            spec = _carrying(spec, carries or [])
            result, errors = await self.cast.introduce(spec)
            if result is None:
                return made, errors
            made.append(spec)
        return made, []

    async def brief(self, place_id: str, *, premise: str = "") -> Brief | None:
        place = self.places.get(place_id)
        if place is None or not place.points:
            log.info("casting: no place %s", place_id)
            return None
        people = await self.link.story_people()
        templates: dict[str, str] = {}
        for kind, (_, pattern, *_rest) in KINDS.items():
            like = sorted(
                p["npc"]
                for p in people
                if pattern.fullmatch(p["npc"])
                # BDT_1014_BANDIT_L is Brago: his script symbol is no nameless template.
                and (kind != "bandit" or str(p.get("name") or "").lower() in {"", "бандит", "bandit"})
            )
            if like:
                templates[kind] = like[0]
        here = {
            p["npc"]: str(p.get("name") or p["npc"])
            for p in people
            if any(self.places.of(e.get("waypoint")) is place for e in p.get("routine", []))
        }
        state = await self.link.story_state(vars=["KAPITEL"], symbols=list(THINGS), items=list(THINGS))
        symbols, names = state.get("symbols") or {}, state.get("items") or {}
        things = {i: str(names.get(i) or i) for i in THINGS if symbols.get(i) == "item"}
        if not templates:
            log.info("casting: no template in the world")
            return None
        points = list(place.points)
        step = max(1, len(points) // MAX_POINTS)
        return Brief(
            place=place.id,
            where=place.where_ru,
            points=points[::step][:MAX_POINTS],
            chapter=int((state.get("vars") or {}).get("KAPITEL") or 1),
            templates=templates,
            things=things,
            people=dict(sorted(here.items())[:MAX_PEOPLE]),
            cast=[c.name for c in await self.cast.characters()],
            premise=premise,
        )
