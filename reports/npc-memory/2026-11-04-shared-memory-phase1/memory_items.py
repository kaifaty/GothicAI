"""Пункты памяти NPC для офлайн-пилота фазы 1 общей памяти (замысел — README.md рядом).

Модель здесь не вызывается. Sol после разговора пишет правки в грамматике ума v3, код их применяет и
сам закрывает старое, Gemma читает короткую выжимку. Запуск из server/:
`uv run python ../reports/npc-memory/2026-11-04-shared-memory-phase1/memory_items.py selftest`.

Что зовёт стенд
---------------
Имена и id:
- `build_names(snapshots=…, entries=…, graph=…, quests=…, items=…)` → `Names`. NPC берутся из карточек
  персонажей (`characters.load`), имён книги знаний (`Lore.names` — их же `fix_names` получает в игре)
  и записанных обстановок (снимки `npc` и `nearby_npcs` из трасс). Места — из `PlaceBook`, квесты — из
  графа сюжета (`StoryGraph`, без игры его нет — передайте `graph` или `quests`) и из фактов хроники.
  Любую таблицу можно передать параметром; `None` — загрузить с сервера.
- `Names.resolve(имя)` → id или `None` (склонения, предлоги, искажения распознавания);
  `Names.find(фраза, near=[id…])` → id, названные во фразе (так же, как `fix_names`).

Состояние NPC — словарь, который целиком кладётся в JSON и переносится между встречами:
- `new_state(npc)`; `prepare(state, minute)` перед свёрткой — время и остывание чувств, возвращает
  `{"elapsed", "cooled"}`; `render_for_sol(state)` — открытые пункты с номерами, время с прошлой свёртки
  и нерешённые столкновения, для промпта свёртки;
- `fold(state, текст_Sol, names=…, lines={номер строки: id хода}, minute=…, entries=[(seq, факт)…])` —
  разбор, применение правок, пределы, забывание мелочи, столкновения. Возвращает
  `{"stats": счёт разбора, "applied": [запись на строку], "errors": число отклонённых строк,
  "forgotten": [id], "collisions": [пары]}`. Части по отдельности: `parse_patches`, `apply_patches`,
  `forget`;
- `apply_truth({npc: state}, entries, minute=…, turn_texts={id хода: текст})` — закрытие по правде
  хроники (`found_out`, `caught`; `lie` только помечает пункт ложным для мира) и по цепочке `rumor`.
  Возвращает `[{"npc", "id", "fact"}]` закрытых пунктов;
- `confirmations(state, item)` — число разных истоков подтверждения пункта.

Порядок на встречу: `prepare` → `render_for_sol` в промпт → вызов Sol → `fold` → `apply_truth` по
фактам ветки для всех NPC → `card` и `items_for` для Gemma. Счёт разбора (`stats`): `lines` — строк
правок, `bad` — непонятых, `bad_tails` — строк с битым хвостом (пункт остаётся, плохое поле отброшено),
`foreign_lines` — номеров строк вне таблицы, `names` и `unknown_names` — имён всего и без id.

Правила кода, которых нет в уме v3:
- слот: у мнения (`opinion:hero`), настроения (`mood`) и «о X» (`other:<id>`) новый пункт закрывает старый;
- «верю» или «сомневаюсь» тем же текстом от другого рассказчика — не новый пункт, а ещё один источник в
  `also`; от того же — `duplicate`, как в v3;
- «сказал X» связывается с фактом `rumor` от X к этому NPC, если пункт о квесте или деле того слуха;
  исток — первый рассказчик цепочки, поэтому два пересказа одного истока — одно подтверждение;
- столкновение — два открытых «верю»/«сомневаюсь» без слота об одном id с разным текстом; героя и самого
  NPC не считаем (о них почти всё). Пару Sol видит в одной свёртке, потом она не поднимается.

Читатель, без LLM (выжимка — `Digest(text, ids, tokens)`, `str(d)` — текст):
- `card(state, tokens=…)` — карточка ядра, ≤ 300 токенов;
- `await items_for(state, фраза, names=…, near=…, embed=EmbedClient|None, tokens=…, skip=card.ids)` —
  пункты к фразе, ≤ 150 токенов и ≤ 5 пунктов;
- `episodes_for(state, пункты_или_id, k=2)` — id ходов, на которые ссылается больше всего пунктов.

Токены считает `Tokens(url)`: токенизатор Gemma через `/tokenize` llama.cpp, если передан адрес, иначе
оценка `ceil(знаков / CHARS_PER_TOKEN)`. Способ один на объект, без тихого перехода с одного на другой.

Пункт: `id` (б1, ч2… — сквозные по цепочке встреч), `section`, `text`, `about` (игровые id),
`about_names` (имена без id), `slot`, `source` (`kind`: видел | сказал | герой | вывел | моё; `who`,
`name`, ссылки `turn` и `fact`, `origin`), `also` (ещё источники), `episodes` (id ходов), `valid_from`,
`valid_to` (игровые минуты), `strength` 1–3, `importance` 1–10, `closed_by` (sol | slot | truth | cap |
cool | forget). Закрытые пункты лежат в `state["history"]`, в выжимки не попадают.

Грамматика правок — ум v3 (`mind_turn_v3_r2.md`) плюс необязательный хвост в квадратных скобках в конце
строки, поля через «;»: `о:` имена через запятую, `стр:` номера строк разговора (можно 12-14), `от:`
источник, `сила:` 1–3, `важность:` 1–10. У «верю» и «сомневаюсь» источник можно писать и на месте
параметра: `видел`, `сказал <Имя>`, `слышал [<Имя>]`, `герой`, `вывел`, `моё`.
    + верю герой: Браго мёртв [о: Браго; стр: 12, 14]
    + верю сказал Кавалорн: бандиты сидят в пещере у башни
    + нить: обещал принести письмо [о: Ватрас; стр: 7; от: герой]
    ~ в3 [стр: 15]        - н2        = мнение: наглый, но письмо принёс [стр: 9]
"""

from __future__ import annotations

import argparse
import asyncio
import base64
import importlib.util
import json
import math
import re
import sys
import traceback
import unicodedata
from collections import Counter, defaultdict
from collections.abc import Iterable, Mapping, Sequence
from copy import deepcopy
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import httpx
import numpy as np

from gothic_ai import characters, lore, talk
from gothic_ai.embeddings import EmbedClient, EmbedError
from gothic_ai.places import PlaceBook

HERE = Path(__file__).resolve().parent
MIND_V3 = HERE.parent.parent / "npc-agent" / "2026-11-02-sol-mind" / "mind.py"

HERO = "PLAYER"
# letter: (word in a patch, heading in a render)
SECTIONS = {
    "б": ("было", "Что было между нами"),
    "ч": ("чувство", "Чувства"),
    "ж": ("хочу", "Желания"),
    "в": ("верю", "Верю"),
    "с": ("сомневаюсь", "Сомневаюсь"),
    "д": ("о", "О других"),
    "н": ("нить", "Нити"),
    "м": ("мнение", "Мнение о герое"),
    "р": ("настроение", "Настроение"),
}
LETTER = {word: letter for letter, (word, _) in SECTIONS.items()}
SLOTS = {"м": "opinion:hero", "р": "mood"}
DURATIONS = {"час": 60, "день": 1440, "долго": 10080}
WISH_TERMS = ("сейчас", "скоро", "давно")
CAPS = {"в": 10, "с": 5, "д": 8, "н": 8}  # the limits of ум v3 (mind.py enforce_caps)
WISH_CAP = 2  # per term
FORGET_IMPORTANCE = 3  # events this small are forgotten at the end of a talk
SOURCES = ("видел", "сказал", "герой", "вывел", "моё")
DEFAULT_SOURCE = {"б": "видел", "ч": "моё", "ж": "моё", "в": None, "с": "вывел", "д": "вывел", "н": "моё",
                  "м": "моё", "р": "моё"}
SOURCE_LABEL = {"видел": "видел", "сказал": "сказал", "герой": "со слов героя", "вывел": "вывел", "моё": ""}
TRUTH = ("found_out", "caught")  # the npc learned that the hero lied (quests.py unmask, caught)
CLOSED_BY = ("sol", "slot", "truth", "cap", "cool", "forget")

CARD_TOKENS = 300
ITEMS_TOKENS = 150
ITEMS_MAX = 5
# Gemma's tokenizer on Russian gives about 3,5–4 знака на токен; 3,0 counts with a margin until the bench
# measures /tokenize on the real service
CHARS_PER_TOKEN = 3.0
FRESH_HALF_LIFE = 3 * 1440  # game minutes: an item this old weighs half as fresh
# ranking of items_for: closeness to the phrase, strength or importance, freshness, how it was reached
W_CLOSE, W_WEIGHT, W_FRESH = 0.45, 0.25, 0.15
REACH_BONUS = {"seed": 0.15, "hop": 0.075, "rest": 0.0}
CARD_FEELINGS, CARD_THREADS, CARD_BELIEFS = 4, 4, 3

CHANGE = r"~\s*([бчжвсднмр]\d+)(?=\s|:|$)(.*)"
REMOVE = r"[\-–−]\s*([бчжвсднмр]\d+)(.*)"
_WORD = re.compile(r"[a-zа-яё]+", re.IGNORECASE)
_STEM_ENDINGS = tuple(sorted(
    "ами ями ого его ому ему ыми ими ой ей ом ем ам ям ах ях ых их ый ий ая яя ое ее ые ие ую юю ою ею "
    "ов ев ы и а я у ю е о ь й".split(), key=len, reverse=True,
))
_STOP = frozenset(
    "что это как его её она они оно тот там тут так для при про над под без или уже ещё еще был была было "
    "были мой моё мое моя мои твой свой себя меня тебя тебе мне нас вас них ним нам вам кто где чем чего "
    "когда если чтобы только очень есть нет все всё вот даже потом сам сама том той тех этот эта эти".split()
)
_PREPOSITIONS = frozenset("у к ко с со о об обо про от для при на в во за из без по до около".split())


# --- names -----------------------------------------------------------------------------------------


def _norm(text: str) -> str:
    return " ".join(_WORD.findall(text.lower().replace("ё", "е")))


def _stem(word: str) -> str:
    word = word.lower().replace("ё", "е")
    for ending in _STEM_ENDINGS:
        if word.endswith(ending) and len(word) - len(ending) >= 3:
            return word[: -len(ending)]
    return word


