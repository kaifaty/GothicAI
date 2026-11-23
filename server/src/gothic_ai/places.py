"""Places by name (docs/npc-tools.md, wave 1 step 5): «во дворе фермы Лобарта» instead of NW_FARM1_OUT_01.

The game only has waypoints. The book of places is made once, offline:
1. `dump`: the world's waypoints and freepoints and its people's routines, from the game (-devmode,
   SCENARIO=places);
2. `group`: points into places. Their names say what part of the world they are in (NW_CITY_HABOUR_03,
   FP_ROAM_NW_FARM1_SHEEP_02: the city's harbour, Lobart's farm); points named alike that lie apart are
   different places; a point or two on their own join the nearest place of their part of the world;
3. `label`: the conversational model names each place from its points' names, who lives or works there
   and the places around: «двор фермы Лобарта», where — «во дворе фермы Лобарта», and in English;
4. `survey` (version 2, docs/npc-tools.md М3): what the waynet and the engine's rooms tell of each place.
   A point in a room (a cave's or a house's sector of the engine) is under a roof; a place mostly so is
   inside, and its ways in are where the waynet goes from such a point to one outside: the bandits' cave
   by Xardas's tower is entered from the steps. The places the waynet joins are neighbours. The code
   guesses each place's kind (cave, house, yard, road…) from its rooms and names, and the model
   confirms it (`kinds`);
5. the book is read through and kept as a file (gamedata/places.json).

At run time `PlaceBook` tells the place of a waypoint, the places around a spot, of what kind, where they
are from the hero and where their ways in are (a companion is sent to one, orders.py; `where`,
whereabouts.py).
"""

from __future__ import annotations

import asyncio
import json
import logging
import math
import re
from collections import Counter, defaultdict
from collections.abc import Iterable, Sequence
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from .game import GameLink
from .llm import LLMBackend, Message, TextDelta

SCENARIOS = {
    "places": ("OFF", {"save": False, "timeout": 240}),
}

log = logging.getLogger(__name__)

VERSION = 2
BOOK = Path(__file__).parent / "gamedata" / "places.json"

LINK_CM = 2500  # points named alike closer than this are one place
SPLIT_POINTS = 40  # a part of the world named alike with more points goes by its third word too (floors)
MIN_POINTS = 3  # fewer, and the points join the nearest place of their part of the world
FOLD_POINTS = 6  # a place this small all in a cave's room another has more of is that cave's (`_fold`)
BATCH = 25  # places named in one call
KIND_BATCH = 40  # places whose kind is confirmed in one call
SAMPLE_NAMES = 10
INSIDE_SHARE = 0.5  # of its points in a room, and a place is inside (a cave, a house)
SIDE_STEPS = 8  # how far out of a way in the waynet is followed to the place outside it
ENTRANCES = 3  # ways in kept for a place

# what kind of place it is, in Russian and English: the conversational model is sent to «пещера» by it
KINDS = {
    "cave": ("пещера", "cave"),
    "house": ("дом", "house"),
    "yard": ("двор", "yard"),
    "street": ("улица", "street"),
    "road": ("дорога", "road"),
    "camp": ("лагерь", "camp"),
    "field": ("поле", "field"),
    "forest": ("лес", "forest"),
    "shore": ("берег", "shore"),
    "other": ("местность", "area"),
}
# the words of points' names and of rooms that say the kind, for the code's guess
KIND_WORDS = {
    "cave": {"CAVE", "HOEHLE", "GROTTE", "MINE", "DUNGEON", "CRYPT", "GRUFT"},
    "road": {"PATH", "ROAD", "WAY", "WEG", "STREET", "TO", "CONNECT", "STAIRS", "BRIDGE", "PASS"},
    "camp": {"CAMP", "CAMPFIRE", "LAGER"},
    "field": {"FIELD", "FELD", "PASTURE", "SHEEP", "WIESE", "MEADOW"},
    "forest": {"FOREST", "WOOD", "WALD", "FORREST", "CITYFOREST"},
    "yard": {"FARM1", "FARM2", "FARM3", "FARM4", "BIGFARM", "OUT", "YARD", "HOF"},
    "street": {"CITY", "MARKET", "SQUARE", "PLATZ", "HABOUR", "HAFEN"},
    "shore": {"BEACH", "SHORE", "COAST", "STRAND", "LAKE", "RIVER"},
}

