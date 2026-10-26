"""The director as author, stage 1 (docs/director.md): a quest of its own for a person of the world.

The model proposes; the code builds the change, the validator checks it, the game plays it, as with the
repairs (director.py) and the amends (amends.py). The quest: someone has lost a thing and asks the hero to
find it and bring it back. It is a quest of the book (questbook.py, stage 3) of one stage, whose one goal is
to bring the thing to the giver:
- the giver's dialog gives it: the talk, the quest set running, the journal, and the thing put into the
  world at a point of a place near the giver (Wld_InsertItem), so it can always be found;
- the dialog that ends it, offered once the hero has the thing: the giver takes it and pays, the quest
  succeeds, the journal closes;
- the reward within the chapter's measure: experience no more than the scripts' XP_AMBIENT (what the game
  gives for a small task in this chapter), as much gold.

The things are ITEMS: ordinary ones anybody could lose, none that a quest of the scripts uses. The book
keeps the quest in the branch's log, the game its state (the variables, the journal). A quest made before
the book (stage 1, facts `authored*` of the chronicle) keeps its dialogs in the log; its giver's knowledge of
it is read from those facts (OldQuest).
"""

from __future__ import annotations

import json
import logging
from collections import Counter
from dataclasses import asdict, dataclass, field
from typing import TYPE_CHECKING, Any

from . import questbook
from .characters import Card
from .chronicle import Chronicle
from .game import GameError, GameLink
from .gamedata.storygraph import StoryGraph
from .journal import Journal
from .llm import LLMBackend, LLMError, Message, Tool, ToolCall, open_ahead
from .places import PlaceBook
from .protocol import DialogChosen, GameMessage, Session, Snapshot
from .questbook import BRING, Goal, QuestBook, Stage, Talk
from .story import StoryPatches
from .turn import ToolOutcome
from .validator import Rules

if TYPE_CHECKING:
    from .dialogue import Conversation, DialogueManager
    from .director import Director

log = logging.getLogger(__name__)

PROPOSE = "propose_quest"
CACHE_KEY = (
    "author"  # its requests' own connection (openai_responses.py): the prompt cache of its instructions
)
ATTEMPTS = 2
# Ordinary supplies and small valuables; the brief also excludes anything named by a script dialog.
ITEMS = (
    "ITMI_SAW",
    "ITMI_HAMMER",
    "ITMI_BROOM",
    "ITMI_RAKE",
    "ITMI_SCOOP",
    "ITMI_BRUSH",
    "ITMI_PLIERS",
    "ITMI_STOMPER",
    "ITMI_LUTE",
    "ITMI_SILVERRING",
    "ITMI_GOLDRING",
    "ITFO_ADDON_SHELLFLESH",
    "ITFO_SMELLYFISH",
    "ITFO_CORAGONSBEER",
    "ITPL_BLUEPLANT",
    "ITPL_FORESTBERRY",
    "ITPL_PLANEBERRY",
    "ITPL_WEED",
    "ITAT_MEATBUGFLESH",
    "ITAT_BUGMANDIBLES",
    "ITAT_CRAWLERMANDIBLES",
    "ITAT_TEETH",
    "ITAT_CLAW",
    "ITAT_LURKERCLAW",
    "ITAT_LURKERSKIN",
    "ITAT_SHARKSKIN",
    "ITMI_ADDON_SHELL_01",
    "ITMI_ADDON_SHELL_02",
    "ITMI_ADDON_WHITEPEARL",
    "ITMI_GOLDCANDLEHOLDER",
    "ITMI_GOLDCHALICE",
    "ITMI_GOLDPLATE",
    "ITMI_GOLDNECKLACE",
    "ITMI_SILVERCANDLEHOLDER",
    "ITMI_SILVERNECKLACE",
    "ITMI_SKULL",
    "ITMI_POCKET",
)
NEAR_CM = 4000  # places around the giver's own, where the thing may lie
MAX_PLACES = 6
XP_FALLBACK = 50  # when the game does not say XP_AMBIENT
MAX_TITLE = 40

