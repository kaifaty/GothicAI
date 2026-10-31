"""Npc's chests (docs/npc-tools.md «Сундуки»): an npc knows what lies in its chest, and goes to take it out
or to put it in.

**Which chests are its own** the bridge decides (npcchests.cpp, `npc_chests`, v28): the world's data names
it the owner (Lobart, Lee, Rukhar), it carries its key, or nobody owns the chest and it stands by a point of
the npc's daily routine (Brago's in the bandits' cave).

**Truth and belief** (the deception pillar: code owns truth, npc's own beliefs):
- the game knows what really lies in a chest;
- the npc knows what lay there when it last looked: the first time it comes up, what is there (it is its
  own chest from the start), then what it saw each time it opened it. Kept in the save branch's memory
  (kind `chest`), so a load brings it back;
- the hero may take things out unseen: the npc goes on believing they are there until it opens the chest,
  and then notices what is gone.

**In a turn:** the situation tells what lies in its chests, as it knows; the tool `chest` takes things out
and puts them in. The body walks up to the chest after the npc's line, opens it and closes it; what it took
is in its inventory for its next turn (a give, a deal: deals.py points to the chest when the npc offers
what it keeps there).
"""

from __future__ import annotations

import asyncio
import json
import logging
import time
from collections.abc import Callable, Coroutine, Mapping
from dataclasses import dataclass, field
from typing import TYPE_CHECKING, Any

from .contracts import carried
from .game import GameError, GameLink
from .journal import Journal
from .llm import Tool, ToolCall
from .memory import MemoryStore
from .protocol import Chest, GameMessage, Loading, MemoryCmd, Session, Snapshot
from .turn import ToolOutcome

if TYPE_CHECKING:
    from .crimes import Crimes
    from .dialogue import Conversation, DialogueManager
    from .places import PlaceBook

log = logging.getLogger(__name__)

CHEST = "chest"
FRESH_S = 30.0  # what the game said of an npc's chests holds this long
CHEST_S = 90.0  # the bridge gives the walk to the chest 60 s once the npc has said its lines
NEAR_HERO_CM = 1500.0  # it went from the hero this close: it comes back to him after
BACK_CM = 250  # ...this close
SHOWN = 2  # chests in the situation, the nearest first
SHOWN_ITEMS = 15  # things of a chest in the situation
# back from the chest, it goes on with what it went for (a give, a deal), the note of what it found before
# 2026-10-14 Lobart, back with the clothes he had agreed to sell, gave them away (give_item): the deal it is
BACK = (
    "[Ты вернулся от сундука. Продолжай разговор с героем. Ходил за вещью для сделки — предложи сделку "
    "сейчас (propose_deal), а не отдавай даром.]"
)

Items = dict[str, tuple[int, str]]  # item -> (count, name)

_MOVES = {
    "type": "array",
    "items": {
        "type": "object",
        "properties": {
            "item": {"type": "string", "description": "идентификатор предмета, например ITRW_BOW_L_01"},
            "count": {"type": "integer", "description": "сколько, не меньше 1"},
        },
        "required": ["item", "count"],
        "additionalProperties": False,
    },
}

CHEST_TOOL = Tool(
    CHEST,
    "Достать вещи из своего сундука или положить в него (твои сундуки и что в них, насколько ты знаешь, — "
    "в обстановке). Тело сходит к сундуку после твоей реплики, откроет его и закроет; достанное будет у "
    "тебя в инвентаре — потом его можно отдать герою или предложить в сделке. Скажи, что идёшь за этим. "
    "Ненужный список — пустой.",
    {
        "type": "object",
        "properties": {
            "take": {**_MOVES, "description": "что достать из сундука"},
            "put": {**_MOVES, "description": "что положить в сундук из своего инвентаря"},
            "chest": {
                "type": ["string", "null"],
                "description": "id сундука, если их у тебя несколько; null — ближайший",
            },
        },
        "required": ["take", "put", "chest"],
        "additionalProperties": False,
    },
)

OWNED = {
    "owner": "Твой сундук",
    "key": "Сундук, ключ от которого у тебя",
    "home": "Сундук там, где ты живёшь",
}


@dataclass
class Known:
    """What the npc knows lies in a chest: what it saw there when it last looked."""

    chest: str
    items: Items = field(default_factory=dict)

    def record(self) -> dict[str, Any]:
        return {"chest": self.chest, "items": [[i, c, n] for i, (c, n) in self.items.items()]}

    @classmethod
    def of(cls, raw: dict[str, Any]) -> Known:
        return cls(str(raw["chest"]), {str(i): (int(c), str(n)) for i, c, n in raw.get("items", [])})


