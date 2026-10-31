"""Deals with the hero (docs/npc-tools.md, wave 1): the npc offers terms, the player accepts them with a key.

The conversational model proposes (`propose_deal`) what each side gives, once the two have talked it
over. The game shows it in a window that does not take the controls: the hero keeps walking, talking and
fighting. Proposing again in the same conversation changes the terms in that window (a new version): the
two haggle in words. The hero declines with N (the npc hears of it and may offer again); the offer also
goes when the npc withdraws it (`withdraw_deal`) or the conversation is over.

The hero accepts the version he sees (Y). The game sends it only once he has all he gives and the terms
have not changed for a moment; here an older version is not carried out. Then everything changes hands
at once, through the scripts' own giving, all or nothing (`story_effect` with `check`), and it is a fact
of the chronicle.

The terms are what the code can carry out and check (step 2: deals in the story):
- gold and items, either way. The npc gives only what it has. What the hero has, the npc does not see (his
  purse is his): the window marks what he lacks. It asks for his things by the scripts' ids, and it can
  only guess them: a guessed id that is no item («ITWR_CAVALORNLETTER») is the thing of its own story
  (the story graph) of that kind the hero carries, when he carries just one: Cavalorn's letter is
  ITWR_SATURASFIRSTMESSAGE_ADDON_SEALED. Else it is told the things of its story;
- the npc forgives a lie of the hero's it knows of (`forgive`): trust.py no longer counts it against his
  word with it, and a storylet of amends for it at it (amends.py) is settled;
- the npc keeps silent about it (`silence`): it no longer passes the rumor on (social.py); whom it told
  already, knows;
- the hero gives later, by a day (`hero_later`): a debt. Paid back in a later deal (`repay`); a debt not paid
  by its day is a broken word (trust.py) and a rumor (social.py);
- past the inventory (contracts.py, docs/contracts.md): experience (`npc_xp`), teaching for the hero's
  learning points (`npc_teach`), the things the npc keeps elsewhere (its stash, in `npc_gives` like the rest);
- a task (`task`: whom to kill, what to bring, by a day or with none) and its reward (`reward`): the code
  pays it when the task is done, the next time the hero talks to the npc;
- the npc leads the hero to a place (`lead`, guide.py): once he says yes, it sets off and he goes after it;
- a meeting (`meet`, meetings.py): where, on which game day and hour, how long the npc waits; the hero gives
  his word to come, and not coming breaks it.
- a bandit's truce (`truce`, parley.py): its gang leaves the hero alone for the agreed game hours,
  unless he gives a new cause for violence.
- a bandit's secret (`secret`, bounty.py): the hero pays for a name or a hideout; the npc may then
  reveal it with `tell_secret`.
"""

from __future__ import annotations

import asyncio
import itertools
import logging
import uuid
from collections.abc import Callable, Collection, Coroutine, Mapping
from copy import deepcopy
from dataclasses import dataclass, field
from typing import TYPE_CHECKING, Any, Literal

from .chronicle import Chronicle, Fact
from .contracts import (
    GOLD,
    MAX_TASK_DAYS,
    TEACH_KINDS,
    Contracts,
    Give,
    Reward,
    Stash,
    Task,
    Teach,
    carried,
    parse_kills,
    parse_teach,
    split,
)
from .game import GameError, GameLink
from .gates import GateResultUnknown
from .journal import Journal
from .llm import Tool, ToolCall
from .npc_cheat import INTENT_LABELS, breach, decide, hearers, keeps
from .people_knowledge import rumor_subject
from .protocol import (
    ChatStatusCmd,
    DealAccept,
    DealDecline,
    DealHideCmd,
    DealShowCmd,
    DealTerm,
    GameMessage,
    Loading,
    RosterEntry,
    Session,
    Snapshot,
)
from .purse import Purse, meeting_words
from .turn import ToolOutcome

if TYPE_CHECKING:
    from .bounty import Bounty
    from .characters import Card
    from .crimes import Crimes
    from .dialogue import Conversation, DialogueManager
    from .gates import Gates
    from .guide import Guide, Way
    from .meetings import Meetings, MeetTerm
    from .parley import Parley

log = logging.getLogger(__name__)

PROPOSE_DEAL = "propose_deal"
WITHDRAW_DEAL = "withdraw_deal"
MAX_COUNT = 100_000  # a sanity bound, not a price limit: what is fair is the npc's to judge
MAX_TERMS = 6  # things on one side
MAX_VOUCH = 200
MAX_DAYS = 10  # the furthest a debt may be due
STORY_ITEMS = 8  # the things of its story an npc is told of, when it asked for one that is not
DEBTS_EVERY_S = 10.0  # how often the debts' days are checked
LIE_KINDS = ("found_out", "caught")

_GIVES = {
    "type": "array",
    "items": {
        "type": "object",
        "properties": {
            "item": {"type": "string", "description": "идентификатор предмета, золото — ITMI_GOLD"},
            "count": {"type": "integer", "description": "сколько, не меньше 1"},
        },
        "required": ["item", "count"],
        "additionalProperties": False,
    },
}
_IDS = {"type": "array", "items": {"type": "string"}}
_TEACH = {
    "type": "array",
    "items": {
        "type": "object",
        "properties": {
            "what": {"type": "string", "enum": list(TEACH_KINDS)},
            "arg": {"type": "string", "description": "константа скриптов; для circle — пусто"},
            "count": {"type": "integer", "description": "очки, проценты или номер круга; иначе 1"},
        },
        "required": ["what", "arg", "count"],
        "additionalProperties": False,
    },
}
_KILLS = {
    "type": "array",
    "items": {
        "type": "object",
        "properties": {
            "target": {
                "type": "string",
                "description": "кого: человек или существо — символ скриптов или имя; вид — гильдия GIL_…",
            },
            "count": {"type": "integer", "description": "сколько; для одного человека — 1"},
        },
        "required": ["target", "count"],
        "additionalProperties": False,
    },
}

_COVER_DESCRIPTION = (
    "Прикрыть героя, если спросят (cover): виденное тобой дело crime, следы за сегодня или вчера day "
    "(день из «Время: день N»), слова вместо них vouch (до 200 знаков; пусто — скажешь, что не видел). "
    "Хотя бы одно поле; только vouch — за сегодня. "
)
_TRUCE_DESCRIPTION = (
    "Только бандитам: перемирие с твоей шайкой (truce) на 1–24 игровых часа. Для откупа проси золото "
    "или вещи в hero_gives, а взамен предложи truce. Уговор действует, когда герой примет сделку. "
)
_GATE_DESCRIPTION = (
    "Пропуск через ворота (let_pass: true): только на своём посту, за золото героя сейчас; "
    "без других условий. Герой согласится — код одновременно возьмёт плату и разрешит проход. "
)
_SECRET_DESCRIPTION = (
    "Продать известную тебе тайну охоты на героя (secret): client_name — имя заказчика, client_place — "
    "его логово. Плату золотом сейчас проси в hero_gives, не ниже цены в «Охота на героя». "
    "После принятия сделки раскрой проданную тайну через tell_secret. "
)
_SECRETS = {"client_name": "имя заказчика", "client_place": "логово заказчика"}