INSTRUCTIONS = """\
Ты — сценарист Gothic II. Придумай небольшое поручение, которое житель мира даёт герою: у него пропала вещь \
— потерял, выронил, растащили звери или унёс ветер — и он просит её найти и принести. Вещь лежит в одном из \
мест рядом, и он примерно знает, где. Пиши, как написал бы сценарист оригинальной игры: просто, грубовато, \
в характере этого человека, без пафоса и современных слов.

Выбери вещь и место из сводки и напиши:
- title — тему в журнале героя, 2–4 слова;
- give_description — что герой говорит в меню диалога, до 60 знаков (с этих слов он и начинает \
разговор); give_lines — разговор, в котором NPC просит (2–6 реплик; hero — герой, npc — этот NPC) и говорит, \
где искать, словами места из сводки;
- done_description и done_lines — когда герой приносит вещь (1–4 реплики): NPC берёт её и платит;
- gold и xp — награду, не больше пределов из сводки (можно меньше: это мелкое дело);
- journal_given и journal_done — записи журнала от первого лица героя.
Не выдумывай людей и мест, которых нет в сводке. Ответ — один вызов propose_quest."""

_LINES = {
    "type": "array",
    "items": {
        "type": "object",
        "properties": {
            "speaker": {"type": "string", "enum": ["hero", "npc"]},
            "text": {"type": "string"},
        },
        "required": ["speaker", "text"],
        "additionalProperties": False,
    },
}

PROPOSE_TOOL = Tool(
    PROPOSE,
    "Предложить поручение: что пропало, где лежит, разговоры, награда и журнал. Код проверит его и применит "
    "или вернёт причины отказа.",
    {
        "type": "object",
        "properties": {
            "title": {"type": "string", "description": "тема журнала, 2–4 слова"},
            "item": {"type": "string", "description": "id вещи из сводки"},
            "place": {"type": "string", "description": "id места из сводки"},
            "give_description": {"type": "string", "description": "строка героя в меню, до 60 знаков"},
            "give_lines": _LINES,
            "done_description": {"type": "string", "description": "строка героя в меню, до 60 знаков"},
            "done_lines": _LINES,
            "gold": {"type": "integer"},
            "xp": {"type": "integer"},
            "journal_given": {"type": "string"},
            "journal_done": {"type": "string"},
            "reason": {"type": "string", "description": "почему такое поручение, одной фразой"},
        },
        "required": [
            "title",
            "item",
            "place",
            "give_description",
            "give_lines",
            "done_description",
            "done_lines",
            "gold",
            "xp",
            "journal_given",
            "journal_done",
            "reason",
        ],
        "additionalProperties": False,
    },
)


@dataclass
class Spot:
    """A place near the giver where the thing may lie."""

    id: str
    where: str  # «на полях фермы Лобарта»
    distance_m: int
    point: str  # where the thing is put


@dataclass
class Brief:
    """What the model is told about the one who asks."""

    npc: str
    name: str
    card: str  # «Ты — …», or ""
    home: str  # where it lives and works, or ""
    chapter: int
    items: dict[str, str]  # instance -> name in the game
    spots: list[Spot]
    xp: int  # the most experience
    gold: int  # the most gold
    given: list[str] = field(default_factory=list)  # titles of its quests the author made before
    premise: str = ""
    requires: list[dict[str, Any]] = field(default_factory=list)
    follows: str = ""  # the vanilla quest's title, in words rather than its script variable

    def text(self, *, errand: bool = True) -> str:
        out = [f"Кто просит: {self.name} ({self.npc})."]
        if self.card:
            out.append(f"Его характер (написан ему самому): {self.card}")
        if self.home:
            out.append(f"Где живёт и работает: {self.home}.")
        out.append(f"Глава {self.chapter}.")
        if self.follows:
            out.append(f"Поручение отвечает на дело «{self.follows}».")
        if self.premise:
            out.append(f"Замысел режиссёра: {self.premise}")
        out.append(
            "Вещи, одна из которых у него пропала (id — название):"
            if errand
            else "Вещи для поручения (id — название):"
        )
        out += [f"- {i} — {n}" for i, n in self.items.items()]
        out.append(
            "Места рядом, где она может лежать (id — где, как далеко от его места):"
            if errand
            else "Места рядом (id — где, как далеко от его места):"
        )
        out += [f"- {s.id} — {s.where}, {s.distance_m} м" for s in self.spots]
        out.append(f"Награда: опыт до {self.xp}, золото до {self.gold}.")
        if self.given:
            out.append(
                "Его поручения герою уже были, не повторяйся: " + "; ".join(f"«{t}»" for t in self.given)
            )
        return "\n".join(out)

    def knowledge(self, description: str) -> str:
        if not self.follows:
            return description
        return " ".join(
            part
            for part in (description, f"Твоё поручение связано с делом «{self.follows}».", self.premise)
            if part
        )