def items_of(chest: Chest, *, start: bool = False) -> Items:
    """What lies in the chest now, or (`start`) what the world's data put in it."""
    out: Items = {}
    for it in chest.start if start else chest.items:
        count, _ = out.get(it.item, (0, it.name))
        out[it.item] = (count + it.count, it.name)
    return out


def listed(items: Items, limit: int = SHOWN_ITEMS) -> str:
    """«Короткий лук (ITRW_BOW_L_01), Золото (ITMI_GOLD) ×29»."""
    parts = [f"{name} ({item}){f' ×{count}' if count > 1 else ''}" for item, (count, name) in items.items()]
    more = f" и ещё {len(parts) - limit}" if len(parts) > limit else ""
    return ", ".join(parts[:limit]) + more


def parse_moved(detail: str | None) -> tuple[dict[str, int], dict[str, int]]:
    """What the bridge says moved: "took=ITEM:N,…;put=ITEM:N,…"."""
    took: dict[str, int] = {}
    put: dict[str, int] = {}
    for part in (detail or "").split(";"):
        key, _, rest = part.partition("=")
        into = took if key.strip() == "took" else put if key.strip() == "put" else None
        if into is None:
            continue
        for pair in filter(None, rest.split(",")):
            item, _, n = pair.partition(":")
            into[item.strip().upper()] = into.get(item.strip().upper(), 0) + int(n or 1)
    return took, put