# what freepoints are for, between FP_ and the place: FP_ROAM_NW_FARM1_SHEEP_02, FP_STAND_NW_CITY_...
ACTIVITIES = {
    "ROAM", "STAND", "SIT", "SMALLTALK", "CAMPFIRE", "SLEEP", "PICK", "ITEM", "GUARD", "SWEEP", "PRAY",
    "EAT", "DRINK", "PEE", "SMITH", "COOK", "READ", "PRACTICE", "PICKHERB", "REPAIR", "WASH", "LUTE",
    "SAW", "RAKE", "STOMP", "ROASTSCAVENGER", "DEAD", "SPAWN", "ARMSCROSSED", "TALK",
}  # fmt: skip


async def dump(link: GameLink, path: Path) -> dict[str, Any]:
    """The world's points and people, as the game has them now, into `path`."""
    points = await link.waypoints()
    people = await link.story_people()
    data = {"world": link.world, "points": points, "people": people}
    await asyncio.to_thread(path.write_text, json.dumps(data, ensure_ascii=False), encoding="utf-8")
    return data


def tokens(name: str) -> list[str]:
    """What a point's name says of where it is: FP_ROAM_NW_FARM1_SHEEP_02 -> [FARM1, SHEEP]."""
    t = name.upper().split("_")
    if t and t[0] == "FP":
        t = t[1:]
        while t and t[0] in ACTIVITIES:
            t = t[1:]
    return [x for x in t if x != "NW" and not re.fullmatch(r"\d+[A-Z]?", x)]


def _key(name: str, words: int = 2) -> str:
    t = tokens(name)
    return "_".join(t[:words]) if t else "?"


@dataclass
class Group:
    """Points that make one place."""

    id: str
    points: list[dict[str, Any]]
    residents: list[str] = field(default_factory=list)  # who lives or works there, and when

    @property
    def area(self) -> str:
        return self.id.split("_")[0].split("#")[0]

    @property
    def center(self) -> tuple[float, float, float]:
        n = len(self.points)
        return (
            sum(p["x"] for p in self.points) / n,
            sum(p["y"] for p in self.points) / n,
            sum(p["z"] for p in self.points) / n,
        )


def _near(a: dict[str, Any], b: dict[str, Any], link: float = LINK_CM) -> bool:
    # height counts double: floors of a tower are different places
    return (a["x"] - b["x"]) ** 2 + (a["z"] - b["z"]) ** 2 + (2 * (a["y"] - b["y"])) ** 2 <= link**2


def _components(points: list[dict[str, Any]]) -> list[list[dict[str, Any]]]:
    parent = list(range(len(points)))

    def root(i: int) -> int:
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i

    for i in range(len(points)):
        for j in range(i + 1, len(points)):
            if _near(points[i], points[j]):
                parent[root(i)] = root(j)
    out: dict[int, list[dict[str, Any]]] = defaultdict(list)
    for i, p in enumerate(points):
        out[root(i)].append(p)
    return list(out.values())


def _dist(a: tuple[float, float, float], p: dict[str, Any]) -> float:
    return math.hypot(a[0] - p["x"], a[1] - p["y"], a[2] - p["z"])


