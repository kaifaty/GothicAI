"""Contracts: what an npc gives besides the things it carries, and the tasks it pays the hero for
(docs/contracts.md).

- **Rewards past the inventory.** Experience (`xp`, the scripts' B_GivePlayerXP); teaching through the
  scripts' own teacher functions, for the hero's learning points as a teacher in the game takes them
  (`teach`, protocol v17: the deal's check says beforehand whether he has enough); the things an npc keeps
  but does not carry (its stash, gamedata/stash.json), created into its hands as it gives them.
- **A task.** Whom the hero kills (a person or a creature by its symbol, or a kind of them by guild) and
  what he brings, by a game day or with no day at all. The reward is the npc's word, given with the deal:
  when the task is done, it is paid the next time the hero talks to an npc that keeps its word. A recorded
  dishonest intention instead makes it refuse once, with no automatic payment later. The game reports
  whoever the hero kills (`hero_kill`, v17);
  kills before the contract don't count.

The facts in the chronicle: `contract` (the terms), `contract_kill` (one more of a kill line), `contract_done`
(the kills done), `contract_paid`, `contract_void` (the one to be killed died by another hand) and
`broken {contract}` (its day passed). A deal's own gives from the stash are written in the `deal` fact
(`npc_gives[].stash`), so what an npc still keeps is its stash less what those facts say it gave.
"""

from __future__ import annotations

import asyncio
import itertools
import json
import logging
import re
from collections.abc import Callable, Coroutine, Mapping
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING, Any, Literal

from .chronicle import Chronicle
from .game import GameError, GameLink
from .journal import Journal
from .npc_cheat import breach, hearers, keeps
from .protocol import ChatStatusCmd, DealTerm, GameMessage, HeroKill, NpcDied, RosterEntry, Session, Snapshot

if TYPE_CHECKING:
    from .dialogue import Conversation, DialogueManager
    from .llm import Tool, ToolCall
    from .turn import ToolOutcome

log = logging.getLogger(__name__)

GOLD = "ITMI_GOLD"
STASH_FILE = Path(__file__).parent / "gamedata" / "stash.json"
MAX_COUNT = 100_000  # a sanity bound, not a price limit: what is fair is the npc's to judge
MAX_TERMS = 6  # lines on one side
MAX_TASK_DAYS = 60  # the furthest a task's day may be; 0: no day at all
SYMBOL = re.compile(r"^[A-Z][A-Z0-9_]*$")


def gold_text(count: int) -> str:
    return f"{count} {'золотой' if count % 10 == 1 and count % 100 != 11 else 'золотых'}"


@dataclass(frozen=True)
class Give:
    item: str  # instance symbol
    count: int
    name: str  # in the game
    stash: int = 0  # of the count, what the npc takes from its stash (created into its hands first)

    def text(self) -> str:
        if self.item == GOLD:
            return gold_text(self.count)
        return f"{self.name} ×{self.count}" if self.count > 1 else self.name

    def term(self, text: str | None = None) -> DealTerm:
        return DealTerm(item=self.item, count=self.count, text=text or self.text())

    def effect(self, to: Literal["hero", "self"]) -> dict[str, Any]:
        return {"do": "give", "item": self.item, "count": self.count, "to": to}

    def effects(self, to: Literal["hero", "self"]) -> list[dict[str, Any]]:
        """The give, and before it what the npc takes out of its stash."""
        made = (
            [{"do": "create", "item": self.item, "count": self.stash}] if self.stash and to == "hero" else []
        )
        return made + [self.effect(to)]

    def fact(self) -> dict[str, Any]:
        out: dict[str, Any] = {"item": self.item, "count": self.count, "name": self.name}
        if self.stash:
            out["stash"] = self.stash
        return out


# --- teaching -----------------------------------------------------------------------------------------

