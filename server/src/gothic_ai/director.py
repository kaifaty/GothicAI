"""The director, v0 (docs/story-graph.md, stage 5): keeps the quests the hero is on finishable.

When someone dies the director looks at the quests in progress: one whose every ending was a dialog of the
dead has no ending left. The cloud model then proposes a repair, as a writer of the game would: who takes
the ending over and in what words, or that the quest is over. The code builds the change from the
proposal: the condition and the effects come from the lifted ending, never more than it gave. Then the
validator checks it (validator.py) and only then it goes into the game, as patches of the save branch. A
rejected proposal goes back to the model with the reasons, once.

The pace: one step at a time, a pause between steps, none while a dialog or a cutscene runs.
"""

from __future__ import annotations

import asyncio
import copy
import json
import logging
import time
from collections.abc import Awaitable, Callable, Iterable
from dataclasses import dataclass, field
from typing import TYPE_CHECKING, Any

from .chronicle import Chronicle, Fact
from .game import GameError, GameLink
from .gamedata.storygraph import Expr, Node, StoryGraph, render, walk
from .journal import Journal
from .llm import LLMBackend, LLMError, Message, Tool, ToolCall, open_ahead
from .protocol import GameMessage, Session
from .story import StoryPatches
from .validator import (
    DIRECTOR_PREFIX,
    ENDINGS,
    LOG_RUNNING,
    Caps,
    Patches,
    Rules,
    Story,
    WorldState,
    check_effects,
    names_in,
    validate,
)

if TYPE_CHECKING:
    from .dialogue import DialogueManager
    from .questbook import DeadEnd, Quest, QuestBook

log = logging.getLogger(__name__)

PROPOSE = "propose_repair"
SETTLE_S = 2.0  # after a death: the scripts' own reaction (ZS_Dead) comes first
CACHE_KEY = (
    "director"  # its requests' own connection (openai_responses.py): the prompt cache of its instructions
)
NEARBY_CM = 3000  # who stood near the dead: candidates to take its ending over
MAX_SUCCESSORS = 8
ATTEMPTS = 2

INSTRUCTIONS = """\
Ты — режиссёр сюжета Gothic II. Код следит за миром и зовёт тебя, когда квест героя остался без концовки: \
например, умер тот, кому надо сдать работу. Предложи ремонт, как сделал бы сценарист оригинальной игры: \
бережно к её миру, людям и тону.

Ремонт бывает двух видов:
- handover: концовку берёт на себя другой живой NPC из списка кандидатов — тот, кому это естественно: \
родня, напарник, хозяин того же дела, тот, кто был рядом. Ты пишешь строку героя для меню диалога \
(description), короткий разговор (lines: 2–5 реплик; hero — герой, npc — преемник, он говорит своим голосом \
и знает о смерти), выбираешь эффекты концовки по id из сводки (награда не больше исходной, можно меньше, \
если так честнее) и две записи в журнал героя от первого лица: journal_now — что он узнаёт сразу (о смерти и \
к кому теперь идти), journal_done — после сдачи. Эффект-«вариант ответа» — это выбор героя в меню после \
разговора, как в оригинале (например, золото или скидка): выбери те варианты, что преемнику естественно \
предложить, и, если нужно, напиши его ответ на каждый (answers: id варианта и одна реплика). Вариант, \
который обещает то, что мог дать только умерший, не бери. То, что даёт вариант, преемник отдаёт в ответе \
на него, а не раньше, в разговоре.
- close: если передать концовку некому или это нелепо, квест закрывается (status: obsolete — потерял смысл, \
failed — провален), с записью journal_now.

Преемник — из списка кандидатов, эффекты — только из сводки. Речь — по-русски, в духе Gothic: просто, \
грубовато, без пафоса и современных слов. Не выдумывай событий, которых нет в сводке. Ответ — один вызов \
propose_repair."""

PROPOSE_TOOL = Tool(
    PROPOSE,
    "Предложить ремонт квеста, оставшегося без концовки. Код проверит предложение и применит его или вернёт "
    "причины отказа.",
    {
        "type": "object",
        "properties": {
            "kind": {"type": "string", "enum": ["handover", "close"]},
            "ending": {"type": "string", "description": "id концовки из сводки; для close пусто"},
            "npc": {
                "type": "string",
                "description": "символ преемника из списка кандидатов; для close пусто",
            },
            "description": {
                "type": "string",
                "description": "что герой говорит в меню диалога, до 60 знаков",
            },
            "lines": {
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
            },
            "effects": {"type": "array", "items": {"type": "string"}, "description": "id эффектов: E1, E2…"},
            "answers": {
                "type": "array",
                "description": "ответ преемника на вариант ответа героя; можно пусто",
                "items": {
                    "type": "object",
                    "properties": {"effect": {"type": "string"}, "text": {"type": "string"}},
                    "required": ["effect", "text"],
                    "additionalProperties": False,
                },
            },
            "status": {"type": "string", "enum": ["obsolete", "failed"], "description": "для close"},
            "journal_now": {"type": "string"},
            "journal_done": {"type": "string", "description": "для handover"},
            "reason": {"type": "string", "description": "почему так, одной фразой"},
        },
        "required": [
            "kind",
            "ending",
            "npc",
            "description",
            "lines",
            "effects",
            "answers",
            "status",
            "journal_now",
            "journal_done",
            "reason",
        ],
        "additionalProperties": False,
    },
)