def group(points: Iterable[dict[str, Any]], people: Iterable[dict[str, Any]] = ()) -> list[Group]:
    """Points into places: named alike and close together; strays join the nearest place, of their part of
    the world if it has one. Who lives or works there comes from the people's routines."""
    by_key: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for p in points:
        by_key[_key(p["name"])].append(p)
    for key in [k for k, v in by_key.items() if len(v) > SPLIT_POINTS]:  # a tower's floors, a city's huts
        for p in by_key.pop(key):
            by_key[_key(p["name"], 3)].append(p)
    groups: list[Group] = []
    strays: list[dict[str, Any]] = []
    for key in sorted(by_key):
        parts = sorted(_components(by_key[key]), key=lambda c: (-len(c), min(p["name"] for p in c)))
        for i, part in enumerate(parts):
            if len(part) >= MIN_POINTS:
                groups.append(Group(key if i == 0 else f"{key}#{i + 1}", part))
            else:
                strays += part
    for p in strays:
        area = tokens(p["name"])[:1]
        same = [g for g in groups if area and g.area == area[0]] or groups
        best = min(same, key=lambda g: min(_dist((q["x"], q["y"], q["z"]), p) for q in g.points))
        best.points.append(p)
    where = {p["name"].upper(): g for g in groups for p in g.points}
    for person in people:
        for e in person.get("routine", []):
            g = where.get(str(e.get("waypoint", "")).upper())
            if g is None:
                continue
            line = f"{person['name']}: {e['state']} {e['start'] // 60:02d}–{e['end'] // 60:02d} ч"
            if line not in g.residents:
                g.residents.append(line)
    return _fold(groups)


def _fold(groups: list[Group]) -> list[Group]:
    """A few points all in a cave's room another place has more of, nobody's home and not named a cave, are
    that cave's: WP_XXX_ADD_* are the way through the bandits' cave, not «площадка у дороги» (v2 dumps
    tell rooms)."""
    rooms = {g.id: Counter(str(p["room"]) for p in g.points if p.get("room")) for g in groups}
    gone: set[str] = set()
    for g in sorted(groups, key=lambda g: len(g.points)):
        r = rooms[g.id]
        if g.residents or len(g.points) > FOLD_POINTS or sum(r.values()) != len(g.points):
            continue
        room = r.most_common(1)[0][0]
        if _room_kind(room) != "cave" or _named_cave(g):
            continue
        owner = max(
            (o for o in groups if o.id not in gone and o is not g and rooms[o.id][room] > r[room]),
            key=lambda o: rooms[o.id][room],
            default=None,
        )
        if owner is not None:
            owner.points += g.points
            rooms[owner.id].update(r)
            gone.add(g.id)
    return [g for g in groups if g.id not in gone]


def _named_cave(g: Group) -> bool:
    return any(c in w for p in g.points for w in tokens(p["name"]) for c in KIND_WORDS["cave"])


# --- naming ----------------------------------------------------------------------------------------

LABEL_INSTRUCTIONS = """Ты составляешь справочник мест острова Хоринис из игры Gothic 2: Ночь Ворона.
Каждое место — группа точек из файлов игры. О месте дано: ключ, имена его точек (так их назвали
разработчики, по-английски и по-немецки), сколько в нём точек, кто там живёт или работает (имя: состояние
скрипта ZS_…, часы) и соседние места с расстоянием.

Назови каждое место так, как сказал бы житель острова: коротко, по-русски, без слова «точка» и без
английских имён — «двор фермы Лобарта», «кузница Гарада», «рыночная площадь Хориниса», «второй этаж башни
Ксардаса», «дорога от фермы Лобарта к городу», «лес к востоку от таверны». Имена людей бери из списка
жителей, и только для их дома, работы или постоянного места («кузница Гарада»); кто там просто стоит,
место не называет. Звери в именах точек (WOLF, BLOODFLY, GOBBO, SHEEP, MONSTER, SPAWN) — это где они
появляются, а не название места: зверей могли перебить, поэтому называй по местности и соседям («опушка
у городской стены»), кроме пастбищ и загонов. Не выдумывай того, чего нет в данных: если неясно, назови по
виду и по соседям. Одинаковых названий у разных мест быть не должно: различай их («северная часть…»,
«у ворот…»).

Ответ — только JSON-массив, по объекту на место, в том же порядке:
{"id": ключ, "ru": название, "where_ru": где? («во дворе фермы Лобарта», «на рыночной площади
Хориниса»), "en": English name, "where_en": where? ("in Lobart's farmyard")}"""


