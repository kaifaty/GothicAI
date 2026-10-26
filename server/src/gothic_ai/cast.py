"""The director's own characters in the world (docs/director.md, stage 2, layer 2).

A character is an npc of ours (DIR_NPC_<n>, a script symbol of its own) that the game makes like a nameless
npc of the scripts: a farmer, a townsman, a mercenary. The template gives its class, body, fight and senses;
the description gives its name, guild, look, things and day, and where it comes. The code checks the
description, the game makes it once (story_npc), and the branch's log keeps it (StoryPatches): a load brings
back what the save had, and every session the game gets the branch's characters again, before the graph's
changes.

Who it is as a person is layer 3 (Person): a character card like the scripts' npc's have (characters.py), a
voice for the speech, where it comes from, what it knows, whom it knows. It goes with its record in the
branch's log, and everything that reads npc's by id reads it as their own: the cards (characters.Cards: the
persona, deals, the behavioral model), the persona (its past, knowledge and acquaintances in its
instructions, its voice), the people's ties (social.py: whom it knows, so rumors reach it and they call each
other by name).
"""

from __future__ import annotations

import logging
import re
from collections.abc import Callable, Collection, Mapping
from typing import Any

from pydantic import BaseModel, ConfigDict

from .characters import SCALES, Card
from .chronicle import Chronicle
from .game import GameError, GameLink
from .protocol import GameMessage, NpcSpec, Session
from .story import StoryPatches

log = logging.getLogger(__name__)

ID = re.compile(r"DIR_NPC_[A-Z0-9_]+")
# nameless npc's of the scripts, of the kinds a stranger may be; what the scripts tie to them stays theirs
TEMPLATES = re.compile(r"BAU_\d+_BAUER|VLK_\d+_BUERGER(IN)?|SLD_\d+_SOELDNER|BDT_\d+_BANDIT_[LMH]")
GUILDS = frozenset({"GIL_BAU", "GIL_VLK", "GIL_OUT", "GIL_SLD", "GIL_BDT"})
# the scripts' heads and faces that are nobody's: no face of a person the hero knows
MALE_HEADS = ("BALD", "FATBALD", "FIGHTER", "THIEF", "PONY", "PSIONIC")
HEADS = {
    "MALE": frozenset(f"HUM_HEAD_{h}" for h in MALE_HEADS),
    "FEMALE": frozenset({"HUM_HEAD_BABE", "HUM_HEAD_BABEHAIR", *(f"HUM_HEAD_BABE{i}" for i in range(1, 9))}),
}
MALE_FACES = re.compile(r"FACE_[NLBP]_NORMAL(BART)?\d+")
FEMALE_FACES = frozenset(
    f"FACEBABE_N_{f}"
    for f in (
        "BAUBLONDE",
        "BLACKHAIR",
        "BLONDIE",
        "BROWN",
        "GREYCLOTH",
        "HAIRANDCLOTH",
        "MIDBAUBLONDE",
        "OLDBLONDE",
        "OLDBROWN",
        "VLKBLONDE",
        "WHITECLOTH",
        "YOUNGBLONDE",
    )
)
SKINS = frozenset({"BODYTEX_N", "BODYTEX_L", "BODYTEX_B", "BODYTEX_P"})
ARMORS = {
    "MALE": frozenset(
        f"ITAR_{a}" for a in ("BAU_L", "BAU_M", "VLK_L", "VLK_M", "VLK_H", "SLD_L", "BDT_M")
    ),
    "FEMALE": frozenset(
        f"ITAR_{a}BABE_{s}" for a, s in (("BAU", "L"), ("BAU", "M"), ("VLK", "L"), ("VLK", "M"), ("VLK", "H"))
    ),
}
MAX_COUNT = 100  # of one thing
MAX_GOLD = 1000
DAY = 24 * 60
NAME_MAX = 40


PERSON_TEXT = 600  # its card's, its past's letters
FACTS_MAX, FACT_TEXT = 10, 240
TIES_MAX, TIE_TEXT = 8, 200
VOICE_TEXT = 300


class PersonTie(BaseModel):
    """Someone a character knows: `npc` and how, in Russian with the name («Хильда с фермы Лобарта: берёшь
    у неё молоко»)."""

    model_config = ConfigDict(frozen=True, extra="forbid")
    npc: str
    how: str