@dataclass
class Offer:
    """An effect of the lifted ending the repair may keep (the bridge's vocabulary), or a choice of it the
    hero picks in the menu after the talk, as in the scripts (`choice`: its node; `inside`: what it gives)."""

    id: str
    effect: dict[str, Any]
    text: str
    choice: str = ""
    inside: list[dict[str, Any]] = field(default_factory=list)
    when: Expr | None = None  # a choice: when the scripts offer it


@dataclass
class Ending:
    """An ending of the quest no one can play any more."""

    node: str
    npc: str
    description: str
    value: int  # what it sets the quest to: LOG_SUCCESS...
    condition: Expr | None  # when it is offered, with what the ending needs (`guard`)
    lines: list[str]
    offers: list[Offer]
    terms: list[str] = field(default_factory=list)


@dataclass
class Brief:
    """What the model is told about a quest left without an ending."""

    quest: str
    title: str  # of its journal topic, "" if it has none
    dead: str
    dead_name: str
    killer: str
    told: list[str]  # what the journal told the hero
    endings: list[Ending]
    successors: dict[str, str]  # npc -> "Name: why"
    remaining: str = ""  # a quest of the book: its stage and the goals still ahead, in order

    def offers(self) -> dict[str, Offer]:
        return {o.id: o for e in self.endings for o in e.offers}

    def text(self) -> str:
        out = [
            f"Квест «{self.title or self.quest}» ({self.quest}) у героя в работе, "
            "но закончить его больше нельзя.",
            f"Что случилось: {self.dead_name} ({self.dead}) мёртв, убил его {self.killer}.",
        ]
        if self.told:
            out.append("Что герой знает из журнала:")
            out += [f"- «{t}»" for t in self.told]
        if self.remaining:
            out.append(self.remaining)
        out.append("Концовки, которые теперь недоступны:")
        for i, e in enumerate(self.endings, 1):
            out.append(
                f"{i}. {e.node} — диалог «{e.description}», ставит квесту {ENDINGS.get(e.value, 'running')}. "
                f"Когда: {render(e.condition)}."
            )
            out += [
                f"   Умерший сам снял пункты уговора: {t}. Преемник держит этот уговор" for t in e.terms
            ]
            if e.lines:
                out.append("   Как это звучало: " + " ".join(f"«{t}»" for t in e.lines))
            out.append(
                "   Эффекты (выбери нужные по id):" if e.offers else "   Эффектов, кроме концовки, нет."
            )
            out += [f"   {o.id}: {o.text}" for o in e.offers]
        out.append("Кандидаты в преемники:")
        out += [f"- {npc} — {why}" for npc, why in self.successors.items()] or ["- нет"]
        return "\n".join(out)


@dataclass
class Proposal:
    kind: str
    ending: str = ""
    npc: str = ""
    description: str = ""
    lines: list[dict[str, str]] = field(default_factory=list)
    effects: list[str] = field(default_factory=list)
    answers: dict[str, str] = field(default_factory=dict)  # choice offer -> the successor's answer
    status: str = "obsolete"
    journal_now: str = ""
    journal_done: str = ""
    reason: str = ""

    @classmethod
    def of(cls, args: dict[str, Any]) -> Proposal:
        return cls(
            kind=str(args.get("kind", "")),
            ending=str(args.get("ending", "")).upper(),
            npc=str(args.get("npc", "")).upper(),
            description=str(args.get("description", "")),
            lines=[dict(x) for x in args.get("lines") or [] if isinstance(x, dict)],
            effects=[str(x).upper() for x in args.get("effects") or []],
            answers={
                str(a.get("effect", "")).upper(): str(a.get("text", ""))
                for a in args.get("answers") or []
                if isinstance(a, dict) and str(a.get("text", "")).strip()
            },
            status=str(args.get("status") or "obsolete"),
            journal_now=str(args.get("journal_now", "")),
            journal_done=str(args.get("journal_done", "")),
            reason=str(args.get("reason", "")),
        )


@dataclass
class Repair:
    """A change that went into the game."""

    quest: str
    kind: str
    npc: str = ""
    node: str = ""
    ops: list[dict[str, Any]] = field(default_factory=list)


class RepairModel:
    """The cloud model that proposes repairs."""

    def __init__(self, llm: LLMBackend) -> None:
        self.llm = llm

    def warm(self) -> None:
        """Its connection opened at once (a death): the request after the pacing does not wait for it."""
        open_ahead(self.llm, CACHE_KEY, keep=True)

    async def propose(
        self, brief: Brief, previous: Proposal | None = None, errors: list[str] | None = None
    ) -> Proposal | None:
        messages = [Message("user", brief.text())]
        if previous is not None and errors:
            sent = json.dumps(previous.__dict__, ensure_ascii=False)
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
            log.warning("director: the model failed: %s", err)
            return None
        return Proposal.of(calls[0].arguments) if calls else None