def _stems(text: str) -> list[str]:
    return list(dict.fromkeys(
        _stem(w) for w in _WORD.findall(text.lower().replace("ё", "е")) if len(w) >= 3 and w not in _STOP
    ))


def _stem_match(a: str, b: str) -> bool:
    """Stems of one word: equal, or one a prefix of the other («хорин» and «хорини»)."""
    if a == b:
        return True
    short, long = sorted((a, b), key=len)
    return len(short) >= 4 and long.startswith(short) and len(long) - len(short) <= 2


def _matched(entry: Sequence[str], query: Sequence[str]) -> int:
    return sum(any(_stem_match(e, q) for q in query) for e in entry)


class Names:
    """Names ↔ game ids: npc's and the hero by their names and case forms, places, quests and items by
    the words of their names."""

    def __init__(self, entries: Iterable[tuple[str, str, str]]) -> None:
        self.entries: list[tuple[str, str, str]] = []
        seen: set[tuple[str, str]] = set()
        for id_, name, kind in entries:
            key = (id_, _norm(name))
            if name and key[1] and key not in seen:
                seen.add(key)
                self.entries.append((id_, name, kind))
        self._name: dict[str, str] = {}
        self._kind: dict[str, str] = {}
        self._exact: dict[str, set[str]] = defaultdict(set)  # a whole name, any kind
        self._forms: dict[str, set[str]] = defaultdict(set)  # a person's name and its case forms
        self._parts: dict[str, set[str]] = defaultdict(set)  # words of persons' names, for name_edits
        self._multi: list[tuple[str, tuple[str, ...]]] = []  # places, quests, items: id and stems
        common = {_norm(n) for n in lore.COMMON_NAMES}
        for id_, name, kind in self.entries:
            self._name.setdefault(id_, name)
            self._kind.setdefault(id_, kind)
            full = _norm(name)
            if kind in ("npc", "hero"):
                if full in common:
                    continue  # «стражник», «бандит»: a kind of people, not one of them
                self._exact[full].add(id_)
                for hint in talk._asr_name_hints(name):
                    self._forms[_norm(hint)].add(id_)
                for part in full.split():
                    if len(part) >= 3 and part not in talk._ADDRESS:
                        self._parts[part].add(id_)
            else:
                self._exact[full].add(id_)
                stems = tuple(_stems(name))
                if stems:
                    self._multi.append((id_, stems))
        self.persons = [name for id_, name, kind in self.entries if kind == "npc"]
        self._seen: dict[tuple[str, bool], tuple[float, str] | None] = {}

    def name(self, id_: str) -> str:
        return self._name.get(id_, id_)

    def kind(self, id_: str) -> str | None:
        return self._kind.get(id_)

    def __contains__(self, id_: str) -> bool:
        return id_ in self._kind

    @staticmethod
    def _unique(ids: set[str] | None) -> str | None:
        return next(iter(ids)) if ids and len(ids) == 1 else None

    def _person(self, word: str, misheard: bool) -> tuple[float, str] | None:
        """The one person a word names: a case form, else the nearest name (one at that distance)."""
        if (word, misheard) not in self._seen:
            self._seen[word, misheard] = self._nearest(word, misheard)
        return self._seen[word, misheard]

    def _nearest(self, word: str, misheard: bool) -> tuple[float, str] | None:
        if word in talk._ADDRESS or len(word) < 3:
            return None
        if self._forms.get(word):
            id_ = self._unique(self._forms[word])
            return (0.0, id_) if id_ else None
        found: dict[str, float] = {}
        for part, ids in self._parts.items():
            if len(ids) == 1:
                d = talk.name_edits(word, part, misheard=misheard)
                if d is not None:
                    id_ = next(iter(ids))
                    found[id_] = min(found.get(id_, d), d)
        if not found:
            return None
        best = min(found.values())
        winners = [id_ for id_, d in found.items() if d == best]
        return (best, winners[0]) if len(winners) == 1 else None

    def _best_multi(self, words: list[str]) -> str | None:
        query = [_stem(w) for w in words if w not in _STOP]
        if not query:
            return None
        scored = []
        for id_, stems in self._multi:
            m = _matched(stems, query)
            if m and m / len(stems) >= 0.6 and m / len(query) >= 0.5:
                scored.append((m / len(stems) + m / len(query), id_))
        if not scored:
            return None
        scored.sort(reverse=True)
        if len(scored) > 1 and scored[1][0] == scored[0][0] and scored[1][1] != scored[0][1]:
            return None
        return scored[0][1]

    def resolve(self, text: str, misheard: bool = True) -> str | None:
        """The id behind a name Sol wrote («Кавалорну», «у Ксардаса», «Сардас», «пещера у башни»); None
        if there is none or several fit. A name is known to be a name here, so a misheard one is allowed
        (`misheard`, as for a call in fix_names)."""
        raw = text.strip().strip("«»\"'")
        if raw.upper() in self._kind:
            return raw.upper()
        words = _norm(raw).split()
        while len(words) > 1 and words[0] in _PREPOSITIONS:
            words.pop(0)
        if not words:
            return None
        full = " ".join(words)
        for table in (self._exact, self._forms):
            if table.get(full):
                return self._unique(table[full])
        significant = [w for w in words if len(w) >= 3 and w not in _STOP and w not in _PREPOSITIONS]
        if len(significant) >= 2 and (place := self._best_multi(significant)):
            return place
        persons = {}
        for w in significant:
            hit = self._person(w, misheard)
            if hit is not None:
                persons[hit[1]] = min(persons.get(hit[1], hit[0]), hit[0])
        if persons:
            best = min(persons.values())
            winners = [id_ for id_, d in persons.items() if d == best]
            return winners[0] if len(winners) == 1 else None
        return self._best_multi(significant)

    def find(self, phrase: str, near: Iterable[str] = (), *, heard: bool = True) -> list[str]:
        """Ids named in a free phrase: names first corrected by fix_names (`heard`: the hero's words through
        speech recognition; calls to `near` allow the wider tolerance), then case forms and strict
        name_edits; places, quests and items by most of their words."""
        if heard:
            phrase, _ = talk.fix_names(phrase, [self.name(n) for n in near], self.persons)
        words = _norm(phrase).split()
        out: list[str] = []
        for i, w in enumerate(words):
            pair = " ".join(words[i:i + 2])
            id_ = self._unique(self._forms.get(pair)) if i + 1 < len(words) else None
            if id_ is None:
                hit = self._person(w, misheard=False)
                id_ = hit[1] if hit else None
            if id_ is not None and id_ not in out:
                out.append(id_)
        query = [_stem(w) for w in words if len(w) >= 3 and w not in _STOP]
        places = []
        for id_, stems in self._multi:
            m = _matched(stems, query)
            if (len(stems) >= 2 and m >= 2 and m / len(stems) >= 0.6) or (
                len(stems) == 1 and m == 1 and len(stems[0]) >= 5
            ):
                places.append((m / len(stems), m, id_))
        places.sort(reverse=True)
        out += [id_ for *_, id_ in places[:3] if id_ not in out]
        return out


def npc_names(
    *,
    snapshots: Iterable[Mapping[str, Any]] = (),
    cards: Mapping[str, Any] | None = None,
    lore_names: Mapping[str, str] | None = None,
) -> list[tuple[str, str]]:
    """(npc id, name) pairs: character cards, the book of knowledge's names (both are what fix_names gets
    in the game, session.py) and the people of recorded situations (a trace turn's `snapshot`)."""
    cards = characters.load() if cards is None else cards
    if lore_names is None:
        book = lore.Lore.load(None, None)
        lore_names = book.names if book is not None else {}
    out = [(npc.upper(), getattr(card, "name", card)) for npc, card in cards.items()]
    out += [(npc.upper(), name) for npc, name in lore_names.items()]
    for snap in snapshots:
        people = [snap.get("npc") or {}, *(snap.get("nearby_npcs") or ())]
        out += [(p["id"].upper(), p["name"]) for p in people if p.get("id") and p.get("name")]
    return [(npc, str(name)) for npc, name in out if name]


def place_names(book: PlaceBook | None = None) -> list[tuple[str, str]]:
    book = PlaceBook.load() if book is None else book
    return [(p.id, p.ru) for p in book.places] if book is not None else []


def quest_names(
    *, graph: Any = None, entries: Iterable[tuple[int, Mapping[str, Any]]] = (),
    quests: Mapping[str, str] | None = None,
) -> list[tuple[str, str]]:
    """Quests: the story graph's (journal titles; the graph needs the game, so it is a parameter), the
    director's from the chronicle (`quest`, `claim`, `lie`…) and any given as {id: title}."""
    out = list((quests or {}).items())
    if graph is not None:
        out += [(var, title) for var, q in graph.quests.items() for title in q.topics.values() if title]
    for _, f in entries:
        qid = f.get("quest") or (f.get("id") if f.get("fact") == "quest" else None)
        if qid and f.get("title") and not f.get("said"):
            out.append((str(qid), str(f["title"])))
    return out


def build_names(
    *,
    snapshots: Iterable[Mapping[str, Any]] = (),
    cards: Mapping[str, Any] | None = None,
    lore_names: Mapping[str, str] | None = None,
    places: Mapping[str, str] | None = None,
    graph: Any = None,
    entries: Iterable[tuple[int, Mapping[str, Any]]] = (),
    quests: Mapping[str, str] | None = None,
    items: Mapping[str, str] | None = None,
) -> Names:
    rows = [(HERO, "герой", "hero")]
    rows += [(npc, name, "npc") for npc, name in npc_names(snapshots=snapshots, cards=cards,
                                                          lore_names=lore_names)]
    rows += [(id_, name, "place") for id_, name in (places.items() if places is not None else place_names())]
    rows += [(id_, name, "quest") for id_, name in quest_names(graph=graph, entries=entries, quests=quests)]
    rows += [(id_, name, "item") for id_, name in (items or {}).items()]
    return Names(rows)


# --- tokens ----------------------------------------------------------------------------------------


class Tokens:
    """One explicit way to count tokens: Gemma's tokenizer via llama.cpp `/tokenize` if `url` is given,
    else ceil(chars / CHARS_PER_TOKEN)."""

    def __init__(self, url: str | None = None, *, transport: httpx.BaseTransport | None = None) -> None:
        self.url = url
        self._http = (
            httpx.Client(base_url=url.rstrip("/") + "/", transport=transport, timeout=5.0) if url else None
        )
        self._cache: dict[str, int] = {}

    def close(self) -> None:
        if self._http is not None:
            self._http.close()

    @property
    def method(self) -> str:
        return f"llama.cpp {self.url}/tokenize" if self._http else f"знаки / {CHARS_PER_TOKEN}"

    def count(self, text: str) -> int:
        if not text:
            return 0
        if self._http is None:
            return math.ceil(len(text) / CHARS_PER_TOKEN)
        if text not in self._cache:
            response = self._http.post("tokenize", json={"content": text, "add_special": False})
            response.raise_for_status()
            self._cache[text] = len(response.json()["tokens"])
        return self._cache[text]