def describe(g: Group, groups: list[Group]) -> str:
    """What the model is told of a place."""
    center = g.center
    names = sorted({p["name"] for p in g.points})
    sample = names[:SAMPLE_NAMES] + (["…"] if len(names) > SAMPLE_NAMES else [])
    others = sorted(
        ((math.dist(center, o.center) / 100, o.id) for o in groups if o is not g), key=lambda x: x[0]
    )[:4]
    lines = [
        f"id: {g.id}",
        f"точки ({len(g.points)}): {', '.join(sample)}",
        "жители: " + ("; ".join(g.residents[:8]) if g.residents else "нет"),
        "соседи: " + ", ".join(f"{oid} ({d:.0f} м)" for d, oid in others),
    ]
    return "\n".join(lines)


async def label(
    groups: list[Group], llm: LLMBackend, batch: int = BATCH, *, among: list[Group] | None = None
) -> dict[str, dict[str, str]]:
    """The model's names of the places, by id; batches go by part of the world, so neighbours are named
    together (`among`: the places around them, all of the world; by default those named). A batch that
    comes back broken is asked once more."""
    names: dict[str, dict[str, str]] = {}
    ordered = sorted(groups, key=lambda g: (g.area, g.id))
    batches = [ordered[i : i + batch] for i in range(0, len(ordered), batch)]
    for n, part in enumerate(batches, 1):
        prompt = "\n\n".join(describe(g, among or groups) for g in part)
        for attempt in (1, 2):
            text = ""
            async for ev in llm.stream(instructions=LABEL_INSTRUCTIONS, messages=[Message("user", prompt)]):
                if isinstance(ev, TextDelta):
                    text += ev.text
            got = _parse(text)
            if got is not None:
                break
            log.warning("places: batch %d came back broken (attempt %d)", n, attempt)
        for item in got or []:
            if isinstance(item, dict) and item.get("id") in {g.id for g in part}:
                names[item["id"]] = {
                    k: str(item.get(k, "")).strip() for k in ("ru", "where_ru", "en", "where_en")
                }
        log.info("places: batch %d/%d, %d named", n, len(batches), len(names))
    return names


def _parse(text: str) -> list[Any] | None:
    m = re.search(r"\[.*\]", text, re.DOTALL)
    if m is None:
        return None
    try:
        data = json.loads(m.group(0))
    except json.JSONDecodeError:
        return None
    return data if isinstance(data, list) else None


def book(
    groups: list[Group],
    names: dict[str, dict[str, str]],
    world: str | None,
    surveys: dict[str, Survey] | None = None,
    kind: dict[str, str] | None = None,
) -> dict[str, Any]:
    """The file: places, of what kind, their ways in and neighbours, and which place each point belongs to."""
    places = []
    points: dict[str, int] = {}
    waynet = [p for g in groups for p in g.points if on_waynet(p)]
    for g in sorted(groups, key=lambda g: g.id):
        if g.id not in names:
            continue
        cx, cy, cz = g.center
        entry: dict[str, Any] = {"id": g.id, **names[g.id], "center": [round(cx), round(cy), round(cz)]}
        if (wp := anchor(g.points, g.center, waynet)) is not None:
            entry["anchor"] = wp
        if surveys is not None and (s := surveys.get(g.id)) is not None:
            entry["kind"] = (kind or {}).get(g.id, s.guess)
            entry["inside"] = s.inside >= INSIDE_SHARE
            entry["entrances"] = [w for w in s.entrances if w["side"] is None or w["side"] in names]
            entry["neighbours"] = [o for o in s.neighbours if o in names]
        places.append(entry)
        for p in g.points:
            points[p["name"].upper()] = len(places) - 1
    return {"version": VERSION, "world": (world or "").upper(), "places": places, "points": points}


def on_waynet(p: dict[str, Any]) -> bool:
    """A point of the dump that is a waypoint the waynet joins (not a freepoint, not a lone one)."""
    return not p.get("fp") and bool(p.get("conn"))


def anchor(
    own: Sequence[dict[str, Any]], center: Sequence[float], waynet: Sequence[dict[str, Any]]
) -> str | None:
    """A place's anchor: of its own waypoints the one nearest its middle; with none, the nearest of the
    world's (`waynet`): where one stands to wait there, or walks to by the waynet (meetings.py)."""
    pool = [p for p in own if on_waynet(p)] or list(waynet)
    best = min(pool, key=lambda p: math.dist((p["x"], p["y"], p["z"]), center), default=None)
    return str(best["name"]).upper() if best is not None else None