PROPOSE_TOOL = Tool(
    PROPOSE_DEAL,
    "Предложить герою сделку. Герой увидит условия и примет их сам, если согласен, — тогда всё сразу "
    "перейдёт из рук в руки. Торгуетесь — вызови снова с новыми условиями, они заменят прежние. "
    "Отдать можешь только то, что есть в твоём инвентаре (он в обстановке); у героя проси предметы по "
    "идентификатору, золото — ITMI_GOLD. Не знаешь точного — пиши догадку того же вида (ITWR_… письма и "
    "книги, ITMI_… прочие вещи): вещь из твоих дел, которая у него с собой, код подставит сам. Сколько у "
    "него с собой, ты не знаешь, но сделка сама тебя защищает: если у него нет того, что он отдаёт, она "
    "не состоится — осматривать вещь заранее не нужно. "
    "Кроме вещей: простить ему ложь или дело против тебя, о котором ты знаешь (forgive), пообещать "
    "молчать о лжи или деле, которое видел сам (silence). " + _COVER_DESCRIPTION + "Дать "
    "в долг — он отдаст позже, к сроку (hero_later), принять возврат его долга тебе (repay); id лжи и долгов "
    "— в обстановке. Пообещать свои вещи потом — npc_later: items и day, с какого дня отдашь, "
    f"не дальше {MAX_DAYS} дней. День — как в «Время: день N». "
    "Украденное у тебя тоже можно вернуть через repay по id дела crime:…: впиши в "
    "hero_gives украденные вещи целиком или оговорённую цену золотом, тогда простишь это дело. "
    "Вещи из твоего тайника (если он есть, он в обстановке) отдаёшь так же, через npc_gives. "
    "Ещё можешь дать опыт (npc_xp) и научить (npc_teach) за его очки обучения, как учитель в игре: "
    "attribute — ATR_STRENGTH сила, ATR_DEXTERITY ловкость, ATR_MANA_MAX мана, count — очки; fight — "
    "NPC_TALENT_1H, NPC_TALENT_2H, NPC_TALENT_BOW, NPC_TALENT_CROSSBOW, count — проценты; thief — "
    "NPC_TALENT_PICKLOCK, NPC_TALENT_SNEAK, NPC_TALENT_PICKPOCKET, NPC_TALENT_ACROBAT; circle — круг магии, "
    "count 1–6; rune — SPL_…; alchemy — POTION_…; smith — WEAPON_…; trophy — TROPHY_…. Не хватит у него "
    "очков — сделка не состоится. Поручение (task): кого убить — человека или существо по символу или "
    "имени, или вид по гильдии (GIL_WOLF волки, GIL_ORC орки, GIL_DRAGON драконы, GIL_SKELETON скелеты, "
    "GIL_ZOMBIE зомби, GIL_BDT бандиты, GIL_GOBBO гоблины, GIL_TROLL тролли, GIL_DEMON демоны и другие "
    "GIL_…), "
    "и что принести; days — срок в днях, 0 — без срока. reward — что ты отдашь, когда он исполнит: код "
    "сам выдаст награду, как только он с тобой заговорит, — так что обещай только то, что готов отдать. "
    "Отвести героя к месту (lead) — тоже сделка, хоть и даром: он согласится, и ты пойдёшь, а он за тобой. "
    "Место — id из ответа where или название; ведёшь в общие места и в свои, в чужой дом — до входа. "
    "Встреча (meet) — тоже сделка: где, день и час (день — как в «Время: день N»), сколько часов ты ждёшь; "
    "герой даёт слово прийти, и ты придёшь туда сам к этому часу. "
    + _TRUCE_DESCRIPTION
    + _GATE_DESCRIPTION
    + _SECRET_DESCRIPTION
    + "Сначала договоритесь о сути словами, потом предлагай и скажи, что предлагаешь. Ненужные списки — "
    "пустые, ненужные числа — 0, ненужные строки — пустые.",
    {
        "type": "object",
        "properties": {
            "npc_gives": {**_GIVES, "description": "что отдаёшь ты"},
            "hero_gives": {**_GIVES, "description": "что отдаёт герой сейчас"},
            "let_pass": {"type": "boolean", "description": "разрешить проход через ворота; иначе false"},
            "forgive": {
                **_IDS,
                "description": "id известной лжи или дела героя против тебя, которое прощаешь",
            },
            "silence": {**_IDS, "description": "id лжи или виденного тобой дела, о котором обещаешь молчать"},
            "cover": {
                "type": ["object", "null"],
                "properties": {
                    "crime": {"type": ["string", "null"], "description": "id виденного тобой дела"},
                    "day": {
                        "type": ["integer", "null"],
                        "description": "сегодня или вчера: день из «Время: день N»",
                    },
                    "vouch": {
                        "type": ["string", "null"],
                        "description": "что скажешь, до 200 знаков; пусто — не видел",
                    },
                },
                "required": ["crime", "day", "vouch"],
                "additionalProperties": False,
                "description": "прикрытие героя; ненужные поля — null; без прикрытия — null",
            },
            "hero_later": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {
                        "item": {
                            "type": "string",
                            "description": "идентификатор предмета, золото — ITMI_GOLD",
                        },
                        "count": {"type": "integer", "description": "сколько, не меньше 1"},
                        "days": {"type": "integer", "description": f"за сколько дней, от 1 до {MAX_DAYS}"},
                    },
                    "required": ["item", "count", "days"],
                    "additionalProperties": False,
                },
                "description": "что герой отдаст позже, к сроку: его долг тебе",
            },
            "npc_later": {
                "type": ["object", "null"],
                "properties": {
                    "items": {**_GIVES, "description": "что ты отдашь герою позже"},
                    "day": {"type": "integer", "description": "с какого дня игры отдашь, день от 1"},
                },
                "required": ["items", "day"],
                "additionalProperties": False,
                "description": f"твоя плата позже, не дальше {MAX_DAYS} дней; без неё — null",
            },
            "repay": {
                **_IDS,
                "description": "id долгов героя тебе, которые он возвращает сейчас; что он должен, войдёт в "
                "сделку само — не добавляй это в hero_gives ещё раз. Для кражи — id crime:…; украденное "
                "или оговорённую цену золотом впиши в hero_gives, тогда простишь дело",
            },
            "npc_xp": {"type": "integer", "description": "опыт, который ты даёшь герою сейчас; 0 — нет"},
            "npc_teach": {**_TEACH, "description": "чему ты учишь его сейчас, за его очки обучения"},
            "task": {
                "type": "object",
                "properties": {
                    "kill": {**_KILLS, "description": "кого ему убить"},
                    "bring": {**_GIVES, "description": "что ему принести тебе"},
                    "days": {"type": "integer", "description": "срок в днях; 0 — без срока"},
                },
                "required": ["kill", "bring", "days"],
                "additionalProperties": False,
                "description": "поручение герою; пустые списки — без поручения",
            },
            "reward": {
                "type": "object",
                "properties": {
                    "npc_gives": {**_GIVES, "description": "что ты отдашь"},
                    "xp": {"type": "integer", "description": "опыт; 0 — нет"},
                    "teach": {**_TEACH, "description": "чему научишь"},
                },
                "required": ["npc_gives", "xp", "teach"],
                "additionalProperties": False,
                "description": "награда, когда он исполнит поручение; без поручения — пустая",
            },
            "lead": {
                "type": "string",
                "description": "куда ты отведёшь героя: id места или его название; пусто — никуда",
            },
            "meet": {
                "type": "object",
                "properties": {
                    "where": {
                        "type": "string",
                        "description": "id места или его название; пусто — без встречи",
                    },
                    "day": {"type": "integer", "description": "день игры, как в «Время: день N»"},
                    "hour": {"type": "integer", "description": "час, 0–23"},
                    "minute": {"type": "integer", "description": "минута, 0–59"},
                    "hours": {"type": "integer", "description": "сколько часов ты ждёшь там, 1–4"},
                },
                "required": ["where", "day", "hour", "minute", "hours"],
                "additionalProperties": False,
                "description": "встреча с героем; без встречи — where пустое, числа 0",
            },
            "truce": {
                "type": ["object", "null"],
                "properties": {"hours": {"type": "integer", "minimum": 1, "maximum": 24}},
                "required": ["hours"],
                "additionalProperties": False,
                "description": "только бандитам: перемирие с твоей шайкой на 1–24 игровых часа; иначе null",
            },
            "secret": {
                "type": "string",
                "enum": ["", *_SECRETS],
                "description": "тайна, которую продаёшь; пусто — без тайны",
            },
        },
        "required": [
            "npc_gives",
            "hero_gives",
            "let_pass",
            "forgive",
            "silence",
            "cover",
            "hero_later",
            "npc_later",
            "repay",
            "npc_xp",
            "npc_teach",
            "task",
            "reward",
            "lead",
            "meet",
            "truce",
            "secret",
        ],
        "additionalProperties": False,
    },
)

_NO_COVER_PARAMETERS = deepcopy(PROPOSE_TOOL.parameters)
del _NO_COVER_PARAMETERS["properties"]["cover"]
_NO_COVER_PARAMETERS["required"].remove("cover")
NO_COVER_TOOL = Tool(
    PROPOSE_DEAL, PROPOSE_TOOL.description.replace(_COVER_DESCRIPTION, ""), _NO_COVER_PARAMETERS
)

_NO_GATES_PARAMETERS = deepcopy(PROPOSE_TOOL.parameters)
del _NO_GATES_PARAMETERS["properties"]["let_pass"]
_NO_GATES_PARAMETERS["required"].remove("let_pass")
NO_GATES_TOOL = Tool(
    PROPOSE_DEAL, PROPOSE_TOOL.description.replace(_GATE_DESCRIPTION, ""), _NO_GATES_PARAMETERS
)
_NO_COVER_NO_GATES_PARAMETERS = deepcopy(_NO_GATES_PARAMETERS)
del _NO_COVER_NO_GATES_PARAMETERS["properties"]["cover"]
_NO_COVER_NO_GATES_PARAMETERS["required"].remove("cover")
NO_COVER_NO_GATES_TOOL = Tool(
    PROPOSE_DEAL, NO_GATES_TOOL.description.replace(_COVER_DESCRIPTION, ""), _NO_COVER_NO_GATES_PARAMETERS
)

_NO_TRUCE_TOOLS = {}
for _tool in (PROPOSE_TOOL, NO_COVER_TOOL, NO_GATES_TOOL, NO_COVER_NO_GATES_TOOL):
    _parameters = deepcopy(_tool.parameters)
    del _parameters["properties"]["truce"]
    _parameters["required"].remove("truce")
    _NO_TRUCE_TOOLS[_tool.description] = Tool(
        PROPOSE_DEAL, _tool.description.replace(_TRUCE_DESCRIPTION, ""), _parameters
    )

_NO_SECRET_TOOLS = {}
for _tool in (
    PROPOSE_TOOL, NO_COVER_TOOL, NO_GATES_TOOL, NO_COVER_NO_GATES_TOOL, *_NO_TRUCE_TOOLS.values()
):
    _parameters = deepcopy(_tool.parameters)
    del _parameters["properties"]["secret"]
    _parameters["required"].remove("secret")
    _NO_SECRET_TOOLS[_tool.description] = Tool(
        PROPOSE_DEAL, _tool.description.replace(_SECRET_DESCRIPTION, ""), _parameters
    )

WITHDRAW_TOOL = Tool(
    WITHDRAW_DEAL,
    "Отозвать своё предложение сделки: передумал, или разговор ушёл в сторону.",
    {"type": "object", "properties": {}, "required": [], "additionalProperties": False},
)


# how the conversation ended, for the npc's memory of an offer not taken
ENDED = {
    "npc": "Ты закончил разговор, а герой так и не согласился",
    "switched": "Герой отвернулся к другому, не приняв его",
    "gone": "Разговор оборвался, герой не согласился",
    "walked_away": "Герой ушёл, не приняв его",
}


@dataclass(frozen=True)
class LieRef:
    """A lie of the hero's an npc knows of: lied to it, or heard of (the key of amends.Lie, social.Rumor)."""

    key: str  # kind:about:quest
    title: str
    about: str  # the one lied to
    about_name: str
    said: bool = False  # caught: the title is his own words, seen to be false (sightings.py), not a quest

    def words(self, npc: str) -> str:
        """What the hero did, to the npc's face."""
        whom = "тебя" if self.about == npc else f"человека по имени {self.about_name}"
        if self.key.startswith("found_out:"):
            return f"обманул {whom}: взял награду за «{self.title}», а дела не сделал"
        if self.said:
            return f"обманул {whom}: сказал «{self.title}», а это неправда"
        return f"обманул {whom}: уверял, что сделал «{self.title}», а это неправда"