@dataclass
class Pacing:
    """When the director may step in (v0): not sooner than `cooldown_s` after its last step, not while a
    dialog or a cutscene runs."""

    cooldown_s: float = 20.0
    poll_s: float = 1.0
    max_wait_s: float = 300.0
    last: float = float("-inf")

    async def wait(self, busy: Callable[[], Awaitable[bool]]) -> None:
        pause = self.last + self.cooldown_s - time.monotonic()
        if pause > 0:
            await asyncio.sleep(pause)
        deadline = time.monotonic() + self.max_wait_s
        while time.monotonic() < deadline:
            if not await busy():
                return
            await asyncio.sleep(self.poll_s)

    def stepped(self) -> None:
        self.last = time.monotonic()


class Director:
    def __init__(
        self,
        link: GameLink,
        graph: StoryGraph,
        patches: StoryPatches,
        chronicle: Chronicle,
        llm: LLMBackend,
        *,
        dialogue: DialogueManager | None = None,
        journal: Journal | None = None,
        pacing: Pacing | None = None,
        settle_s: float = SETTLE_S,
        book: QuestBook | None = None,
    ) -> None:
        self.link = link
        self.graph = graph
        self.patches = patches
        self.chronicle = chronicle
        self.model = RepairModel(llm)
        self.dialogue = dialogue
        self.journal = journal or Journal()
        self.pacing = pacing or Pacing()
        self.settle_s = settle_s
        self.book = book
        self.rules = Rules.of(graph)
        self.repairs: list[Repair] = []
        self.given_up: set[str] = set()  # quests no repair was found for, in this session
        self._queue: asyncio.Queue[str | None] = asyncio.Queue()
        self._npcs = sorted({n.npc for n in graph.nodes.values()})
        self._dialogs = sorted(n.id for n in graph.nodes.values() if not n.is_choice and not n.permanent)
        self.titles = topic_titles(graph)  # journal topic constant -> its title
        chronicle.on_fact.append(self._on_fact)
        link.listeners.append(self._on_message)

    # --- watching -----------------------------------------------------------------------------

    def _on_fact(self, fact: Fact) -> None:
        if fact.get("fact") == "died" and not fact.get("tag") and fact.get("npc"):
            self._queue.put_nowait(str(fact["npc"]).upper())

    def _on_message(self, msg: GameMessage) -> None:
        if isinstance(msg, Session):
            self.given_up.clear()
            self._queue.put_nowait(None)  # a loaded world: the deaths of its branch

    async def run(self) -> None:
        while True:
            npc = await self._queue.get()
            await asyncio.sleep(self.settle_s)
            try:
                if npc is None:
                    await self._look_after_load()
                else:
                    await self.look([npc])
            except (GameError, TimeoutError) as err:
                log.warning("director: %s", err)

    async def _look_after_load(self) -> None:
        """The deaths the chronicle of this branch has seen (a save made before a repair, after a death);
        quests broken before the director watched are left to `look()` from the console: an old save may
        have many, each a request to the model."""
        dead = {
            str(f["npc"]).upper() for f in self.chronicle.facts("died") if f.get("npc") and not f.get("tag")
        }
        self.given_up |= {str(f["quest"]) for f in self.chronicle.facts("unrepaired")}
        if dead:
            await self.look(dead)

    async def world(self, npcs: Iterable[str] = (), symbols: Iterable[str] = ()) -> WorldState:
        """The state the rules read, from the game."""
        value = await self.link.story_state(
            vars=sorted({*self.graph.vars, "KAPITEL"}),
            npcs=sorted({*self._npcs, *npcs}),
            knows=self._dialogs,
            symbols=sorted(symbols),
            timeout=10.0,
        )
        return WorldState.from_game(value)

    async def story(self) -> Story:
        return Story(self.graph, await self.world(), Patches.of(self.patches.ops()))

    async def look(self, dead: Iterable[str] | None = None) -> list[str]:
        """Quests the deaths of `dead` (None: of anyone) left without an ending; each gets a repair."""
        story = await self.story()
        deaths = None if dead is None else {d.upper() for d in dead}
        broken = []
        for q in story.dead_ends():
            owners = {story.owner(n) for n in story.endings(q)}
            if q in self.given_up or (deaths is not None and not deaths & owners):
                continue
            if not any(o is not None and story.state.dead(o) for o in owners):
                continue  # no ending was ever reachable: not something a death broke
            broken.append(q)
        for q in broken:
            await self.repair(q, story)
        if self.book is not None:
            broken.extend(await self._look_book(story, deaths))
        return broken

    async def _look_book(self, story: Story, deaths: set[str] | None) -> list[str]:
        """The book checks the order of its goals; a trigger writing a stage cannot mend a dead dialog."""
        assert self.book is not None
        await self.book.settle()
        broken: list[str] = []
        while True:
            state = story.state.merge(await self.book.world())
            story = Story(self.graph, state, Patches.of(self.patches.ops()))
            blocked = next(
                (
                    b
                    for b in self.book.dead_ends(state)
                    if b.quest not in self.given_up and (deaths is None or b.npc in deaths)
                ),
                None,
            )
            if blocked is None:
                return broken
            if blocked.quest not in broken:
                broken.append(blocked.quest)
            # One dialog at a time: the same dead npc may own another later in this quest.
            if await self.repair(blocked.quest, story) is None:
                self.given_up.add(blocked.quest)

    # --- repairing ----------------------------------------------------------------------------

    async def repair(self, quest: str, story: Story) -> Repair | None:
        self.model.warm()
        await self.pacing.wait(self.busy)
        try:
            brief = await self.brief(quest, story)
            if brief is None:
                return None
            self.chronicle.record(
                {
                    "fact": "dead_end",
                    "quest": quest,
                    "npc": brief.dead,
                    "endings": [e.node for e in brief.endings],
                }
            )
            self.journal.add("dead_end", quest=quest, npc=brief.dead, name=brief.dead_name)
            previous: Proposal | None = None
            errors: list[str] = []
            for attempt in range(1, ATTEMPTS + 1):
                proposal = await self.model.propose(brief, previous, errors)
                log.info("director: %s, proposal %d: %s", quest, attempt, proposal)
                if proposal is None:
                    errors = ["нет предложения: ответ — вызов propose_repair"]
                    previous = Proposal(kind="")
                else:
                    done, errors = await self._try(proposal, brief, story)
                    if done is not None:
                        return done
                    previous = proposal
                log.info("director: %s, attempt %d rejected: %s", quest, attempt, errors)
                self.chronicle.record(
                    {"fact": "repair_rejected", "quest": quest, "attempt": attempt, "why": errors}
                )
                self.journal.add("repair_rejected", quest=quest, attempt=attempt, why=errors)
            self.given_up.add(quest)
            self.chronicle.record({"fact": "unrepaired", "quest": quest})
            return None
        finally:
            self.pacing.stepped()

    async def brief(self, quest: str, story: Story) -> Brief | None:
        if self.book is not None and quest in self.book.all():
            blocked = next((b for b in self.book.dead_ends(story.state) if b.quest == quest), None)
            return await self._book_brief(blocked, story) if blocked is not None else None
        q = self.graph.quests.get(quest)
        if q is None:
            return None
        dropped = {
            d
            for f in self.chronicle.facts("quest_terms")
            if f.get("quest") == quest and story.state.dead(f["npc"])
            for d in f["drop"]
        }
        if dropped:
            names = await self.link.story_state(npcs=sorted(dropped), items=sorted(dropped))
            story.state.names.update(WorldState.from_game(names).names)
            story.state.names.update(names.get("items", {}))
        endings = []
        n = 1
        for id in story.endings(quest):
            node = self.graph.nodes.get(id)
            if node is None or not story.state.dead(node.npc):
                continue  # the director's own endings: when their npc dies too, the original is described
            ending = self._ending(node, quest, story, n)
            if ending is not None:
                endings.append(ending)
                n += len(ending.offers)
        if not endings:
            return None
        dead = endings[0].npc
        killer = next(
            (f.get("killer") for f in reversed(self.chronicle.facts("died")) if f.get("npc") == dead), ""
        )
        return Brief(
            quest=quest,
            title=next(iter(q.topics.values()), ""),
            dead=dead,
            dead_name=story.state.name(dead),
            killer="герой"
            if killer == "PLAYER"
            else story.state.name(killer)
            if killer
            else "неизвестно кто",
            told=self._told(quest),
            endings=endings,
            successors=await self._successors(quest, dead, story),
        )

    async def _book_brief(self, blocked: DeadEnd, story: Story) -> Brief | None:
        assert self.book is not None
        from .questbook import GOLD, TALKS, done_node, goal_id, goal_node

        q = self.book.all()[blocked.quest]
        node = story.patches.nodes.get(blocked.node)
        if node is None:
            return None
        offers = []
        for e in node.get("effects", []):
            if e.get("do") == "give" and e.get("item") == GOLD and e.get("to") != "self":
                offers.append(Offer("E1", copy.deepcopy(e), f"золото герою: {e['count']}"))
            elif e.get("do") == "xp":
                offers.append(Offer("E2", copy.deepcopy(e), f"опыт +{e['amount']}"))
        value = next(
            (e["value"] for e in node.get("effects", []) if e.get("do") == "set" and e.get("var") == q.id),
            LOG_RUNNING,
        )
        told = []
        current = story.state.vars.get(f"{q.id}_STAGE", self.book.progress(q.id).stage)
        remaining = [f"Стадия {current} из {len(q.stages)}. Осталось сделать по порядку:"]
        for k, stage in enumerate(q.stages, 1):
            if k <= current and stage.journal:
                told.append(stage.journal)
            pending = []
            for j, g in enumerate(stage.goals, 1):
                gid = goal_id(q, k, j)
                dialog = done_node(q) if k == len(q.stages) else goal_node(gid)
                done = k < current or story.state.vars.get(gid, 0) > 0 or dialog in story.patches.told
                if done:
                    if g.journal:
                        told.append(g.journal)
                    continue
                if g.kind in TALKS:
                    text = f"поговорить с {q.name(g.npc)}: «{g.talk.description if g.talk else ''}»"
                    if g.kind == "bring":
                        text = f"принести {g.item} ×{g.count} к {q.name(g.npc)}; {text}"
                elif g.kind == "kill":
                    text = "убить: " + ", ".join(q.name(n) for n in g.npcs)
                elif g.kind == "reach":
                    text = f"дойти до {g.point or q.name(g.npc)}"
                elif g.kind == "have":
                    text = f"заполучить {g.item} ×{g.count}"
                else:
                    text = f"подождать {g.days} дн."
                pending.append(text)
            if pending:
                remaining.append(f"- Стадия {k}: {stage.journal} " + "; ".join(pending) + ".")
        killer = next(
            (f.get("killer") for f in reversed(self.chronicle.facts("died")) if f.get("npc") == blocked.npc),
            "",
        )
        return Brief(
            quest=q.id,
            title=q.title,
            dead=blocked.npc,
            dead_name=story.state.name(blocked.npc),
            killer="герой"
            if killer == "PLAYER"
            else story.state.name(killer)
            if killer
            else "неизвестно кто",
            told=list(dict.fromkeys(told)),
            endings=[
                Ending(
                    blocked.node,
                    blocked.npc,
                    node["description"],
                    value,
                    copy.deepcopy(node.get("condition")),
                    [x.get("text", "") for x in node.get("lines", [])],
                    offers,
                )
            ],
            successors=await self._book_successors(q, blocked.npc, story),
            remaining="\n".join(remaining),
        )

    async def _book_successors(self, q: Quest, dead: str, story: Story) -> dict[str, str]:
        why = {g.npc: "другая цель этого поручения" for s in q.stages for g in s.goals if g.npc}
        for s in q.stages:
            for g in s.goals:
                for npc in g.npcs:
                    why.setdefault(npc, "другая цель этого поручения")
        try:
            near = await self.link.story_nearby(dead, NEARBY_CM)
        except (GameError, TimeoutError):
            near = []
        for p in sorted(near, key=lambda p: p.get("distance", 0)):
            why.setdefault(p["npc"].upper(), f"был рядом ({round(p.get('distance', 0) / 100)} м)")
        missing = [n for n in why if n not in story.state.npcs]
        if missing:
            more = await self.world(missing)
            story.state.npcs.update(more.npcs)
            story.state.names.update(more.names)
        out = {}
        for npc, reason in why.items():
            if npc != dead and story.state.alive(npc) and not self.rules.plot_npc(npc):
                out[npc] = f"{story.state.name(npc)}: {reason}"
                if len(out) == MAX_SUCCESSORS:
                    break
        return out

    def _ending(self, node: Node, quest: str, story: Story, first: int) -> Ending | None:
        """The ending as a repair could replay it: when it is offered, what it needs, what it gives."""
        guard: Expr | None = None
        value = None
        for e in node.effects:
            v = _ends(e, quest)
            if v is not None:
                value, guard = v, e.get("when")
                break
        if value is None:
            return None
        needs = _hero_items(guard)
        condition = node.condition
        if node.is_choice and node.parents:
            condition = (
                self.graph.nodes[node.parents[0]].condition if node.parents[0] in self.graph.nodes else None
            )
        running = ["==", ["var", quest], ["int", LOG_RUNNING, "LOG_RUNNING"]]
        condition = _all(running, condition, guard)
        terms = [
            f
            for f in self.chronicle.facts("quest_terms")
            if f.get("quest") == quest and f.get("npc") == node.npc
        ]
        claim = None
        dropped: list[str] = []
        if terms:
            from .vanilla import Claim

            dropped = list(dict.fromkeys(d for f in terms for d in f["drop"]))
            items = _hero_items(condition)
            claim = Claim(items={d: items.get(d, 1) for d in dropped}, dead=dropped, knows=dropped)
            needs = {i: n for i, n in needs.items() if i not in dropped}

        def assume(e: Any) -> Any:
            return claim.assume(e, self.graph) if claim is not None else e

        condition = assume(condition)
        offers: list[Offer] = []
        self._offers(node, quest, story, needs, offers, first, label="", assume=assume, dropped=dropped)
        for item, count in needs.items():
            offers.append(
                Offer(
                    f"E{first + len(offers)}",
                    {"do": "give", "item": item, "count": count, "to": "self"},
                    f"забрать у героя {item} ×{count}",
                )
            )
        lines = [
            e.get("text", "")
            for e in node.effects
            if e["do"] == "line"
            and e.get("speaker") == ["inst", "SELF"]
            and story.possible(assume(e.get("when")), self_npc=node.npc, hero_items=needs)
        ][:4]
        said = []
        for f in terms:
            names = ", ".join(
                self.graph.nodes[d].description if d in self.graph.nodes else story.state.name(d)
                for d in f["drop"]
            )
            said.append(f"{names} — «{f['why']}»")
        return Ending(node.id, node.npc, node.description, value, condition, lines, offers, said)

    def _offers(
        self,
        node: Node,
        quest: str,
        story: Story,
        needs: dict[str, int],
        out: list[Offer],
        first: int,
        label: str,
        assume: Callable[[Any], Any],
        dropped: list[str],
        depth: int = 0,
    ) -> None:
        for e in node.effects:
            when = assume(e.get("when"))
            if not story.possible(when, self_npc=node.npc, hero_items=needs):
                continue
            made = _effect(e, quest, self.titles)
            if made is not None:
                effect, text = made
                if effect.get("to") == "self" and effect.get("item") in dropped:
                    continue
                out.append(Offer(f"E{first + len(out)}", effect, text + label))
            elif e["do"] == "choice" and depth == 0 and e.get("node") in self.graph.nodes:
                # the hero's choice in the menu, as the scripts offer it: kept whole, its program played
                choice = self.graph.nodes[e["node"]]
                inside: list[Offer] = []
                self._offers(choice, quest, story, needs, inside, 0, "", assume, dropped, 1)
                gives = ", ".join(o.text for o in inside) or "сам ничего не даёт"
                said = " ".join(
                    f"«{x.get('text', '')}»"
                    for x in choice.effects
                    if x["do"] == "line"
                    and x.get("speaker") == ["inst", "SELF"]
                    and story.possible(assume(x.get("when")), self_npc=node.npc, hero_items=needs)
                )
                text = f"вариант ответа «{choice.description}»" + (f", в ответ: {said}" if said else "")
                text += f" — {gives}"
                effects = [o.effect for o in inside]
                out.append(Offer(f"E{first + len(out)}", {}, text, choice.id, effects, when))

    def _told(self, quest: str) -> list[str]:
        q = self.graph.quests[quest]
        out = []
        for id in sorted(q.running):
            for e in self.graph.nodes[id].effects:
                if e["do"] == "journal_entry" and e.get("topic") in q.topics and e.get("text") not in out:
                    out.append(e["text"])
        return out[:4]

    async def _successors(self, quest: str, dead: str, story: Story) -> dict[str, str]:
        """Who may take the ending over: people of the quest and of the dead's other quests, people near."""
        why: dict[str, str] = {}
        info = self.graph.vars.get(quest)
        for id in sorted((info.readers | info.writers) if info else set()):
            node = self.graph.nodes.get(id)
            if node is not None:
                why.setdefault(node.npc, "этот квест есть в диалогах")
        for q in self.graph.quests_of(dead):
            for id in sorted(q.movers()):
                why.setdefault(
                    self.graph.nodes[id].npc, f"в другом квесте, где есть {story.state.name(dead)}"
                )
        try:
            near = await self.link.story_nearby(dead, NEARBY_CM)
        except (GameError, TimeoutError):
            near = []
        for p in sorted(near, key=lambda p: p.get("distance", 0)):
            why.setdefault(p["npc"].upper(), f"был рядом ({round(p.get('distance', 0) / 100)} м)")
        missing = [n for n in why if n not in story.state.npcs]
        if missing:
            more = await self.world(missing)
            story.state.npcs.update(more.npcs)
            story.state.names.update(more.names)
        out = {}
        for npc, reason in why.items():
            if npc == dead or not story.state.alive(npc) or self.rules.plot_npc(npc):
                continue
            out[npc] = f"{story.state.name(npc)}: {reason}"
            if len(out) == MAX_SUCCESSORS:
                break
        return out

    async def _try(self, p: Proposal, brief: Brief, story: Story) -> tuple[Repair | None, list[str]]:
        """Build the change from the proposal, check it, make it."""
        if self.book is not None and brief.quest in self.book.all():
            done, errors = await self.book.repair(p, brief, story)
            if done is not None:
                self._done(done, brief, p)
            return done, errors
        if p.kind == "close":
            return await self._close(p, brief, story)
        if p.kind != "handover":
            return None, [f"вид ремонта: handover или close, а не {p.kind!r}"]
        ending = next((e for e in brief.endings if e.node == p.ending), None)
        if ending is None:
            return None, [f"нет такой концовки в сводке: {p.ending!r}"]
        offers = brief.offers()
        unknown = [x for x in p.effects if x not in {o.id for o in ending.offers}]
        if unknown:
            return None, [f"эффектов {', '.join(unknown)} нет у концовки {ending.node}"]
        if not p.journal_now.strip() or not p.journal_done.strip():
            return None, ["нужны обе записи журнала: journal_now и journal_done"]
        id = self._node_id(brief.quest, story)
        picked = [offers[x] for x in p.effects]
        effects: list[dict[str, Any]] = [o.effect for o in picked if not o.choice]
        effects.append({"do": "set", "var": brief.quest, "value": ending.value})
        program: list[dict[str, Any]] = []
        for o in picked:
            if o.choice:
                try:
                    own = await self.link.story_program(o.choice, own=True)
                except (GameError, TimeoutError) as err:
                    return None, [f"вариант ответа {o.id} не прочитать в игре: {err}"]
                steps = borrowed(own.get("steps") or [], p.answers.get(o.id, ""))
                text = self.graph.nodes[o.choice].description
                choice: dict[str, Any] = {"do": "choice", "text": text, "steps": steps}
                # offered when the scripts offer it, but for what the dialog already asked (it may have taken
                # the things since: the turnips handed over)
                when = _rest(o.when, ending.condition)
                program.append({"do": "if", "cond": when, "then": [choice]} if when is not None else choice)
        if brief.title:
            status = {2: "LOG_SUCCESS", 3: "LOG_FAILED", 4: "LOG_OBSOLETE"}[ending.value]
            effects.append({"do": "journal", "topic": brief.title, "text": p.journal_done, "status": status})
        node = {
            "id": id,
            "npc": p.npc,
            "description": p.description,
            "nr": self.graph.nodes[ending.node].nr,
            "condition": ending.condition,
            "lines": [
                {"speaker": "other" if x.get("speaker") == "hero" else "self", "text": x.get("text", "")}
                for x in p.lines
            ],
            "effects": effects,
        }
        if program:
            node["program"] = program
        ops = [{"op": "add_node", "node": node}]
        story = await self.with_symbols(story, names_in(ops))
        caps = _caps([x for o in ending.offers for x in ([o.effect] if not o.choice else o.inside)])
        errors = validate(ops, story, self.rules, repairs=brief.quest, caps=caps)
        if errors:
            return None, errors
        for op in ops:
            await self.patches.apply(op)
        if brief.title and p.journal_now:
            await self._effects(p.npc, [{"do": "journal", "topic": brief.title, "text": p.journal_now}])
        name = story.state.name(p.npc)
        if self.dialogue is not None:
            self.dialogue.remember(
                p.npc,
                name,
                f"{brief.dead_name} мёртв. Его поручение герою «{brief.title or brief.quest}» теперь "
                f"принимаешь ты — когда герой придёт с ним, в разговоре: «{p.description}»",
            )
        done = Repair(brief.quest, "handover", p.npc, id, ops)
        self._done(done, brief, p)
        return done, []

    async def _close(self, p: Proposal, brief: Brief, story: Story) -> tuple[Repair | None, list[str]]:
        status = {"obsolete": 4, "failed": 3}.get(p.status)
        if status is None:
            return None, [f"статус закрытия: obsolete или failed, а не {p.status!r}"]
        if not p.journal_now.strip():
            return None, ["нужна запись журнала journal_now"]
        effects: list[dict[str, Any]] = [{"do": "set", "var": brief.quest, "value": status}]
        if brief.title:
            log_status = "LOG_OBSOLETE" if status == 4 else "LOG_FAILED"
            effects.append(
                {"do": "journal", "topic": brief.title, "text": p.journal_now, "status": log_status}
            )
        story = await self.with_symbols(story, {brief.quest})
        errors = check_effects(effects, story.state.symbols, self.rules, None)
        if errors:
            return None, errors
        await self._effects(brief.dead, effects)
        done = Repair(brief.quest, "close")
        self._done(done, brief, p)
        return done, []

    def _done(self, done: Repair, brief: Brief, p: Proposal) -> None:
        self.repairs.append(done)
        fact = {"fact": "repair", "quest": done.quest, "kind": done.kind, "npc": done.npc, "node": done.node}
        self.chronicle.record({**fact, "from": brief.dead, "reason": p.reason})
        self.journal.add(
            "repair", quest=done.quest, how=done.kind, npc=done.npc, node=done.node, reason=p.reason
        )
        log.info("director: %s repaired (%s %s): %s", done.quest, done.kind, done.npc or "-", p.reason)

    async def _effects(self, npc: str, effects: list[dict[str, Any]]) -> None:
        try:
            await self.link.story_effect(npc, effects)
        except (GameError, TimeoutError) as err:
            log.warning("director: effects on %s: %s", npc, err)

    async def with_symbols(self, story: Story, names: set[str]) -> Story:
        """The story with what the game says of the names a change uses, and how the npc's among them are:
        a character of the director's own is in no dialog of the graph (cast.py)."""
        known = await self.world(npcs=names, symbols=names)
        state = story.state.merge(known)
        state.symbols = known.symbols
        return Story(self.graph, state, story.patches)

    def _node_id(self, quest: str, story: Story) -> str:
        n = 1 + sum(1 for id in story.patches.nodes if id.startswith(f"{DIRECTOR_PREFIX}{quest}_"))
        return f"{DIRECTOR_PREFIX}{quest}_{n}"

    async def busy(self) -> bool:
        try:
            state = await self.link.story_state()
        except (GameError, TimeoutError):
            return False
        return bool(state.get("dialog") or state.get("cutscene"))