# --- the lie of the land (version 2) ----------------------------------------------------------------


@dataclass
class Survey:
    """What the waynet and the engine's rooms tell of a place."""

    inside: float  # the share of its points in a room: a cave's or a house's sector
    rooms: list[str]  # the rooms, the most points first
    guess: str  # the code's guess of its kind (KINDS)
    neighbours: list[str]  # the places the waynet joins it with, the most ways first
    entrances: list[dict[str, Any]]  # ways in, if it is inside: {"in", "out", "pos", "side"}


def _room_kind(room: str) -> str:
    words = set(re.split(r"[^A-Z]+", room.upper().removeprefix("S:")))
    stem = room.upper().removeprefix("S:").split("_")[0]
    if any(w in stem for w in KIND_WORDS["cave"]) or words & KIND_WORDS["cave"]:
        return "cave"
    return "house"


def guess_kind(g: Group, inside: float, rooms: Sequence[str]) -> str:
    """The code's guess of a place's kind: a room says cave or house; else the words of its points' names."""
    if inside >= INSIDE_SHARE and rooms:
        return _room_kind(rooms[0])
    words = Counter(w for p in g.points for w in tokens(p["name"]))
    words.update({w: 2 * len(g.points) for w in g.id.split("#")[0].split("_")})
    if _named_cave(g):
        return "other"  # a cave's points outside any room: by it, not in it
    best, score = "other", 0
    for kind in ("camp", "road", "field", "forest", "shore", "street", "yard"):
        n = sum(c for w, c in words.items() if w in KIND_WORDS[kind])
        if n > score:
            best, score = kind, n
    return best


def survey(groups: list[Group]) -> dict[str, Survey]:
    """Rooms, neighbours and ways in of each place, from the dump's waynet links (`conn`) and rooms.

    A way in is a link from a point in a room to one outside. It belongs to the place inside: the point's
    own if that is inside, else the one that has most of the room (a cave's room may reach out over the
    points of the forest at its mouth)."""
    place = {p["name"].upper(): g.id for g in groups for p in g.points}
    point = {p["name"].upper(): p for g in groups for p in g.points}
    links: dict[str, Counter[str]] = defaultdict(Counter)
    for name, p in point.items():
        for other in p.get("conn") or ():
            o = place.get(str(other).upper())
            if o is not None and o != place[name]:
                links[place[name]][o] += 1
    rooms = {g.id: Counter(str(p["room"]) for p in g.points if p.get("room")) for g in groups}
    inside = {g.id: sum(rooms[g.id].values()) / len(g.points) for g in groups}
    owner: dict[str, str] = {}  # room -> the place inside with most of it
    for gid in sorted(rooms, key=lambda gid: -max(rooms[gid].values(), default=0)):
        if inside[gid] >= INSIDE_SHARE:
            for room in rooms[gid]:
                owner.setdefault(room, gid)
    ways: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for name in sorted(point):
        p = point[name]
        if not p.get("room"):
            continue
        into = place[name] if inside[place[name]] >= INSIDE_SHARE else owner.get(str(p["room"]))
        if into is None:
            continue
        for other in p.get("conn") or ():
            q = point.get(str(other).upper())
            if q is None or q.get("room"):
                continue
            side = _side(q, into, place, point)
            if all(w["side"] != side for w in ways[into]):
                pos = [round(q["x"]), round(q["y"]), round(q["z"])]
                ways[into].append({"in": name, "out": q["name"].upper(), "pos": pos, "side": side})
    out: dict[str, Survey] = {}
    for g in groups:
        ordered = [r for r, _ in rooms[g.id].most_common()]
        found = sorted(ways[g.id], key=lambda w: w["side"] is None)  # a way in by a known place first
        if any(w["side"] is not None for w in found):
            found = [w for w in found if w["side"] is not None]
        out[g.id] = Survey(
            inside=round(inside[g.id], 2),
            rooms=ordered,
            guess=guess_kind(g, inside[g.id], ordered),
            neighbours=[o for o, _ in links[g.id].most_common()],
            entrances=found[:ENTRANCES],
        )
    return out