TeachWhat = Literal["attribute", "fight", "thief", "circle", "rune", "alchemy", "smith", "trophy"]
ATTRIBUTES = {"ATR_STRENGTH": "сила", "ATR_DEXTERITY": "ловкость", "ATR_MANA_MAX": "мана"}
FIGHT = {
    "NPC_TALENT_1H": "одноручное оружие",
    "NPC_TALENT_2H": "двуручное оружие",
    "NPC_TALENT_BOW": "лук",
    "NPC_TALENT_CROSSBOW": "арбалет",
}
THIEF = {
    "NPC_TALENT_PICKLOCK": "взлом замков",
    "NPC_TALENT_SNEAK": "умение красться",
    "NPC_TALENT_PICKPOCKET": "карманная кража",
    "NPC_TALENT_ACROBAT": "акробатика",
}
# what is taught one by one: the prefix of its constant, and of the item it is named after
PREFIXES = {
    "rune": ("SPL_", "ITRU_"),
    "alchemy": ("POTION_", "ITPO_"),
    "smith": ("WEAPON_", ""),
    "trophy": ("TROPHY_", ""),
}
TEACH_KINDS: tuple[TeachWhat, ...] = (
    "attribute",
    "fight",
    "thief",
    "circle",
    "rune",
    "alchemy",
    "smith",
    "trophy",
)


@dataclass(frozen=True)
class Teach:
    what: TeachWhat
    arg: str  # the scripts' constant; empty for a circle
    count: int  # points (attribute), percent (fight), the circle (circle); 1 otherwise
    label: str  # what it is, in words

    def text(self) -> str:
        match self.what:
            case "attribute":
                return f"{self.label} +{self.count}"
            case "fight":
                return f"{self.label} +{self.count}%"
            case "circle":
                return f"{self.count}-й круг магии"
            case "rune":
                return f"руна «{self.label}»"
            case "alchemy":
                return f"зелье «{self.label}»"
            case "smith":
                return f"ковка: {self.label}"
            case "trophy":
                return f"трофеи: {self.label}"
            case _:
                return self.label

    def effect(self) -> dict[str, Any]:
        return {"do": "teach", "teach": self.what, "arg": self.arg, "count": self.count}

    def fact(self) -> dict[str, Any]:
        return {"what": self.what, "arg": self.arg, "count": self.count, "label": self.label}

    @staticmethod
    def of(f: dict[str, Any]) -> Teach:
        return Teach(f["what"], f.get("arg", ""), int(f.get("count", 1)), f.get("label", ""))


async def parse_teach(link: GameLink, raw: Any) -> tuple[tuple[Teach, ...] | None, str]:
    """[{what, arg, count}] -> what the npc teaches; or why not. The constants are checked in the scripts."""
    if not raw:
        return (), ""
    if not isinstance(raw, list):
        return None, "teach — список."
    if len(raw) > MAX_TERMS:
        return None, f"не больше {MAX_TERMS} уроков."
    wanted: list[tuple[TeachWhat, str, int]] = []
    for x in raw:
        if not isinstance(x, dict):
            return None, "каждый урок — {what, arg, count}."
        what = str(x.get("what") or "").strip().lower()
        arg = str(x.get("arg") or "").strip().upper()
        try:
            count = int(x.get("count") or 0)
        except (TypeError, ValueError):
            count = 0
        if what not in TEACH_KINDS:
            return None, f"нельзя учить «{what}»: можно {', '.join(TEACH_KINDS)}."
        if what == "circle":
            if not 1 <= count <= 6:
                return None, "круг магии — от 1 до 6 (count)."
            arg = ""
        elif what in ("attribute", "fight"):
            table = ATTRIBUTES if what == "attribute" else FIGHT
            if arg not in table:
                return None, f"{what}: arg — одно из {', '.join(table)}."
            if not 1 <= count <= MAX_COUNT:
                return None, f"{what}: count — сколько очков или процентов, от 1."
        elif what == "thief":
            if arg not in THIEF:
                return None, f"thief: arg — одно из {', '.join(THIEF)}."
            count = 1
        else:
            prefix = PREFIXES[what][0]
            if not arg.startswith(prefix) or not SYMBOL.match(arg):
                return None, f"{what}: arg — константа {prefix}… из скриптов."
            count = 1
        wanted.append((what, arg, count))  # type: ignore[arg-type]
    asked = [a for w, a, _ in wanted if w in PREFIXES]
    names = [
        PREFIXES[w][1] + a[len(PREFIXES[w][0]) :] for w, a, _ in wanted if w in PREFIXES and PREFIXES[w][1]
    ]
    known: dict[str, Any] = {}
    named: dict[str, Any] = {}
    if asked:
        try:
            state = await link.story_state(vars=asked, items=names)
        except (GameError, TimeoutError) as err:
            return None, f"не выходит проверить уроки: {err}"
        known, named = state.get("vars", {}), state.get("items", {})
    out = []
    for what, arg, count in wanted:
        if what in PREFIXES:
            if arg not in known:
                return None, f"в игре нет {arg}."
            item = PREFIXES[what][1] + arg[len(PREFIXES[what][0]) :] if PREFIXES[what][1] else ""
            label = str(named.get(item) or arg)
        else:
            label = {**ATTRIBUTES, **FIGHT, **THIEF}.get(arg, "")
        out.append(Teach(what, arg, count, label))
    return tuple(out), ""