@dataclass(frozen=True)
class CrimeRef:
    """A chronicle deed the npc really knows, referenced like a lie in the same deal window."""

    key: str  # crime:<id>, never a model-created deed
    title: str
    victim: str
    kind: str
    degree: str
    items: tuple[tuple[str, int], ...] = ()

    @property
    def crime(self) -> str:
        return self.key.removeprefix("crime:")


def _amends(ref: LieRef | CrimeRef, *, silence: bool = False, window: bool = False) -> str:
    if isinstance(ref, CrimeRef):
        what = f"{'деле' if silence else 'дело'} «{ref.title}»"
    else:
        what = f"{'лжи' if silence else 'ложь'} про «{ref.title}»"
    if silence:
        return f"Молчит о {what}" if window else f"обещаешь молчать о {what}"
    return f"Прощает {what}" if window else f"прощаешь ему {what}"


@dataclass(frozen=True)
class Cover:
    crime: CrimeRef | None = None
    day: int | None = None  # internal game day, 0-based
    vouch: str | None = None

    def text(self) -> str:
        parts = []
        if self.crime is not None:
            parts.append(f"дело «{self.crime.title}»")
        if self.day is not None:
            parts.append(f"день {self.day + 1}")
        parts.append(f"скажет «{self.vouch}»" if self.vouch else "скажет, что не видел")
        return "; ".join(parts)

    def fact(self) -> dict[str, Any]:
        return {
            **({"crime": self.crime.crime} if self.crime is not None else {}),
            **({"day": self.day} if self.day is not None else {}),
            **({"vouch": self.vouch} if self.vouch is not None else {}),
        }


@dataclass(frozen=True)
class Later:
    """What the hero gives later: a debt, due by the end of a game day (0-based, as the game counts)."""

    give: Give
    due: int

    def text(self) -> str:
        return f"{self.give.text()} — до конца дня {self.due + 1}"


@dataclass
class NpcLater:
    gives: tuple[Give, ...]
    day: int
    deal: str = ""
    npc: str = ""
    name: str = ""
    paid: bool = False
    missing: str = ""

    def text(self) -> str:
        return f"{_list(self.gives)}, с дня {self.day + 1}"

    def fact(self) -> dict[str, Any]:
        return {"items": [g.fact() for g in self.gives], "day": self.day}


@dataclass
class Debt:
    id: str
    npc: str
    name: str
    give: Give
    due: int
    paid: bool = False
    broken: bool = False

    def what(self) -> str:
        return f"{self.give.text()} до конца дня {self.due + 1}"


DealState = Literal["open", "accepting", "unknown", "done", "withdrawn", "declined", "expired"]


@dataclass
class Deal:
    id: str
    npc: str
    name: str
    version: int
    npc_gives: tuple[Give, ...]
    hero_gives: tuple[Give, ...]
    forgive: tuple[LieRef | CrimeRef, ...] = ()
    silence: tuple[LieRef | CrimeRef, ...] = ()
    later: tuple[Later, ...] = ()
    repay: tuple[Debt, ...] = ()
    xp: int = 0  # the npc gives experience now
    teach: tuple[Teach, ...] = ()  # the npc teaches now, for the hero's learning points
    task: Task | None = None  # the hero takes on a task...
    reward: Reward = field(default_factory=Reward)  # ...the npc pays this when it is done (contracts.py)
    lead: Way | None = None  # it leads him there (guide.py)
    meet: MeetTerm | None = None  # they meet there and then; his word to come (meetings.py)
    crime_repay: tuple[CrimeRef, ...] = ()  # paid amends through hero_gives, no second transfer
    cover: Cover | None = None
    state: DealState = "open"
    npc_later: NpcLater | None = None
    truce: int | None = None  # agreed game hours, only bandits (parley.py)
    let_pass: bool = False
    secret: str = ""

    def same(self, other: Deal) -> bool:
        return self.terms() == other.terms()

    def gold(self) -> int:
        """What the hero pays in gold now."""
        return sum(g.count for g in (*self.hero_gives, *(d.give for d in self.repay)) if g.item == GOLD)

    def terms(self) -> str:
        """In the npc's words: what it gives and what the hero gives."""
        mine = [_amends(ref) for ref in self.forgive]
        mine += [_amends(ref, silence=True) for ref in self.silence]
        if self.cover:
            mine.append(f"обещаешь прикрыть его: {self.cover.text()}")
        if self.npc_later:
            mine.append(f"отдашь потом: {self.npc_later.text()}")
        if self.let_pass:
            mine.append("пропускаешь через ворота")
        mine += [f"прощаешь дело «{ref.title}» после возмещения" for ref in self.crime_repay]
        if self.xp:
            mine.append(f"даёшь ему опыт +{self.xp}")
        mine += [f"учишь его: {t.text()}" for t in self.teach]
        if self.task:
            mine.append(f"когда он исполнит поручение, отдашь: {self.reward.text()}")
        if self.lead:
            mine.append(f"ведёшь его — {self.lead.title()}")
        if self.truce is not None:
            mine.append(f"перемирие с твоей шайкой на {self.truce} ч.")
        if self.secret:
            mine.append(f"раскроешь {_SECRETS[self.secret]}")
        his = [f"возвращает долг — {d.give.text()}" for d in self.repay]
        his += [f"отдаст позже {x.give.text()} до конца дня {x.due + 1}" for x in self.later]
        if self.task:
            his.append(f"берётся за поручение — {self.task.text()}")
        if self.meet:
            his.append(f"обещает прийти на встречу — {self.meet.text()}")
        return f"ты {_side('отдаёшь', self.npc_gives, mine)}; герой {_side('отдаёт', self.hero_gives, his)}"

    def show(self) -> DealShowCmd:
        mine = [g.term() for g in self.npc_gives]
        mine += [DealTerm(item="", count=0, text=_amends(ref, window=True)) for ref in self.forgive]
        mine += [
            DealTerm(item="", count=0, text=_amends(ref, silence=True, window=True)) for ref in self.silence
        ]
        if self.cover:
            mine.append(DealTerm(item="", count=0, text=f"прикрыть тебя: {self.cover.text()}"))
        if self.npc_later:
            mine.append(DealTerm(item="", count=0, text=f"отдаст потом: {self.npc_later.text()}"))
        if self.let_pass:
            mine.append(DealTerm(item="", count=0, text="Пропускает через ворота"))
        mine += [
            DealTerm(item="", count=0, text=f"Прощает дело после возмещения: {ref.title}")
            for ref in self.crime_repay
        ]
        if self.xp:
            mine.append(DealTerm(item="", count=0, text=f"Опыт +{self.xp}"))
        mine += [DealTerm(item="", count=0, text=f"Учит: {t.text()}") for t in self.teach]
        if self.task:
            mine += [DealTerm(item="", count=0, text=f"За поручение: {x}") for x in self.reward.parts()]
        if self.lead:
            mine.append(DealTerm(item="", count=0, text=f"Проводит: {self.lead.title()}"))
        if self.truce is not None:
            mine.append(DealTerm(item="", count=0, text=f"Перемирие с шайкой на {self.truce} ч."))
        if self.secret:
            mine.append(DealTerm(item="", count=0, text=f"Раскроет: {_SECRETS[self.secret]}"))
        his = [g.term() for g in self.hero_gives]
        his += [d.give.term(f"Долг: {d.give.text()}") for d in self.repay]
        his += [DealTerm(item="", count=0, text=f"Позже: {x.text()}") for x in self.later]
        if self.task:
            his += [DealTerm(item="", count=0, text=f"Поручение: {x}") for x in self.task.parts()]
            his.append(DealTerm(item="", count=0, text=f"Срок: {self.task.when()}"))
        if self.meet:
            his.append(DealTerm(item="", count=0, text=f"Встреча: {self.meet.text()}"))
        # the game's codepage (cp1251) has no "×": the window would show "?"
        mine = [t.model_copy(update={"text": t.text.replace("×", "x")}) for t in mine]
        his = [t.model_copy(update={"text": t.text.replace("×", "x")}) for t in his]
        return DealShowCmd(deal=self.id, version=self.version, npc=self.npc, npc_gives=mine, hero_gives=his)

    def effects(self) -> list[dict[str, Any]]:
        """Teaching first (the check has counted his learning points), the hero's things, the npc's (out of
        its stash first), experience last."""
        out = [t.effect() for t in self.teach]
        out += [g.effect("self") for g in self.hero_gives] + [d.give.effect("self") for d in self.repay]
        for g in self.npc_gives:
            out += g.effects("hero")
        if self.xp:
            out.append({"do": "xp", "amount": self.xp})
        return out


