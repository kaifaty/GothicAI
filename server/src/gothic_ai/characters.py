"""Character cards (docs/npc-tools.md, wave 1 step 3): who an npc is and how it behaves.

How readily an npc haggles, comes up to the hero or takes his word is its character's, not the code's
(the author's call, 2026-10-12). So every npc with dialogs gets a card, written once by the large model
from its own lines in the game (the story graph has them, chapter by chapter) and what it does for the
hero (trade, teaching, work, joining), then reviewed and kept in a file: gamedata/characters.json.

A card is about character and manner only, never the plot: what happens later is not the npc's to know
(M6), and a card is read in every chapter.

What reads it:
- persona.py: the card's text and its traits in words, for the conversational model;
- the behavioral model (brain/): one English line.
"""

from __future__ import annotations

import json
import logging
import re
from collections.abc import Callable, Iterator, Mapping
from dataclasses import asdict, dataclass, fields
from pathlib import Path
from typing import Any

from .crime_law_graph import init_ops
from .gamedata.storygraph import StoryGraph
from .llm import LLMBackend, LLMError, Message, Tool, ToolCall
from .story import freed
from .validator import Patches

log = logging.getLogger(__name__)

CARDS_FILE = Path(__file__).parent / "gamedata" / "characters.json"
SAMPLE_LINES = 30  # of its own lines, for the model
BATCH = 8  # npc's a call
LEVELS = ["очень низкая", "низкая", "средняя", "высокая", "очень высокая"]
STRANGERS = ["враждебен", "подозрителен", "сдержан", "приветлив", "доверчив"]
# what each scale means, for the model and for the persona
SCALES = {
    "greed": "жадность: как дорого продаёт, как торгуется, сколько хочет за услугу",
    "honesty": "честность: держит ли слово, может ли обмануть или покрывать обман",
    "courage": "смелость: дерётся или бежит, спорит ли с сильным",
    "talk": "разговорчивость: охотно ли болтает, рассказывает новости и слухи",
    "pushy": "назойливость: сам ли подходит к герою и пристаёт с делом, или ждёт, пока к нему придут",
    "strangers": "к чужакам: 1 — подозрителен и неприветлив, 5 — открыт и доверчив",
}
GUILDS = {
    0: "без гильдии",
    1: "паладин короля",
    2: "ополченец Хориниса",
    3: "горожанин Хориниса",
    4: "маг Огня",
    5: "послушник монастыря",
    6: "охотник на драконов",
    7: "наёмник с фермы Онара",
    8: "крестьянин",
    9: "бандит",
    10: "заключённый",
    11: "ищущий",
    12: "поселенец из окрестностей",
    13: "пират",
    14: "маг Воды",
}


@dataclass(frozen=True)
class Card:
    npc: str
    name: str
    ru: str  # 2–4 sentences in the second person, «Ты — …»: who it is, its temper and manner
    en: str  # one English line for the behavioral model
    greed: int  # 1..5, see SCALES
    honesty: int
    courage: int
    talk: int
    pushy: int
    strangers: int

    @property
    def lead(self) -> int:
        """How much it takes the lead among others: bold and pushy (Brago among his bandits). The first to
        answer a phrase to several (talk.py)."""
        return self.courage + self.pushy

    def traits_ru(self) -> str:
        """Its traits in words, for the conversational model."""
        parts = [
            f"{SCALES[k].split(':')[0]} — {LEVELS[getattr(self, k) - 1]}" for k in SCALES if k != "strangers"
        ]
        parts.append(f"к чужакам — {STRANGERS[self.strangers - 1]}")
        return "Твой характер: " + "; ".join(parts) + "."


class Cards(Mapping[str, Card]):
    """The cards of the scripts' npc's (characters.json) and of the director's own characters, as one: the
    readers of cards (persona, deals, the behavioral model, the author) see a character the director brings
    in like any other, as soon as it comes. `extra` gives the current branch's own; its cards win."""

    def __init__(self, static: Mapping[str, Card], extra: Callable[[], Mapping[str, Card]] = dict) -> None:
        self.static = static
        self.extra = extra

    def __getitem__(self, npc: str) -> Card:
        own = self.extra()
        return own[npc] if npc in own else self.static[npc]

    def __iter__(self) -> Iterator[str]:
        own = self.extra()
        yield from own
        yield from (npc for npc in self.static if npc not in own)

    def __len__(self) -> int:
        own = self.extra()
        return len(own) + sum(1 for npc in self.static if npc not in own)