class Person(BaseModel):
    """Who a character of the director's is (layer 3). `ru`: 2–4 sentences in the second person, «Ты — …»:
    who it is, its temper and manner, as a card's; `en`: one English line for the behavioral model; the
    scales 1..5 as a card's (characters.SCALES); `voice`: the speech's voice, an English description that
    says a man's or a woman's; `past`: where it comes from and why it is here («Ты пришёл…»); `knows`: what
    it knows of the world; `ties`: whom it knows."""

    model_config = ConfigDict(frozen=True, extra="forbid")
    ru: str
    en: str
    greed: int
    honesty: int
    courage: int
    talk: int
    pushy: int
    strangers: int
    voice: str
    past: str = ""
    knows: tuple[str, ...] = ()
    ties: tuple[PersonTie, ...] = ()

    def card(self, npc: str, name: str) -> Card:
        return Card(
            npc,
            name,
            self.ru,
            self.en,
            self.greed,
            self.honesty,
            self.courage,
            self.talk,
            self.pushy,
            self.strangers,
        )


def person_problems(person: Person, spec: NpcSpec, alive: Collection[str]) -> list[str]:
    """What is wrong with who a character is, in words for the model. `alive`: the npc's of the world it may
    know (living people)."""
    out: list[str] = []
    for scale in SCALES:
        if not 1 <= getattr(person, scale) <= 5:
            out.append(f"{scale}: от 1 до 5")
    if not person.ru.startswith("Ты") or not 20 <= len(person.ru) <= PERSON_TEXT:
        out.append(f"карточка: от 20 до {PERSON_TEXT} знаков, начинается с «Ты — …»")
    if not 10 <= len(person.en) <= 200:
        out.append("en: одна строка по-английски, от 10 до 200 знаков")
    voice = person.voice.lower()
    # her look's, else the template's (a townswoman, VLK_*_BUERGERIN)
    woman = spec.look.gender == "FEMALE" if spec.look is not None else spec.like.endswith("IN")
    if not 20 <= len(voice) <= VOICE_TEXT or not voice.isascii():
        out.append(f"голос: описание по-английски, от 20 до {VOICE_TEXT} знаков")
    elif ("woman" in voice) != woman or not re.search(r"\b(man|male|woman)\b", voice):
        # the scenes of two tell a woman by her voice (gossip.py)
        out.append(f"голос: скажи, что это голос {'женщины (woman)' if woman else 'мужчины (man)'}")
    if len(person.past) > PERSON_TEXT:
        out.append(f"предыстория: не длиннее {PERSON_TEXT} знаков")
    if len(person.knows) > FACTS_MAX or any(not 5 <= len(f) <= FACT_TEXT for f in person.knows):
        out.append(f"что знает: до {FACTS_MAX} фактов, каждый от 5 до {FACT_TEXT} знаков")
    if len(person.ties) > TIES_MAX:
        out.append(f"знакомых: не больше {TIES_MAX}")
    for tie in person.ties:
        if tie.npc == spec.npc or tie.npc not in alive:
            out.append(f"знакомый {tie.npc}: такого живого человека нет")
        if not 5 <= len(tie.how) <= TIE_TEXT:
            out.append(f"знакомый {tie.npc}: как знакомы — от 5 до {TIE_TEXT} знаков")
    return out


def _minutes(clock: str) -> int | None:
    m = re.fullmatch(r"(\d\d):(\d\d)", clock)
    if m is None or int(m[1]) > 23 or int(m[2]) > 59:
        return None
    return int(m[1]) * 60 + int(m[2])


def _day_problems(spec: NpcSpec) -> list[str]:
    """The routine fills the day: one after another, no gaps, no overlaps."""
    spans: list[tuple[int, int]] = []
    for e in spec.routine:
        start, end = _minutes(e.start), _minutes(e.end)
        if start is None or end is None:
            return [f"распорядок: время пишется ЧЧ:ММ, а не {e.start}–{e.end}"]
        if start == end:
            return [f"распорядок: {e.start}–{e.end} — пустой отрезок"]
        spans.append((start, (end - start) % DAY))
    if sum(length for _, length in spans) != DAY:
        return ["распорядок: отрезки должны занимать ровно сутки"]
    spans.sort()
    for (start, length), (following, _) in zip(spans, [*spans[1:], spans[0]], strict=True):
        if (start + length) % DAY != following:
            return ["распорядок: каждый отрезок начинается там, где кончился прошлый"]
    return []