# --- whom to kill ---------------------------------------------------------------------------------------

# kinds of creatures and people by the scripts' guilds, for the deal's words (the model may name others)
GUILDS = {
    "GIL_WOLF": "волки",
    "GIL_ORC": "орки",
    "GIL_UNDEADORC": "орки-нежить",
    "GIL_DRAGON": "драконы",
    "GIL_DRACONIAN": "ящеролюди",
    "GIL_SKELETON": "скелеты",
    "GIL_ZOMBIE": "зомби",
    "GIL_GOBBO": "гоблины",
    "GIL_LURKER": "луркеры",
    "GIL_SNAPPER": "снепперы",
    "GIL_SHADOWBEAST": "мракорисы",
    "GIL_TROLL": "тролли",
    "GIL_DEMON": "демоны",
    "GIL_HARPY": "гарпии",
    "GIL_WARAN": "вараны",
    "GIL_SCAVENGER": "падальщики",
    "GIL_MOLERAT": "кротокрысы",
    "GIL_GIANT_RAT": "крысы",
    "GIL_GIANT_BUG": "полевые жуки",
    "GIL_BLOODFLY": "кровавые мухи",
    "GIL_MINECRAWLER": "ползуны",
    "GIL_SWAMPSHARK": "болотные акулы",
    "GIL_ALLIGATOR": "аллигаторы",
    "GIL_STONEGOLEM": "каменные големы",
    "GIL_FIREGOLEM": "огненные големы",
    "GIL_ICEGOLEM": "ледяные големы",
    "GIL_SWAMPGOLEM": "болотные големы",
    "GIL_SHEEP": "овцы",
    "GIL_BDT": "бандиты",
    "GIL_PIR": "пираты",
    "GIL_DMT": "ищущие",
    "GIL_MIL": "ополченцы",
    "GIL_PAL": "паладины",
    "GIL_SLD": "наёмники",
    "GIL_BAU": "крестьяне",
    "GIL_VLK": "горожане",
}


@dataclass
class Kill:
    target: str  # an npc's symbol, or a guild's constant (GIL_*)
    guild: int | None  # the guild's number for a kind; None for one npc
    label: str
    count: int
    done: int = 0

    def matches(self, msg: HeroKill) -> bool:
        return msg.guild == self.guild if self.guild is not None else msg.npc.upper() == self.target

    def text(self) -> str:
        return f"{self.label} ×{self.count}" if self.count > 1 else self.label

    def fact(self) -> dict[str, Any]:
        return {"target": self.target, "guild": self.guild, "label": self.label, "count": self.count}