# --- state ----------------------------------------------------------------------------------------


def new_state(npc: str) -> dict[str, Any]:
    return {
        "npc": npc.upper(),
        "minute": None,
        "time": "—",
        "folded": None,  # the minute of the last fold
        "counters": dict.fromkeys(SECTIONS, 0),
        "items": [],  # open, in the order they came
        "history": [],  # closed, with valid_to and closed_by
        "collisions": [],  # pairs the next fold puts to Sol
        "collisions_seen": [],  # pairs Sol has been shown already
        "truth_seen": [],  # chronicle seqs applied by apply_truth
        "log": [],  # every add, change, confirm and close: {op, id, by, minute, …}
    }


def time_text(minute: int) -> str:
    day, rest = divmod(minute, 1440)
    return f"день {day}, {rest // 60:02d}:{rest % 60:02d}"


def minute_of(situation: str) -> int | None:
    """The game minute of a situation's «Время: день N, HH:MM» (as mind.py prepare_state reads it)."""
    match = re.search(r"(?m)^Время: день (\d+), (\d{2}):(\d{2})", situation)
    return int(match[1]) * 1440 + int(match[2]) * 60 + int(match[3]) if match else None


def open_items(state: Mapping[str, Any], letters: str | None = None) -> list[dict[str, Any]]:
    return [item for item in state["items"] if letters is None or item["section"] in letters]


def find_item(state: Mapping[str, Any], id_: str) -> dict[str, Any] | None:
    return next((i for i in (*state["items"], *state["history"]) if i["id"] == id_), None)


def _close(state: dict[str, Any], item: dict[str, Any], by: str, minute: int, **why: Any) -> None:
    assert by in CLOSED_BY, by
    state["items"].remove(item)
    item["valid_to"] = minute
    item["closed_by"] = by
    state["history"].append(item)
    state["log"].append({"op": "close", "id": item["id"], "by": by, "minute": minute, **why})