def problems(spec: NpcSpec, symbols: Mapping[str, str], taken: Collection[str]) -> list[str]:
    """What is wrong with a description, in words for the model. `symbols`: what the game says the names are
    (story_state, "npc", "item", "const", "func"...); `taken`: the ids of the branch's characters."""
    out: list[str] = []

    def kind(name: str, want: str, what: str) -> None:
        if symbols.get(name) != want:
            out.append(f"{what} {name} нет в игре")

    if not ID.fullmatch(spec.npc):
        out.append(f"персонаж режиссёра называется DIR_NPC_…: {spec.npc}")
    if spec.npc in taken:
        out.append(f"персонаж {spec.npc} уже есть")
    if not TEMPLATES.fullmatch(spec.like):
        out.append(
            f"шаблон — безымянный крестьянин, горожанин, наёмник или бандит скриптов, а не {spec.like}"
        )
    else:
        kind(spec.like, "npc", "NPC")
    if not spec.name.strip() or len(spec.name) > NAME_MAX:
        out.append(f"имя — от 1 до {NAME_MAX} знаков")
    if spec.guild is not None:
        if spec.guild not in GUILDS:
            out.append(f"гильдия — одна из {', '.join(sorted(GUILDS))}, а не {spec.guild}")
        else:
            kind(spec.guild, "const", "гильдии")
    if (look := spec.look) is not None:
        if look.gender not in HEADS:
            out.append(f"пол — MALE или FEMALE, а не {look.gender}")
        else:
            if look.head.upper() not in HEADS[look.gender]:
                out.append(f"голова {look.head} не годится: {', '.join(sorted(HEADS[look.gender]))}")
            faces_ok = MALE_FACES.fullmatch(look.face) if look.gender == "MALE" else look.face in FEMALE_FACES
            if not faces_ok:
                out.append(f"лицо {look.face} не годится: только лица безымянных")
            else:
                kind(look.face, "const", "лица")
            if look.armor is not None:
                if look.armor not in ARMORS[look.gender]:
                    out.append(f"одежда {look.armor} не годится: {', '.join(sorted(ARMORS[look.gender]))}")
                else:
                    kind(look.armor, "item", "одежды")
        if look.skin not in SKINS:
            out.append(f"кожа — одна из {', '.join(sorted(SKINS))}, а не {look.skin}")
    for thing in spec.items:
        kind(thing.item, "item", "предмета")
        top = MAX_GOLD if thing.item == "ITMI_GOLD" else MAX_COUNT
        if not 1 <= thing.count <= top:
            out.append(f"{thing.item}: от 1 до {top}")
    if spec.routine:
        out += _day_problems(spec)
        for state in sorted({e.state for e in spec.routine}):
            if not state.startswith("ZS_"):
                out.append(f"распорядок: состояние скриптов называется ZS_…: {state}")
            else:
                kind(state, "func", "состояния")
    return out


def _names(spec: NpcSpec) -> list[str]:
    """The names the game must know."""
    names = [spec.like, *(t.item for t in spec.items), *(e.state for e in spec.routine)]
    if spec.guild:
        names.append(spec.guild)
    if spec.look is not None:
        names += [spec.look.face, *([spec.look.armor] if spec.look.armor else [])]
    return sorted(set(names))