# --- from the lifted graph to the bridge's effects ----------------------------------------------


def _ends(effect: dict[str, Any], quest: str) -> int | None:
    """The ending the effect sets the quest to, if it does."""
    if effect.get("do") != "set" or effect.get("var") != quest:
        return None
    value = effect.get("value")
    v = value[1] if isinstance(value, list) and value[:1] == ["int"] else None
    return v if v in ENDINGS else None


def _effect(e: dict[str, Any], quest: str, titles: dict[str, str]) -> tuple[dict[str, Any], str] | None:
    """One effect of a scripts' dialog in the bridge's vocabulary, with how to say it; None if a repair
    can't replay it (lines, choices, teaching, script code...)."""
    match e["do"]:
        case "set" if e.get("var") != quest and _int(e.get("value")) is not None:
            v = e["value"]
            return {
                "do": "set",
                "var": e["var"],
                "value": v[1],
            }, f"{e['var']} = {v[2] if len(v) > 2 else v[1]}"
        case "xp" if _int(e.get("amount")) is not None:
            a = e["amount"]
            return {"do": "xp", "amount": a[1]}, f"опыт +{a[1]}" + (f" ({a[2]})" if len(a) > 2 else "")
        case "give" if _sym(e.get("item")) and _int(e.get("amount")) is not None:
            item, n = _sym(e["item"]), e["amount"][1]
            if e.get("from") == ["inst", "SELF"] and e.get("to") == ["inst", "OTHER"]:
                return {"do": "give", "item": item, "count": n}, f"герою: {item} ×{n}"
            if e.get("from") == ["inst", "OTHER"] and e.get("to") == ["inst", "SELF"]:
                return {"do": "give", "item": item, "count": n, "to": "self"}, f"забрать у героя {item} ×{n}"
        case "remove_items" if e.get("npc") == ["inst", "OTHER"] and _sym(e.get("item")):
            item, n = _sym(e["item"]), _int(e.get("amount"))
            if n is not None:
                return {"do": "give", "item": item, "count": n, "to": "self"}, f"забрать у героя {item} ×{n}"
        case "journal_entry" if e.get("topic") in titles and e.get("text"):
            title = titles[e["topic"]]
            return {"do": "journal", "topic": title, "text": e["text"]}, f"журнал «{title}»: «{e['text']}»"
    return None