async def parse_kills(
    link: GameLink, raw: Any, names: Callable[[str], str | None] | None = None
) -> tuple[tuple[Kill, ...] | None, str]:
    """[{target, count}] -> whom the hero is to kill; or why not. A target is an npc's symbol or name (one
    of them: the count is 1), or a kind by guild (GIL_*)."""
    if not raw:
        return (), ""
    if not isinstance(raw, list):
        return None, "kill — список."
    if len(raw) > MAX_TERMS:
        return None, f"не больше {MAX_TERMS} целей."
    asked: list[tuple[str, int]] = []
    for x in raw:
        if not isinstance(x, dict):
            return None, "каждая цель — {target, count}."
        target = str(x.get("target") or "").strip()
        try:
            count = int(x.get("count") or 1)
        except (TypeError, ValueError):
            count = 1
        if not target:
            return None, "у цели нет target."
        if not 1 <= count <= MAX_COUNT:
            return None, f"неверное число целей: {count}."
        if not SYMBOL.match(target.upper()):
            symbol = names(target) if names is not None else None
            if symbol is None:
                return (
                    None,
                    f"не знаю, кто это — «{target}». Назови его символом из скриптов "
                    "или вид по гильдии (GIL_…).",
                )
            target = symbol
        asked.append((target.upper(), count))
    guilds = [t for t, _ in asked if t.startswith("GIL_")]
    people = [t for t, _ in asked if not t.startswith("GIL_")]
    try:
        state = await link.story_state(vars=guilds, symbols=people, npcs=people)
    except (GameError, TimeoutError) as err:
        return None, f"не выходит проверить цели: {err}"
    values, kinds, npcs = state.get("vars", {}), state.get("symbols", {}), state.get("npcs", {})
    out = []
    for target, count in asked:
        if target.startswith("GIL_"):
            if target not in values:
                return None, f"в игре нет гильдии {target}."
            out.append(Kill(target, int(values[target]), GUILDS.get(target, target), count))
            continue
        if kinds.get(target) != "npc":
            return None, f"в игре нет такого человека или существа: {target}."
        seen = npcs.get(target, {})
        if seen.get("state") == "dead":
            return None, f"{seen.get('name') or target} уже мёртв."
        out.append(Kill(target, None, str(seen.get("name") or target), 1))
    return tuple(out), ""


def symbol_by_name(cards: dict[str, Any], name: str) -> str | None:
    """The npc a name means, by the character cards (one, or none if the name is anyone's)."""
    want = name.strip().lower()
    found = [sym for sym, card in cards.items() if str(getattr(card, "name", "")).strip().lower() == want]
    return found[0] if len(found) == 1 else None


# --- the task and its reward ------------------------------------------------------------------------------


@dataclass(frozen=True)
class Reward:
    gives: tuple[Give, ...] = ()  # what the npc gives (from its inventory or its stash, as it has them then)
    xp: int = 0
    teach: tuple[Teach, ...] = ()

    def __bool__(self) -> bool:
        return bool(self.gives or self.xp or self.teach)

    def parts(self) -> list[str]:
        out = [g.text() for g in self.gives]
        if self.xp:
            out.append(f"опыт +{self.xp}")
        out += [f"учишь: {t.text()}" for t in self.teach]
        return out

    def text(self) -> str:
        return ", ".join(self.parts()) or "ничего"

    def fact(self) -> dict[str, Any]:
        return {
            "gives": [g.fact() for g in self.gives],
            "xp": self.xp,
            "teach": [t.fact() for t in self.teach],
        }

    @staticmethod
    def of(f: dict[str, Any]) -> Reward:
        gives = tuple(
            Give(g["item"], int(g["count"]), g.get("name") or g["item"]) for g in f.get("gives", [])
        )
        return Reward(gives, int(f.get("xp", 0)), tuple(Teach.of(t) for t in f.get("teach", [])))


@dataclass(frozen=True)
class Task:
    kill: tuple[Kill, ...] = ()
    bring: tuple[Give, ...] = ()  # what the hero hands over when he comes for the reward
    due: int | None = None  # the game day (0-based) it must be done by; None: no day

    def __bool__(self) -> bool:
        return bool(self.kill or self.bring)

    def parts(self) -> list[str]:
        out = [f"убить: {k.text()}" for k in self.kill]
        out += [f"принести: {g.text()}" for g in self.bring]
        return out

    def when(self) -> str:
        return f"до конца дня {self.due + 1}" if self.due is not None else "без срока"

    def text(self) -> str:
        return f"{', '.join(self.parts())} ({self.when()})"


ContractState = Literal["open", "done", "paid", "broken", "void", "unpaid"]