def _side(q: dict[str, Any], own: str, place: dict[str, str], point: dict[str, dict[str, Any]]) -> str | None:
    """The place just outside a way in: of the outer point, or, if that is still the place's own, of the
    first point of another one out along the waynet (outside, a few steps)."""
    seen = {q["name"].upper()}
    edge = [q]
    for _ in range(SIDE_STEPS):
        nxt = []
        for p in edge:
            pid = place.get(p["name"].upper())
            if pid is not None and pid != own:
                return pid
            for other in p.get("conn") or ():
                o = point.get(str(other).upper())
                if o is not None and not o.get("room") and o["name"].upper() not in seen:
                    seen.add(o["name"].upper())
                    nxt.append(o)
        edge = nxt
    return None


KIND_INSTRUCTIONS = """Ты размечаешь справочник мест острова Хоринис из игры Gothic 2: Ночь Ворона: какого
рода каждое место. Род — одно из:
- cave: пещера, грот, шахта, подземелье, склеп;
- house: дом, башня, таверна, кузница, храм — любое помещение под крышей;
- yard: двор фермы или дома, огород;
- street: улица, площадь, переулок, гавань города;
- road: дорога, тропа, мост, ступени, перевал;
- camp: лагерь, стоянка у костра;
- field: поле, пастбище, луг;
- forest: лес, опушка, роща;
- shore: берег, пляж, озеро, река;
- other: что-то иное — скалы, руины, долина, площадка.

О месте дано: ключ, название, имена его точек, какая доля точек в закрытом секторе движка («внутри»:
пещера или помещение), имена секторов, догадка кода. Сектор надёжен: внутри — значит пещера или помещение.
Место у пещеры, но вне её сектора — снаружи (у входа, на тропе): не cave. Под скалой или деревом без
сектора — тоже снаружи.

Ответ — только JSON-массив, по объекту на место, в том же порядке: {"id": ключ, "kind": род}."""


def describe_kind(g: Group, name: dict[str, str], s: Survey) -> str:
    """What the model is told of a place to say its kind."""
    names = sorted({p["name"] for p in g.points})
    sample = names[:SAMPLE_NAMES] + (["…"] if len(names) > SAMPLE_NAMES else [])
    rooms = ", ".join(s.rooms[:3]) or "нет"
    return "\n".join(
        [
            f"id: {g.id}",
            f"название: {name.get('ru', '')}",
            f"точки ({len(g.points)}): {', '.join(sample)}",
            f"внутри: {round(s.inside * 100)}% точек; секторы: {rooms}",
            f"догадка: {s.guess}",
        ]
    )


async def kinds(
    groups: list[Group],
    names: dict[str, dict[str, str]],
    surveys: dict[str, Survey],
    llm: LLMBackend,
    batch: int = KIND_BATCH,
) -> dict[str, str]:
    """The model's kind of each place, by id; where it says none or one not in KINDS, the code's guess."""
    out = {g.id: surveys[g.id].guess for g in groups}
    ordered = sorted(groups, key=lambda g: (g.area, g.id))
    batches = [ordered[i : i + batch] for i in range(0, len(ordered), batch)]
    for n, part in enumerate(batches, 1):
        prompt = "\n\n".join(describe_kind(g, names.get(g.id, {}), surveys[g.id]) for g in part)
        got: list[Any] | None = None
        for attempt in (1, 2):
            text = ""
            async for ev in llm.stream(instructions=KIND_INSTRUCTIONS, messages=[Message("user", prompt)]):
                if isinstance(ev, TextDelta):
                    text += ev.text
            got = _parse(text)
            if got is not None:
                break
            log.warning("places: kinds batch %d came back broken (attempt %d)", n, attempt)
        ids = {g.id for g in part}
        changed = 0
        for item in got or []:
            if isinstance(item, dict) and item.get("id") in ids and item.get("kind") in KINDS:
                changed += out[item["id"]] != item["kind"]
                out[item["id"]] = item["kind"]
        log.info("places: kinds batch %d/%d, %d changed from the guess", n, len(batches), changed)
    return out


# --- at run time -----------------------------------------------------------------------------------