def borrowed(steps: list[dict[str, Any]], answer: str = "") -> list[dict[str, Any]]:
    """A choice of the dead npc's dialog as its successor plays it: the hero says his own lines, the dead
    one's are gone (the successor's `answer` comes after the hero's first line), the choices it would offer
    next are not (a handover's menu has one level), and Info_ClearChoices clears the successor's."""

    def block(ss: list[dict[str, Any]]) -> list[dict[str, Any]]:
        out = []
        for st in ss:
            call = st.get("call") if st.get("do") == "call" else None
            name = str(call[1]).upper() if isinstance(call, list) and len(call) > 1 else ""
            if name == "AI_OUTPUT" and call[2] != ["inst", "OTHER"]:
                continue  # the dead one's line
            if name == "INFO_ADDCHOICE":
                continue
            if name == "INFO_CLEARCHOICES":
                out.append({"do": "clear_choices"})
                continue
            st = dict(st)
            for k in ("then", "else"):
                if isinstance(st.get(k), list):
                    st[k] = block(st[k])
            pure = not any(x[0] in ("call", "fn") for x in walk(st.get("cond")))
            if st.get("do") == "if" and not st.get("then") and not st.get("else") and pure:
                continue  # it held only the dead one's lines
            out.append(st)
        return out

    out = block(copy.deepcopy(steps))
    if answer.strip():
        said = next((i + 1 for i, st in enumerate(out) if st.get("do") == "call"), 0)
        out.insert(said, {"do": "say", "speaker": "self", "text": answer.strip()})
    return out