@dataclass
class Contract:
    id: str
    npc: str
    name: str
    kill: list[Kill]
    bring: tuple[Give, ...]
    due: int | None
    reward: Reward
    state: ContractState = "open"
    missing: str = ""  # why the last payment did not go, as told to the npc
    deal: str = ""  # the accepted deal, whose NPC intention is recorded in the chronicle

    def kills_done(self) -> bool:
        return all(k.done >= k.count for k in self.kill)

    def progress(self) -> str:
        parts = [f"убить: {k.label} — {min(k.done, k.count)} из {k.count}" for k in self.kill]
        parts += [f"принести: {g.text()}" for g in self.bring]
        return "; ".join(parts)

    def what(self) -> str:
        task = Task(tuple(self.kill), self.bring, self.due)
        return f"{', '.join(task.parts())} ({task.when()})"


class Stash:
    """What npc's keep but don't carry (gamedata/stash.json), less what the chronicle says they gave."""

    def __init__(self, chronicle: Chronicle, path: Path = STASH_FILE) -> None:
        self.chronicle = chronicle
        self.base: dict[str, dict[str, int]] = {}
        self.names: dict[str, str] = {}  # item -> its name in the game, as last read
        if path.is_file():
            data = json.loads(path.read_text(encoding="utf-8"))
            for npc, items in data.get("stash", {}).items():
                kept = self.base.setdefault(npc.upper(), {})
                for x in items:
                    kept[str(x["item"]).upper()] = kept.get(str(x["item"]).upper(), 0) + int(x["count"])

    def left(self, npc: str) -> dict[str, int]:
        npc = npc.upper()
        out = dict(self.base.get(npc, {}))
        if not out:
            return out
        for f in self.chronicle.facts():
            gives: list[dict[str, Any]] = []
            if f.get("fact") == "deal" and f.get("npc") == npc:
                gives = f.get("npc_gives", [])
            elif f.get("fact") in ("contract_paid", "npc_later_paid") and f.get("npc") == npc:
                gives = f.get("gives", [])
            for g in gives:
                if g.get("stash") and g.get("item") in out:
                    out[g["item"]] = max(0, out[g["item"]] - int(g["stash"]))
        return {i: c for i, c in out.items() if c > 0}

    async def name_all(self, link: GameLink, npc: str) -> None:
        """The game's names of what the npc keeps (once)."""
        unnamed = [i for i in self.base.get(npc.upper(), {}) if i not in self.names]
        if not unnamed:
            return
        state = await link.story_state(items=unnamed)
        for i in unnamed:
            self.names[i] = str(state.get("items", {}).get(i) or i)

    def text(self, npc: str) -> str:
        left = self.left(npc)
        return "; ".join(
            f"{gold_text(c) if i == GOLD else self.names.get(i, i)} ({i}) ×{c}" for i, c in left.items()
        )


def split(item: str, count: int, name: str, carried: int, kept: int) -> Give | None:
    """A give of `count`: from what the npc carries first, the rest from its stash; None if both fall
    short."""
    if carried >= count:
        return Give(item, count, name)
    if carried + kept >= count:
        return Give(item, count, name, stash=count - carried)
    return None


def carried(snap: Snapshot | None) -> dict[str, tuple[int, str]]:
    """item -> (how many the npc carries, not worn; its name)."""
    if snap is None:
        return {}
    return {i.item: (i.count, i.name) for i in snap.npc.inventory if not i.equipped and i.count > 0}