class Deals:
    """The deals npc's offer the hero in conversation: a TurnExtension, what the window answers, and the
    hero's debts."""

    def __init__(
        self,
        link: GameLink,
        dialogue: DialogueManager,
        chronicle: Chronicle,
        *,
        journal: Journal | None = None,
        names: Callable[[str], str | None] | None = None,
        things: Callable[[str], Collection[str]] | None = None,
        crimes: Crimes | None = None,
        cover: bool = True,
        cards: Mapping[str, Card] | None = None,
        cheat: bool = True,
        parley: Parley | None = None,
        gates: Gates | None = None,
        bounty: Bounty | None = None,
    ) -> None:
        self.link = link
        self.dialogue = dialogue
        self.chronicle = chronicle
        self.journal = journal or dialogue.journal
        self.names = names  # an npc's name -> its symbol, for whom a task is to kill (the character cards)
        self.things = things  # an npc -> the item ids of its story (StoryGraph.things_of), for a guessed id
        self.crimes = crimes
        self.cover = cover
        self.cards = dialogue.personas.cards if cards is None else cards
        self.cheat = cheat
        self.parley = parley
        self.gates = gates
        self.bounty = bounty
        self.chronicle.npc_cheat_enabled = cheat
        self.roster: Callable[[], Mapping[str, RosterEntry]] = dict
        self.stash = Stash(chronicle)
        self.purse = Purse(chronicle)
        # what the npc knows it keeps in its chests (chests.py): a thing it offers may be there
        self.in_chests: Callable[[str], dict[str, tuple[int, str]]] = lambda npc: {}
        # the tasks of the deals and their rewards: a TurnExtension of its own (cli.py)
        self.contracts = Contracts(link, dialogue, chronicle, self.stash, journal=self.journal)
        self.guide: Guide | None = None  # where it may lead the hero, and the way (guide.py)
        self.meetings: Meetings | None = None  # where and when it may meet him, and the meetings
        self.open: dict[str, Deal] = {}  # npc -> its offer, while the conversation lasts
        self.debts: dict[str, Debt] = {}  # by id, of the save branch
        self.npc_later: dict[str, NpcLater] = {}
        self._paying: dict[str, asyncio.Task[bool]] = {}
        self._world_generation = 0
        self.day: int | None = None  # the game day, as last read
        self.on_day: list[Callable[[int], None]] = []  # a new game day (concerns.py: a debt due today)
        self.on_reload: list[Callable[[], None]] = []  # the debts were read again (concerns.py)
        self._ids = itertools.count(1)
        self._background: set[asyncio.Task[Any]] = set()
        link.listeners.append(self._on_message)
        self._load()

    def _on_message(self, msg: GameMessage) -> None:
        match msg:
            case DealAccept():
                self._spawn(self.accept(msg.deal, msg.version))
            case DealDecline():
                self._spawn(self.decline(msg.deal, msg.version))
            case Loading():
                self._cancel_payments()
                self.npc_later.clear()
                self.open.clear()  # the game drops its window with the world
                self.day = None  # the world loaded may be of another day
            case Session():
                self._load()

    # --- the conversation (TurnExtension) ------------------------------------------------------

    async def prepare(self, npc: str) -> None:
        generation = self._world_generation
        pending = [x for x in self.npc_later.values() if x.npc == npc.upper() and not x.paid]
        if not pending:
            return
        day = await _day(self.link)
        if generation != self._world_generation or "day" not in day:
            return
        self._set_day(day["day"])
        for later in pending:
            if generation != self._world_generation:
                return
            if day["day"] < later.day:
                continue
            task = self._paying.get(later.deal)
            if task is None or task.done():
                task = self._paying[later.deal] = asyncio.create_task(
                    self.pay_later(later, day, generation=generation)
                )
            try:
                await asyncio.shield(task)
            except asyncio.CancelledError:
                if generation != self._world_generation:
                    return
                raise

    def situation(self, npc: str) -> str | None:
        npc = npc.upper()
        lines = []
        if self.cheat:
            for intent in self.chronicle.facts("npc_intent"):
                if intent.get("npc") == npc and not intent.get("keeps", True):
                    terms = ", ".join(INTENT_LABELS.get(h, h) for h in intent.get("how", ()))
                    lines.append(f"По сделке {intent['deal']} ты не собираешься выполнять: {terms}.")
        for later in self.npc_later.values():
            if later.npc != npc or later.paid:
                continue
            if not keeps(self.chronicle, later.deal, "npc_later") and any(
                f.get("deal") == later.deal and f.get("how") == "unpaid"
                for f in self.chronicle.facts("npc_broken")
            ):
                lines.append("[Герой ждёт обещанные вещи или деньги; ты решил не отдавать]")
            else:
                missing = f"; {later.missing}" if later.missing else ""
                lines.append(f"Ты обещал герою отдать потом: {later.text()} ({later.deal}){missing}.")
        if (deal := self.open.get(npc)) is not None:
            if deal.state == "unknown":
                lines.append("Исход принятой сделки за проход неизвестен. Повторно брать плату нельзя.")
            else:
                lines.append(f"Твоё предложение герою ждёт его согласия: {deal.terms()}.")
        known, forgiven, silent = self.lies_of(npc)
        open_lies = {k: lie for k, lie in known.items() if k not in forgiven}
        if open_lies:
            lines.append(
                "Ложь героя, о которой ты знаешь (в сделке её можно простить — forgive, "
                "или пообещать молчать — silence, по id):"
            )
            for k, lie in open_lies.items():
                hushed = "; ты уже обещал о ней молчать" if k in silent else ""
                lines.append(f"- {k}: он {lie.words(npc)}{hushed}")
        crimes, forgiven_crimes, silent_crimes = self.crimes_of(npc)
        open_crimes = {k: ref for k, ref in crimes.items() if k not in forgiven_crimes}
        if open_crimes:
            lines.append("Дела героя для сделки (используй только эти id):")
            for key, ref in open_crimes.items():
                terms = []
                if ref.victim == npc:
                    terms.append("простить — forgive")
                    if ref.kind == "theft" and ref.items:
                        terms.append("возместить — repay; вещи или оговорённая цена — в hero_gives")
                if ref.degree == "seen" and key not in silent_crimes:
                    terms.append("молчать — silence")
                if ref.degree == "seen" and self.cover and self.crimes is not None:
                    terms.append("прикрыть — cover")
                if key in silent_crimes:
                    terms.append("ты уже обещал молчать")
                if terms:
                    lines.append(f"- {key}: {ref.title}; {'; '.join(terms)}")
        debts = [d for d in self.debts.values() if d.npc == npc and not d.paid]
        if debts:
            lines.append("Долги героя тебе (вернуть — repay в сделке, по id):")
            for d in debts:
                state = " — срок прошёл, он не сдержал слово" if d.broken else ""
                lines.append(f"- {d.id}: {d.what()}{state}")
        return "\n".join(lines) or None

    @property
    def all_tools(self) -> tuple[Tool, ...]:
        if self.gates is not None:
            # The offered schema depends on whether this npc is at its gate post.
            return (WITHDRAW_TOOL,)
        tool = NO_GATES_TOOL if self.cover and self.crimes is not None else NO_COVER_NO_GATES_TOOL
        if self.parley is None:
            tool = _NO_TRUCE_TOOLS[tool.description]
        if self.bounty is None:
            tool = _NO_SECRET_TOOLS[tool.description]
        return (tool, WITHDRAW_TOOL)

    def tools(self, npc: str) -> list[Tool]:
        if (deal := self.open.get(npc.upper())) is not None and deal.state == "unknown":
            return []
        if self.gates is not None and self.gates.on_post(npc):
            tool = PROPOSE_TOOL if self.cover and self.crimes is not None else NO_COVER_TOOL
        else:
            tool = NO_GATES_TOOL if self.cover and self.crimes is not None else NO_COVER_NO_GATES_TOOL
        if self.parley is None:
            tool = _NO_TRUCE_TOOLS[tool.description]
        if self.bounty is None:
            tool = _NO_SECRET_TOOLS[tool.description]
        return [tool, WITHDRAW_TOOL] if npc.upper() in self.open else [tool]

    async def handle(
        self, dialogue: DialogueManager, conv: Conversation, call: ToolCall, snap: Snapshot | None
    ) -> ToolOutcome | None:
        if call.name == PROPOSE_DEAL:
            return await self.propose(
                conv.npc, conv.persona.name, call.arguments, snap, hero_words=meeting_words(conv)
            )
        if call.name == WITHDRAW_DEAL:
            deal = self.open.get(conv.npc)
            if deal is None:
                return ToolOutcome("Отклонено: ты ничего не предлагал.", "rejected")
            if deal.state in ("accepting", "unknown"):
                return ToolOutcome(
                    "Отклонено: герой уже согласился, вещи переходят из рук в руки.", "rejected"
                )
            await self._close(deal, "withdrawn")
            return ToolOutcome("Предложение снято.", "accepted")
        return None

    async def propose(
        self,
        npc: str,
        name: str,
        args: dict[str, Any],
        snap: Snapshot | None,
        *,
        hero_words: Collection[str] = (),
    ) -> ToolOutcome:
        npc = npc.upper()
        current = self.open.get(npc)
        if current is not None and current.state in ("accepting", "unknown"):
            return ToolOutcome("Отклонено: герой уже согласился на прежние условия.", "rejected")
        truce, why = self._truce(npc, args.get("truce"))
        if why:
            return ToolOutcome(f"Отклонено: {why}", "rejected")
        let_pass = args.get("let_pass", False)
        if type(let_pass) is not bool:
            return ToolOutcome("Отклонено: let_pass — true или false.", "rejected")
        if let_pass and self.gates is None:
            return ToolOutcome("Отклонено: пропуск через ворота выключен.", "rejected")
        secret = args.get("secret", "")
        if not isinstance(secret, str) or (secret and secret not in _SECRETS):
            return ToolOutcome("Отклонено: secret — client_name, client_place или пустая строка.", "rejected")
        if secret and self.bounty is None:
            return ToolOutcome("Отклонено: продажа тайн выключена.", "rejected")
        npc_gives, why = self._npc_gives(npc, args.get("npc_gives"), snap)
        if npc_gives is None:
            return ToolOutcome(f"Отклонено: {why}", "rejected")
        xp, why = _parse_xp(args.get("npc_xp"))
        if xp is None:
            return ToolOutcome(f"Отклонено: {why}", "rejected")
        teach, why = await parse_teach(self.link, args.get("npc_teach"))
        if teach is None:
            return ToolOutcome(f"Отклонено: {why}", "rejected")
        task_raw = args.get("task") if isinstance(args.get("task"), dict) else {}
        reward_raw = args.get("reward") if isinstance(args.get("reward"), dict) else {}
        kills, why = await parse_kills(self.link, task_raw.get("kill"), self.names)
        if kills is None:
            return ToolOutcome(f"Отклонено: {why}", "rejected")
        bring, why = _parse(task_raw.get("bring"))
        if bring is None:
            return ToolOutcome(f"Отклонено: {why}", "rejected")
        days, why = _parse_days(task_raw.get("days"))
        if days is None:
            return ToolOutcome(f"Отклонено: {why}", "rejected")
        paid, why = self._npc_gives(npc, reward_raw.get("npc_gives"), snap)
        if paid is None:
            return ToolOutcome(f"Отклонено: награда — {why}", "rejected")
        paid_xp, why = _parse_xp(reward_raw.get("xp"))
        if paid_xp is None:
            return ToolOutcome(f"Отклонено: награда — {why}", "rejected")
        paid_teach, why = await parse_teach(self.link, reward_raw.get("teach"))
        if paid_teach is None:
            return ToolOutcome(f"Отклонено: награда — {why}", "rejected")
        # what it pays with is decided when it pays: what it carries then, the rest from its stash
        reward = Reward(tuple(Give(g.item, g.count, g.name) for g in paid), paid_xp, paid_teach)
        if reward and not (kills or bring):
            return ToolOutcome(
                "Отклонено: награда бывает только за поручение (task). Без поручения отдавай сразу — "
                "npc_gives, npc_xp, npc_teach.",
                "rejected",
            )
        forgive, silence, why = self._lies(npc, args.get("forgive"), args.get("silence"))
        if forgive is None or silence is None:
            return ToolOutcome(f"Отклонено: {why}", "rejected")
        repay, crime_repay, why = self._repay(npc, args.get("repay"))
        if repay is None:
            return ToolOutcome(f"Отклонено: {why}", "rejected")
        later_raw, why = _parse_later(args.get("hero_later"))
        if later_raw is None:
            return ToolOutcome(f"Отклонено: {why}", "rejected")
        npc_later_raw, why = _parse_npc_later(args.get("npc_later"))
        if why:
            return ToolOutcome(f"Отклонено: {why}", "rejected")
        wanted, why = _parse(args.get("hero_gives"))
        if wanted is None:
            return ToolOutcome(f"Отклонено: {why}", "rejected")
        later_ids = {i for i, _, _ in later_raw}
        asked = {*wanted, *bring, *later_ids, *(npc_later_raw[0] if npc_later_raw else ())}
        things, swaps, day, why = await self._items(npc, asked, set(wanted) - set(bring) - later_ids)
        if things is None:
            return ToolOutcome(f"Отклонено: {why}", "rejected")
        npc_later = None
        if npc_later_raw:
            items, due = npc_later_raw
            if not day <= due <= day + MAX_DAYS:
                return ToolOutcome(
                    f"Отклонено: npc_later — день от {day + 1} до {day + MAX_DAYS + 1}.", "rejected"
                )
            npc_later = NpcLater(tuple(Give(i, c, things[i]) for i, c in items.items()), due)
        cover, why = self._cover(npc, args.get("cover"), day)
        if why:
            return ToolOutcome(f"Отклонено: {why}", "rejected")
        wanted = _swapped(wanted, swaps)
        hero_gives = tuple(Give(i, c, things[i]) for i, c in wanted.items())
        if secret and (why := self.bounty.validate_sale(npc, secret, hero_gives, npc_gives)):
            return ToolOutcome(f"Отклонено: {why}", "rejected")
        if why := self._crime_payment(crime_repay, hero_gives):
            return ToolOutcome(f"Отклонено: {why}", "rejected")
        later = tuple(Later(Give(i, c, things[i]), day + d) for i, c, d in later_raw)
        task = None
        if kills or bring:
            brought = tuple(Give(i, c, things[i]) for i, c in bring.items())
            task = Task(kills, brought, day + days if days else None)
        lead, why = await self._lead(npc, args.get("lead"), snap)
        if why:
            return ToolOutcome(f"Отклонено: {why}", "rejected")
        meet, why = await self._meet(npc, args.get("meet"), snap)
        if why:
            return ToolOutcome(f"Отклонено: встреча — {why}", "rejected")
        given = (
            npc_gives,
            hero_gives,
            forgive,
            silence,
            later,
            repay,
            crime_repay,
            xp,
            teach,
            task,
            lead,
            meet,
            cover,
            npc_later,
            truce,
            secret,
        )
        if let_pass:
            if any(given[:1] + given[2:]):
                return ToolOutcome(
                    "Отклонено: пропуск через ворота — только за золото сейчас, без других условий.",
                    "rejected",
                )
            if not hero_gives or any(g.item != GOLD for g in hero_gives):
                return ToolOutcome("Отклонено: за проход герой платит золотом сейчас.", "rejected")
            try:
                why = await self.gates.bribe_error(npc, sum(g.count for g in hero_gives))
            except (GameError, TimeoutError) as err:
                why = f"не выходит проверить разрешение прохода: {err}"
            if why:
                return ToolOutcome(f"Отклонено: {why}", "rejected")
        if not any(given):
            return ToolOutcome("Отклонено: в сделке ничего нет.", "rejected")
        current = self.open.get(npc)  # he may have accepted the shown terms meanwhile
        if current is not None and current.state in ("accepting", "unknown"):
            return ToolOutcome("Отклонено: герой уже согласился на прежние условия.", "rejected")
        deal_id, version = (current.id, current.version + 1) if current else (f"D{next(self._ids)}", 1)
        deal = Deal(
            deal_id,
            npc,
            name,
            version,
            npc_gives,
            hero_gives,
            forgive,
            silence,
            later,
            repay,
            xp,
            teach,
            task,
            reward,
            lead,
            meet,
            crime_repay,
            cover,
            npc_later=npc_later,
            truce=truce,
            let_pass=let_pass,
            secret=secret,
        )
        if why := self.purse.error(deal, self.debts.values(), hero_words):
            return ToolOutcome(why, "rejected")
        if current is not None and current.same(deal):
            return ToolOutcome(f"Эти условия герой уже видит: {current.terms()}.", "accepted")
        try:
            await self.link.send(deal.show())
        except GameError as err:
            return ToolOutcome(f"Отклонено: {err}", "rejected")
        self.open[npc] = deal
        log.info("deal %s v%d with %s: %s", deal.id, deal.version, npc, deal.terms())
        terms = deal.terms()
        self.journal.add("deal_offer", npc=npc, name=name, deal=deal.id, version=deal.version, terms=terms)
        changed = "Условия изменились" if current is not None else "Герой видит твоё предложение"
        after = "Он примет его сам, если согласен; пока не принял, можно торговаться."
        if swaps:
            after += " " + " ".join(
                f"Предмета {guess} нет: вписано то, что герой держит из твоих дел, — {things[i]} ({i})."
                for guess, i in swaps.items()
            )
        twice = [g for g in hero_gives if any(d.give.item == g.item for d in repay)]
        if twice:  # the debt brings its own: what was added on top is more, not the debt again
            total = ", ".join(
                Give(
                    g.item, g.count + sum(d.give.count for d in repay if d.give.item == g.item), g.name
                ).text()
                for g in twice
            )
            after += f" Учти: долг уже включает своё, с hero_gives герой отдаёт всего {total}."
        return ToolOutcome(f"{changed}: {deal.terms()}. {after}", "accepted")

    def end(self, npc: str, why: str = "") -> None:
        """The conversation is over (`why` as TalkDesk tells it: npc, gone, walked_away, switched): an offer
        not taken goes."""
        if (deal := self.open.get(npc.upper())) is not None and deal.state == "open":
            self._spawn(self._close(deal, "expired", ENDED.get(why, ENDED["walked_away"])))

    # --- what the npc knows ----------------------------------------------------------------------

    def lies_of(self, npc: str) -> tuple[dict[str, LieRef], set[str], set[str]]:
        """The hero's lies the npc knows of (lied to it, or heard of), those it forgave, those it keeps
        silent about."""
        npc = npc.upper()
        known: dict[str, LieRef] = {}
        forgiven: set[str] = set()
        silent: set[str] = set()
        for f in self.chronicle.facts():
            if f.get("fact") == "rumor" and rumor_subject(f) != "PLAYER":
                continue
            match f.get("fact"):
                case "found_out" | "caught" if f.get("npc") == npc:
                    key = f"{f['fact']}:{npc}:{f.get('quest', '')}"
                    ref = LieRef(key, _title(f), npc, f.get("name") or npc, said=bool(f.get("said")))
                    known.setdefault(key, ref)
                case "rumor" if f.get("to") == npc and f.get("kind", "found_out") in LIE_KINDS:
                    about = f.get("about") or f.get("from", "")
                    key = f"{f.get('kind', 'found_out')}:{about}:{f.get('quest', '')}"
                    ref = LieRef(
                        key, _title(f), about, f.get("about_name") or about, said=bool(f.get("said"))
                    )
                    known.setdefault(key, ref)
                case "deal" if f.get("npc") == npc:
                    forgiven.update(f.get("forgive", []))
                    if keeps(self.chronicle, f.get("deal", ""), "silence"):
                        silent.update(f.get("silence", []))
                case "amends" if npc in (f.get("npc"), f.get("to")) and f.get("lie"):
                    forgiven.add(f["lie"])
        return known, forgiven, silent

    def crimes_of(self, npc: str) -> tuple[dict[str, CrimeRef], set[str], set[str]]:
        """Deal options come exclusively from this npc's certain chronicle knowledge."""
        known: dict[str, CrimeRef] = {}
        forgiven, silent = set(), set()
        if self.crimes is None:
            return known, forgiven, silent
        for knowledge in self.crimes.known(npc):
            if knowledge.degree not in {"seen", "heard"} or knowledge.suspect != "PLAYER":
                continue
            crime = knowledge.crime
            key = f"crime:{crime['id']}"
            items = crime.get("items") or [{"item": crime.get("item", ""), "count": crime.get("count", 0)}]
            ref = CrimeRef(
                key,
                self.crimes.describe(crime),
                crime.get("victim") or crime.get("owner") or "",
                crime["kind"],
                knowledge.degree,
                tuple(
                    (entry["item"], entry["count"]) for entry in items if entry["item"] and entry["count"] > 0
                ),
            )
            known[key] = ref
            if knowledge.forgiven:
                forgiven.add(key)
            if knowledge.silenced:
                silent.add(key)
        return known, forgiven, silent

    # --- the terms ------------------------------------------------------------------------------

    def _npc_gives(self, npc: str, raw: Any, snap: Snapshot | None) -> tuple[tuple[Give, ...] | None, str]:
        """What the npc gives: what it carries first, the rest from its stash (contracts.py)."""
        wanted, why = _parse(raw)
        if wanted is None:
            return None, why
        if not wanted:
            return (), ""
        kept = self.stash.left(npc)
        if snap is None and not kept:
            return None, "не видно, что у тебя есть."
        have = carried(snap)
        out = []
        for item, count in wanted.items():
            n, name = have.get(item, (0, ""))
            got = split(item, count, name or self.stash.names.get(item, item), n, kept.get(item, 0))
            if got is None:
                listed = "; ".join(f"{nm} ({i}) ×{c}" for i, (c, nm) in have.items()) or "пусто"
                hidden = f" Твой тайник: {self.stash.text(npc)}." if kept else ""
                stored = self.in_chests(npc).get(item)
                if stored is not None:
                    hidden += (
                        f" {stored[1]} ({item}) ×{stored[0]} лежит у тебя в сундуке: сначала достань "
                        "(инструмент chest), потом предлагай."
                    )
                if n or kept.get(item):
                    return None, f"у тебя только {n + kept.get(item, 0)} — {name or item} ({item}).{hidden}"
                return None, f"у тебя нет {item}. Твой инвентарь: {listed}.{hidden}"
            out.append(got)
        return tuple(out), ""

    def _truce(self, npc: str, raw: Any) -> tuple[int | None, str]:
        if raw is None:
            return None, ""
        if self.parley is None:
            return None, "truce выключено."
        from .dialogue import KILLERS

        if self.dialogue.guild_of(npc) not in KILLERS:
            return None, "только бандит может предложить перемирие со своей шайкой."
        if not isinstance(raw, dict) or set(raw) != {"hours"}:
            return None, "truce — объект {hours}, от 1 до 24 игровых часов."
        hours = raw["hours"]
        if type(hours) is not int or not 1 <= hours <= 24:
            return None, "truce.hours — целое число от 1 до 24 игровых часов."
        return hours, ""

    def _lies(
        self, npc: str, forgive: Any, silence: Any
    ) -> tuple[tuple[LieRef | CrimeRef, ...] | None, tuple[LieRef | CrimeRef, ...] | None, str]:
        known, forgiven, silent = self.lies_of(npc)
        crimes, crime_forgiven, crime_silent = self.crimes_of(npc)
        refs: dict[str, LieRef | CrimeRef] = {**known, **crimes}
        forgiven.update(crime_forgiven)
        silent.update(crime_silent)
        out: list[tuple[LieRef | CrimeRef, ...]] = []
        for raw, done, what in ((forgive, forgiven, "простил"), (silence, silent, "обещал молчать о")):
            keys = [str(k).strip() for k in (raw or [])]
            unknown = [k for k in keys if k not in refs]
            if unknown:
                listed = ", ".join(refs) or "никакой"
                why = f"ты не знаешь такой лжи или дела героя: {', '.join(unknown)}. Ты знаешь: {listed}."
                return None, None, why
            again = [k for k in keys if k in done]
            if again:
                return None, None, f"ты уже {what} этой лжи: {', '.join(again)}."
            for key in keys:
                if key not in crimes:
                    continue
                ref = crimes[key]
                if what == "простил" and ref.victim != npc:
                    return None, None, f"простить дело {key} может только пострадавший."
                if what != "простил" and ref.degree != "seen":
                    return None, None, f"обещать молчать о деле {key} может только видевший его свидетель."
            out.append(tuple(refs[k] for k in dict.fromkeys(keys)))
        return out[0], out[1], ""

    def _cover(self, npc: str, raw: Any, today: int) -> tuple[Cover | None, str]:
        if raw is None:
            return None, ""
        if not self.cover or self.crimes is None:
            return None, "прикрытие (cover) выключено."
        if not isinstance(raw, dict):
            return None, "cover — условие с crime, day или vouch."
        if set(raw) - {"crime", "day", "vouch"}:
            return None, "в cover допустимы только crime, day и vouch."
        crime, day, vouch = raw.get("crime"), raw.get("day"), raw.get("vouch")
        if crime is None and day is None and vouch is None:
            return None, "в cover нужно хотя бы одно из полей crime, day, vouch."
        ref = None
        if crime is not None:
            if not isinstance(crime, str) or not crime.strip():
                return None, "crime в cover — id дела, которое ты видел сам."
            key = f"crime:{crime.strip().removeprefix('crime:')}"
            ref = self.crimes_of(npc)[0].get(key)
            if ref is None or ref.degree != "seen":
                return None, f"прикрыть дело {key} может только видевший его свидетель."
        if day is not None:
            if type(day) is not int or day < 1 or day - 1 not in {today, today - 1}:
                return None, f"cover.day — сегодня (день {today + 1}) или вчера (день {today})."
            day -= 1
        if vouch is not None:
            if not isinstance(vouch, str) or len(vouch) > MAX_VOUCH:
                return None, f"cover.vouch — слова до {MAX_VOUCH} знаков."
            vouch = vouch.strip()
        if ref is None and day is None:
            day = today
        return Cover(ref, day, vouch), ""

    async def _lead(self, npc: str, raw: Any, snap: Snapshot | None) -> tuple[Way | None, str]:
        """Where it leads him (`lead`), None for nowhere; or why it cannot ("" when it can)."""
        what = str(raw or "").strip()
        if not what:
            return None, ""
        if self.guide is None:
            return None, "вести героя ты не можешь"
        way, why = await self.guide.way(npc, what, snap)
        return way, "" if way is not None else why

    async def _meet(self, npc: str, raw: Any, snap: Snapshot | None) -> tuple[MeetTerm | None, str]:
        """Where and when they meet (`meet`), None for no meeting; or why not ("" when it is fine)."""
        where = str(raw.get("where") or "").strip() if isinstance(raw, dict) else ""
        if not where:
            return None, ""
        if self.meetings is None:
            return None, "назначать встречи ты не можешь"
        return await self.meetings.term(npc, raw, snap)

    def _repay(self, npc: str, raw: Any) -> tuple[tuple[Debt, ...] | None, tuple[CrimeRef, ...], str]:
        ids = [str(k).strip() for k in (raw or [])]
        mine = {d.id: d for d in self.debts.values() if d.npc == npc and not d.paid}
        crimes, forgiven, _ = self.crimes_of(npc)
        claims = {
            key: ref
            for key, ref in crimes.items()
            if ref.victim == npc and ref.kind == "theft" and ref.items and key not in forgiven
        }
        unknown = [i for i in ids if i not in mine and i not in claims]
        if unknown:
            options = [f"{d.id} ({d.what()})" for d in mine.values()] + list(claims)
            listed = ", ".join(options) or "никаких"
            return None, (), f"у героя нет такого долга тебе: {', '.join(unknown)}. Его долги тебе: {listed}."
        ids = list(dict.fromkeys(ids))
        return tuple(mine[i] for i in ids if i in mine), tuple(claims[i] for i in ids if i in claims), ""

    @staticmethod
    def _crime_payment(refs: tuple[CrimeRef, ...], gives: tuple[Give, ...]) -> str:
        if not refs:
            return ""
        paid = {give.item: give.count for give in gives}
        if paid.get(GOLD, 0) > 0:
            return ""  # the victim agreed this price; the normal atomic transfer must still succeed
        wanted: dict[str, int] = {}
        for ref in refs:
            for item, count in ref.items:
                wanted[item] = wanted.get(item, 0) + count
        if any(paid.get(item, 0) < count for item, count in wanted.items()):
            return "для возмещения кражи впиши в hero_gives все украденные вещи или оговорённую цену золотом."
        return ""

    async def _items(
        self, npc: str, items: set[str], hero: set[str]
    ) -> tuple[dict[str, str] | None, dict[str, str], int, str]:
        """The names of the items in the game, what stands for an id the npc guessed for a thing the hero
        gives now (`hero`: the guess -> the one of that kind of its story he carries), and today's game
        day; or why not."""
        story = sorted(set(self.things(npc)) - items) if self.things is not None else []
        try:
            state = await self.link.story_state(symbols=[*items, *story], items=[*items, *story])
        except (GameError, TimeoutError) as err:
            return None, {}, 0, f"не выходит проверить предметы: {err}"
        kinds, names = state.get("symbols", {}), state.get("items", {})
        story = [i for i in story if kinds.get(i) == "item"]
        unknown = sorted(i for i in items if kinds.get(i) != "item")
        swaps: dict[str, str] = {}
        for guess in (i for i in unknown if i in hero):
            alike = [i for i in story if _kind(i) == _kind(guess)]
            try:
                have = await self.link.story_eval(npc, [_has(i) for i in alike]) if alike else []
            except (GameError, TimeoutError) as err:
                log.info("deal: what %s means by %s: %s", npc, guess, err or type(err).__name__)
                have = []
            held = [i for i, n in zip(alike, have, strict=False) if isinstance(n, int) and n > 0]
            if len(held) == 1:
                log.info("deal: %s guessed %s for %s", npc, guess, held[0])
                swaps[guess] = held[0]
        unknown = [i for i in unknown if i not in swaps]
        if unknown:
            why = f"нет таких предметов: {', '.join(unknown)}."
            if story:
                kinds_asked = {_kind(i) for i in unknown}
                story.sort(key=lambda i: (_kind(i) not in kinds_asked, i))
                listed = "; ".join(f"{names.get(i) or i} ({i})" for i in story[:STORY_ITEMS])
                why += f" Вещи из твоих дел: {listed}."
            return None, {}, 0, why
        known = {*(i for i in items if i not in swaps), *swaps.values()}
        return {i: str(names.get(i) or i) for i in known}, swaps, int(state.get("day", 0)), ""

    # --- the hero accepts ------------------------------------------------------------------------

    async def accept(self, deal_id: str, version: int) -> None:
        deal = next((d for d in self.open.values() if d.id == deal_id), None)
        if deal is None or deal.state != "open":
            log.info("deal %s v%d accepted, but it is not open", deal_id, version)
            return
        if version != deal.version:
            # the terms changed as he pressed: he takes only what he saw
            log.info("deal %s: v%d accepted, v%d is shown", deal_id, version, deal.version)
            await self._notice("Условия изменились — посмотри ещё раз")
            await self._send(deal.show())
            return
        truce_now = self.parley.now() if self.parley is not None else None
        if deal.truce is not None and self.parley is not None and truce_now is None:
            raise ValueError("truce needs the game clock")
        deal.state = "accepting"
        day = await _day(self.link)
        if "day" in day:
            self._set_day(day["day"])
        effects = deal.effects()
        try:
            if deal.let_pass:
                why = (
                    await self.gates.bribe_error(deal.npc, deal.gold())
                    if self.gates is not None
                    else "пропуск через ворота выключен."
                )
                if why:
                    await self._failed(deal, why)
                    return
                await self.gates.bribe(deal.npc, effects=effects)
            elif effects:
                await self.link.story_effect(deal.npc, effects, check=True)
        except GateResultUnknown as err:
            deal.state = "unknown"
            await self._send(DealHideCmd(deal=deal.id))
            await self._notice("Исход сделки за проход неизвестен")
            self.dialogue.note(
                deal.npc,
                f"Исход принятой сделки за проход неизвестен: {err}. Повторно брать плату нельзя.",
            )
            return
        except (GameError, TimeoutError) as err:
            await self._failed(deal, str(err))
            return
        debts = [Debt(uuid.uuid4().hex[:6], deal.npc, deal.name, x.give, x.due) for x in deal.later]
        how = []
        for term, present in (
            ("silence", deal.silence),
            ("cover", deal.cover),
            ("contract", deal.task),
            ("npc_later", deal.npc_later),
            ("meet", deal.meet),
        ):
            if present:
                how.append(term)
        decide(
            self.chronicle,
            deal.npc,
            deal.id,
            how,
            cards=self.cards,
            crimes=self.crimes,
            enabled=self.cheat,
        )
        self.chronicle.record(
            {
                "fact": "deal",
                "npc": deal.npc,
                "name": deal.name,
                "deal": deal.id,
                "version": deal.version,
                "npc_gives": [g.fact() for g in deal.npc_gives],
                "hero_gives": [g.fact() for g in deal.hero_gives],
                "gold": deal.gold(),
                **({"let_pass": True} if deal.let_pass else {}),
                "forgive": [lie.key for lie in deal.forgive],
                "silence": [lie.key for lie in deal.silence],
                "later": [{**d.give.fact(), "due": d.due, "debt": d.id} for d in debts],
                "repay": [d.id for d in deal.repay] + [ref.key for ref in deal.crime_repay],
                **({"xp": deal.xp} if deal.xp else {}),
                **({"teach": [t.fact() for t in deal.teach]} if deal.teach else {}),
                **({"lead": deal.lead.place.id} if deal.lead else {}),
                **({"meet": deal.meet.text()} if deal.meet else {}),
                **({"cover": deal.cover.fact()} if deal.cover else {}),
                **({"npc_later": deal.npc_later.fact()} if deal.npc_later else {}),
                **({"truce": {"hours": deal.truce}} if deal.truce is not None else {}),
                **({"secret": deal.secret} if deal.secret else {}),
                **day,
            }
        )
        if self.parley is not None:
            await self.parley.accept_deal(deal.npc, deal.truce, now=truce_now)
        deal.state = "done"
        if self.open.get(deal.npc) is deal:
            del self.open[deal.npc]
        await self._send(DealHideCmd(deal=deal.id))
        await self._notice("Сделка заключена")
        repaid = {ref.key for ref in deal.crime_repay}
        forgiven = {ref.key: ref for ref in (*deal.forgive, *deal.crime_repay) if isinstance(ref, CrimeRef)}
        for ref in forgiven.values():
            self.chronicle.record(
                {
                    "fact": "forgiven",
                    "npc": deal.npc,
                    "crime": ref.crime,
                    "deal": deal.id,
                    "paid": ref.key in repaid,
                    **({"hero_gives": [g.fact() for g in deal.hero_gives]} if ref.key in repaid else {}),
                    **day,
                }
            )
        for ref in deal.silence:
            if isinstance(ref, CrimeRef) and keeps(self.chronicle, deal.id, "silence"):
                self.chronicle.record(
                    {"fact": "silenced", "npc": deal.npc, "crime": ref.crime, "deal": deal.id, **day}
                )
        if deal.cover and keeps(self.chronicle, deal.id, "cover"):
            self.chronicle.record({"fact": "covered", "npc": deal.npc, "deal": deal.id, **deal.cover.fact()})
            if deal.cover.crime is not None:
                self.chronicle.record(
                    {
                        "fact": "silenced",
                        "npc": deal.npc,
                        "crime": deal.cover.crime.crime,
                        "deal": deal.id,
                        "cover": deal.id,
                        **day,
                    }
                )
        if deal.task:
            self.contracts.add(deal.id, deal.npc, deal.name, deal.task, deal.reward, day.get("day"))
        if deal.npc_later:
            later = deal.npc_later
            later.deal, later.npc, later.name = deal.id, deal.npc, deal.name
            self.npc_later[deal.id] = later
        if deal.meet is not None and self.meetings is not None:
            self.meetings.add(deal.npc, deal.name, deal.meet, deal.id)
        for d in debts:
            self.debts[d.id] = d
            self.chronicle.record(
                {
                    "fact": "debt",
                    "id": d.id,
                    "npc": d.npc,
                    "npc_name": d.name,
                    **d.give.fact(),
                    "due": d.due,
                    **day,
                }
            )
        for d in deal.repay:
            d.paid = True
            self.chronicle.record({"fact": "debt_paid", "id": d.id, "npc": d.npc, **day})
        self.journal.add("deal", npc=deal.npc, name=deal.name, deal=deal.id, version=deal.version)
        log.info("deal %s v%d with %s done: %s", deal.id, deal.version, deal.npc, deal.terms())
        self.dialogue.note(deal.npc, f"Герой принял твою сделку, она состоялась: {deal.terms()}")
        if deal.lead is not None and self.guide is not None:
            self.guide.start(deal.npc, deal.name, deal.lead, deal.id)  # it tells him to come along, and goes
            return
        self.dialogue.event_turn(
            deal.npc,
            "[Герой согласился, сделка состоялась. Отзовись коротко. Если ты хотел от него именно этого, "
            "твоё желание исполнено — обнови его (set_intent).]",
            tools=("set_intent",),
        )

    async def decline(self, deal_id: str, version: int) -> None:
        """The hero declined (N): the offer goes, the npc hears of it and may offer otherwise."""
        deal = next((d for d in self.open.values() if d.id == deal_id), None)
        if deal is None or deal.state != "open":
            log.info("deal %s v%d declined, but it is not open", deal_id, version)
            return
        await self._close(deal, "declined")
        self.dialogue.note(deal.npc, f"Герой отказался от твоего предложения: {deal.terms()}")
        self.dialogue.event_turn(
            deal.npc,
            "[Герой отказался от сделки. Отзовись коротко; захочешь — предложи иначе.]",
            tools=(PROPOSE_DEAL,),
        )

    async def _failed(self, deal: Deal, error: str) -> None:
        """Something is missing after all: the hero's things (his window marks them), or the npc's (it no
        longer has what it offered: the offer goes)."""
        log.info("deal %s v%d with %s failed: %s", deal.id, deal.version, deal.npc, error)
        if deal.let_pass and "missing:" not in error:
            await self._close(deal, "withdrawn")
            await self._notice("Проход не открыт")
            self.dialogue.note(deal.npc, f"Сделка за проход не состоялась: {error}")
            self.dialogue.event_turn(
                deal.npc,
                f"[Сделка за проход не состоялась: {error}. Объясни герою.]",
                tools=(PROPOSE_DEAL,),
            )
            return
        if "missing: hero LP" in error:
            # he can't find learning points in his pocket: the offer goes, the npc may offer otherwise
            await self._close(deal, "withdrawn")
            await self._notice("Не хватает очков обучения")
            need = error.removeprefix("missing: hero LP ").replace("(has", "(у него")
            self.dialogue.note(
                deal.npc, f"Сделка сорвалась: у героя не хватает очков обучения — нужно {need}"
            )
            self.dialogue.event_turn(
                deal.npc,
                f"[Сделка не состоялась: у героя не хватает очков обучения — нужно {need}. Скажи ему; "
                "захочешь — предложи иначе.]",
                tools=(PROPOSE_DEAL,),
            )
            return
        if "missing: npc" in error:
            await self._close(deal, "withdrawn")
            await self._notice("Сделка не состоялась")
            self.dialogue.note(deal.npc, "Сделка сорвалась: у тебя уже нет того, что ты обещал отдать")
            self.dialogue.event_turn(
                deal.npc, "[Сделка сорвалась: у тебя нет того, что ты предлагал. Объясни герою.]", tools=()
            )
            return
        deal.state = "open"
        await self._notice("Не хватает того, что ты отдаёшь")
        await self._send(deal.show())  # the same version: the window lets Y be pressed again

    async def _close(self, deal: Deal, state: DealState, how: str = "") -> None:
        deal.state = state
        if self.open.get(deal.npc) is deal:
            del self.open[deal.npc]
        await self._send(DealHideCmd(deal=deal.id))
        log.info("deal %s with %s: %s", deal.id, deal.npc, state)
        self.journal.add(f"deal_{state}", npc=deal.npc, name=deal.name, deal=deal.id, version=deal.version)
        if state == "expired":
            self.dialogue.note(deal.npc, f"{how}, твоё предложение снято: {deal.terms()}")

    # --- the debts --------------------------------------------------------------------------------

    async def pay_later(self, later: NpcLater, day: dict[str, int], *, generation: int | None = None) -> bool:
        """The NPC's deferred items, once due: an honest transfer or its recorded refusal."""
        if generation is None:
            generation = self._world_generation
        if generation != self._world_generation:
            return False
        if later.paid:
            return True
        if not keeps(self.chronicle, later.deal, "npc_later"):
            breach(
                self.chronicle,
                later.npc,
                later.deal,
                "unpaid",
                npc_name=later.name,
                items=[g.fact() for g in later.gives],
                witnesses=hearers(self.dialogue, later.npc, self.roster()),
                **day,
            )
            return False
        have, kept = carried(self.link.snapshots.get(later.npc)), self.stash.left(later.npc)
        gives = []
        for give in later.gives:
            count, name = have.get(give.item, (0, give.name))
            got = split(give.item, give.count, name or give.name, count, kept.get(give.item, 0))
            if got is None:
                later.missing = f"тебе нечем заплатить: нет {give.text()}"
                return False
            gives.append(got)
        effects = [effect for give in gives for effect in give.effects("hero")]
        try:
            await self.link.story_effect(later.npc, effects, check=True)
        except (GameError, TimeoutError) as err:
            if generation != self._world_generation:
                return False
            if "missing: npc" in str(err):
                later.missing = "тебе нечем заплатить: у тебя уже нет того, что ты обещал"
            else:
                log.info("npc_later %s: %s", later.deal, err)
            return False
        if generation != self._world_generation:
            return False
        later.paid, later.missing = True, ""
        self.chronicle.record(
            {
                "fact": "npc_later_paid",
                "deal": later.deal,
                "npc": later.npc,
                "gives": [g.fact() for g in gives],
                **day,
            }
        )
        self.dialogue.note(
            later.npc, f"Ты отдал герою обещанное по сделке {later.deal}: {_list(later.gives)}"
        )
        await self._notice("Обещанное получено")
        return True

    async def run(self) -> None:
        """A debt not paid by the end of its day is a broken word."""
        while True:
            await asyncio.sleep(DEBTS_EVERY_S)
            tasks = any(c.state == "open" and c.due is not None for c in self.contracts.contracts.values())
            if not tasks and not any(not d.paid and not d.broken for d in self.debts.values()):
                continue
            await self.link.wait_world()
            try:
                day = int((await self.link.story_state()).get("day", 0))
            except (GameError, TimeoutError) as err:
                log.info("debts: %s", err or type(err).__name__)
                continue
            self._set_day(day)
            self.overdue(day)
            self.contracts.overdue(day)

    def overdue(self, day: int) -> list[Debt]:
        """The debts past their day as of `day`, now broken words."""
        out = []
        for d in self.debts.values():
            if d.paid or d.broken or day <= d.due:
                continue
            d.broken = True
            out.append(d)
            self.chronicle.record(
                {
                    "fact": "broken",
                    "debt": d.id,
                    "npc": d.npc,
                    "name": d.name,
                    "what": d.what(),
                    "due": d.due,
                    "day": day,
                }
            )
            self.journal.add("debt_broken", npc=d.npc, name=d.name, what=d.what())
            log.info("debt %s to %s broken: %s", d.id, d.npc, d.what())
            broke = f"Герой не вернул тебе долг в срок: {d.what()}. Он не сдержал слово"
            self.dialogue.remember(d.npc, d.name, broke)
        return out

    def _set_day(self, day: int) -> None:
        if day != self.day:
            self.day = day
            for listener in self.on_day:
                listener(day)

    def reload(self) -> None:
        """The debts again from the chronicle: after a fact written past the deals (the e2e seeds a debt)."""
        self._load()
        for listener in self.on_reload:
            listener()

    def _load(self) -> None:
        self._cancel_payments()
        self.debts.clear()
        self.npc_later.clear()
        ids = [d.id for d in self.open.values()]
        for f in self.chronicle.facts():
            self._on_fact(f)
            if f.get("fact") == "deal":
                ids.append(f.get("deal", ""))
        highest = max((int(i[1:]) for i in ids if i.startswith("D") and i[1:].isdigit()), default=0)
        self._ids = itertools.count(highest + 1)

    def _cancel_payments(self) -> None:
        """An in-flight payment belongs only to the world in which the conversation started."""
        self._world_generation += 1
        for task in self._paying.values():
            task.cancel()
        self._paying.clear()

    def _on_fact(self, f: Fact) -> None:
        match f.get("fact"):
            case "deal" if f.get("npc_later"):
                raw = f["npc_later"]
                gives = tuple(
                    Give(x["item"], int(x["count"]), x.get("name") or x["item"]) for x in raw["items"]
                )
                later = NpcLater(gives, int(raw["day"]), f["deal"], f["npc"], f.get("name") or f["npc"])
                self.npc_later[later.deal] = later
            case "npc_later_paid" if f.get("deal") in self.npc_later:
                self.npc_later[f["deal"]].paid = True
            case "debt":
                give = Give(f.get("item", GOLD), int(f.get("count", 1)), f.get("name") or f.get("item", ""))
                npc = f.get("npc", "")
                self.debts[f["id"]] = Debt(f["id"], npc, f.get("npc_name") or npc, give, int(f["due"]))
            case "debt_paid" if f.get("id") in self.debts:
                self.debts[f["id"]].paid = True
            case "broken" if f.get("debt") in self.debts:
                self.debts[f["debt"]].broken = True

    async def _notice(self, text: str) -> None:
        await self._send(ChatStatusCmd(status="notice", detail=text))

    async def _send(self, cmd: Any) -> None:
        try:
            await self.link.send(cmd)
        except GameError as err:
            log.info("deals: %s", err)

    def _spawn(self, coro: Coroutine[Any, Any, Any]) -> None:
        task = asyncio.get_running_loop().create_task(coro)
        self._background.add(task)
        task.add_done_callback(self._background.discard)