def prepare(state: dict[str, Any], minute: int) -> dict[str, Any]:
    """Before a fold: the time, and feelings cool by game time as in mind.py prepare_state."""
    previous = state["minute"]
    state["minute"], state["time"] = minute, time_text(minute)
    cooled = []
    for item in open_items(state, "ч"):
        period = DURATIONS[item["duration"]]
        periods = max(0, (minute - item["updated_minute"]) // period)
        gone = item["updated_minute"] + item["strength"] * period  # when it would reach zero
        item["strength"] -= periods
        item["updated_minute"] += periods * period
        if item["strength"] <= 0:
            item["strength"] = 0
            _close(state, item, "cool", min(minute, gone))
            cooled.append(item["id"])
    since = state["folded"] if state["folded"] is not None else previous
    return {"elapsed": minute - since if since is not None else 0, "cooled": cooled}


# --- parsing patches -------------------------------------------------------------------------------


def _new_stats() -> dict[str, int]:
    return {"lines": 0, "bad": 0, "bad_tails": 0, "foreign_lines": 0, "unknown_names": 0, "names": 0}


def _split_tail(line: str) -> tuple[str, str | None, bool]:
    """The line without its tail, the tail's inside, and whether the tail is broken (not closed)."""
    match = re.search(r"\s*\[([^\[\]]*)\]\s*$", line)
    if match:
        return line[: match.start()], match[1], False
    start = line.rfind("[")
    if start >= 0 and "]" not in line[start:]:
        return line[:start].rstrip(), line[start + 1:], True
    return line, None, False


def _source(text: str, names: Names | None, stats: dict[str, int]) -> dict[str, Any]:
    """«видел», «сказал Кавалорн», «слышал [от Ватраса]», «герой», «вывел», «моё» → a source."""
    if _norm(text) in ("со слов героя", "слова героя", "сказал герой", "герой сказал"):
        return {"kind": "герой", "who": HERO}
    words = text.strip().split(maxsplit=1)
    head = words[0].casefold().replace("ё", "е") if words else ""
    rest = words[1].strip() if len(words) > 1 else ""
    if head in ("видел", "видела") and rest in ("", "сам", "сама"):
        return {"kind": "видел"}
    if head in ("герой", "утверждает") and rest in ("", "герой"):
        return {"kind": "герой", "who": HERO}
    if head in ("вывел", "вывела", "думаю") and rest in ("", "сам", "сама"):
        return {"kind": "вывел"}
    if head == "мое" and not rest:
        return {"kind": "моё"}
    if head in ("сказал", "сказала", "слышал", "слышала", "говорил", "говорила"):
        if not rest:
            return {"kind": "сказал", "who": None, "name": ""}
        rest = re.sub(r"^(от|со слов)\s+", "", rest)
        stats["names"] += 1
        who = names.resolve(rest) if names is not None else None
        if who == HERO:
            return {"kind": "герой", "who": HERO}
        if who is None:
            stats["unknown_names"] += 1
        return {"kind": "сказал", "who": who, "name": names.name(who) if who and names else rest}
    raise ValueError("bad_source")


def _lines(text: str, lines: Mapping[int, str], stats: dict[str, int]) -> list[str]:
    out = []
    for token in re.split(r"[,\s]+", text.strip()):
        if not token:
            continue
        span = re.fullmatch(r"(\d+)(?:[-–](\d+))?", token)
        if span is None:
            raise ValueError("bad_lines")
        first, last = int(span[1]), int(span[2] or span[1])
        if last < first or last - first > 20:
            raise ValueError("bad_lines")
        for number in range(first, last + 1):
            if number in lines:
                out.append(lines[number])
            else:
                stats["foreign_lines"] += 1
    return list(dict.fromkeys(out))


def _tail(
    text: str, names: Names | None, lines: Mapping[int, str], stats: dict[str, int]
) -> tuple[dict, bool]:
    """A tail's fields; a bad field is dropped, the rest stays. Returns (fields, all fields were good)."""
    fields: dict[str, Any] = {}
    good = True
    for part in text.split(";"):
        if not part.strip():
            continue
        key, colon, value = part.partition(":")
        key = key.strip().casefold().rstrip(".")
        try:
            if not colon or not value.strip():
                raise ValueError("bad_field")
            if key in ("о", "об", "про"):
                about, unknown = fields.setdefault("about", []), fields.setdefault("about_names", [])
                for name in value.split(","):
                    if not name.strip():
                        continue
                    stats["names"] += 1
                    id_ = names.resolve(name) if names is not None else None
                    if id_ is None:
                        stats["unknown_names"] += 1
                        unknown.append(name.strip())
                    elif id_ not in about:
                        about.append(id_)
            elif key in ("стр", "строка", "строки"):
                fields["episodes"] = _lines(value, lines, stats)
            elif key in ("от", "источник"):
                fields["source"] = _source(value, names, stats)
            elif key in ("сила", "важность"):
                number = int(value.strip())
                if not (1 <= number <= (3 if key == "сила" else 10)):
                    raise ValueError("bad_number")
                fields["strength" if key == "сила" else "importance"] = number
            else:
                raise ValueError("bad_key")
        except ValueError:
            good = False
    return fields, good


def _params(letter: str, params: str, *, adding: bool, names: Names | None, stats: dict) -> dict[str, Any]:
    """The parameters before «:» as ум v3 reads them (mind.py patch_params), the source of a belief or a
    doubt as above."""
    tokens = params.split()
    if not tokens:
        if adding and letter in "бчжд":
            raise ValueError("syntax")
        return {}
    if letter == "б" and len(tokens) == 1 and tokens[0].isdigit() and 1 <= int(tokens[0]) <= 10:
        return {"importance": int(tokens[0])}
    if letter == "ч":
        values: dict[str, Any] = {}
        if tokens[0].isdigit():
            values["strength"] = int(tokens.pop(0))
            if not 1 <= values["strength"] <= 3:
                raise ValueError("bad_param")
        if tokens and tokens[0] in DURATIONS:
            values["duration"] = tokens.pop(0)
        if not tokens and values and (not adding or len(values) == 2):
            return values
    if letter == "ж" and len(tokens) == 1 and params in WISH_TERMS:
        return {"term": params}
    if letter in "вс":
        return {"source": _source(params, names, stats)}
    if letter == "д":
        stats["names"] += 1
        who = names.resolve(params) if names is not None else None
        if who is None:
            stats["unknown_names"] += 1
        return {"name": names.name(who) if who and names is not None else params.strip(), "who": who}
    raise ValueError("bad_param")


def parse_patches(text: str, *, names: Names | None = None, lines: Mapping[int, str] | None = None) -> dict:
    """Sol's patches → {"patches": [...], "stats": {lines, bad, bad_tails, foreign_lines, unknown_names,
    names}}. A line the grammar does not know is counted in `bad` and kept with its error; a broken tail
    loses only its bad fields."""
    lines = lines or {}
    stats = _new_stats()
    body = text.split("[Правки]", 1)[1] if "[Правки]" in text else text
    patches = []
    for raw in body.splitlines():
        line = raw.strip()
        if not line or re.fullmatch(r"\[[^\]]*\]", line):
            continue
        stats["lines"] += 1
        patch: dict[str, Any] = {"line": raw, "op": None, "letter": None, "values": {}, "text": "",
                                 "tail": {}, "error": None}
        patches.append(patch)
        line, tail, broken = _split_tail(line)
        if tail is not None:
            patch["tail"], good = _tail(tail, names, lines, stats)
            if broken or not good:
                stats["bad_tails"] += 1
        try:
            if line in ("—", "–", "-"):
                patch["op"] = "noop"
            elif line.startswith("="):
                patch["op"] = "="
                match = re.fullmatch(r"=\s*(мнение|настроение)\s*:\s*(.+)", line)
                if match is None:
                    raise ValueError("syntax")
                patch["letter"], patch["text"] = LETTER[match[1]], match[2].strip()
            elif line.startswith("+"):
                patch["op"] = "+"
                match = re.fullmatch(
                    r"\+\s*(было|чувство|хочу|верю|сомневаюсь|нить|о)(?:\s+([^:]*?))?\s*:\s*(.+)", line
                )
                if match is None:
                    raise ValueError("syntax")
                patch["letter"] = LETTER[match[1]]
                patch["values"] = _params(patch["letter"], (match[2] or "").strip(), adding=True,
                                          names=names, stats=stats)
                patch["text"] = match[3].strip()
                sourced = "source" in patch["values"] or "source" in patch["tail"]
                if patch["letter"] == "в" and not sourced:
                    raise ValueError("syntax")  # ум v3: a belief says where it comes from
            elif line.startswith(("~", "-", "–", "−")):
                patch["op"] = "~" if line.startswith("~") else "-"
                pattern = CHANGE if patch["op"] == "~" else REMOVE
                match = re.fullmatch(pattern, line)
                if match is None:
                    raise ValueError("syntax")
                patch["id"], patch["letter"] = match[1], match[1][0]
                if patch["op"] == "~":
                    params, _, text_ = match[2].strip().partition(":")
                    patch["values"] = _params(patch["letter"], params.strip(), adding=False, names=names,
                                              stats=stats)
                    patch["text"] = text_.strip()
                    if not patch["values"] and not patch["text"] and not patch["tail"]:
                        raise ValueError("syntax")
            else:
                raise ValueError("syntax")
        except ValueError as error:
            patch["error"] = str(error)
            stats["bad"] += 1
    return {"patches": patches, "stats": stats}


# --- applying patches ------------------------------------------------------------------------------


def normalized(text: str) -> str:
    """As mind.py: case and spaces folded, end punctuation dropped (duplicates are found by it)."""
    text = " ".join(text.casefold().split())
    while text and unicodedata.category(text[-1]).startswith("P"):
        text = text[:-1].rstrip()
    return text


def _rumor_ref(
    entries: Sequence[tuple[int, Mapping]], holder: str, who: str, about: Iterable[str]
) -> int | None:
    """The latest rumor fact the holder heard from `who` of the quest or deed the item is about; None if
    the item names none (then it is plain hearsay of the talk, not tied to a chronicle rumor)."""
    about = set(about)
    matching = [
        seq for seq, f in entries
        if f.get("fact") == "rumor" and f.get("to") == holder and f.get("from") == who
        and {f.get("quest"), f.get("crime")} & about
    ]
    return matching[-1] if matching else None


def _origin(entries: Sequence[tuple[int, Mapping]], source: Mapping[str, Any]) -> str:
    """Where a piece of news started: the first teller of its rumor chain, the hero, or the npc itself."""
    kind = source.get("kind")
    if kind == "герой":
        return HERO
    if kind != "сказал":
        return "self"
    facts = dict(entries)
    fact = facts.get(source.get("fact")) if source.get("fact") is not None else None
    if fact is None:
        return source.get("who") or "name:" + normalized(source.get("name") or "?")
    seq = source["fact"]
    while int(fact.get("hops") or 1) > 1:
        earlier = [
            (s, f) for s, f in entries
            if s < seq and f.get("fact") == "rumor" and f.get("to") == fact.get("from")
            and _same_news(f, fact)
        ]
        if not earlier:
            break
        seq, fact = earlier[-1]
    return str(fact.get("from"))


def _same_news(a: Mapping[str, Any], b: Mapping[str, Any]) -> bool:
    if a.get("quest") or b.get("quest"):
        return a.get("quest") == b.get("quest")
    return bool(a.get("crime")) and a.get("crime") == b.get("crime")


def _sourced(src: dict[str, Any] | None, letter: str, episodes: list[str], state: Mapping[str, Any],
             entries: Sequence[tuple[int, Mapping]], about: list[str]) -> dict[str, Any]:
    src = dict(src or {"kind": DEFAULT_SOURCE[letter] or "вывел"})
    src.setdefault("who", None)
    src.setdefault("name", "")
    src.setdefault("turn", episodes[0] if episodes else None)
    if src["kind"] == "сказал" and src.get("who") and "fact" not in src:
        src["fact"] = _rumor_ref(entries, state["npc"], src["who"], about)
    src.setdefault("fact", None)
    src["origin"] = _origin(entries, src)
    return src


def _teller(source: Mapping[str, Any]) -> tuple[Any, ...]:
    """Who told it, as one: the same source again is a duplicate, another one a retelling."""
    return source["kind"], source.get("who") or normalized(source.get("name") or ""), source.get("fact")


def _slot(letter: str, who: str | None, name: str) -> str | None:
    if letter in SLOTS:
        return SLOTS[letter]
    if letter == "д":
        return f"other:{who}" if who else f"other:name:{normalized(name)}"
    return None


def _close_slot(state: dict[str, Any], item: dict[str, Any], minute: int) -> list[str]:
    closed = []
    for other in list(state["items"]):
        if other is not item and item["slot"] and other["slot"] == item["slot"]:
            _close(state, other, "slot", minute, by_item=item["id"])
            closed.append(other["id"])
    return closed


def enforce_caps(state: dict[str, Any], minute: int) -> list[str]:
    """The section limits of ум v3: the oldest open items over the limit are closed (`cap`)."""
    dropped = []
    for letter, limit in (*CAPS.items(), ("ж", WISH_CAP)):
        for term in WISH_TERMS if letter == "ж" else (None,):
            items = [i for i in open_items(state, letter) if term is None or i["term"] == term]
            for item in items[:-limit]:
                _close(state, item, "cap", minute)
                dropped.append(item["id"])
    return dropped


def apply_patches(
    state: dict[str, Any],
    parsed: Mapping[str, Any],
    *,
    minute: int,
    names: Names | None = None,
    entries: Iterable[tuple[int, Mapping]] = (),
) -> list[dict[str, Any]]:
    """Apply parsed patches at a game minute: one record per line {line, op, ok, error, id, closed}."""
    entries = sorted(entries, key=lambda e: e[0])
    records = []
    for patch in parsed["patches"]:
        record = {"line": patch["line"], "op": patch["op"], "ok": False, "error": patch["error"], "id": None,
                  "closed": []}
        records.append(record)
        if patch["error"]:
            continue
        try:
            if patch["op"] == "noop":
                pass
            elif patch["op"] in ("+", "="):
                record["id"], record["closed"] = _add(state, patch, minute, names, entries)
            elif patch["op"] == "-":
                item = next((i for i in state["items"] if i["id"] == patch["id"]), None)
                if item is None:
                    raise ValueError("unknown_id")
                record["id"] = item["id"]
                _close(state, item, "sol", minute)
            else:
                record["id"], record["closed"] = _change(state, patch, minute, names, entries)
            record["ok"] = True
            record["closed"] += enforce_caps(state, minute)
        except ValueError as error:
            record["error"] = str(error)
    return records


def _add(state: dict[str, Any], patch: Mapping[str, Any], minute: int, names: Names | None,
         entries: Sequence[tuple[int, Mapping]]) -> tuple[str, list[str]]:
    letter, tail, values = patch["letter"], patch["tail"], dict(patch["values"])
    about = list(tail.get("about", []))
    about_names = list(tail.get("about_names", []))
    name, who = values.get("name", ""), values.get("who")
    if letter == "д":
        if who is None:
            about_names = list(dict.fromkeys([name, *about_names]))
        else:
            about.insert(0, who)
    if names is not None:  # Sol's own text: written, not heard
        about += [id_ for id_ in names.find(patch["text"], heard=False) if id_ not in about]
    about = list(dict.fromkeys(about))
    episodes = list(tail.get("episodes", []))
    source = _sourced(values.get("source") or tail.get("source"), letter, episodes, state, entries, about)
    slot = _slot(letter, who, name)
    text = patch["text"]
    same = [i for i in open_items(state, letter) if normalized(i["text"]) == normalized(text)
            and (slot is None or i["slot"] == slot)]
    if same:
        item = same[0]
        known = {_teller(s) for s in (item["source"], *item["also"])}
        if letter in "вс" and _teller(source) not in known:
            item["also"].append(source)  # told again: confirmations() counts the different origins
            item["episodes"] = list(dict.fromkeys(item["episodes"] + episodes))
            state["log"].append({"op": "confirm", "id": item["id"], "by": "sol", "minute": minute,
                                 "origin": source["origin"]})
            return item["id"], []
        if letter in SLOTS or letter == "д":
            return item["id"], []  # the same opinion, mood or view again: nothing changes
        raise ValueError("duplicate")
    state["counters"][letter] += 1
    item = {
        "id": f"{letter}{state['counters'][letter]}",
        "section": letter,
        "text": text,
        "about": about,
        "about_names": about_names,
        "slot": slot,
        "source": source,
        "also": [],
        "episodes": episodes,
        "valid_from": minute,
        "valid_to": None,
        "strength": values.get("strength", tail.get("strength", 2)),
        "importance": values.get("importance", tail.get("importance", 5)),
        "closed_by": None,
    }
    if letter == "б":
        item["time"] = time_text(minute)
    elif letter == "ч":
        item.update(duration=values["duration"], updated_minute=minute)
    elif letter == "ж":
        item["term"] = values["term"]
    elif letter == "д":
        item["name"] = name
    state["items"].append(item)
    state["log"].append({"op": "add", "id": item["id"], "by": "sol", "minute": minute})
    return item["id"], _close_slot(state, item, minute)


def _change(state: dict[str, Any], patch: Mapping[str, Any], minute: int, names: Names | None,
            entries: Sequence[tuple[int, Mapping]]) -> tuple[str, list[str]]:
    item = next((i for i in state["items"] if i["id"] == patch["id"]), None)
    if item is None:
        raise ValueError("unknown_id")
    letter, tail, values = item["section"], patch["tail"], dict(patch["values"])
    before = {k: deepcopy(item[k]) for k in ("text", "about", "source", "strength", "importance")}
    if patch["text"]:
        values["text"] = patch["text"]
    if letter == "ч":
        values["updated_minute"] = minute
    source = values.pop("source", None) or tail.get("source")
    who = values.pop("who", None)
    if "name" in values and letter == "д":  # renamed: the view is of another person
        item["about"] = [who] if who else []
        item["slot"] = _slot("д", who, values["name"])
    item.update({k: v for k, v in values.items()})
    for key in ("strength", "importance"):
        if key in tail:
            item[key] = tail[key]
    item["about"] = list(dict.fromkeys(item["about"] + tail.get("about", [])))
    item["about_names"] = list(dict.fromkeys(item["about_names"] + tail.get("about_names", [])))
    item["episodes"] = list(dict.fromkeys(item["episodes"] + tail.get("episodes", [])))
    if names is not None and patch["text"]:
        item["about"] += [id_ for id_ in names.find(patch["text"], heard=False) if id_ not in item["about"]]
    if source is not None:
        item["source"] = _sourced(source, letter, item["episodes"], state, entries, item["about"])
    state["log"].append({"op": "change", "id": item["id"], "by": "sol", "minute": minute, "before": before})
    return item["id"], _close_slot(state, item, minute)


def forget(state: dict[str, Any], minute: int) -> list[str]:
    """After a conversation (mind.py forget_events): trifles are forgotten, and what the npc wanted from
    this talk is over."""
    gone = []
    for item in list(state["items"]):
        if (item["section"] == "б" and item["importance"] <= FORGET_IMPORTANCE) or (
            item["section"] == "ж" and item["term"] == "сейчас"
        ):
            _close(state, item, "forget", minute)
            gone.append(item["id"])
    return gone


def _pair(a: str, b: str) -> list[str]:
    return sorted((a, b), key=lambda i: (i[0], int(i[1:])))


def find_collisions(state: dict[str, Any]) -> list[dict[str, Any]]:
    """Open beliefs and doubts without a slot that share a game id and say different things. The hero and
    the npc itself are left out: almost every item is about them, the clash shows through the other ids."""
    seen = {tuple(p) for p in state["collisions_seen"]}
    items = [i for i in open_items(state, "вс") if not i["slot"]]
    out = []
    for n, a in enumerate(items):
        for b in items[n + 1:]:
            shared = (set(a["about"]) & set(b["about"])) - {HERO, state["npc"]}
            ids = _pair(a["id"], b["id"])
            if shared and normalized(a["text"]) != normalized(b["text"]) and tuple(ids) not in seen:
                out.append({"ids": ids, "about": sorted(shared)})
    return out


def fold(
    state: dict[str, Any],
    text: str,
    *,
    names: Names | None,
    lines: Mapping[int, str] | None = None,
    minute: int,
    entries: Iterable[tuple[int, Mapping]] = (),
) -> dict[str, Any]:
    """One Sol fold at the end of a talk: parse, apply, caps, forget trifles, collisions. The collisions
    present before it were shown to Sol (render_for_sol): they are not raised again."""
    shown = [c["ids"] for c in state["collisions"]]
    parsed = parse_patches(text, names=names, lines=lines)
    applied = apply_patches(state, parsed, minute=minute, names=names, entries=entries)
    forgotten = forget(state, minute)
    state["collisions_seen"] += [ids for ids in shown if ids not in state["collisions_seen"]]
    state["collisions"] = find_collisions(state)
    state["folded"] = minute
    return {
        "stats": parsed["stats"],
        "applied": applied,
        "errors": sum(1 for r in applied if not r["ok"]),
        "forgotten": forgotten,
        "collisions": state["collisions"],
    }


# --- truth -----------------------------------------------------------------------------------------


def facts_for(entries: Iterable[tuple[int, Mapping]], npc: str) -> list[tuple[int, Mapping]]:
    """The chronicle facts that involve an npc (for the fold prompt)."""
    keys = ("npc", "giver", "from", "to", "about", "subject", "witness")
    return [(seq, f) for seq, f in entries if npc in {str(f.get(k, "")).upper() for k in keys}]


def _linked(item: Mapping[str, Any], fact: Mapping[str, Any], facts: Mapping[int, Mapping],
            turn_texts: Mapping[str, str] | None) -> bool:
    """The item is about the claim the fact speaks of: by quest id (in about, or through the fact its
    source cites), by the turn (`turn`/`turns` of the fact, or the turn whose words were caught), or, for
    a lie in words, by those words in the item itself."""
    quest = fact.get("quest")
    if quest and quest in item["about"]:
        return True
    cited = facts.get(item["source"].get("fact")) if item["source"].get("fact") is not None else None
    if quest and cited is not None and cited.get("quest") == quest:
        return True
    turns = set(item["episodes"]) | {item["source"].get("turn")} - {None}
    fact_turns = {fact.get("turn"), *(fact.get("turns") or ())} - {None}
    said = _stems(str(fact.get("title", ""))) if fact.get("said") else []
    if said and turn_texts:
        fact_turns |= {t for t, words in turn_texts.items() if _matched(said, _stems(words)) == len(said)}
    if turns & fact_turns:
        return True
    return bool(said) and _matched(said, _stems(item["text"])) == len(said)


def apply_truth(
    states: Mapping[str, dict[str, Any]],
    entries: Iterable[tuple[int, Mapping]],
    *,
    minute: int,
    turn_texts: Mapping[str, str] | None = None,
) -> list[dict[str, Any]]:
    """Close by the chronicle's truth:
    - `found_out` / `caught`: the npc that learned it loses its items from the hero's word about that
      claim; everyone the rumor of it reached before the truth (`rumor` chain from that npc, `from` → `to`)
      loses its items from someone's word about it. `closed_by=truth`;
    - `lie` is written the moment the hero lies, when the npc does not know yet: it only marks the item
      false for the world (`false_by`), the item stays open until `found_out` or `caught`.
    Each fact is applied to each npc once (`truth_seen`)."""
    entries = sorted(entries, key=lambda e: e[0])
    facts = dict(entries)
    closed = []
    for seq, fact in entries:
        kind = fact.get("fact")
        if kind == "lie" and (state := states.get(str(fact.get("npc", "")).upper())) is not None:
            for item in open_items(state):
                if item["source"]["kind"] == "герой" and _linked(item, fact, facts, turn_texts):
                    item.setdefault("false_by", seq)
        if kind not in TRUTH:
            continue
        holder = str(fact.get("npc", "")).upper()
        reach, frontier = {holder}, [holder]
        while frontier:
            teller = frontier.pop()
            for s, f in entries:
                if s < seq and f.get("fact") == "rumor" and f.get("from") == teller and _same_news(f, fact):
                    if (to := str(f.get("to", "")).upper()) not in reach:
                        reach.add(to)
                        frontier.append(to)
        for npc in sorted(reach):
            state = states.get(npc)
            if state is None or seq in state["truth_seen"]:
                continue
            state["truth_seen"].append(seq)
            want = "герой" if npc == holder else "сказал"
            for item in open_items(state):
                cited = item["source"].get("fact")
                late = cited is not None and cited > seq  # heard the truth itself, after it
                if item["source"]["kind"] == want and not late and _linked(item, fact, facts, turn_texts):
                    _close(state, item, "truth", minute, fact=seq)
                    closed.append({"npc": npc, "id": item["id"], "fact": seq})
            state["collisions"] = find_collisions(state)
    return closed


def confirmations(state: Mapping[str, Any], item: Mapping[str, Any]) -> int:
    """How many different origins back the item: two retellings of one origin are one."""
    del state  # the origins are resolved when the item is written (they need the chronicle)
    return len({s.get("origin") or s.get("who") or s.get("kind") for s in (item["source"], *item["also"])})


# --- rendering -------------------------------------------------------------------------------------


def _source_label(source: Mapping[str, Any]) -> str:
    label = SOURCE_LABEL[source["kind"]]
    if source["kind"] == "сказал" and source.get("name"):
        label += " " + source["name"]
    return label


def _detail(item: Mapping[str, Any], ids: bool) -> str:
    letter = item["section"]
    if letter == "б":
        return item["time"] + (f", важность {item['importance']}" if ids else "")
    if letter == "ч":
        return f"сила {item['strength']}, {item['duration']}"
    if letter == "ж":
        return item["term"]
    if letter in "вс":
        return _source_label(item["source"])
    return ""


def item_line(item: Mapping[str, Any], *, ids: bool = False, section: bool = False) -> str:
    """One line in the style of ум v3's render: «- (сказал Кавалорн): Браго мёртв»; `section` puts the
    section's word first, for a mixed list; `ids` puts the number first, for Sol."""
    letter = item["section"]
    head = [SECTIONS[letter][0]] if section else []
    if ids:
        head.append(item["id"])
    if letter == "д":
        head.append(item.get("name") or "")
    detail = _detail(item, ids)
    prefix = " ".join(h for h in head if h)
    if detail:
        prefix += f" ({detail})"
    prefix = prefix.strip()
    return "- " + (f"{prefix}: " if prefix else "") + item["text"]


def _events(items: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """The 12 most important events, in the order they happened (as mind.py render)."""
    def number(item: Mapping[str, Any]) -> int:
        return int(item["id"][1:])

    top = sorted(items, key=lambda i: (i["importance"], i["valid_from"], number(i)), reverse=True)[:12]
    return sorted(top, key=lambda i: (i["valid_from"], number(i)))


def render_for_sol(state: Mapping[str, Any]) -> str:
    """The open items with their numbers, for the fold prompt: time since the last fold first, the
    collisions the code left to Sol last."""
    items = state["items"]
    opinion = next((i["text"] for i in items if i["section"] == "м"), "—")
    mood = next((i["text"] for i in items if i["section"] == "р"), "—")
    lines = []
    since = state["folded"]
    if since is not None and state["minute"] is not None and state["minute"] - since > 60:
        days, hours = divmod((state["minute"] - since) // 60, 24)
        lines.append("С прошлого раза прошло: " + " ".join(
            ([f"{days} дн."] if days else []) + ([f"{hours} ч"] if hours else [])))
    if not items:
        return "\n".join([*lines, "Пока пусто."])
    lines.append("Мнение о герое: " + opinion)
    for letter in "бчжвсдн":
        lines.append(SECTIONS[letter][1] + ":")
        section = open_items(state, letter)
        if letter == "б":
            section = _events(section)
        lines += [item_line(i, ids=True) for i in section] or ["- —"]
    lines.append("Настроение: " + mood)
    if state["collisions"]:
        lines.append("Не решено, пункты спорят (оставь, закрой или слей):")
        for c in state["collisions"]:
            lines.append(f"- {c['ids'][0]} и {c['ids'][1]}")
    return "\n".join(lines)


@dataclass
class Digest:
    text: str
    ids: list[str]
    tokens: int

    def __str__(self) -> str:
        return self.text


def _fit(blocks: list[tuple[float, str, str | None]], tokens: Tokens, limit: int) -> Digest:
    """Blocks (priority, line, item id or None for a heading); the lowest-priority item lines go until the
    text fits; a heading goes with its last line. The highest one is cut if it alone is too long."""
    def text_of(rows: list[tuple[float, str, str | None]]) -> str:
        kept = []
        for n, (_, line, id_) in enumerate(rows):
            if id_ is None and line.endswith(":"):
                following = next((r for r in rows[n + 1:] if r[2] is not None or not r[1].startswith("-")),
                                 None)
                if following is None or following[2] is None:
                    continue  # a heading without lines
            kept.append(line)
        return "\n".join(kept)

    rows = list(blocks)
    while rows and tokens.count(text_of(rows)) > limit:
        removable = [r for r in rows if r[2] is not None]
        if len(removable) <= 1:
            break
        rows.remove(min(removable, key=lambda r: r[0]))
    text = text_of(rows)
    while text and tokens.count(text) > limit:
        cut = text[: int(len(text) * 0.9)].rsplit(" ", 1)[0]
        text = cut + "…" if len(cut) > 1 else ""
    ids = [r[2] for r in rows if r[2] is not None and r[1] in text]
    return Digest(text, ids, tokens.count(text))


_PROMISE = re.compile(r"обещ", re.IGNORECASE)


def card(state: Mapping[str, Any], *, tokens: Tokens | None = None, max_tokens: int = CARD_TOKENS) -> Digest:
    """The core card for Gemma: opinion, mood, feelings of strength 2+, open promises and threads, the
    strongest beliefs. No numbers; the lowest lines go first when it does not fit."""
    tokens = tokens or Tokens()
    items = state["items"]
    blocks: list[tuple[float, str, str | None]] = []
    opinion = next((i for i in items if i["section"] == "м"), None)
    mood = next((i for i in items if i["section"] == "р"), None)
    if opinion:
        blocks.append((100, "Мнение о герое: " + opinion["text"], opinion["id"]))
    if mood:
        blocks.append((95, "Настроение: " + mood["text"], mood["id"]))
    feelings = sorted((i for i in items if i["section"] == "ч" and i["strength"] >= 2),
                      key=lambda i: (i["strength"], i["valid_from"]), reverse=True)[:CARD_FEELINGS]
    if feelings:
        blocks.append((0, "Чувства:", None))
        blocks += [(80 - n, item_line(i), i["id"]) for n, i in enumerate(feelings)]
    threads = [i for i in reversed(items) if i["section"] == "н"]
    threads += [i for i in reversed(items) if i["section"] not in "нмр" and _PROMISE.search(i["text"])]
    threads = threads[:CARD_THREADS]
    if threads:
        blocks.append((0, "Нити и обещания:", None))
        blocks += [
            (70 - n, item_line(i, section=i["section"] != "н"), i["id"]) for n, i in enumerate(threads)
        ]
    beliefs = sorted(
        (i for i in items if i["section"] == "в" and i not in threads),
        key=lambda i: (i["strength"], confirmations(state, i), i["importance"], i["valid_from"]),
        reverse=True,
    )[:CARD_BELIEFS]
    if beliefs:
        blocks.append((0, "Верю:", None))
        blocks += [(60 - n, item_line(i), i["id"]) for n, i in enumerate(beliefs)]
    return _fit(blocks, tokens, max_tokens)


def _overlap(a: Sequence[str], b: Sequence[str]) -> float:
    if not a or not b:
        return 0.0
    return sum(1 for x in a if x in set(b)) / math.sqrt(len(a) * len(b))


async def closeness(phrase: str, texts: list[str], embed: EmbedClient | None) -> tuple[list[float], str]:
    """Cosine of the phrase and each text by the embedding service, if given and up; else the overlap
    of their word stems. Returns the scores and how they were got."""
    if embed is not None and texts:
        try:
            query = (await embed.embed([phrase], "query"))[0]
            docs = await embed.embed(texts[:256], "document")
            q = query / (np.linalg.norm(query) or 1.0)
            d = docs / np.maximum(np.linalg.norm(docs, axis=1, keepdims=True), 1e-9)
            scores = [float(x) for x in d @ q] + [0.0] * (len(texts) - len(docs))
            return scores, "embed"
        except EmbedError:
            pass
    words = _stems(phrase)
    return [_overlap(words, _stems(t)) for t in texts], "words"


async def items_for(
    state: Mapping[str, Any],
    phrase: str,
    *,
    names: Names,
    near: Iterable[str] = (),
    embed: EmbedClient | None = None,
    tokens: Tokens | None = None,
    skip: Iterable[str] = (),
    now: int | None = None,
    max_tokens: int = ITEMS_TOKENS,
    max_items: int = ITEMS_MAX,
) -> Digest:
    """Items for the hero's phrase. Seeds: ids named in it (names.find) and the hero. Step 1: open items
    about a seed; step 2, one hop: items sharing another id with those (not through the hero or the npc
    itself). If they are fewer than `max_items`, the rest of the open items come last. Ranked by
    closeness to the phrase, strength or importance, freshness in game time and how they were reached."""
    tokens = tokens or Tokens()
    skip = set(skip)
    seeds = set(names.find(phrase, near)) | {HERO}
    pool = [i for i in state["items"] if i["section"] not in SLOTS and i["id"] not in skip]
    reach = {}
    for item in pool:
        if seeds & set(item["about"]):
            reach[item["id"]] = "seed"
    hops = {a for i in pool if reach.get(i["id"]) == "seed" for a in i["about"]} - seeds - {state["npc"]}
    for item in pool:
        if item["id"] not in reach and hops & set(item["about"]):
            reach[item["id"]] = "hop"
    if len(reach) < max_items:
        for item in pool:
            reach.setdefault(item["id"], "rest")
    candidates = [i for i in pool if i["id"] in reach]
    if not candidates:
        return Digest("", [], 0)
    close, _ = await closeness(phrase, [i["text"] for i in candidates], embed)
    now = now if now is not None else (state["minute"] if state["minute"] is not None
                                       else max(i["valid_from"] for i in candidates))
    scored = []
    for item, c in zip(candidates, close, strict=True):
        weight = max(item["strength"] / 3, item["importance"] / 10)
        fresh = 0.5 ** (max(0, now - item["valid_from"]) / FRESH_HALF_LIFE)
        score = W_CLOSE * c + W_WEIGHT * weight + W_FRESH * fresh + REACH_BONUS[reach[item["id"]]]
        scored.append((score, item))
    scored.sort(key=lambda s: s[0], reverse=True)
    heading = "К слову вспоминаю:"
    chosen: list[dict[str, Any]] = []
    for _, item in scored:
        if len(chosen) >= max_items:
            break
        text = "\n".join([heading, *(item_line(i, section=True) for i in [*chosen, item])])
        if tokens.count(text) <= max_tokens:
            chosen.append(item)
    if not chosen:
        return Digest("", [], 0)
    text = "\n".join([heading, *(item_line(i, section=True) for i in chosen)])
    return Digest(text, [i["id"] for i in chosen], tokens.count(text))


def episodes_for(state: Mapping[str, Any], items: Iterable[Any], k: int = 2) -> list[str]:
    """The k turns most of the chosen items cite (as AriGraph picks episodes)."""
    counts: Counter[str] = Counter()
    for entry in items:
        item = find_item(state, entry) if isinstance(entry, str) else entry
        if item is not None:
            counts.update(dict.fromkeys(item["episodes"], 1))
    return [turn for turn, _ in counts.most_common(k)]


# --- selftest --------------------------------------------------------------------------------------


CAVALORN, XARDAS, BRAGO = "BAU_4300_ADDON_CAVALORN", "NONE_100_XARDAS", "BDT_1014_BANDIT_L"
VATRAS, LOBART = "VLK_439_VATRAS", "BAU_950_LOBART"


def _mind_v3() -> Any:
    spec = importlib.util.spec_from_file_location("_items_mind_v3", MIND_V3)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class _Check:
    def __init__(self) -> None:
        self.passed = 0
        self.failed: list[str] = []

    def __call__(self, name: str, ok: bool, detail: Any = "") -> None:
        if ok:
            self.passed += 1
        else:
            self.failed.append(f"{name}: {detail}")
            print(f"FAIL {name}: {detail}")


def _names() -> Names:
    quests = {"Q1": "Овцы топчут поле", "MIS_ADDON_CAVALORN_BANDITS": "Бандиты у башни Ксардаса"}
    return build_names(quests=quests, items={"ITWR_VATRAS_LETTER": "письмо Ватраса"})


def _test_names(check: _Check, names: Names) -> None:
    for name, expected in (
        ("Кавалорн", CAVALORN), ("Кавалорну", CAVALORN), ("у Ксардаса", XARDAS), ("Ксардасом", XARDAS),
        ("Сардас", XARDAS), ("Ксардоз", XARDAS), ("Каваларн", CAVALORN), ("Браго", BRAGO), ("Браго,", BRAGO),
        ("о Ватрасе", VATRAS), ("герою", HERO), ("NONE_100_XARDAS", XARDAS), ("бандит", None),
        ("Хренозавр", None), ("овцы на поле", "Q1"),
    ):
        check(f"resolve {name}", names.resolve(name) == expected, names.resolve(name))
    found = names.find("Каваларн, ты видел Браго у Ксардоса?", near=[CAVALORN])
    check("find distorted", {CAVALORN, BRAGO, XARDAS} <= set(found), found)
    found = names.find("скажи Кавалорну, что письмо Ватраса у меня")
    check("find declined", {CAVALORN, VATRAS, "ITWR_VATRAS_LETTER"} <= set(found), found)
    for phrase in ("в городе сегодня жарко", "позови стражника, рыбак"):
        check(f"find nothing: {phrase}", names.find(phrase) == [], names.find(phrase))
    check("ids are the game's", names.kind(CAVALORN) == "npc" and names.name(XARDAS) == "Ксардас")
    snap = {"npc": {"id": "NEW_1_TESTER", "name": "Тестомес"}, "nearby_npcs": [{"id": "x2", "name": "Нечто"}]}
    more = build_names(snapshots=[snap], cards={}, lore_names={}, places={})
    check("names from situations", more.resolve("Тестомесу") == "NEW_1_TESTER", more.resolve("Тестомесу"))


def _test_parse(check: _Check, names: Names) -> None:
    lines = {12: "t12", 14: "t14", 3: "t3"}
    text = "\n".join([
        "[Правки]",
        "+ верю герой: Браго мёртв [о: Браго; стр: 12, 14]",
        "+ нить: принести письмо [о: Ватрас; стр: 12",  # not closed
        "+ было 7: он украл моё письмо [стр: двенадцать; о: Кавалорн]",  # bad lines field
        "+ верю видел: у пещеры дым [стр: 99]",  # a foreign line
        "+ о Хренозавр: странный тип [о: Хренозавр]",  # names without ids
        "+ сомневаюсь сказал Кавалорн: что Браго один [ключ: значение]",  # unknown key
        "что-то непонятное",
        "+ верю: без источника",
        "—",
    ])
    parsed = parse_patches(text, names=names, lines=lines)
    stats, patches = parsed["stats"], parsed["patches"]
    check("parse counts lines", stats["lines"] == 9, stats)
    check("parse bad lines", stats["bad"] == 2, stats)
    check("parse bad tails", stats["bad_tails"] == 3, stats)
    check("parse foreign lines", stats["foreign_lines"] == 1, stats)
    check("parse unknown names", stats["unknown_names"] == 2, stats)
    first = patches[0]
    check("tail about", first["tail"].get("about") == [BRAGO], first["tail"])
    check("tail lines → turns", first["tail"].get("episodes") == ["t12", "t14"], first["tail"])
    check("source param", first["values"]["source"]["kind"] == "герой", first["values"])
    check("broken tail keeps line", patches[1]["error"] is None and patches[1]["text"] == "принести письмо")
    check("broken tail keeps fields", patches[1]["tail"].get("about") == [VATRAS], patches[1]["tail"])
    check("bad field keeps others",
          patches[2]["tail"].get("about") == [CAVALORN] and patches[2]["error"] is None)
    told = patches[5]["values"]["source"]
    check("source who", told["kind"] == "сказал" and told["who"] == CAVALORN, told)
    state = new_state(CAVALORN)
    applied = apply_patches(state, parsed, minute=1440 + 600, names=names)
    ok = [r["id"] for r in applied if r["ok"] and r["id"]]
    check("apply ok lines", ok == ["в1", "н1", "б1", "в2", "д1", "с1"], applied)
    b1 = find_item(state, "в1")
    check("item fields", b1["about"] == [BRAGO] and b1["episodes"] == ["t12", "t14"]
          and b1["source"]["turn"] == "t12" and b1["valid_from"] == 2040 and b1["valid_to"] is None, b1)
    check("unknown name kept as text", find_item(state, "д1")["about_names"] == ["Хренозавр"])
    check("view slot by its person", find_item(state, "д1")["slot"] == "other:name:хренозавр")
    check("auto about from text", HERO not in find_item(state, "б1")["about"]
          and CAVALORN in find_item(state, "б1")["about"], find_item(state, "б1")["about"])


def _test_fold_and_slots(check: _Check, names: Names) -> None:
    state = new_state(CAVALORN)
    prepare(state, 1440 + 600)
    report = fold(state, "\n".join([
        "= мнение: наглый, но письмо принёс [стр: 2]",
        "= настроение: настороже",
        "+ о Ксардас: странный маг из башни",
        "+ чувство 3 день: злость к герою: украл письмо",
        "+ хочу сейчас: чтобы он ушёл — устал",
        "+ хочу скоро: вернуть письмо — оно моё",
        "+ было 2: поздоровался → ничего",
        "+ было 8: герой отдал письмо → должник",
        "+ верю сказал Ватрас: Браго мёртв [о: Браго]",
        "+ сомневаюсь: Браго жив, его видели у пещеры [о: Браго]",
        "~ ч1 2",
        "- ж9",
    ]), names=names, lines={2: "t2"}, minute=1440 + 600)
    check("fold applies", report["errors"] == 1 and report["stats"]["lines"] == 12, report["errors"])
    check("forget trifles and wishes for now", sorted(report["forgotten"]) == ["б1", "ж1"],
          report["forgotten"])
    check("collision kept for Sol", report["collisions"] == [{"ids": ["в1", "с1"], "about": [BRAGO]}],
          report["collisions"])
    check("render shows collision", "в1 и с1" in render_for_sol(state))
    first_opinion = find_item(state, "м1")
    prepare(state, 3 * 1440)
    report = fold(state, "\n".join([
        "= мнение: помог с бандитами, можно доверять",
        "= настроение: спокоен",
        "+ о Ксардасе: учитель героя, ему можно верить",
        "~ в1 [стр: 5]",
    ]), names=names, lines={5: "t5"}, minute=3 * 1440)
    check("slot closes opinion", first_opinion["closed_by"] == "slot"
          and first_opinion["valid_to"] == 3 * 1440
          and find_item(state, "м2")["valid_from"] == 3 * 1440, first_opinion)
    check("slot closes mood", find_item(state, "р1")["closed_by"] == "slot")
    check("slot closes view of another", find_item(state, "д1")["closed_by"] == "slot"
          and find_item(state, "д2")["slot"] == f"other:{XARDAS}", find_item(state, "д1"))
    check("history keeps closed", {i["id"] for i in state["history"]} >= {"м1", "р1", "д1", "б1", "ж1"})
    check("collision not raised again", report["collisions"] == [], report["collisions"])
    check("change adds episode", find_item(state, "в1")["episodes"] == ["t5"])
    check("ids run through meetings", state["counters"]["м"] == 2)
    check("closed not in card", "наглый" not in card(state).text and "помог с бандитами" in card(state).text)
    json.dumps(state, ensure_ascii=False)  # the carry is plain JSON


def _test_v3_rules(check: _Check, names: Names) -> None:
    """Cooling, caps and forgetting give mind.py's result on the same input."""
    v3 = _mind_v3()
    lines = [
        *(f"+ верю видел: факт номер {n}" for n in range(1, 13)),
        *(f"+ сомневаюсь: сомнение {n}" for n in range(1, 8)),
        *(f"+ нить: нить {n}" for n in range(1, 10)),
        *(f"+ о Человек{n}: мнение {n}" for n in range(1, 10)),
        *(f"+ хочу {t}: желание {t} {n} — так" for t in WISH_TERMS for n in range(1, 4)),
        "+ чувство 3 час: досада к герою: перебил",
        "+ чувство 2 день: тепло к герою: помог",
        "+ чувство 3 долго: злость к герою: украл",
        "+ чувство 1 час: скука: долгий разговор",
        *(f"+ было {n}: событие {n} → значит {n}" for n in range(1, 11)),
        "~ ч2 3",
        "- в12",
        "+ верю видел: факт номер 3",  # a duplicate
    ]
    s3 = v3.new_state()
    _, changes3 = v3.prepare_state(s3, "Время: день 1, 10:00")
    v3.apply_patches(s3, lines, changes3)
    mine = new_state(CAVALORN)
    prepare(mine, 1440 + 600)
    parsed = parse_patches("\n".join(lines), names=names)
    records = apply_patches(mine, parsed, minute=1440 + 600, names=names)
    for letter, (key, _, _) in v3.SECTIONS.items():
        theirs = [(i["id"], i["text"]) for i in s3[key]]
        ours = [(i["id"], i["text"]) for i in open_items(mine, letter)]
        check(f"v3 caps {key}", theirs == ours, (theirs, ours))
    capped = sorted(i["id"] for i in mine["history"] if i["closed_by"] == "cap")
    check("v3 cap drops", capped == sorted(c["id"] for c in changes3["cap_dropped"]), capped)
    check("v3 duplicate", records[-1]["error"] == "duplicate", records[-1])
    v3.forget_events(s3, changes3)
    forgotten = forget(mine, 1440 + 660)
    check("v3 forget", sorted(forgotten) == sorted(c["id"] for c in changes3["forgotten"]), forgotten)
    for situation, minute in (("Время: день 1, 13:30", 1440 + 810), ("Время: день 3, 09:00", 3 * 1440 + 540),
                              ("Время: день 9, 12:00", 9 * 1440 + 720),
                              ("Время: день 30, 12:00", 30 * 1440 + 720)):
        _, changes = v3.prepare_state(s3, situation)
        cooled = prepare(mine, minute)["cooled"]
        theirs = [(i["id"], i["strength"], i["updated_minute"]) for i in s3["feelings"]]
        ours = [(i["id"], i["strength"], i["updated_minute"]) for i in open_items(mine, "ч")]
        check(f"v3 cooling {situation}", theirs == ours and cooled == [c["id"] for c in changes["cooled"]],
              (theirs, ours, cooled))
    cooled = [i for i in mine["history"] if i["section"] == "ч"]
    check("cooled closed by cool", len(cooled) == 4 and all(i["closed_by"] == "cool" for i in cooled))
    check("minute_of as v3", minute_of("Время: день 3, 09:00") == 3 * 1440 + 540)


def _lie_entries() -> list[tuple[int, dict[str, Any]]]:
    claim = {"quest": "Q1", "npc": LOBART, "name": "Лобарт", "title": "Овцы топчут поле"}
    return [
        (1, {"fact": "quest", "id": "Q1", "giver": LOBART, "giver_name": "Лобарт",
             "title": "Овцы топчут поле"}),
        (2, {"fact": "paid", "quest": "Q1", "item": "ITMI_GOLD", "count": 50}),
        (3, {"fact": "claim", **claim, "reward": "золото ×50", "believed": True, "truth": False}),
        (4, {"fact": "belief", "npc": LOBART, "quest": "Q1", "claim": "done", "believed": True,
             "source": "hero"}),
        (5, {"fact": "lie", "quest": "Q1", "npc": LOBART, "truth": "killed 0 of 3"}),
    ]


def _test_lies(check: _Check, names: Names) -> None:
    # 1. the hero lied about a quest; found_out closes the item
    entries = _lie_entries()
    lobart = new_state(LOBART)
    fold(lobart, "+ верю герой: он перебил овец на моём поле [о: овцы на поле; стр: 4]\n"
                 "+ было 7: заплатил герою за овец → он мне помог",
         names=names, lines={4: "t4"}, minute=2000, entries=entries)
    belief = find_item(lobart, "в1")
    check("lie 1: belief about the quest", "Q1" in belief["about"] and belief["source"]["kind"] == "герой",
          belief)
    apply_truth({LOBART: lobart}, entries, minute=2100)
    check("lie 1: lie alone keeps it open", belief["closed_by"] is None and belief.get("false_by") == 5,
          belief)
    entries.append((6, {"fact": "found_out", "quest": "Q1", "npc": LOBART, "name": "Лобарт",
                        "title": "Овцы топчут поле", "how": "увидел их живыми в 12 м от себя", "day": 2}))
    closed = apply_truth({LOBART: lobart}, entries, minute=2200)
    check("lie 1: found_out closes", belief["closed_by"] == "truth" and belief["valid_to"] == 2200
          and closed == [{"npc": LOBART, "id": "в1", "fact": 6}], closed)
    check("lie 1: event stays", find_item(lobart, "б1")["closed_by"] is None)
    check("lie 1: closed out of card", "перебил овец" not in card(lobart).text)
    check("lie 1: applied once", apply_truth({LOBART: lobart}, entries, minute=2300) == [])

    # 2. the hero lied to A, A retold B, B retold C; the truth closes all three, a later rumor of the truth
    # stays
    quest = "MIS_ADDON_CAVALORN_BANDITS"
    entries = [
        (10, {"fact": "claim", "quest": quest, "npc": CAVALORN, "title": "Бандиты у башни Ксардаса"}),
        (11, {"fact": "rumor", "quest": quest, "kind": "claim", "about": CAVALORN, "from": CAVALORN,
              "to": XARDAS, "hops": 1, "how": "met"}),
        (12, {"fact": "rumor", "quest": quest, "kind": "claim", "about": CAVALORN, "from": XARDAS,
              "to": VATRAS, "hops": 2, "how": "met"}),
    ]
    a, b, c, d = (new_state(n) for n in (CAVALORN, XARDAS, VATRAS, LOBART))
    fold(a, "+ верю герой: бандиты у башни перебиты [о: Бандиты у башни Ксардаса]", names=names,
         minute=3000, entries=entries)
    fold(b, "+ верю сказал Кавалорн: герой перебил бандитов у башни\n"
            "+ верю сказал Кавалорн: Кавалорн мне друг", names=names, minute=3100, entries=entries)
    fold(c, "+ верю слышал от Ксардаса: бандитов у башни больше нет", names=names, minute=3200,
         entries=entries)
    check("lie 2: B links the rumor", find_item(b, "в1")["source"]["fact"] == 11,
          find_item(b, "в1")["source"])
    entries += [
        (13, {"fact": "caught", "quest": quest, "npc": CAVALORN, "name": "Кавалорн",
              "title": "Бандиты у башни Ксардаса", "how": "сам видел бандитов живыми"}),
        (14, {"fact": "rumor", "quest": quest, "kind": "caught", "about": CAVALORN, "from": CAVALORN,
              "to": LOBART, "hops": 1, "how": "heard"}),
    ]
    fold(d, "+ верю сказал Кавалорн: герой соврал про бандитов у башни", names=names, minute=3300,
         entries=entries)
    states = {CAVALORN: a, XARDAS: b, VATRAS: c, LOBART: d}
    closed = apply_truth(states, entries, minute=3400)
    check("lie 2: A, B and C closed", sorted((x["npc"], x["id"]) for x in closed)
          == sorted([(CAVALORN, "в1"), (XARDAS, "в1"), (VATRAS, "в1")]), closed)
    check("lie 2: the truth's rumor stays", find_item(d, "в1")["closed_by"] is None)
    check("lie 2: other hearsay stays", find_item(b, "в2")["closed_by"] is None
          and find_item(b, "в2")["source"]["fact"] is None, find_item(b, "в2"))

    # a lie in words, found through the turn whose words were caught
    e = new_state(VATRAS)
    fold(e, "+ верю герой: Браго убит [стр: 5]", names=names, lines={5: "t5"}, minute=3500)
    said = [(20, {"fact": "caught", "quest": "SAID_VLK_439_VATRAS_3600", "npc": VATRAS, "said": True,
                  "title": "я сам убил его вчера", "how": "сам видел его живым"})]
    apply_truth({VATRAS: e}, said, minute=3600, turn_texts={"t5": "Я сам убил его вчера, клянусь"})
    check("lie in words: by the turn", find_item(e, "в1")["closed_by"] == "truth")

    # 3. two retellings of one origin are one confirmation
    entries = [
        (30, {"fact": "rumor", "quest": quest, "kind": "caught", "about": CAVALORN, "from": CAVALORN,
              "to": XARDAS, "hops": 1}),
        (31, {"fact": "rumor", "quest": quest, "kind": "caught", "about": CAVALORN, "from": CAVALORN,
              "to": VATRAS, "hops": 1}),
        (32, {"fact": "rumor", "quest": quest, "kind": "caught", "about": CAVALORN, "from": XARDAS,
              "to": LOBART, "hops": 2}),
        (33, {"fact": "rumor", "quest": quest, "kind": "caught", "about": CAVALORN, "from": VATRAS,
              "to": LOBART, "hops": 2}),
    ]
    f = new_state(LOBART)
    lies = "герой лжёт про бандитов у башни"
    fold(f, f"+ верю сказал Ксардас: {lies}", names=names, minute=4000, entries=entries)
    report = fold(f, f"+ верю сказал Ватрас: {lies}", names=names, minute=4100, entries=entries)
    item = find_item(f, "в1")
    check("lie 3: retelling merges", report["applied"][0]["id"] == "в1" and len(item["also"]) == 1
          and report["applied"][0]["ok"] and len(f["items"]) == 1, report["applied"])
    check("lie 3: one origin, one confirmation", confirmations(f, item) == 1, item)
    fold(f, f"+ верю видел: {lies}", names=names, minute=4200, entries=entries)
    check("lie 3: own eyes is another", confirmations(f, item) == 2, item["also"])


def _big_state(names: Names) -> dict[str, Any]:
    state = new_state(CAVALORN)
    people = ["Ксардас", "Ватрас", "Браго", "Лобарт", "Онар", "Бодо"]
    prepare(state, 1440)
    for day in range(1, 11):
        lines = [f"= мнение: день {day}: " + "очень длинное мнение о герое с причинами " * 3,
                 f"= настроение: день {day}, устал и зол"]
        for n in range(12):
            who = people[(day + n) % len(people)]
            lines += [
                f"+ верю сказал {who}: {who} знает про письмо и про бандитов, день {day} номер {n} "
                f"[о: {who}; стр: {n}]",
                f"+ было {(n % 10) + 1}: {who} помог герою в деле {day}-{n} → это важно надолго [стр: {n}]",
                f"+ чувство {(n % 3) + 1} долго: досада номер {day}-{n} к герою: из-за {who}",
                f"+ нить: обещал узнать у {who} про дело {day}-{n}",
                f"+ сомневаюсь: {who} говорит правду о деле {day}-{n}",
            ]
        prepare(state, day * 1440 + 600)
        fold(state, "\n".join(lines), names=names, lines={n: f"d{day}t{n}" for n in range(12)},
             minute=day * 1440 + 600)
    return state


def _test_reader(check: _Check, names: Names) -> None:
    state = _big_state(names)
    check("big state", len(state["items"]) + len(state["history"]) > 400, len(state["items"]))
    tokens = Tokens()
    core = card(state, tokens=tokens)
    check("card ≤ 300 tokens", core.tokens <= CARD_TOKENS and tokens.count(core.text) == core.tokens,
          core.tokens)
    check("card has opinion and mood", core.text.startswith("Мнение о герое: день 10")
          and "Настроение: день 10" in core.text, core.text[:200])
    check("card has no numbers", not re.search(r"(?<![а-яё])[бчжвсднмр]\d+\b", core.text), core.text)
    check("card no closed", all(find_item(state, i)["closed_by"] is None for i in core.ids))
    check("card feelings ≥ 2", all(find_item(state, i)["strength"] >= 2 for i in core.ids
                                    if i.startswith("ч")))
    phrase = "Ксардас знает про письмо?"
    picked = asyncio.run(items_for(state, phrase, names=names, tokens=tokens, skip=core.ids))
    check("items ≤ 150 tokens and ≤ 5", 0 < len(picked.ids) <= ITEMS_MAX and picked.tokens <= ITEMS_TOKENS,
          (picked.ids, picked.tokens))
    check("items reach the seed", any(XARDAS in find_item(state, i)["about"] for i in picked.ids),
          picked.text)
    check("items skip the card", not set(picked.ids) & set(core.ids))
    check("items text no numbers", not re.search(r"(?<![а-яё])[бчжвсднмр]\d+\b", picked.text), picked.text)
    episodes = episodes_for(state, picked.ids, k=2)
    check("episodes k=2", len(episodes) <= 2 and all(e.startswith("d") for e in episodes), episodes)
    small = new_state(CAVALORN)
    fold(small, "\n".join([
        "+ было 8: герой отдал письмо Ватраса [стр: 1]",
        "+ верю видел: у пещеры дым [стр: 2]",
        "+ нить: герой обещал вернуться с мясом [стр: 2]",
    ]), names=names, lines={1: "t1", 2: "t2"}, minute=100)
    check("episodes most cited", episodes_for(small, ["б1", "в1", "н1"], k=1) == ["t2"])

    # the tokenizer and the embeddings, through their real clients on a fake transport
    def tokenize(request: httpx.Request) -> httpx.Response:
        content = json.loads(request.content)["content"]
        return httpx.Response(200, json={"tokens": list(range(len(content.split())))})

    words = Tokens("http://gemma.test", transport=httpx.MockTransport(tokenize))
    check("tokenize counts", words.count("три слова тут") == 3 and words.method.endswith("/tokenize"))
    check("tokenize limit", card(state, tokens=words, max_tokens=40).tokens <= 40)

    def embedding(request: httpx.Request) -> httpx.Response:
        texts = json.loads(request.content)["texts"]
        vectors = np.array([[1.0, 0.0] if "мясо" in t or "мясом" in t else [0.0, 1.0] for t in texts], "<f4")
        return httpx.Response(200, json={"model": "fake", "dim": 2,
                                         "vectors": base64.b64encode(vectors.tobytes()).decode()})

    async def ranked() -> Digest:
        client = EmbedClient("http://frida.test", transport=httpx.MockTransport(embedding))
        try:
            return await items_for(small, "принёс мясо", names=names, embed=client, max_items=1)
        finally:
            await client.close()

    check("embed closeness ranks", asyncio.run(ranked()).ids == ["н1"], asyncio.run(ranked()).text)


def selftest() -> int:
    check = _Check()
    names = _names()
    for test in (_test_names, _test_parse, _test_fold_and_slots, _test_v3_rules, _test_lies, _test_reader):
        try:
            test(check, names)
        except Exception as error:  # a crash is one failed check, the rest still run
            traceback.print_exc()
            check(test.__name__, False, repr(error))
    print(f"selftest: {check.passed} passed, {len(check.failed)} failed")
    return 1 if check.failed else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("command", choices=["selftest"])
    parser.parse_args()
    return selftest()


if __name__ == "__main__":
    sys.exit(main())