class Cast:
    """The director's characters on the current branch, and how a new one comes."""

    def __init__(self, link: GameLink, patches: StoryPatches, chronicle: Chronicle | None = None) -> None:
        self.link = link
        self.patches = patches
        self.chronicle = chronicle
        # after a character came: whoever keeps the world's people (the rumors' ties) takes them again
        self.on_change: list[Callable[[], None]] = []
        self._people: tuple[object, dict[str, tuple[NpcSpec, Person]]] = (None, {})  # prepared session
        self._cards: tuple[object, dict[str, Card]] = (None, {})  # made of that `people`
        link.listeners.append(self._on_message)

    async def characters(self) -> list[NpcSpec]:
        return await self.patches.characters()

    async def _on_message(self, message: GameMessage) -> None:
        if isinstance(message, Session):
            await self.prepare()

    async def prepare(self) -> None:
        version, branch, session = self.patches.version, self.patches.branch, self.link.session

        async def load() -> None:
            if (
                version != self.patches.version
                or branch != self.patches.branch
                or session is not self.link.session
            ):
                raise GameError("session changed while preparing character cards")
            records = await self.patches.cast()
            if (
                version != self.patches.version
                or branch != self.patches.branch
                or session is not self.link.session
            ):
                raise GameError("session changed while preparing character cards")
            out = {}
            for record in records:
                if record.get("person") is not None:
                    spec = NpcSpec.model_validate(record["spec"])
                    out[spec.npc] = (spec, Person.model_validate(record["person"]))
            self._people = (session, out)

        await self.patches.memory.complete(load)

    def people(self) -> dict[str, tuple[NpcSpec, Person]]:
        """The characters that are persons (layer 3), by id: their description and who they are. Read often
        (every card asked for), so prepared on Session and after introducing a character."""
        if self.patches.branch is None:
            return {}
        if self._people[0] is not self.link.session:
            raise RuntimeError("character cards are not prepared for the current story")
        return self._people[1]

    def person(self, npc: str) -> Person | None:
        found = self.people().get(npc.upper())
        return found[1] if found else None

    def cards(self) -> dict[str, Card]:
        """The persons' cards, for characters.Cards."""
        people = self.people()
        if self._cards[0] is not people:
            self._cards = (
                people,
                {npc: person.card(npc, spec.name) for npc, (spec, person) in people.items()},
            )
        return self._cards[1]

    def bonds(self) -> dict[str, dict[str, str]]:
        """Whom the persons know, {npc: {other: how}}: the ties of social.py."""
        return {npc: {t.npc: t.how for t in p.ties} for npc, (_, p) in self.people().items() if p.ties}

    async def get(self, npc: str) -> NpcSpec | None:
        return next((c for c in await self.characters() if c.npc == npc), None)

    async def next_id(self) -> str:
        """DIR_NPC_<n>, the next one free on the branch."""
        taken = {c.npc for c in await self.characters()}
        n = 1
        while f"DIR_NPC_{n}" in taken:
            n += 1
        return f"DIR_NPC_{n}"

    async def introduce(
        self, spec: NpcSpec, person: Person | None = None
    ) -> tuple[dict[str, Any] | None, list[str]]:
        """A new character, and who it is as a person if given: the code checks it, the game makes it, the
        branch keeps it. The game's answer ({npc, state, index}), or None and what is wrong."""
        await self.link.wait_session_ready()
        branch, session, generation = self.patches.branch, self.link.session, self.link.world_generation
        known = [t.npc for t in person.ties] if person is not None else []
        state = await self.link.story_state(symbols=_names(spec), npcs=known)
        characters = await self.characters()
        if (
            branch != self.patches.branch
            or session is not self.link.session
            or generation != self.link.world_generation
        ):
            raise GameError("session changed while introducing a character")
        issues = problems(spec, state.get("symbols", {}), {c.npc for c in characters})
        name = spec.name.strip().casefold()
        nameless_bandit = (
            person is None
            and re.fullmatch(r"BDT_\d+_BANDIT_[LMH]", spec.like) is not None
            and name in {"бандит", "bandit"}
        )
        if name and not nameless_bandit:
            people = await self.link.story_people()
            if (
                branch != self.patches.branch
                or session is not self.link.session
                or generation != self.link.world_generation
            ):
                raise GameError("session changed while introducing a character")
            other_names = {
                str(p.get("name") or "").strip().casefold() for p in people if p.get("npc") != spec.npc
            } | {c.name.strip().casefold() for c in characters if c.npc != spec.npc}
            if name in other_names:
                issues.append(f"имя «{spec.name.strip()}» уже есть у человека мира")
        if person is not None:
            alive = {n for n, s in state.get("npcs", {}).items() if s.get("state") == "alive"}
            issues += person_problems(person, spec, alive)
        if not issues:
            try:
                made = await self.patches.introduce(spec, person.model_dump() if person is not None else None)
            except GameError as err:
                issues = [f"игра не сделала персонажа: {err}"]
            else:
                if (
                    branch != self.patches.branch
                    or session is not self.link.session
                    or generation != self.link.world_generation
                ):
                    raise GameError("session changed while introducing a character")
                await self.prepare()
                if (
                    branch != self.patches.branch
                    or session is not self.link.session
                    or generation != self.link.world_generation
                ):
                    raise GameError("session changed while introducing a character")
                log.info("cast: %s «%s» like %s at %s", spec.npc, spec.name, spec.like, spec.at)
                if self.chronicle is not None:
                    await self.chronicle.record(
                        {"fact": "cast", "npc": spec.npc, "name": spec.name, "like": spec.like, "at": spec.at}
                    )
                if (
                    branch != self.patches.branch
                    or session is not self.link.session
                    or generation != self.link.world_generation
                ):
                    raise GameError("session changed while introducing a character")
                for listener in self.on_change:
                    listener()
                return made, []
        log.info("cast: %s rejected: %s", spec.npc, "; ".join(issues))
        return None, issues