def _parse(raw: Any) -> tuple[dict[str, int] | None, str]:
    """[{item, count}] -> {ITEM: count}, the same item added up; or why not."""
    if raw is None:
        return {}, ""
    if not isinstance(raw, list):
        return None, "нужен список предметов."
    out: dict[str, int] = {}
    for x in raw:
        if not isinstance(x, dict):
            return None, "каждый предмет — {item, count}."
        item = str(x.get("item") or "").strip().upper()
        try:
            count = int(x.get("count") or 0)
        except (TypeError, ValueError):
            count = 0
        if not item or count < 1:
            return None, f"неверный предмет: {x}."
        out[item] = out.get(item, 0) + count
    if len(out) > MAX_TERMS:
        return None, f"не больше {MAX_TERMS} разных предметов с одной стороны."
    if any(c > MAX_COUNT for c in out.values()):
        return None, f"не больше {MAX_COUNT} штук."
    return out, ""


def _kind(item: str) -> str:
    """The kind of an item by its id's prefix: ITWR_ writings, ITMI_ things, ITAR_ armour..."""
    return item.split("_", 1)[0]


def _has(item: str) -> list[Any]:
    """How many the hero has (story_eval, self = the npc)."""
    return ["call", "NPC_HASITEMS", ["inst", "OTHER"], ["sym", item]]