class ChestDesk:
    """The npc's chests: a TurnExtension (the situation, the tool `chest`)."""

    def __init__(
        self,
        link: GameLink,
        memory: MemoryStore,
        *,
        places: PlaceBook | None = None,
        journal: Journal | None = None,
        crimes: Crimes | None = None,
        clock: Callable[[], float] = time.monotonic,
    ) -> None:
        self.link = link
        self.memory = memory
        self.places = places
        self.journal = journal or Journal()
        self.crimes = crimes
        self.npc_suspect: Callable[..., list[Mapping[str, Any]]] | None = None
        self.clock = clock
        self.branch: str | None = link.session.branch if link.session is not None else None
        self._chests: dict[str, tuple[float, list[Chest]]] = {}  # npc -> (when asked, its chests)
        self._known: dict[str, dict[str, Known]] = {}  # npc -> chest -> what it knows lies there
        self._going: set[str] = set()  # npc's on their way to a chest
        self._tasks: set[asyncio.Task[Any]] = set()
        self._generation = 0
        link.listeners.append(self._on_message)

    def _on_message(self, msg: GameMessage) -> None:
        match msg:
            case Session():
                self.branch = msg.branch
                self._forget()
            case Loading():
                self._forget()

    def _forget(self) -> None:
        self._generation += 1
        self._chests.clear()
        self._known.clear()
        self._going.clear()

    # --- what is there and what it knows ---------------------------------------------------------

    async def chests(self, npc: str, *, fresh: bool = False) -> list[Chest]:
        """The npc's chests as the game has them now (asked again after FRESH_S). A chest it has not known
        before it knows as the world's data filled it (its own from the start: what was taken out unseen
        since is missing when it looks), else as it is."""
        npc = npc.upper()
        generation = self._generation
        cached = self._chests.get(npc)
        if cached is not None and not fresh and self.clock() - cached[0] < FRESH_S:
            return cached[1]
        try:
            found = await self.link.npc_chests(npc)
        except (GameError, TimeoutError) as err:
            log.info("chests of %s: %s", npc, err or type(err).__name__)
            return cached[1] if cached is not None else []
        if generation != self._generation:
            return []
        self._chests[npc] = (self.clock(), found)
        known = self.known(npc)
        for c in found:
            if c.id not in known:
                self._learn(npc, Known(c.id, items_of(c, start=bool(c.start))))
        return found

    def known(self, npc: str) -> dict[str, Known]:
        """What the npc knows lies in its chests, read from the save branch the first time."""
        npc = npc.upper()
        if (known := self._known.get(npc)) is not None:
            return known
        known = {}
        for raw in self.memory.chests(self.branch, npc) if self.branch is not None else []:
            try:
                k = Known.of(json.loads(raw))
            except (ValueError, TypeError, KeyError) as err:
                log.warning("chests of %s: a broken record: %s", npc, err)
                continue
            known[k.chest] = k  # the later record wins
        self._known[npc] = known
        return known

    def _learn(self, npc: str, k: Known) -> None:
        self.known(npc)[k.chest] = k
        if self.branch is None:
            return
        seq = self.memory.add_chest(self.branch, npc, json.dumps(k.record(), ensure_ascii=False))
        self._spawn(self.link.send(MemoryCmd(branch=self.branch, seq=seq)))  # into the next save

    def in_chests(self, npc: str) -> Items:
        """What the npc knows it keeps in its chests, all of them (deals.py: a thing it offers is there)."""
        out: Items = {}
        for k in self.known(npc).values():
            for item, (count, name) in k.items.items():
                out[item] = (out.get(item, (0, name))[0] + count, name)
        return out

    # --- the turn extension ----------------------------------------------------------------------

    async def prepare(self, npc: str) -> None:
        await self.chests(npc)

    def situation(self, npc: str) -> str | None:
        npc = npc.upper()
        cached = self._chests.get(npc)
        if cached is None or not cached[1]:
            return None
        found = cached[1][:SHOWN]
        known = self.known(npc)
        lines = []
        for c in found:
            k = known.get(c.id) or Known(c.id)
            where = self._where(c)
            key = c.has_key and c.why != "key"  # «ключ от которого у тебя» says it already
            lock = (", заперт, ключ у тебя" if key else ", заперт") if c.locked else ""
            ids = f"; id {c.id}" if len(found) > 1 else ""
            inside = (
                f"В нём, насколько ты знаешь: {listed(k.items)}."
                if k.items
                else "Он пуст, насколько ты знаешь."
            )
            lines.append(f"{OWNED[c.why]} — {where}{lock}{ids}. {inside}")
        return "\n".join(lines)

    def _where(self, c: Chest) -> str:
        place = self.places.of(c.waypoint) if self.places is not None else None
        far = f"в {max(1, round(c.distance / 100))} м отсюда"
        return f"{place.where_ru}, {far}" if place is not None else far

    all_tools: tuple[Tool, ...] = (CHEST_TOOL,)

    def tools(self, npc: str) -> list[Tool]:
        cached = self._chests.get(npc.upper())
        return [CHEST_TOOL] if cached is not None and cached[1] else []

    async def handle(
        self, dialogue: DialogueManager, conv: Conversation, call: ToolCall, snap: Snapshot | None
    ) -> ToolOutcome | None:
        if call.name != CHEST:
            return None
        npc = conv.npc
        cached = self._chests.get(npc)
        found = cached[1] if cached is not None else []
        if not found:
            return ToolOutcome("Отклонено: у тебя нет сундука.", "rejected")
        want = str(call.arguments.get("chest") or "").strip().upper()
        chest = next((c for c in found if c.id == want), None) if want else found[0]
        if chest is None:
            return ToolOutcome(
                f"Отклонено: нет у тебя сундука {want}; твои: {', '.join(c.id for c in found)}.", "rejected"
            )
        take, why = self._moves(
            call.arguments.get("take"), self.known(npc).get(chest.id, Known(chest.id)).items, "в сундуке"
        )
        if take is None:
            return ToolOutcome(f"Отклонено: {why}", "rejected")
        put, why = self._moves(call.arguments.get("put"), carried(snap), "у тебя в инвентаре")
        if put is None:
            return ToolOutcome(f"Отклонено: {why}", "rejected")
        if not take and not put:
            return ToolOutcome("Отклонено: ни что достать, ни что положить.", "rejected")
        if npc in self._going:
            return ToolOutcome("Ты уже идёшь к сундуку.", "accepted")
        self._going.add(npc)
        args = {"chest": chest.id, "take": take, "put": put}
        log.info("%s goes to its chest %s: %s", npc, chest.id, args)
        self.journal.add("action", npc=npc, name=conv.persona.name, action=CHEST, arguments=args)
        near = snap is not None and snap.player is not None and snap.player.distance <= NEAR_HERO_CM
        dialogue.after_speech(
            conv, lambda: self._go(dialogue, conv, chest.id, take, put, back=near), then=BACK, kind=CHEST
        )
        return ToolOutcome("Принято: сходишь к сундуку после реплики.", "accepted")

    @staticmethod
    def _moves(raw: Any, have: Items, where: str) -> tuple[list[dict[str, Any]] | None, str]:
        """The things to move, each in `have` as many as asked; or why not."""
        out: dict[str, int] = {}
        for m in raw or []:
            item = str((m or {}).get("item", "")).strip().upper()
            count = max(1, int((m or {}).get("count") or 1))
            if item:
                out[item] = out.get(item, 0) + count
        for item, count in out.items():
            n, name = have.get(item, (0, item))
            if n == 0:
                return None, f"{item} нет {where}. Там: {listed(have) or 'пусто'}."
            if n < count:
                return None, f"{name} ({item}) {where} только {n}."
        return [{"item": i, "count": c} for i, c in out.items()], ""

    # --- at the chest ------------------------------------------------------------------------------

    async def _go(
        self,
        dialogue: DialogueManager,
        conv: Conversation,
        chest: str,
        take: list[dict[str, Any]],
        put: list[dict[str, Any]],
        *,
        back: bool = False,
    ) -> tuple[str, bool]:
        """The walk to the chest, and (`back`) to the hero again: it went from him to fetch something."""
        npc = conv.npc
        try:
            if not await dialogue.borrow_body(conv, CHEST):
                return "Тело не смогло сходить к сундуку: ты занят", False
            try:
                res = await self.link.act(npc, "use_chest", chest=chest, take=take, put=put, timeout=CHEST_S)
                if res.status == "done" and back:
                    await self._back(npc)
            except (GameError, TimeoutError) as err:
                return f"Не вышло сходить к сундуку: {err or type(err).__name__}", False
            finally:
                dialogue.return_body(conv, CHEST)
            if res.status != "done":
                log.info("%s at its chest %s: %s %s", npc, chest, res.status, res.detail)
                return f"Не вышло открыть сундук ({res.detail or res.status})", False
            return await self.looked(npc, chest, res.detail), True
        finally:
            self._going.discard(npc)

    async def _back(self, npc: str) -> None:
        """Back to the hero: by the waynet to the point nearest him (out of a house there is no straight
        way: 2026-10-14 Lobart stood by his bed a minute), then up to him."""
        snap = await self.link.snapshot(npc, fresh=True)
        if snap.player is not None and snap.player.distance > BACK_CM * 2:
            pos = list(snap.player.pos)
            way = await self.link.act(npc, "go_to_waypoint", waypoint="", pos=pos, timeout=CHEST_S)
            log.info("%s back from its chest by the waynet: %s %s", npc, way.status, way.detail or "")
        came = await self.link.act(npc, "go_to_npc", npc="PLAYER", radius=BACK_CM, timeout=CHEST_S)
        log.info("%s back from its chest to the hero: %s", npc, came.status)

    async def looked(self, npc: str, chest: str, detail: str | None) -> str:
        """The npc has opened its chest: it sees what is there now. What it took and put, and what is gone
        that it believed was there (taken unseen). Its note."""
        npc = npc.upper()
        generation = self._generation
        before = self.known(npc).get(chest, Known(chest)).items
        took, put = parse_moved(detail)
        now = next((c for c in await self.chests(npc, fresh=True) if c.id == chest), None)
        if generation != self._generation:
            raise GameError("мир сменился во время осмотра сундука")
        after = items_of(now) if now is not None else {}
        names = {i: n for i, (_, n) in {**before, **after}.items()}
        missing: dict[str, int] = {}
        for item, (count, _) in before.items():
            expected = count - took.get(item, 0) + put.get(item, 0)
            gone = expected - after.get(item, (0, ""))[0]
            if gone > 0:
                missing[item] = gone
        self._learn(npc, Known(chest, after))

        def text(moved: dict[str, int]) -> str:
            return ", ".join(f"{names.get(i, i)}{f' ×{n}' if n > 1 else ''}" for i, n in moved.items())

        parts = ["Ты сходил к своему сундуку и открыл его"]
        if took:
            parts.append(f"достал: {text(took)}")
        if put:
            parts.append(f"положил: {text(put)}")
        note = "; ".join(parts) + "."
        if missing:
            note += f" Из сундука пропало: {text(missing)} — кто-то в нём рылся."
            if self.npc_suspect is not None:
                suspects = self.npc_suspect(
                    npc, chest, missing, waypoint=now.waypoint if now is not None else "",
                )
                if suspects:
                    note += (
                        " Ты вспоминаешь людей рядом около времени пропажи; "
                        "это подозрение, не доказательство."
                    )
            if self.crimes is not None:
                suspicions = self.crimes.suspect_chest(
                    npc,
                    chest,
                    missing,
                    waypoint=now.waypoint if now is not None else "",
                )
                if any(f.get("suspect") == "PLAYER" for f in suspicions):
                    note += (
                        " Ты вспоминаешь героя рядом около времени пропажи и подозреваешь его; "
                        "доказательств нет."
                    )
        self.journal.add("chest", npc=npc, chest=chest, took=took, put=put, missing=missing)
        log.info("%s at its chest %s: took %s, put %s, missing %s", npc, chest, took, put, missing)
        return note

    def _spawn(self, coro: Coroutine[Any, Any, Any]) -> None:
        task = asyncio.get_running_loop().create_task(coro)
        self._tasks.add(task)
        task.add_done_callback(self._tasks.discard)