@dataclass(frozen=True)
class Entrance:
    """A way into a place under a roof: where the waynet goes from inside it to outside."""

    inside: str  # the waypoint just inside
    outside: str  # the waypoint just outside
    pos: tuple[float, float, float]  # the outside one's
    side: str | None = None  # the place outside it (its id); None: the place's own edge


@dataclass(frozen=True)
class Place:
    id: str
    ru: str
    where_ru: str
    en: str
    where_en: str
    center: tuple[float, float, float] | None = None  # the middle of its points
    points: tuple[str, ...] = ()  # its waypoints and freepoints
    kind: str = "other"  # KINDS
    inside: bool = False  # under a roof: a cave, a house
    entrances: tuple[Entrance, ...] = ()
    neighbours: tuple[str, ...] = ()  # the places the waynet joins it with (ids)
    anchor: str | None = None  # the waypoint nearest its middle, its own or else the nearest of the world's

    @property
    def kind_ru(self) -> str:
        return KINDS.get(self.kind, KINDS["other"])[0]

    @property
    def kind_en(self) -> str:
        return KINDS.get(self.kind, KINDS["other"])[1]

    @property
    def waypoints(self) -> tuple[str, ...]:
        """Only this place's waypoints: freepoints cannot be targets of NPC_GETDISTTOWP."""
        return tuple(p for p in self.points if not p.upper().startswith("FP_"))

    @property
    def waynet(self) -> tuple[str, ...]:
        """Its points on the waynet, for a walk there: a freepoint (FP_…) is no waypoint, so the game walks to
        it in a straight line, into whatever stands between (Malet into the pasture's fence on his way to the
        mill, 2026-10-15). With none of its own, its anchor: a waypoint by it (74 places have freepoints
        only)."""
        return self.waypoints or ((self.anchor,) if self.anchor else ())


# where a spot is from the hero as he faces (the bridge's v18 words for people, perception.py)
SIDES = {"ahead": "перед героем", "left": "слева от героя", "right": "справа от героя",
         "behind": "за спиной героя"}  # fmt: skip


def side_of(
    hero: Sequence[float], ahead: Sequence[float], right: Sequence[float], at: Sequence[float]
) -> str:
    """Ahead, left, right or behind the hero (`ahead`, `right`: his axes on the ground, x and z; v32)."""
    dx, dz = at[0] - hero[0], at[2] - hero[2]
    length = math.hypot(dx, dz)
    if length < 1:
        return "ahead"
    fwd = (dx * ahead[0] + dz * ahead[1]) / length
    if fwd >= 0.7:
        return "ahead"
    if fwd <= -0.7:
        return "behind"
    return "right" if (dx * right[0] + dz * right[1]) > 0 else "left"