class Traits(Mapping[str, str]):
    """The cards' English lines (the behavioral model's), by npc: live, as the cards are."""

    def __init__(self, cards: Mapping[str, Card]) -> None:
        self.cards = cards

    def __getitem__(self, npc: str) -> str:
        return self.cards[npc].en

    def __iter__(self) -> Iterator[str]:
        return iter(self.cards)

    def __len__(self) -> int:
        return len(self.cards)


def load(path: Path = CARDS_FILE) -> dict[str, Card]:
    """The cards by npc symbol; none if the file is missing or broken."""
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as err:
        log.info("characters: no cards (%s)", err)
        return {}
    names = {f.name for f in fields(Card)}
    out = {}
    for npc, c in data.get("cards", {}).items():
        try:
            out[npc.upper()] = Card(**{k: v for k, v in c.items() if k in names})
        except TypeError as err:
            log.warning("characters: bad card %s: %s", npc, err)
    return out


def save(cards: dict[str, Card], path: Path = CARDS_FILE) -> None:
    data = {"version": 1, "cards": {npc: asdict(c) for npc, c in sorted(cards.items())}}
    path.write_text(json.dumps(data, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")


# --- writing them ----------------------------------------------------------------------------------


@dataclass(frozen=True)
class Subject:
    """What the model is told of an npc."""

    npc: str
    name: str
    guild: int
    does: list[str]  # for the hero: trades, teaches, gives work, takes into the guild
    day: list[str]  # its routine's activities
    lines: list[str]  # its own lines in the game

    def text(self) -> str:
        role = GUILDS.get(self.guild, "")
        head = f"## {self.npc} — {self.name}{f', {role}' if role else ''}"
        parts = [head]
        if self.does:
            parts.append("Для героя: " + ", ".join(self.does) + ".")
        if self.day:
            parts.append("Его день: " + ", ".join(self.day) + ".")
        parts.append("Его реплики в игре:\n" + "\n".join(f"- {t}" for t in self.lines))
        return "\n".join(parts)


INSTRUCTIONS = f"""\
Ты — сценарист Gothic II: Ночь Ворона. Для каждого персонажа из списка составь карточку его характера \
по его собственным репликам в игре — так, как его задумали авторы. Она нужна, чтобы этот персонаж говорил \
и вёл себя в своём характере там, где игра даёт ему свободу: торговался, сам подходил к герою или ждал, \
верил на слово.

В карточке только характер и манера: кто он, нрав, как говорит, чего хочет от жизни. Никакого сюжета: не \
упоминай событий его квестов, того, что случится, судеб других людей и того, что он узнает потом. Только то, \
что видно по нему с первой встречи.

- ru: 2–4 предложения от второго лица, начиная с «Ты — <имя>, …», по-русски, просто, в духе Gothic;
- en: одна строка по-английски, до 25 слов: характер для модели поведения (без имени);
- шкалы от 1 до 5:
{chr(10).join(f"  - {k} — {v}" for k, v in SCALES.items())}

Шкалы — по репликам, а не по профессии: торговец бывает честным, наёмник — трусом. Если о чём-то реплики \
молчат, ставь 3. Ответ — один вызов write_cards со всеми персонажами списка."""

_CARD = {
    "type": "object",
    "properties": {
        "npc": {"type": "string", "description": "символ персонажа из списка"},
        "ru": {"type": "string"},
        "en": {"type": "string"},
        **{k: {"type": "integer", "description": v} for k, v in SCALES.items()},
    },
    "required": ["npc", "ru", "en", *SCALES],
    "additionalProperties": False,
}
WRITE_TOOL = Tool(
    "write_cards",
    "Карточки характера персонажей из списка.",
    {
        "type": "object",
        "properties": {"cards": {"type": "array", "items": _CARD}},
        "required": ["cards"],
        "additionalProperties": False,
    },
)


def subjects(graph: StoryGraph, people: list[dict[str, Any]], only: set[str] | None = None) -> list[Subject]:
    """The npc's with lines of their own in the graph, among the people of the world (story_people)."""
    disabled = Patches.of([*freed(graph.nodes.__contains__), *init_ops(graph)]).disabled
    active: set[str] = set()
    pending = [node.id for node in graph.nodes.values() if not node.is_choice]
    while pending:
        id = pending.pop()
        if id in active or id in disabled:
            continue
        active.add(id)
        pending.extend(graph.nodes[id].choices())
    by_npc: dict[str, list[str]] = {}
    does: dict[str, set[str]] = {}
    for node in graph.nodes.values():
        if node.id not in active:
            continue
        npc = (node.npc or "").upper()
        if not npc or npc.startswith(("$", "PC_HERO")) or npc == "CH":
            continue
        for e in node.effects:
            if e.get("do") == "line" and e.get("speaker") == ["inst", "SELF"] and e.get("text"):
                by_npc.setdefault(npc, []).append(str(e["text"]))
            if e.get("do") == "set_guild":
                does.setdefault(npc, set()).add("принимает в гильдию")
            if e.get("do") == "call" and str(e.get("fn", "")).startswith("B_TEACH"):
                does.setdefault(npc, set()).add("учит")
            if e.get("do") == "set" and str(e.get("var", "")).startswith("MIS_"):
                does.setdefault(npc, set()).add("даёт работу")
        if node.trade:
            does.setdefault(npc, set()).add("торгует")
    out = []
    for p in people:
        npc = str(p.get("npc", "")).upper()
        if npc not in by_npc or (only is not None and npc not in only):
            continue
        day = sorted({_activity(r.get("state", "")) for r in p.get("routine", []) if r.get("state")} - {""})
        out.append(
            Subject(
                npc,
                str(p.get("name") or npc),
                int(p.get("guild", 0)),
                sorted(does.get(npc, ())),
                day,
                _pick(by_npc[npc]),
            )
        )
    return out


async def write(llm: LLMBackend, batch: list[Subject]) -> tuple[dict[str, Card], list[str]]:
    """Cards for a batch, and what was wrong with the answer."""
    text = "\n\n".join(s.text() for s in batch)
    calls: list[ToolCall] = []
    try:
        async for ev in llm.stream(
            instructions=INSTRUCTIONS, messages=[Message("user", text)], tools=[WRITE_TOOL]
        ):
            if isinstance(ev, ToolCall) and ev.name == WRITE_TOOL.name:
                calls.append(ev)
    except LLMError as err:
        return {}, [str(err)]
    if not calls:
        return {}, ["no write_cards call"]
    names = {s.npc: s.name for s in batch}
    out: dict[str, Card] = {}
    errors = []
    for c in calls[0].arguments.get("cards", []):
        npc = str(c.get("npc", "")).upper()
        if npc not in names:
            errors.append(f"{npc}: not in the list")
            continue
        scales = {k: int(c.get(k) or 3) for k in SCALES}
        if any(not 1 <= v <= 5 for v in scales.values()):
            errors.append(f"{npc}: a scale out of 1..5")
            continue
        ru, en = str(c.get("ru", "")).strip(), str(c.get("en", "")).strip()
        if not ru or not en:
            errors.append(f"{npc}: no text")
            continue
        out[npc] = Card(npc, names[npc], ru, en, **scales)
    errors += [f"{npc}: no card" for npc in names if npc not in out]
    return out, errors


def _pick(lines: list[str], n: int = SAMPLE_LINES) -> list[str]:
    """Evenly spread, medium-length, no repeats: its voice, not one quest."""
    good = list(dict.fromkeys(t for t in lines if 25 <= len(t) <= 220)) or list(dict.fromkeys(lines))
    if len(good) <= n:
        return good
    step = len(good) / n
    return [good[int(i * step)] for i in range(n)]


def _activity(state: str) -> str:
    """ZS_SMITH_ANVIL -> smith anvil: what the model reads of a routine's state."""
    return re.sub(r"^(ZS|TA)_", "", state.upper()).replace("_", " ").lower()