def _swapped(wanted: dict[str, int], swaps: dict[str, str]) -> dict[str, int]:
    out: dict[str, int] = {}
    for item, count in wanted.items():
        item = swaps.get(item, item)
        out[item] = out.get(item, 0) + count
    return out


def _parse_xp(raw: Any) -> tuple[int | None, str]:
    try:
        xp = int(raw or 0)
    except (TypeError, ValueError):
        return None, f"опыт — число: {raw}."
    if not 0 <= xp <= MAX_COUNT:
        return None, f"опыт — от 0 до {MAX_COUNT}."
    return xp, ""


def _parse_days(raw: Any) -> tuple[int | None, str]:
    try:
        days = int(raw or 0)
    except (TypeError, ValueError):
        return None, f"срок — число дней: {raw}."
    if not 0 <= days <= MAX_TASK_DAYS:
        return None, f"срок поручения — от 1 до {MAX_TASK_DAYS} дней, 0 — без срока."
    return days, ""


def _parse_later(raw: Any) -> tuple[list[tuple[str, int, int]] | None, str]:
    """[{item, count, days}] -> [(ITEM, count, days)]; or why not."""
    out = []
    for x in raw or []:
        if not isinstance(x, dict):
            return None, "каждый долг — {item, count, days}."
        got, why = _parse([{"item": x.get("item"), "count": x.get("count")}])
        if got is None:
            return None, why
        try:
            days = int(x.get("days") or 0)
        except (TypeError, ValueError):
            days = 0
        if not 1 <= days <= MAX_DAYS:
            return None, f"срок долга — от 1 до {MAX_DAYS} дней."
        ((item, count),) = got.items()
        out.append((item, count, days))
    if len(out) > MAX_TERMS:
        return None, f"не больше {MAX_TERMS} долгов."
    return out, ""