class PlaceBook:
    """The place of a waypoint, from the book (gamedata/places.json)."""

    def __init__(self, data: dict[str, Any]) -> None:
        self.world = str(data.get("world", "")).upper()
        self.points: dict[str, int] = dict(data.get("points", {}))
        names: dict[int, list[str]] = defaultdict(list)
        for name, i in sorted(self.points.items()):
            names[i].append(name)
        self.places = [
            Place(
                p["id"],
                p["ru"],
                p["where_ru"],
                p["en"],
                p["where_en"],
                tuple(float(c) for c in p["center"]) if len(p.get("center") or ()) == 3 else None,  # type: ignore[arg-type]
                tuple(names[i]),
                kind=str(p.get("kind", "other")),
                inside=bool(p.get("inside", False)),
                entrances=tuple(
                    Entrance(e["in"], e["out"], tuple(float(c) for c in e["pos"]), e.get("side"))  # type: ignore[arg-type]
                    for e in p.get("entrances", ())
                ),
                neighbours=tuple(p.get("neighbours", ())),
                anchor=p.get("anchor"),
            )
            for i, p in enumerate(data.get("places", []))
        ]
        self._by_id = {p.id: p for p in self.places}

    @classmethod
    def load(cls, path: Path = BOOK) -> PlaceBook | None:
        if not path.exists():
            return None
        data = json.loads(path.read_text(encoding="utf-8"))
        if data.get("version") != VERSION:
            log.warning("places: %s is version %s, expected %s", path, data.get("version"), VERSION)
            return None
        return cls(data)

    def of(self, *waypoints: str | None) -> Place | None:
        """The place of the first of these waypoints the book knows."""
        for wp in waypoints:
            if wp and (i := self.points.get(wp.upper())) is not None:
                return self.places[i]
        return None

    def is_waypoint(self, name: str) -> bool:
        """A known point usable by NPC_GETDISTTOWP; freepoints are only item/routine positions."""
        return not name.upper().startswith("FP_") and self.of(name) is not None

    def get(self, place_id: str) -> Place | None:
        return self._by_id.get(place_id.strip().upper())

    def distance(self, place: Place, pos: Sequence[float]) -> float:
        """How far a place is from `pos`: from its middle, or from the nearest of its ways in."""
        spots = [e.pos for e in place.entrances] + ([place.center] if place.center is not None else [])
        return min((math.dist(pos, s) for s in spots), default=float("inf"))

    def near(self, pos: Sequence[float], radius_cm: float, limit: int) -> list[tuple[Place, float]]:
        """The places within `radius_cm` of `pos` (x, y, z), nearest first, with how far (`distance`)."""
        out = []
        for p in self.places:
            if p.center is None or not p.points:
                continue
            d = self.distance(p, pos)
            if d <= radius_cm:
                out.append((p, d))
        return sorted(out, key=lambda pd: pd[1])[:limit]

    def around(
        self, pos: Sequence[float], radius_cm: float, limit: int, *, but: str | None = None
    ) -> list[tuple[Place, float]]:
        """The `limit` nearest places but the one `but`, and the nearest of each other kind too, nearest
        first: «зайди в пещеру» finds the cave though six places are nearer (by Cavalorn's fire,
        2026-10-14)."""
        near = [(p, d) for p, d in self.near(pos, radius_cm, len(self.places)) if p.id != but]
        shown = near[:limit]
        kinds = {p.kind for p, _ in shown}
        for p, d in near[limit:]:
            if p.kind not in kinds:
                kinds.add(p.kind)
                shown.append((p, d))
        return sorted(shown, key=lambda pd: pd[1])

    def nearest(self, pos: Sequence[float], kind: str, radius_cm: float) -> tuple[Place, float] | None:
        """The nearest place of a kind within `radius_cm`."""
        return next(((p, d) for p, d in self.near(pos, radius_cm, len(self.places)) if p.kind == kind), None)

    def way_in(self, place: Place, pos: Sequence[float] | None = None) -> Entrance | None:
        """Its way in nearest to `pos` (the first, without it); None: it has none."""
        if not place.entrances:
            return None
        if pos is None:
            return place.entrances[0]
        return min(place.entrances, key=lambda e: math.dist(pos, e.pos))

    def ways_in_ru(self, place: Place) -> str | None:
        """Where its ways in are, by the places outside them: «вход — на ступенях на дороге от башни
        Ксардаса»."""
        sides = []
        for e in place.entrances:
            out = self.get(e.side) if e.side else None
            sides.append(out.where_ru if out is not None else "с краю")
        if not sides:
            return None
        return ("вход — " if len(sides) == 1 else "входы — ") + "; ".join(dict.fromkeys(sides))

    def line_ru(
        self,
        place: Place,
        d_cm: float,
        hero: Sequence[float] | None = None,
        ahead: Sequence[float] | None = None,
        right: Sequence[float] | None = None,
        *,
        mine: bool = False,
    ) -> str:
        """A place for the conversational model: «XARDAS_BANDITS — пещера бандитов…: пещера, 25 м, слева от
        героя; вход — на ступенях…» (`mine`: «25 м от тебя»)."""
        parts = [place.kind_ru, f"{max(1, round(d_cm / 100))} м" + (" от тебя" if mine else "")]
        if hero is not None and ahead is not None and right is not None:
            way = self.way_in(place, hero)
            at = way.pos if way is not None else place.center
            if at is not None:
                parts.append(SIDES[side_of(hero, ahead, right, at)])
        text = f"{place.id} — {place.ru}: {', '.join(parts)}"
        if ways := self.ways_in_ru(place):
            text += f"; {ways}"
        return text