def topic_titles(graph: StoryGraph) -> dict[str, str]:
    titles: dict[str, str] = {}
    for q in graph.quests.values():
        titles.update(q.topics)
    for n in graph.nodes.values():
        for e in n.effects:
            if e["do"] == "journal_topic" and e.get("title"):
                titles.setdefault(e["topic"], e["title"])
    return titles


def _hero_items(guard: Expr | None) -> dict[str, int]:
    """Items the hero must have for the guard to hold: Npc_HasItems(other, X) >= n and the like."""
    out: dict[str, int] = {}
    for e in walk(guard):
        if e[0] not in (">=", ">", "==") or len(e) != 3:
            continue
        call, n = e[1], _int(e[2])
        if n is None or call[:2] != ["call", "NPC_HASITEMS"] or len(call) != 4:
            continue
        if call[2] in (["inst", "OTHER"], ["inst", "HERO"]) and _sym(call[3]):
            out[_sym(call[3])] = n + 1 if e[0] == ">" else n
    return out


def _caps(effects: list[dict[str, Any]]) -> Caps:
    caps = Caps()
    for e in effects:
        if e["do"] == "xp":
            caps.xp += int(e["amount"])
        elif e["do"] == "give" and e.get("to") != "self":
            caps.items[e["item"]] = caps.items.get(e["item"], 0) + int(e["count"])
    return caps