@dataclass
class Proposal:
    title: str = ""
    item: str = ""
    place: str = ""
    give_description: str = ""
    give_lines: list[dict[str, str]] = field(default_factory=list)
    done_description: str = ""
    done_lines: list[dict[str, str]] = field(default_factory=list)
    gold: int = 0
    xp: int = 0
    journal_given: str = ""
    journal_done: str = ""
    reason: str = ""

    @classmethod
    def of(cls, args: dict[str, Any]) -> Proposal:
        known = {k: args[k] for k in cls.__dataclass_fields__ if k in args}
        return cls(**known)


@dataclass
class OldQuest:
    """A quest the author made before the book (stage 1, facts `authored*`), and where it stands."""

    id: str  # its story variable too
    npc: str
    name: str
    title: str
    item: str
    item_name: str
    place: str
    where: str
    gold: int
    xp: int
    give: str  # the dialogs
    done: str
    stage: str = "open"  # open -> given -> done

    def reward(self) -> str:
        parts = [f"{self.gold} золотых"] if self.gold else []
        parts += [f"опыт +{self.xp}"] if self.xp else []
        return ", ".join(parts) or "ничего"


class AuthorModel:
    """The cloud model that writes the quests."""

    def __init__(self, llm: LLMBackend) -> None:
        self.llm = llm

    def warm(self) -> None:
        """Its connection opened at once: the request after the brief does not wait for it."""
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
            log.warning("author: the model failed: %s", err)
            return None
        return Proposal.of(calls[0].arguments) if calls else None