def _parse_npc_later(raw: Any) -> tuple[tuple[dict[str, int], int] | None, str]:
    if raw is None:
        return None, ""
    if not isinstance(raw, dict) or set(raw) - {"items", "day"}:
        return None, "npc_later — условие {items, day}."
    items, why = _parse(raw.get("items"))
    if items is None:
        return None, f"npc_later — {why}"
    if not items:
        return None, "npc_later — нужен хотя бы один предмет."
    try:
        day = int(raw.get("day") or 0)
    except (TypeError, ValueError):
        day = 0
    if day < 1:
        return None, "npc_later — день игры от 1."
    return (items, day - 1), ""


def _side(verb: str, gives: tuple[Give, ...], more: list[str]) -> str:
    """One side of the terms: the things it gives, then the rest ("отдаёшь — ничего" if nothing at all)."""
    parts = [f"{verb} — {_list(gives)}"] if gives else []
    return ", ".join(parts + more) or f"{verb} — ничего"


def _list(gives: tuple[Give, ...]) -> str:
    return ", ".join(g.text() for g in gives) or "ничего"


def _title(f: Fact) -> str:
    return str(f.get("title") or f.get("quest") or "")


async def _day(link: GameLink) -> dict[str, int]:
    try:
        state = await link.story_state()
        return {key: int(state[key]) for key in ("day", "hour", "minute") if key in state}
    except (GameError, TimeoutError):
        return {}