def _rest(guard: Expr | None, condition: Expr | None) -> Expr | None:
    """The terms of `guard` the condition does not have, as a program computes them; None if there are none
    or they are not plain (cases, unknown parts: a program does not take them)."""
    have = condition[1:] if condition is not None and condition[:1] == ["&&"] else [condition]
    terms = [
        t for t in (guard[1:] if guard is not None and guard[:1] == ["&&"] else [guard]) if t is not None
    ]
    rest = [t for t in terms if t not in have]
    if not rest or any(x[0] in ("cases", "unknown", "param", "local") for t in rest for x in walk(t)):
        return None
    return rest[0] if len(rest) == 1 else ["&&", *rest]


def _all(*parts: Expr | None) -> Expr | None:
    """The conjunction, flat and without repeats."""
    found: list[Expr] = []
    for p in parts:
        for c in p[1:] if p is not None and p[:1] == ["&&"] else [p]:
            if c is not None and c not in found:
                found.append(c)
    return None if not found else found[0] if len(found) == 1 else ["&&", *found]


def _int(e: Any) -> int | None:
    return int(e[1]) if isinstance(e, list) and e[:1] == ["int"] else None


def _sym(e: Any) -> str:
    return str(e[1]).upper() if isinstance(e, list) and e[:1] == ["sym"] else ""