class Contracts:
    """The tasks npc's gave the hero: a TurnExtension (each npc sees its own; a done one is paid before the
    npc's next turn), what the game says the hero killed, and their days."""

    def __init__(
        self,
        link: GameLink,
        dialogue: DialogueManager,
        chronicle: Chronicle,
        stash: Stash,
        *,
        journal: Journal | None = None,
    ) -> None:
        self.link = link
        self.dialogue = dialogue
        self.chronicle = chronicle
        self.stash = stash
        self.journal = journal or dialogue.journal
        self.roster: Callable[[], Mapping[str, RosterEntry]] = dict
        self.contracts: dict[str, Contract] = {}  # by id, of the save branch
        self._ids = itertools.count(1)
        self._paying: dict[str, asyncio.Task[bool]] = {}
        self._background: set[asyncio.Task[Any]] = set()
        link.listeners.append(self._on_message)
        self._load()

    # --- the conversation (TurnExtension) --------------------------------------------------------------

    async def prepare(self, npc: str) -> None:
        npc = npc.upper()
        if self.stash.base.get(npc):
            await self.stash.name_all(self.link, npc)
        # a done task is paid before the npc speaks; one only to bring, as soon as he has it with him
        for c in self.of(npc):
            if c.state == "done" or (c.state == "open" and not c.kill):
                task = self._paying.get(c.id)
                if task is None or task.done():
                    task = self._paying[c.id] = asyncio.get_running_loop().create_task(self.pay(c))
                # the turn's wait may run out, the payment may not stop half way
                await asyncio.shield(task)

    def situation(self, npc: str) -> str | None:
        npc = npc.upper()
        lines = []
        if self.stash.left(npc):
            lines.append(
                "Твой тайник — вещи, которые ты держишь не при себе, но в сделке можешь отдать так же, "
                "как из "
                f"инвентаря (npc_gives по идентификатору): {self.stash.text(npc)}."
            )
        mine = [c for c in self.of(npc) if c.state in ("open", "done", "broken", "void", "unpaid")]
        if mine:
            lines.append("Твои поручения герою (уговор заключён; награда обещана за исполнение):")
            for c in mine:
                state = {
                    "open": "",
                    "done": " — исполнено, награда ждёт его",
                    "broken": " — срок прошёл, он не сдержал слово",
                    "void": " — исполнить уже нельзя: цель погибла не от его руки",
                    "unpaid": " — исполнено, ты не заплатил",
                }[c.state]
                why = f" ({c.missing})" if c.missing else ""
                lines.append(
                    f"- {c.id}: {c.progress()}; срок: {Task(due=c.due).when()}; "
                    f"награда: {c.reward.text()}{state}{why}"
                )
                if c.state == "unpaid":
                    lines.append("[Герой выполнил поручение и ждёт платы; ты решил не платить]")
        return "\n".join(lines) or None

    all_tools: tuple[Tool, ...] = ()

    def tools(self, npc: str) -> list[Tool]:
        return []

    async def handle(
        self, dialogue: DialogueManager, conv: Conversation, call: ToolCall, snap: Snapshot | None
    ) -> ToolOutcome | None:
        return None

    def of(self, npc: str) -> list[Contract]:
        return [c for c in self.contracts.values() if c.npc == npc.upper()]

    # --- a deal with a task is made ---------------------------------------------------------------------

    def add(self, deal: str, npc: str, name: str, task: Task, reward: Reward, day: int | None) -> Contract:
        cid = f"C{next(self._ids)}"
        while cid in self.contracts:
            cid = f"C{next(self._ids)}"
        c = Contract(
            cid,
            npc.upper(),
            name,
            [Kill(**k.fact()) for k in task.kill],
            task.bring,
            task.due,
            reward,
            deal=deal,
        )
        self.contracts[cid] = c
        self.chronicle.record(
            {
                "fact": "contract",
                "id": cid,
                "deal": deal,
                "npc": c.npc,
                "name": name,
                "kill": [k.fact() for k in c.kill],
                "bring": [g.fact() for g in c.bring],
                "due": c.due,
                "reward": reward.fact(),
                **({"day": day} if day is not None else {}),
            }
        )
        self.journal.add("contract", npc=c.npc, name=name, contract=cid, what=c.what(), reward=reward.text())
        return c

    # --- what the hero does ---------------------------------------------------------------------------

    def on_kill(self, msg: HeroKill) -> None:
        for c in list(self.contracts.values()):
            if c.state != "open":
                continue
            for i, k in enumerate(c.kill):
                if k.done < k.count and k.matches(msg):
                    k.done += 1
                    self.chronicle.record(
                        {
                            "fact": "contract_kill",
                            "contract": c.id,
                            "line": i,
                            "victim": msg.npc,
                            "name": msg.name,
                        }
                    )
                    break
            else:
                continue
            if c.kill and c.kills_done():
                c.state = "done"
                self.chronicle.record({"fact": "contract_done", "contract": c.id, "npc": c.npc})
                self.journal.add("contract_done", npc=c.npc, name=c.name, contract=c.id)
                more = f": осталось принести {', '.join(g.text() for g in c.bring)}" if c.bring else ""
                self._spawn(self._notice(f"Поручение исполнено ({c.name}) — вернись за наградой{more}"))

    def on_died(self, msg: NpcDied) -> None:
        """One to be killed died by another hand: the task can't be done."""
        if msg.killer == "PLAYER":
            return
        for c in self.contracts.values():
            if c.state == "open" and any(
                k.guild is None and k.target == msg.npc.upper() and k.done < k.count for k in c.kill
            ):
                c.state = "void"
                self.chronicle.record(
                    {"fact": "contract_void", "contract": c.id, "npc": c.npc, "victim": msg.npc}
                )
                self.dialogue.remember(
                    c.npc,
                    c.name,
                    f"{msg.name or msg.npc} погиб не от руки героя: "
                    f"твоё поручение {c.id} исполнить уже нельзя",
                )

    # --- the reward -------------------------------------------------------------------------------------

    async def pay(self, c: Contract) -> bool:
        """The reward for a done task, and what the hero brings: all or nothing (the deal's check)."""
        if c.state == "paid":
            return True
        if c.state == "unpaid":
            return False
        if not keeps(self.chronicle, c.deal, "contract"):
            if not c.kills_done():
                return False
            if c.bring:
                try:
                    amounts = await self.link.story_eval(
                        c.npc,
                        [["call", "NPC_HASITEMS", ["inst", "OTHER"], ["sym", g.item]] for g in c.bring],
                    )
                except (GameError, TimeoutError) as err:
                    log.info("contract %s: cannot check the brought items: %s", c.id, err)
                    return False
                if len(amounts) != len(c.bring) or any(
                    amount is None or int(amount) < g.count
                    for g, amount in zip(c.bring, amounts, strict=True)
                ):
                    return self._unpaid(c, f"он ещё не принёс: {', '.join(g.text() for g in c.bring)}")
            c.state, c.missing = "unpaid", ""
            breach(
                self.chronicle,
                c.npc,
                c.deal,
                "unpaid",
                contract=c.id,
                npc_name=c.name,
                what=c.what(),
                reward=c.reward.text(),
                witnesses=hearers(self.dialogue, c.npc, self.roster()),
                **await _day(self.link),
            )
            return False
        have = carried(self.link.snapshots.get(c.npc))
        kept = self.stash.left(c.npc)
        gives = []
        for g in c.reward.gives:
            count, name = have.get(g.item, (0, g.name))
            got = split(g.item, g.count, name or g.name, count, kept.get(g.item, 0))
            if got is None:
                return self._unpaid(c, f"тебе нечем заплатить: нет {g.text()}")
            gives.append(got)
        day = await _day(self.link)
        effects = [t.effect() for t in c.reward.teach]
        effects += [g.effect("self") for g in c.bring]
        for g in gives:
            effects += g.effects("hero")
        if c.reward.xp:
            effects.append({"do": "xp", "amount": c.reward.xp})
        if effects:
            try:
                await self.link.story_effect(c.npc, effects, check=True)
            except (GameError, TimeoutError) as err:
                error = str(err)
                if "missing: hero LP" in error:
                    return self._unpaid(
                        c, "у героя не хватает очков обучения на то, чему ты обещал его научить"
                    )
                if "missing: hero" in error:
                    return self._unpaid(c, f"он ещё не принёс: {', '.join(g.text() for g in c.bring)}")
                if "missing: npc" in error:
                    return self._unpaid(c, "тебе нечем заплатить: у тебя уже нет того, что ты обещал")
                log.info("contract %s: %s", c.id, error)
                return False
        c.state, c.missing = "paid", ""
        self.chronicle.record(
            {
                "fact": "contract_paid",
                "contract": c.id,
                "npc": c.npc,
                "gives": [g.fact() for g in gives],
                "bring": [g.fact() for g in c.bring],
                "xp": c.reward.xp,
                "teach": [t.fact() for t in c.reward.teach],
                **day,
            }
        )
        self.journal.add("contract_paid", npc=c.npc, name=c.name, contract=c.id, reward=c.reward.text())
        log.info("contract %s with %s paid: %s", c.id, c.npc, c.reward.text())
        brought = f" Он принёс: {', '.join(g.text() for g in c.bring)}." if c.bring else ""
        self.dialogue.note(
            c.npc,
            f"Герой исполнил твоё поручение {c.id} ({c.what()}).{brought} Ты, как уговорились, отдал ему "
            f"награду: {c.reward.text()}",
        )
        await self._notice("Награда получена")
        return True

    def _unpaid(self, c: Contract, why: str) -> bool:
        if why != c.missing:
            c.missing = why
            log.info("contract %s not paid: %s", c.id, why)
        return False

    # --- the days ---------------------------------------------------------------------------------------

    def overdue(self, day: int) -> list[Contract]:
        """The tasks past their day as of `day`, not done: broken words."""
        out = []
        for c in self.contracts.values():
            if c.state != "open" or c.due is None or day <= c.due:
                continue
            if c.kill and c.kills_done():
                continue  # done, only the things are not brought yet: paid when he comes
            c.state = "broken"
            out.append(c)
            self.chronicle.record(
                {
                    "fact": "broken",
                    "contract": c.id,
                    "npc": c.npc,
                    "name": c.name,
                    "what": c.what(),
                    "due": c.due,
                    "day": day,
                }
            )
            self.journal.add("contract_broken", npc=c.npc, name=c.name, contract=c.id)
            self.dialogue.remember(
                c.npc, c.name, f"Герой не исполнил твоё поручение в срок: {c.what()}. Он не сдержал слово"
            )
        return out

    # --- the save branch ----------------------------------------------------------------------------------

    def _on_message(self, msg: GameMessage) -> None:
        match msg:
            case HeroKill():
                self.on_kill(msg)
            case NpcDied():
                self.on_died(msg)
            case Session():
                self._load()

    def _load(self) -> None:
        self.contracts.clear()
        for f in self.chronicle.facts():
            match f.get("fact"):
                case "contract":
                    kill = [Kill(**k) for k in f.get("kill", [])]
                    bring = tuple(
                        Give(g["item"], int(g["count"]), g.get("name") or g["item"])
                        for g in f.get("bring", [])
                    )
                    self.contracts[f["id"]] = Contract(
                        f["id"],
                        f["npc"],
                        f.get("name") or f["npc"],
                        kill,
                        bring,
                        f.get("due"),
                        Reward.of(f.get("reward", {})),
                        deal=f.get("deal", ""),
                    )
                case "contract_kill" if (c := self.contracts.get(f.get("contract", ""))) and f.get(
                    "line", -1
                ) < len(c.kill):
                    c.kill[f["line"]].done += 1
                case "contract_done" if c := self.contracts.get(f.get("contract", "")):
                    c.state = "done"
                case "contract_paid" if c := self.contracts.get(f.get("contract", "")):
                    c.state = "paid"
                case "contract_void" if c := self.contracts.get(f.get("contract", "")):
                    c.state = "void"
                case "broken" if c := self.contracts.get(f.get("contract", "")):
                    c.state = "broken"
                case "npc_broken" if f.get("how") == "unpaid" and (
                    c := self.contracts.get(f.get("contract", ""))
                ):
                    c.state, c.missing = "unpaid", ""
        numbers = [int(i[1:]) for i in self.contracts if i[1:].isdigit()]
        self._ids = itertools.count(max(numbers, default=0) + 1)

    async def _notice(self, text: str) -> None:
        try:
            await self.link.send(ChatStatusCmd(status="notice", detail=text))
        except GameError as err:
            log.info("contracts: %s", err)

    def _spawn(self, coro: Coroutine[Any, Any, Any]) -> None:
        task = asyncio.get_running_loop().create_task(coro)
        self._background.add(task)
        task.add_done_callback(self._background.discard)


async def _day(link: GameLink) -> dict[str, int]:
    try:
        return {"day": int((await link.story_state()).get("day", 0))}
    except (GameError, TimeoutError):
        return {}