class Author:
    """Quests of the director's own, made up by the model and made by the book (questbook.py). Also a
    TurnExtension for the quests made before the book: the giver knows what it asked of the hero."""

    def __init__(
        self,
        link: GameLink,
        graph: StoryGraph,
        patches: StoryPatches,
        chronicle: Chronicle,
        director: Director,
        llm: LLMBackend,
        *,
        places: PlaceBook | None = None,
        cards: dict[str, Card] | None = None,
        dialogue: DialogueManager | None = None,
        journal: Journal | None = None,
        book: QuestBook | None = None,
    ) -> None:
        self.link = link
        self.graph = graph
        script_things = graph.things()
        self._items = tuple(i for i in ITEMS if i not in script_things)
        self.patches = patches
        self.chronicle = chronicle
        self.director = director
        self.model = AuthorModel(llm)
        self.places = places
        self.cards = cards or {}
        self.dialogue = dialogue
        self.journal = journal or Journal()
        self.book = book or QuestBook(
            link, graph, patches, chronicle, director, places=places, dialogue=dialogue, journal=self.journal
        )
        self.rules = Rules.of(graph)
        self.old: dict[str, OldQuest] = {}  # made before the book, by id
        link.listeners.append(self._on_message)
        if dialogue is not None:
            dialogue.extensions.append(self)
        self._load()

    async def _on_message(self, msg: GameMessage) -> None:
        match msg:
            case Session():
                self._load()
            case DialogChosen(made=True):
                for q in self.old.values():
                    if msg.node == q.give and q.stage == "open":
                        await self._stage(q, "given")
                    elif msg.node == q.done and q.stage != "done":
                        await self._stage(q, "done")

    # --- a quest ------------------------------------------------------------------------------

    async def quest(
        self,
        npc: str,
        premise: str = "",
        *,
        requires: list[dict[str, Any]] | None = None,
        follows: str = "",
    ) -> questbook.Quest | None:
        """A quest of the author's that `npc` gives the hero; None if the model has none that passes."""
        npc = npc.upper()
        self.model.warm()
        brief = await self.brief(npc, premise, requires=requires, follows=follows)
        if brief is None:
            return None
        proposal: Proposal | None = None
        errors: list[str] = []
        for attempt in range(ATTEMPTS):
            proposal = await self.model.propose(brief, proposal, errors)
            if proposal is None:
                break
            log.info("author: %s, proposal %d: %s", npc, attempt + 1, proposal)
            made, errors = await self._try(proposal, brief)
            if made is not None:
                return made
            log.info("author: %s, rejected: %s", npc, "; ".join(errors))
            await self.chronicle.record({"fact": "authored_rejected", "npc": npc, "why": errors})
        self.journal.add("authored_none", npc=npc, name=brief.name, why=errors)
        return None

    async def given(self, npc: str) -> list[str]:
        """The titles of the quests the npc gave before, the old ones too."""
        npc = npc.upper()
        old = [q.title for q in self.old.values() if q.npc == npc]
        return old + [q.title for q in (await self.book.all()).values() if q.giver == npc]

    async def brief(
        self,
        npc: str,
        premise: str = "",
        *,
        requires: list[dict[str, Any]] | None = None,
        follows: str = "",
    ) -> Brief | None:
        state = await self.link.story_state(
            vars=["KAPITEL", "XP_AMBIENT"], npcs=[npc], symbols=[npc, *self._items], items=list(self._items)
        )
        person = (state.get("npcs") or {}).get(npc) or {}
        if person.get("state") != "alive":
            log.info("author: %s is %s", npc, person.get("state", "not in the world"))
            return None
        if self.rules.plot_npc(npc):
            log.info("author: %s is a person of the plot", npc)
            return None
        symbols = state.get("symbols") or {}
        names = state.get("items") or {}
        items = {i: str(names.get(i) or i) for i in self._items if symbols.get(i) == "item"}
        home, spots = await self._spots(npc)
        if not items or not spots:
            log.info("author: %s: no things (%d) or places (%d)", npc, len(items), len(spots))
            return None
        vars = state.get("vars") or {}
        xp = int(vars.get("XP_AMBIENT") or XP_FALLBACK)
        card = self.cards.get(npc)
        return Brief(
            npc=npc,
            name=str(person.get("name") or npc),
            card=card.ru if card else "",
            home=home,
            chapter=int(vars.get("KAPITEL") or 1),
            items=items,
            spots=spots,
            xp=xp,
            gold=xp,
            given=await self.given(npc),
            premise=premise,
            requires=requires if requires is not None else [],
            follows=quest.title if (quest := self.graph.quests.get(follows.upper())) else "",
        )

    async def _spots(self, npc: str) -> tuple[str, list[Spot]]:
        """Where the npc lives and works (the place it spends most of its day at), and the places around."""
        if self.places is None:
            return "", []
        try:
            routine = await self.link.routine(npc)
        except (GameError, TimeoutError) as err:
            log.warning("author: the routine of %s: %s", npc, err)
            return "", []
        homes = Counter(p.id for e in routine.entries if (p := self.places.of(e.waypoint)) is not None)
        if not homes:
            return "", []
        home = self.places.get(homes.most_common(1)[0][0])
        if home is None or home.center is None:
            return "", []
        own = {e.waypoint.upper() for e in routine.entries}
        spots = []
        for place, d in self.places.near(home.center, NEAR_CM, MAX_PLACES + 1):
            points = sorted(p for p in place.points if p not in own)
            if place.id == home.id or not points:
                continue  # under its own feet: not lost
            spots.append(Spot(place.id, place.where_ru, round(d / 100), points[len(points) // 2]))
        return home.where_ru, spots[:MAX_PLACES]

    async def _try(self, p: Proposal, brief: Brief) -> tuple[questbook.Quest | None, list[str]]:
        """The quest of the proposal: checked against the brief, then made by the book."""
        errors = []
        item = p.item.strip().upper()
        spot = next((s for s in brief.spots if s.id == p.place.strip().upper()), None)
        if item not in brief.items:
            errors.append(f"вещи {p.item!r} нет в сводке")
        if spot is None:
            errors.append(f"места {p.place!r} нет в сводке")
        title = p.title.strip()
        if not title or len(title) > MAX_TITLE:
            errors.append(f"тема журнала пустая или длиннее {MAX_TITLE} знаков")
        if not p.journal_given.strip() or not p.journal_done.strip():
            errors.append("нужны обе записи журнала: journal_given и journal_done")
        if not 0 <= p.gold <= brief.gold:
            errors.append(f"золота {p.gold}, а можно от 0 до {brief.gold}")
        if not 0 <= p.xp <= brief.xp:
            errors.append(f"опыта {p.xp}, а можно от 0 до {brief.xp}")
        if title in await self.given(brief.npc):
            errors.append(f"поручение «{title}» уже было")
        if errors or spot is None:
            return None, errors

        thing = brief.items[item]
        bring = Goal(
            BRING,
            npc=brief.npc,
            item=item,
            point=spot.point,
            talk=Talk(
                p.done_description,
                p.done_lines,
                remember=f"Герой нашёл и принёс твою пропавшую вещь — {thing}.",
            ),
        )
        q = questbook.Quest(
            id=await self.book.next_id({*self.old}),
            giver=brief.npc,
            title=title,
            give=Talk(p.give_description, p.give_lines),
            stages=[Stage(p.journal_given, [bring])],
            done_journal=p.journal_done,
            knows=brief.knowledge(
                f"У тебя пропала вещь: {thing}. Она лежит {spot.where}; ты знаешь это примерно."
            ),
            asked=f"найти твою пропавшую вещь — {thing}: она лежит {spot.where}.",
            gold=p.gold,
            xp=p.xp,
            names={brief.npc: brief.name},
            reason=p.reason,
            requires=brief.requires,
        )
        errors = await self.book.make(q, gold=brief.gold, xp=brief.xp)
        if errors:
            return None, errors
        self.journal.add(
            "authored", npc=q.giver, name=brief.name, quest=q.id, title=title, item=item, place=spot.id
        )
        log.info("author: %s gives %s «%s»: %s %s", q.giver, q.id, title, item, spot.point)
        return q, []

    async def _stage(self, q: OldQuest, stage: str) -> None:
        branch, generation, session = (
            self.chronicle.branch, self.chronicle._generation, self.chronicle.link.session
        )

        async def persist():
            if (
                branch != self.chronicle.branch or generation != self.chronicle._generation
                or session is not self.chronicle.link.session
            ):
                raise GameError("session changed while recording NPC state")
            await self.chronicle.record({"fact": f"authored_{stage}", "id": q.id, "npc": q.npc})
            if (
                branch != self.chronicle.branch or generation != self.chronicle._generation
                or session is not self.chronicle.link.session
            ):
                raise GameError("session changed while recording NPC state")
            q.stage = stage
            self.journal.add(f"authored_{stage}", npc=q.npc, name=q.name, quest=q.id, title=q.title)
            if stage == "done" and self.dialogue is not None:
                await self.dialogue.remember(
                    q.npc, q.name, f"Герой нашёл и принёс твою пропавшую вещь — {q.item_name}."
                )
                if (
                    branch != self.chronicle.branch or generation != self.chronicle._generation
                    or session is not self.chronicle.link.session
                ):
                    raise GameError("session changed while recording NPC state")

        return await self.chronicle.memory.complete(persist)

    # --- the save branch --------------------------------------------------------------------

    def _load(self) -> None:
        """The quests made before the book, as the branch's chronicle has them."""
        self.old.clear()
        for f in self.chronicle.facts():
            match f.get("fact"):
                case "authored":
                    fields = {k: f[k] for k in OldQuest.__dataclass_fields__ if k in f}
                    q = OldQuest(**fields)
                    self.old[q.id] = q
                case "authored_given" | "authored_done" if f.get("id") in self.old:
                    self.old[f["id"]].stage = f["fact"].removeprefix("authored_")

    # --- the conversation (TurnExtension) ------------------------------------------------------

    async def prepare(self, npc: str) -> None:
        pass

    def situation(self, npc: str) -> str | None:
        npc = npc.upper()
        lines = []
        for q in self.old.values():
            if q.npc != npc:
                continue
            if q.stage == "given":
                lines.append(
                    f"Ты попросил героя найти твою пропавшую вещь — {q.item_name} («{q.title}»): она лежит "
                    f"{q.where}. Принесёт — заплатишь: {q.reward()}."
                )
            elif q.stage == "done":
                lines.append(
                    f"Герой нашёл и принёс твою пропавшую вещь — {q.item_name} («{q.title}»), ты заплатил."
                )
        return "\n".join(lines) or None

    all_tools: tuple[Tool, ...] = ()

    def tools(self, npc: str) -> list[Tool]:
        return []

    async def handle(
        self, dialogue: DialogueManager, conv: Conversation, call: ToolCall, snap: Snapshot | None
    ) -> ToolOutcome | None:
        return None
