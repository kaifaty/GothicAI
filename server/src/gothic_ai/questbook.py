"""Quests as objects (docs/director.md, stage 3, layer 2): one model of a quest of the director's, from which
the code builds the primitives of the story graph. Nothing here runs a quest: the bridge and the graph run
what is built. Server-owned blame goals set their declared variables after an accepted accusation.

A quest has a giver, a journal topic, the dialog that gives it, stages, a reward, maybe a deadline and the
quest it opens after. A stage has the journal entry it begins with and goals; all done, the next stage
begins, the last one ends the quest. The goals are what the code checks:
- `bring` — a thing to an npc, the giver or another: his dialog is offered while the hero has it;
- `talk` — a talk with an npc: his dialog in this stage;
- `reach` — come to a point (of the book of places) or to an npc: a state trigger on the distance;
- `have` — get a thing anyhow (kill, take, buy, find): a state trigger on Npc_HasItems;
- `kill` — npc's of the director's dead: a state trigger on Npc_IsDead. Its own: their symbols outlive a load,
  while the bridge forgets creatures under a tag (story_spawn) after one;
- `wait` — so many days gone: a trigger on Wld_GetDay and the day the stage began.
- `vanilla` — a scripts' quest reaches a state: a state trigger on its variable.
- `blame` — convince the giver who stole: a server-set variable after his `blamed` fact, true or false.
The last stage ends in a talk: its one goal that is a dialog (bring or talk) is the turn-in, which pays and
closes the quest.

What the code builds (`build`):
- the variables: DIR_Q<n> — the status, as the scripts' quests have it (0, LOG_RUNNING, LOG_SUCCESS,
  LOG_FAILED); _STAGE — the stage; _DAY — the day the stage began; _DUE — the last day;
- the dialogs: the one that gives it, the goals' and the turn-in;
- the triggers (triggers.py): the goals', the stages' and the deadline's;
- the journal's entries.
A goal done is its trigger's count, or the variable its dialog sets; a stage's own trigger begins the next.

The description lies in the branch's log as a `quest` record next to the changes built of it (as a character
lies as an `npc` one); only the changes go to the game, and what was built is never built anew. Before it
is made the code checks it (`problems`): its people and things are there, every stage can be done in order
and the last one ends it, the reward is within the chapter's; then the validator checks the changes.

The giver knows what it asked and what it saw itself, the turn-in; how far the hero got only from his words:
the code owns the truth, the npc's their beliefs.
"""

from __future__ import annotations

import asyncio
import copy
import logging
from collections.abc import Callable
from dataclasses import asdict, dataclass, field
from typing import TYPE_CHECKING, Any

from . import triggers
from .chronicle import Chronicle, Fact
from .game import GameError, GameLink
from .gamedata.storygraph import QUEST_STATES, Expr, StoryGraph
from .journal import Journal
from .llm import Tool, ToolCall
from .places import PlaceBook
from .protocol import DialogChosen, GameMessage, Loading, Session, Snapshot, TriggerFired
from .quest_tools import AMEND_TOOL, DROP_TOOL
from .story import QUEST, StoryPatches
from .turn import ToolOutcome
from .validator import (
    DIRECTOR_PREFIX,
    LOG_FAILED,
    LOG_OBSOLETE,
    LOG_RUNNING,
    LOG_SUCCESS,
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
    from .dialogue import Conversation, DialogueManager
    from .director import Brief, Director, Proposal, Repair

log = logging.getLogger(__name__)

Op = dict[str, Any]
Step = dict[str, Any]

BRING, TALK, REACH, HAVE, KILL, WAIT, VANILLA = "bring", "talk", "reach", "have", "kill", "wait", "vanilla"
BLAME = "blame"
KINDS = (BRING, TALK, REACH, HAVE, KILL, WAIT, VANILLA, BLAME)
VANILLA_STATES = {name: value for value, name in QUEST_STATES.items() if value != LOG_OBSOLETE}
TALKS = (BRING, TALK)  # the goals that are dialogs
GOLD = "ITMI_GOLD"
OWN_NPC = f"{DIRECTOR_PREFIX}NPC_"  # the director's characters (cast.py): the only ones a quest kills
MAX_STAGES = 5
MAX_GOALS = 4
MAX_KILL = 8
MAX_COUNT = 20
MAX_WAIT = 7  # days
MAX_DAYS = 30  # a deadline
REACH_CM = 500
MIN_REACH_CM, MAX_REACH_CM = 200, 3000
MAX_TITLE = 40
MAX_DESCRIPTION = 80
MAX_LINES = 10
XP_FALLBACK = 50  # when the game does not say XP_AMBIENT


# --- the description ---------------------------------------------------------------------------


@dataclass
class Talk:
    """A dialog of the quest: the hero's line in the menu, the talk, what the npc remembers of it after."""

    description: str
    lines: list[dict[str, str]]  # {"speaker": "self" | "other", "text"}
    remember: str = ""

    @classmethod
    def of(cls, d: dict[str, Any] | None) -> Talk | None:
        if not d:
            return None
        return cls(str(d.get("description", "")), list(d.get("lines") or []), str(d.get("remember", "")))


@dataclass
class Goal:
    kind: str
    npc: str = ""  # bring, talk: whose dialog it is; reach: whom to come to; have: who holds the thing
    item: str = ""  # bring, have
    count: int = 1
    point: str = ""  # reach: where to come; bring, have: where the thing lies, put there as the stage begins
    radius: int = REACH_CM  # reach
    npcs: list[str] = field(default_factory=list)  # kill
    days: int = 0  # wait
    talk: Talk | None = None  # bring, talk
    journal: str = ""  # an entry when it is done
    quest: str = ""  # vanilla: a quest variable of the scripts
    state: str = ""  # vanilla: running, success, failed
    crime: str = ""  # blame: the NPC deed whose culprit the giver must believe

    @classmethod
    def of(cls, d: dict[str, Any]) -> Goal:
        known = {k: d[k] for k in cls.__dataclass_fields__ if k in d and k != "talk"}
        return cls(**known, talk=Talk.of(d.get("talk")))


@dataclass
class Stage:
    journal: str  # the entry it begins with
    goals: list[Goal]

    @classmethod
    def of(cls, d: dict[str, Any]) -> Stage:
        return cls(str(d.get("journal", "")), [Goal.of(g) for g in d.get("goals") or []])


@dataclass
class Quest:
    id: str  # DIR_Q<n>, its status variable too
    giver: str
    title: str  # the journal's topic
    give: Talk
    stages: list[Stage]
    done_journal: str
    knows: str = ""  # what the giver knows from the moment the quest is made (its trouble), for its model
    asked: str = ""  # what the giver asked of the hero, as the giver knows it
    gold: int = 0
    xp: int = 0
    days: int = 0  # the deadline in days from when it is given; 0: none
    late_journal: str = ""
    after: str = ""  # a quest whose success opens this one
    names: dict[str, str] = field(default_factory=dict)  # npc -> its name in the game
    reason: str = ""
    repairs: list[dict[str, str]] = field(default_factory=list)
    requires: list[dict[str, Any]] = field(default_factory=list)  # vanilla conditions that open the quest

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)

    @classmethod
    def of(cls, d: dict[str, Any]) -> Quest:
        known = {k: d[k] for k in cls.__dataclass_fields__ if k in d and k not in ("give", "stages")}
        return cls(
            **known,
            give=Talk.of(d.get("give")) or Talk("", []),
            stages=[Stage.of(s) for s in d.get("stages") or []],
        )

    def name(self, npc: str) -> str:
        return self.names.get(npc, npc)

    def reward(self) -> str:
        parts = [f"{self.gold} золотых"] if self.gold else []
        parts += [f"опыт +{self.xp}"] if self.xp else []
        return ", ".join(parts) or "ничего"

    def turn_in(self) -> Goal | None:
        """The last stage's dialog: it ends the quest."""
        last = self.stages[-1].goals if self.stages else []
        return next((g for g in last if g.kind in TALKS), None)

    def dialogs(self) -> dict[str, tuple[str, Talk | None]]:
        """Every dialog of the quest by its id: whose it is and its talk."""
        out: dict[str, tuple[str, Talk | None]] = {give_node(self): (self.giver, self.give)}
        for k, s in enumerate(self.stages, 1):
            for j, g in enumerate(s.goals, 1):
                if g.kind in TALKS:
                    node = done_node(self) if k == len(self.stages) else goal_node(goal_id(self, k, j))
                    out[node] = (g.npc, g.talk)
        return out


# --- the names of what is built ------------------------------------------------------------------


def stage_var(q: Quest) -> str:
    return f"{q.id}_STAGE"


def day_var(q: Quest) -> str:
    return f"{q.id}_DAY"


def due_var(q: Quest) -> str:
    return f"{q.id}_DUE"


def give_node(q: Quest) -> str:
    return f"{q.id}_GIVE"


def done_node(q: Quest) -> str:
    return f"{q.id}_DONE"


def goal_id(q: Quest, stage: int, goal: int) -> str:
    """A goal's variable: its trigger's count, or what its dialog sets."""
    return f"{q.id}_S{stage}G{goal}"


def goal_node(goal: str) -> str:
    return f"{goal}_D"


def stage_trigger(q: Quest, stage: int) -> str:
    """The trigger that ends the stage and begins the next."""
    return f"{q.id}_S{stage}"


def late_trigger(q: Quest) -> str:
    return f"{q.id}_LATE"


# --- building ----------------------------------------------------------------------------------


def _int(n: int, name: str = "") -> Expr:
    return ["int", n, name] if name else ["int", n]


def _set(var: str, value: Expr) -> Step:
    return {"do": "set", "target": ["var", var], "value": value}


def _quest_state(quest: str, state: str) -> Expr:
    return ["==", ["var", quest.upper()], _int(VANILLA_STATES[state], f"LOG_{state.upper()}")]


def _requirement(condition: dict[str, Any]) -> Expr:
    """The small vocabulary of vanilla requirements, checked before building."""
    match condition["kind"]:
        case "quest":
            return _quest_state(condition["quest"], condition["state"])
        case "knows":
            return ["call", "NPC_KNOWSINFO", ["inst", "HERO"], ["sym", condition["node"].upper()]]
        case "chapter":
            return [">=", ["var", "KAPITEL"], _int(condition["chapter"])]
    raise ValueError(f"unknown requirement: {condition!r}")


def _entry(q: Quest, text: str) -> list[Step]:
    """A journal entry from a program, under the quest's topic."""
    return [{"do": "call", "call": ["fn", "B_LOGENTRY", ["str", q.title], ["str", text]]}] if text else []


def _begin(q: Quest, k: int) -> list[Step]:
    """What happens as stage k begins: the things it needs lie where they are said to, its day is noted."""
    steps: list[Step] = []
    stage = q.stages[k - 1]
    for g in stage.goals:
        if g.kind in (BRING, HAVE) and g.point:
            put = ["call", "WLD_INSERTITEM", ["sym", g.item.upper()], ["str", g.point.upper()]]
            steps += [{"do": "call", "call": put} for _ in range(max(1, g.count))]
    if any(g.kind == WAIT for g in stage.goals):
        steps.append(_set(day_var(q), ["call", "WLD_GETDAY"]))
    return steps


def _world(q: Quest, g: Goal) -> Expr:
    """The condition of a goal the world decides."""
    match g.kind:
        case "reach" if g.point:
            return triggers.hero_near_point(g.point, g.radius)
        case "reach":
            return triggers.hero_near(g.npc, g.radius)
        case "have":
            return triggers.hero_has(g.item, g.count)
        case "kill":
            return triggers.all_of(*(triggers.dead(n) for n in g.npcs))
        case "vanilla":
            return _quest_state(g.quest, g.state)
        case _:
            return [">=", ["call", "WLD_GETDAY"], ["+", ["var", day_var(q)], _int(g.days)]]


def lines(talk: Talk) -> list[dict[str, str]]:
    """The talk as the bridge plays it; the hero opens it with what he picked in the menu, as in the scripts,
    unless he opens it with words of his own."""
    out = [
        {
            "speaker": "other" if x.get("speaker") in ("hero", "other") else "self",
            "text": str(x.get("text", "")),
        }
        for x in talk.lines
    ]
    if not out or out[0]["speaker"] != "other":
        out.insert(0, {"speaker": "other", "text": talk.description})
    return out


def build(q: Quest) -> list[Op]:
    """The changes of the graph that make the quest."""
    n = len(q.stages)
    status, stage = ["var", q.id], ["var", stage_var(q)]
    running = ["==", status, _int(LOG_RUNNING, "LOG_RUNNING")]

    def at(k: int) -> Expr:
        return triggers.all_of(running, ["==", stage, _int(k)])

    variables = {q.id: 0, stage_var(q): 0}
    if any(g.kind == WAIT for s in q.stages for g in s.goals):
        variables[day_var(q)] = 0
    if q.days:
        variables[due_var(q)] = 0
    for k, s in enumerate(q.stages, 1):
        for j, g in enumerate(s.goals, 1):
            if g.kind == BLAME or (g.kind in TALKS and k < len(q.stages)):
                variables[goal_id(q, k, j)] = 0
    ops: list[Op] = [{"op": "vars", "vars": variables}]

    opens: Expr = ["==", status, _int(0)]
    if q.after:
        opens = ["&&", opens, ["==", ["var", q.after.upper()], _int(LOG_SUCCESS, "LOG_SUCCESS")]]
    if q.requires:
        opens = triggers.all_of(opens, *(_requirement(r) for r in q.requires))
    begin = _begin(q, 1)
    if q.days:
        begin.append(_set(due_var(q), ["+", ["call", "WLD_GETDAY"], _int(q.days)]))
    give: dict[str, Any] = {
        "id": give_node(q),
        "npc": q.giver,
        "description": q.give.description,
        "condition": opens,
        "lines": lines(q.give),
        "effects": [
            {"do": "set", "var": q.id, "value": LOG_RUNNING},
            {"do": "set", "var": stage_var(q), "value": 1},
            {"do": "journal", "topic": q.title, "text": q.stages[0].journal, "status": "LOG_RUNNING"},
        ],
    }
    if begin:
        give["program"] = begin
    ops.append({"op": "add_node", "node": give})

    for k, s in enumerate(q.stages, 1):
        last = k == n
        done: list[str] = []  # the goals' variables, but the turn-in's
        for j, g in enumerate(s.goals, 1):
            gid = goal_id(q, k, j)
            if g.kind in TALKS and last:
                continue
            done.append(gid)
            if g.kind == BLAME:
                continue  # The server sets this variable; no world truth check may decide belief.
            if g.kind not in TALKS:
                ops.append(
                    triggers.trigger(
                        gid, triggers.STATE, _entry(q, g.journal), when=triggers.all_of(at(k), _world(q, g))
                    )
                )
                continue
            assert g.talk is not None
            effects: list[dict[str, Any]] = _takes(g)
            effects.append({"do": "set", "var": gid, "value": 1})
            if g.journal:
                effects.append(
                    {"do": "journal", "topic": q.title, "text": g.journal, "status": "LOG_RUNNING"}
                )
            node = {
                "id": goal_node(gid),
                "npc": g.npc.upper(),
                "description": g.talk.description,
                "condition": triggers.all_of(at(k), ["==", ["var", gid], _int(0)], *_has(g)),
                "lines": lines(g.talk),
                "effects": effects,
            }
            ops.append({"op": "add_node", "node": node})
        before = [triggers.fired(gid) for gid in done]
        if not last:
            steps = [_set(stage_var(q), _int(k + 1)), *_entry(q, q.stages[k].journal), *_begin(q, k + 1)]
            ops.append(
                triggers.trigger(
                    stage_trigger(q, k), triggers.STATE, steps, when=triggers.all_of(at(k), *before)
                )
            )
            continue
        t = q.turn_in()
        assert t is not None and t.talk is not None
        reward: list[dict[str, Any]] = [{"do": "give", "item": GOLD, "count": q.gold}] if q.gold else []
        reward += [{"do": "xp", "amount": q.xp}] if q.xp else []
        turn_in = {
            "id": done_node(q),
            "npc": t.npc.upper(),
            "description": t.talk.description,
            "condition": triggers.all_of(at(k), *before, *_has(t)),
            "lines": lines(t.talk),
            "effects": [
                *_takes(t),
                *reward,
                {"do": "set", "var": q.id, "value": LOG_SUCCESS},
                {"do": "set", "var": stage_var(q), "value": n + 1},
                {"do": "journal", "topic": q.title, "text": q.done_journal, "status": "LOG_SUCCESS"},
            ],
        }
        ops.append({"op": "add_node", "node": turn_in})

    if q.days:
        steps = [
            _set(q.id, _int(LOG_FAILED, "LOG_FAILED")),
            {
                "do": "call",
                "call": ["call", "LOG_SETTOPICSTATUS", ["str", q.title], _int(LOG_FAILED, "LOG_FAILED")],
            },
            *_entry(q, q.late_journal),
        ]
        late = [">", ["call", "WLD_GETDAY"], ["var", due_var(q)]]
        ops.append(
            triggers.trigger(late_trigger(q), triggers.STATE, steps, when=triggers.all_of(running, late))
        )
    return ops


def _has(g: Goal) -> list[Expr]:
    return [triggers.hero_has(g.item, g.count)] if g.kind == BRING else []


def _takes(g: Goal) -> list[dict[str, Any]]:
    return [{"do": "give", "item": g.item.upper(), "count": g.count, "to": "self"}] if g.kind == BRING else []


def _gold(effect: dict[str, Any]) -> bool:
    return effect.get("do") == "give" and effect.get("item") == GOLD and effect.get("to") != "self"


def _reward(effect: dict[str, Any]) -> bool:
    return _gold(effect) or effect.get("do") == "xp"


# --- checking ----------------------------------------------------------------------------------


@dataclass
class Ground:
    """What a quest is checked against."""

    symbols: dict[str, str]  # what the game's names are: npc, item...
    states: dict[str, str]  # npc -> alive, dead, absent
    plot: Callable[[str], bool]  # an npc of the plot: the director leaves it alone
    point: Callable[[str], bool]  # a point the book of places knows
    gold: int  # the most of the reward
    xp: int
    quests: dict[str, Quest] = field(default_factory=dict)  # the branch's others
    story: Story | None = None  # vanilla graph, its current state and the branch's changes
    waypoint: Callable[[str], bool] | None = None  # if known: reach needs a waypoint, not a freepoint
    npc_crimes: set[str] = field(default_factory=set)  # blame may refer only to actual NPC deeds


def problems(q: Quest, ground: Ground) -> list[str]:
    """Why the quest can't be made, in the words a model reads; empty: it can. The code also goes through it
    the way the hero would: every stage can be done in order, and the last one ends it."""
    errors: list[str] = []
    if not q.id.startswith(f"{DIRECTOR_PREFIX}Q"):
        errors.append(f"id квеста — {DIRECTOR_PREFIX}Q<n>, а не {q.id}")
    if not q.title.strip() or len(q.title) > MAX_TITLE:
        errors.append(f"тема журнала пустая или длиннее {MAX_TITLE} знаков")
    if any(o.title == q.title for o in ground.quests.values() if o.id != q.id):
        errors.append(f"поручение «{q.title}» уже было")
    if not 1 <= len(q.stages) <= MAX_STAGES:
        errors.append(f"стадий должно быть от 1 до {MAX_STAGES}")
    errors += _person(q.giver, "заказчик", ground)
    errors += _talk(q.give, "выдачи")
    if not q.done_journal.strip():
        errors.append("нет записи журнала о том, что квест выполнен")
    if not 0 <= q.gold <= ground.gold:
        errors.append(f"золота {q.gold}, а можно от 0 до {ground.gold}")
    if not 0 <= q.xp <= ground.xp:
        errors.append(f"опыта {q.xp}, а можно от 0 до {ground.xp}")
    if q.days and not 1 <= q.days <= MAX_DAYS:
        errors.append(f"срок — от 1 до {MAX_DAYS} дней, а не {q.days}")
    if q.days and not q.late_journal.strip():
        errors.append("у квеста со сроком нужна запись журнала о провале")
    if q.after and q.after.upper() not in ground.quests:
        errors.append(f"квест {q.after} не найден: этот открывается после него")
    if not isinstance(q.requires, list):
        errors.append("requires — список условий открытия")
    else:
        for i, requirement in enumerate(q.requires, 1):
            errors += [f"условие открытия {i}: {e}" for e in _vanilla(requirement, ground)]
    if errors:
        return errors

    # as the hero goes: who is dead by then, which things he has
    killed: set[str] = set()
    held: set[str] = set()
    for k, s in enumerate(q.stages, 1):
        last = k == len(q.stages)
        here = f"стадия {k}"
        got = held | {g.item.upper() for g in s.goals if g.kind == HAVE}  # what the stage itself gets
        if not s.journal.strip():
            errors.append(f"{here}: нет записи журнала")
        if not 1 <= len(s.goals) <= MAX_GOALS:
            errors.append(f"{here}: целей должно быть от 1 до {MAX_GOALS}")
        talks = [g for g in s.goals if g.kind in TALKS]
        if last and len(talks) != 1:
            errors.append(
                f"{here}, последняя: в ней одна сдача — цель «принести» или «поговорить», а не {len(talks)}"
            )
        for j, g in enumerate(s.goals, 1):
            errors += [f"{here}, цель {j}: {e}" for e in _goal(g, ground, killed, got)]
        for g in s.goals:
            if g.kind == KILL:
                killed |= {n.upper() for n in g.npcs}
            if g.kind == HAVE:
                held.add(g.item.upper())
            if g.kind == BRING:
                held.discard(g.item.upper())  # given away
    return errors


def _person(npc: str, who: str, ground: Ground) -> list[str]:
    npc = npc.upper()
    if ground.symbols.get(npc) != "npc":
        return [f"{who}: нет такого NPC {npc}"]
    if ground.plot(npc):
        return [f"{who}: {npc} — сюжетный NPC, его не трогаем"]
    if ground.states.get(npc) != "alive":
        return [f"{who}: {npc} {'мёртв' if ground.states.get(npc) == 'dead' else 'не в мире'}"]
    return []


def _vanilla(condition: dict[str, Any], ground: Ground) -> list[str]:
    """A requirement or a vanilla goal must name the graph and still be attainable."""
    if not isinstance(condition, dict):
        return ["условие — словарь с kind: quest, knows или chapter"]
    story = ground.story
    if story is None:
        return ["нет состояния ванильного графа для проверки условия"]
    kind = condition.get("kind")
    match kind:
        case "quest":
            quest = str(condition.get("quest", "")).upper()
            state = condition.get("state")
            if quest not in story.graph.quests:
                return [f"{quest} — не переменная квеста ванильного графа"]
            if not isinstance(state, str) or state not in VANILLA_STATES:
                return [f"состояние квеста — running, success или failed, а не {state!r}"]
            now = story.state.vars.get(quest)
            if now in (LOG_SUCCESS, LOG_FAILED, LOG_OBSOLETE) and now != VANILLA_STATES[state]:
                return [f"квест {quest} уже закрыт ({QUEST_STATES[now]}): состояние {state} недостижимо"]
        case "knows":
            node = str(condition.get("node", "")).upper()
            dialog = story.graph.nodes.get(node)
            if dialog is None:
                return [f"нет ванильного диалога {node} в графе"]
            if dialog.is_choice:
                return [f"{node} — вариант ответа, нужен ванильный диалог"]
        case "chapter":
            chapter = condition.get("chapter")
            if type(chapter) is not int or not 1 <= chapter <= 6:
                return [f"глава — целое число от 1 до 6, а не {chapter!r}"]
        case _:
            return [f"вид условия — quest, knows или chapter, а не {kind!r}"]
    if not story.possible(_requirement(condition)):
        return [f"условие {condition} уже не может выполниться в этом состоянии мира"]
    return []


def _talk(talk: Talk | None, what: str) -> list[str]:
    if talk is None:
        return [f"у диалога {what} нет разговора"]
    errors = []
    if not talk.description.strip() or len(talk.description) > MAX_DESCRIPTION:
        errors.append(f"строка героя в меню диалога {what} пустая или длиннее {MAX_DESCRIPTION} знаков")
    if not 1 <= len(talk.lines) <= MAX_LINES or any(not str(x.get("text", "")).strip() for x in talk.lines):
        errors.append(f"в диалоге {what} должно быть от 1 до {MAX_LINES} непустых реплик")
    return errors


def _goal(g: Goal, ground: Ground, killed: set[str], held: set[str]) -> list[str]:
    if g.kind not in KINDS:
        return [f"вид цели — {', '.join(KINDS)}, а не {g.kind!r}"]
    errors = []
    if g.kind in (BRING, TALK):
        errors += _person(g.npc, "с кем разговор", ground)
        if g.npc.upper() in killed:
            errors.append(f"{g.npc} к этому времени убит целью «убить»")
        errors += _talk(g.talk, "цели")
    if g.kind in (BRING, HAVE):
        if ground.symbols.get(g.item.upper()) != "item":
            errors.append(f"нет такой вещи {g.item}")
        if not 1 <= g.count <= MAX_COUNT:
            errors.append(f"сколько вещей: от 1 до {MAX_COUNT}, а не {g.count}")
        if g.point and not ground.point(g.point):
            errors.append(f"точки {g.point} нет в книге мест")
    if g.kind == BRING and not g.point and g.item.upper() not in held:
        errors.append(f"{g.item} неоткуда взять: ни точки, где она лежит, ни цели «заполучить» раньше")
    if g.kind == HAVE and not g.point:
        if not g.npc:
            errors.append(f"{g.item}: у кого она — нужна точка, где она лежит, или NPC, у которого она")
        elif ground.symbols.get(g.npc.upper()) != "npc":
            errors.append(f"нет такого NPC {g.npc}, у которого вещь")
    if g.kind == REACH:
        if bool(g.point) == bool(g.npc):
            errors.append("дойти: или до точки, или до NPC")
        elif g.point and not ground.point(g.point):
            errors.append(f"точки {g.point} нет в книге мест")
        elif g.point and ground.waypoint is not None and not ground.waypoint(g.point):
            errors.append(f"точка {g.point} — не вейпоинт: цель «дойти» не работает с фрипоинтами")
        elif g.npc and ground.symbols.get(g.npc.upper()) != "npc":
            errors.append(f"нет такого NPC {g.npc}")
        if not MIN_REACH_CM <= g.radius <= MAX_REACH_CM:
            errors.append(f"радиус — от {MIN_REACH_CM} до {MAX_REACH_CM} см, а не {g.radius}")
    if g.kind == KILL:
        if not 1 <= len(g.npcs) <= MAX_KILL:
            errors.append(f"убить: от 1 до {MAX_KILL} NPC")
        for n in g.npcs:
            if not n.upper().startswith(OWN_NPC):
                errors.append(f"убить можно только своих персонажей режиссёра ({OWN_NPC}…), а не {n}")
            elif ground.symbols.get(n.upper()) != "npc":
                errors.append(f"нет такого NPC {n}")
    if g.kind == WAIT and not 1 <= g.days <= MAX_WAIT:
        errors.append(f"ждать: от 1 до {MAX_WAIT} дней, а не {g.days}")
    if g.kind == BLAME:
        if not g.crime or g.crime not in ground.npc_crimes:
            errors.append("blame: нужно существующее дело NPC")
        if g.npc or g.item or g.point or g.npcs or g.talk is not None:
            errors.append("blame: указывается только crime; убедить нужно заказчика")
    if g.kind == VANILLA:
        errors += _vanilla({"kind": "quest", "quest": g.quest, "state": g.state}, ground)
    return errors


def names_of(q: Quest) -> set[str]:
    """The game's names the description uses: for story_state."""
    out = {q.giver.upper(), GOLD}
    for s in q.stages:
        for g in s.goals:
            out |= {x.upper() for x in (g.npc, g.item, *g.npcs) if x}
    return out


# --- the book ----------------------------------------------------------------------------------


@dataclass
class Progress:
    """Where a quest stands, as the branch's chronicle has it."""

    state: str = "open"  # open -> given -> done | failed | obsolete
    stage: int = 0  # the stage the hero is at; 0: not given


@dataclass
class DeadEnd:
    """The first unfinished dialog of a quest that belongs to someone dead."""

    quest: str
    stage: int
    goal: int
    node: str
    npc: str


class QuestBook:
    """The director's quests on the branch: made from their descriptions, followed through the dialogs and the
    triggers. Also a TurnExtension: the giver knows what it asked of the hero."""

    def __init__(
        self,
        link: GameLink,
        graph: StoryGraph,
        patches: StoryPatches,
        chronicle: Chronicle,
        director: Director,
        *,
        places: PlaceBook | None = None,
        dialogue: DialogueManager | None = None,
        journal: Journal | None = None,
    ) -> None:
        self.link = link
        self.graph = graph
        self.patches = patches
        self.chronicle = chronicle
        self.director = director
        self.places = places
        self.dialogue = dialogue
        self.journal = journal or Journal()
        self.rules = Rules.of(graph)
        self.quests: dict[str, Quest] = {}  # by id, in the order they were made
        self._version = -1
        self._generation = 0
        self._completed_goals: set[str] = set()
        self.dropping: set[str] = set()
        self._blame_tasks: set[asyncio.Task[Any]] = set()
        self._blame_lock = asyncio.Lock()
        chronicle.on_fact.append(self._on_fact)
        link.listeners.append(self._on_message)
        if dialogue is not None:
            dialogue.extensions.append(self)

    async def close(self) -> None:
        if self._on_fact in self.chronicle.on_fact:
            self.chronicle.on_fact.remove(self._on_fact)
        if self._on_message in self.link.listeners:
            self.link.listeners.remove(self._on_message)
        for task in tuple(self._blame_tasks):
            task.cancel()
        if self._blame_tasks:
            await asyncio.gather(*tuple(self._blame_tasks), return_exceptions=True)

    # --- the branch -----------------------------------------------------------------------------

    def all(self) -> dict[str, Quest]:
        """The quests of the current branch, from the records of its log."""
        if self._version != self.patches.version:
            self._version = self.patches.version
            self.quests = {}
            for record in self.patches.records(QUEST):
                q = Quest.of(record["quest"])
                self.quests[q.id] = q
        return self.quests

    def next_id(self, taken: set[str] = frozenset()) -> str:
        """DIR_Q<n> nobody has: no quest, no variable of the branch."""
        vars = Patches.of(self.patches.ops()).vars
        prefix = f"{DIRECTOR_PREFIX}Q"
        used = [
            int(k[len(prefix) :])
            for k in [*vars, *self.all(), *taken]
            if k.startswith(prefix) and k[len(prefix) :].isdigit()
        ]
        return f"{prefix}{1 + max([0, *used])}"

    def progress(self, id: str) -> Progress:
        id = id.upper()
        p = Progress()
        for f in self.chronicle.facts():
            if f.get("id") != id:
                continue
            match f.get("fact"):
                case "story_quest_given":
                    p = Progress("given", 1)
                case "story_quest_stage":
                    p.stage = int(f.get("stage", p.stage))
                case "story_quest_done":
                    p.state = "done"
                case "story_quest_failed":
                    p.state = "failed"
                case "story_quest_obsolete":
                    p.state = "obsolete"
        return p

    async def world(self) -> WorldState:
        """The save's stages and goals, and the people the descriptions mention."""
        variables: set[str] = set()
        people: set[str] = set()
        for q in self.all().values():
            variables.update((q.id, stage_var(q)))
            people.add(q.giver.upper())
            for k, stage in enumerate(q.stages, 1):
                for j, goal in enumerate(stage.goals, 1):
                    variables.add(goal_id(q, k, j))
                    people.update(n.upper() for n in (goal.npc, *goal.npcs) if n)
        return WorldState.from_game(await self.link.story_state(vars=sorted(variables), npcs=sorted(people)))

    def dead_ends(self, state: WorldState) -> list[DeadEnd]:
        """Find blocked dialogs in stage order, one per quest. World goals do not die with their NPCs."""
        told = Patches.of(self.patches.ops()).told
        out = []
        for q in self.all().values():
            progress = self.progress(q.id)
            status = state.vars.get(q.id)
            if status != LOG_RUNNING and (status is not None or progress.state != "given"):
                continue
            current = state.vars.get(stage_var(q), progress.stage)
            if not 1 <= current <= len(q.stages):
                continue
            for k, stage in enumerate(q.stages, 1):
                if k < current:
                    continue
                for j, goal in enumerate(stage.goals, 1):
                    if goal.kind not in TALKS:
                        continue
                    gid = goal_id(q, k, j)
                    node = done_node(q) if k == len(q.stages) else goal_node(gid)
                    if state.vars.get(gid, 0) or node in told:
                        continue
                    if state.dead(goal.npc):
                        out.append(DeadEnd(q.id, k, j, node, goal.npc.upper()))
                        break
                else:
                    continue
                break
        return out

    # --- repairing ----------------------------------------------------------------------------

    async def repair(self, p: Proposal, brief: Brief, story: Story) -> tuple[Repair | None, list[str]]:
        """Replace just the blocked dialog. Its conditions and required effects belong to the book."""
        from .director import Repair

        q = self.all().get(brief.quest)
        if q is None:
            return None, [f"нет квеста книги {brief.quest}"]
        if p.kind == "close":
            status = {"obsolete": LOG_OBSOLETE, "failed": LOG_FAILED}.get(p.status)
            if status is None:
                return None, [f"статус закрытия: obsolete или failed, а не {p.status!r}"]
            if not p.journal_now.strip():
                return None, ["нужна запись журнала journal_now"]
            effects = [
                {"do": "set", "var": q.id, "value": status},
                {
                    "do": "journal",
                    "topic": q.title,
                    "text": p.journal_now,
                    "status": "LOG_OBSOLETE" if status == LOG_OBSOLETE else "LOG_FAILED",
                },
            ]
            errors = check_effects(effects, story.symbols, self.rules, None)
            if errors:
                return None, errors
            try:
                await self.link.story_effect(brief.dead, effects)
            except (GameError, TimeoutError) as err:
                return None, [f"игра не приняла закрытие: {err}"]
            self._fact(q, "story_quest_obsolete" if status == LOG_OBSOLETE else "story_quest_failed")
            return Repair(q.id, "close"), []
        if p.kind != "handover":
            return None, [f"вид ремонта: handover или close, а не {p.kind!r}"]
        ending = next((e for e in brief.endings if e.node == p.ending), None)
        if ending is None:
            return None, [f"нет такой концовки в сводке: {p.ending!r}"]
        if story.state.dead(p.npc):
            return None, [f"{story.state.name(p.npc)} ({p.npc}) мёртв"]
        if p.npc not in brief.successors:
            return None, [f"{p.npc} нет среди кандидатов в преемники"]
        unknown = [x for x in p.effects if x not in {o.id for o in ending.offers}]
        if unknown:
            return None, [f"эффектов {', '.join(unknown)} нет у концовки {ending.node}"]
        if not p.journal_now.strip() or not p.journal_done.strip():
            return None, ["нужны обе записи журнала: journal_now и journal_done"]
        original = story.patches.nodes.get(ending.node)
        if original is None:
            return None, [f"нет диалога книги {ending.node}"]
        target = next(
            (
                (k, j)
                for k, stage in enumerate(q.stages, 1)
                for j, goal in enumerate(stage.goals, 1)
                if goal.kind in TALKS
                and (done_node(q) if k == len(q.stages) else goal_node(goal_id(q, k, j))) == ending.node
            ),
            None,
        )
        if target is None or original.get("npc") != brief.dead:
            return None, [f"{ending.node} не является диалогом умершего в этом квесте"]
        mandatory = [copy.deepcopy(effect) for effect in original.get("effects", []) if not _reward(effect)]
        for effect in mandatory:
            if effect.get("do") == "journal":
                effect["text"] = p.journal_done
        if not any(effect.get("do") == "journal" for effect in mandatory):
            mandatory.append(
                {"do": "journal", "topic": q.title, "text": p.journal_done, "status": "LOG_RUNNING"}
            )
        picked = [copy.deepcopy(brief.offers()[effect].effect) for effect in p.effects]
        node = copy.deepcopy(original)
        node.update(
            npc=p.npc,
            description=p.description,
            lines=lines(Talk(p.description, p.lines)),
            effects=[*mandatory, *picked],
        )
        ops = [{"op": "add_node", "node": node}]
        caps = Caps(
            xp=sum(
                int(effect.get("amount", 0))
                for effect in original.get("effects", [])
                if effect.get("do") == "xp"
            ),
            items={
                GOLD: sum(
                    int(effect.get("count", 0)) for effect in original.get("effects", []) if _gold(effect)
                )
            },
        )
        try:
            checked = await self.director.with_symbols(story, names_in(ops))
        except (GameError, TimeoutError) as err:
            return None, [f"игра не ответила: {err}"]
        errors = validate(ops, checked, self.rules, repairs=q.id, caps=caps)
        if errors:
            return None, errors
        updated = copy.deepcopy(q)
        k, j = target
        goal = updated.stages[k - 1].goals[j - 1]
        goal.npc = p.npc
        # what the successor remembers after the talk: its own view, not the hero's journal
        took = f"Герой пришёл к тебе с поручением «{q.title}» умершего ({brief.dead_name}), которое ты принял"
        goal.talk = Talk(p.description, node["lines"], f"{took}: «{p.description}»")
        goal.journal = p.journal_done
        updated.names[p.npc] = checked.state.name(p.npc)
        updated.repairs.append(
            {"goal": goal_id(q, k, j), "dead": brief.dead, "npc": p.npc, "reason": p.reason}
        )
        if ending.node == done_node(q):
            updated.gold = sum(int(effect.get("count", 0)) for effect in picked if _gold(effect))
            updated.xp = sum(int(effect.get("amount", 0)) for effect in picked if effect.get("do") == "xp")
            updated.done_journal = p.journal_done
        try:
            await self.patches.make({"op": QUEST, "quest": updated.to_dict()}, ops)
        except (GameError, TimeoutError) as err:
            return None, [f"игра не приняла правку: {err}"]
        self.all()
        try:
            await self.link.story_effect(p.npc, [{"do": "journal", "topic": q.title, "text": p.journal_now}])
        except (GameError, TimeoutError) as err:
            log.warning("questbook: repair journal: %s", err)
        if self.dialogue is not None:
            self.dialogue.remember(
                p.npc,
                updated.name(p.npc),
                f"{brief.dead_name} мёртв. Ты принял его поручение герою «{q.title}»: "
                f"ждёшь, когда герой придёт с ним, в разговоре: «{p.description}».",
            )
        return Repair(q.id, "handover", p.npc, ending.node, ops), []

    # --- making -------------------------------------------------------------------------------

    async def ground(self, q: Quest) -> Ground:
        names = names_of(q)
        state = await self.link.story_state(vars=["XP_AMBIENT"], npcs=sorted(names), symbols=sorted(names))
        xp = int((state.get("vars") or {}).get("XP_AMBIENT") or XP_FALLBACK)
        places = self.places
        vanilla = q.requires or any(g.kind == VANILLA for s in q.stages for g in s.goals)
        return Ground(
            symbols=state.get("symbols") or {},
            states={n: str(s.get("state")) for n, s in (state.get("npcs") or {}).items()},
            plot=self.rules.plot_npc,
            point=lambda p: places is not None and places.of(p) is not None,
            gold=xp,
            xp=xp,
            quests=self.all(),
            story=await self.director.story() if vanilla else None,
            waypoint=lambda p: places is not None and places.is_waypoint(p),
            npc_crimes={str(f["id"]) for f in self.chronicle.facts("npc_crime")},
        )

    async def make(self, q: Quest, *, gold: int | None = None, xp: int | None = None) -> list[str]:
        """Check the quest, build it, make it: why not, in the words a model reads; empty: it is made.
        `gold`, `xp`: the most of the reward, the chapter's (XP_AMBIENT) if not given."""
        _, errors = await self.make_chain([q], gold=gold, xp=xp)
        return errors

    async def make_chain(
        self, quests: list[Quest], *, gold: int | None = None, xp: int | None = None
    ) -> tuple[list[Quest] | None, list[str]]:
        """Check every quest before the game sees any, then introduce their graph in one patch.

        The descriptions may refer to earlier quests in this list through `after`. Reward limits apply to
        each quest. A refusal leaves neither descriptions nor graph changes of the chain on the branch.
        """
        if not quests:
            return None, ["в цепочке нет квестов"]
        previous = dict(self.all())
        ops: list[Op] = []
        try:
            for q in quests:
                if q.id in previous:
                    return None, [f"квест {q.id} уже есть"]
                ground = await self.ground(q)
                ground.quests = dict(previous)
                if gold is not None:
                    ground.gold = gold
                if xp is not None:
                    ground.xp = xp
                errors = problems(q, ground)
                if errors:
                    return None, errors
                previous[q.id] = q
                ops.extend(build(q))
            story = await self.director.with_symbols(await self.director.story(), names_in(ops))
        except (GameError, TimeoutError) as err:
            return None, [f"игра не ответила: {err}"]
        errors = validate(
            ops, story, self.rules, caps=Caps(xp=ground.xp, items={GOLD: ground.gold}), book=True
        )
        if errors:
            return None, errors
        try:
            await self.patches.make([{"op": QUEST, "quest": q.to_dict()} for q in quests], ops)
        except GameError as err:
            return None, [f"игра не приняла правку: {err}"]
        for q in quests:
            self._made(q)
        return quests, []

    def _made(self, q: Quest) -> None:
        """The accepted description and what its giver knows, after the entire graph has been accepted."""
        if q.knows and self.dialogue is not None:
            self.dialogue.remember(q.giver, q.name(q.giver), q.knows)
        self.chronicle.record(
            {"fact": "story_quest", "id": q.id, "giver": q.giver, "title": q.title, "reason": q.reason}
        )
        self.journal.add("story_quest", npc=q.giver, name=q.name(q.giver), quest=q.id, title=q.title)
        log.info("questbook: %s gives %s «%s», %d stage(s)", q.giver, q.id, q.title, len(q.stages))

    # --- following -----------------------------------------------------------------------------

    def _on_message(self, msg: GameMessage) -> None:
        match msg:
            case Loading() | Session():
                self._generation += 1
                self._completed_goals.clear()
                self.dropping.clear()
                for task in tuple(self._blame_tasks):
                    task.cancel()
                self._version = -1
            case DialogChosen(made=True):
                for q in self.all().values():
                    if (dialog := q.dialogs().get(msg.node)) is not None:
                        self._chosen(q, msg.node, *dialog)
            case TriggerFired():
                for q in self.all().values():
                    self._fired(q, msg.id)

    def _on_fact(self, fact: Fact) -> None:
        if fact.get("fact") != "blamed":
            return
        task = asyncio.create_task(self._settle_blame(list(self.all().values())))
        self._blame_tasks.add(task)
        task.add_done_callback(self._blame_tasks.discard)

    async def _settle_blame(self, quests: list[Quest]) -> None:
        """A belief satisfies the task, even a lie. The stage graph still advances and pays normally."""
        generation = self._generation
        async with self._blame_lock:
            if generation != self._generation:
                return
            for q in quests:
                p = self.progress(q.id)
                if p.state != "given" or not 1 <= p.stage <= len(q.stages):
                    continue
                goals = [(j, g) for j, g in enumerate(q.stages[p.stage - 1].goals, 1) if g.kind == BLAME]
                if not goals:
                    continue
                facts = self.chronicle.facts()
                began = max((i for i, f in enumerate(facts) if f.get("id") == q.id and (
                    f.get("fact") == "story_quest_given" and p.stage == 1
                    or f.get("fact") == "story_quest_stage" and f.get("stage") == p.stage
                )), default=len(facts))
                names = [q.id, stage_var(q), *(goal_id(q, p.stage, j) for j, _ in goals)]
                try:
                    state = (await self.link.story_state(vars=names)).get("vars") or {}
                    if generation != self._generation:
                        return
                    if state.get(q.id) != LOG_RUNNING or state.get(stage_var(q)) != p.stage:
                        continue
                    for j, goal in goals:
                        gid = goal_id(q, p.stage, j)
                        if state.get(gid):
                            continue
                        belief = next((f for f in facts[began + 1:] if f.get("fact") == "blamed"
                                       and f.get("crime") == goal.crime and f.get("npc") == q.giver), None)
                        if belief is None:
                            continue
                        recorded = any(f.get("fact") == "story_quest_goal" and f.get("id") == q.id
                                       and f.get("goal") == gid for f in facts)
                        effects: list[dict[str, Any]] = [{"do": "set", "var": gid, "value": 1}]
                        if goal.journal and not recorded:
                            effects.append({"do": "journal", "topic": q.title, "text": goal.journal,
                                            "status": "LOG_RUNNING"})
                        await self.link.story_effect(q.giver, effects)
                        if generation != self._generation:
                            return
                        if not recorded:
                            self._fact(q, "story_quest_goal", stage=p.stage, goal=gid,
                                       crime=goal.crime, blamed=belief.get("id", ""))
                except (GameError, TimeoutError) as err:
                    log.warning("questbook: blame goal: %s", err)

    def _chosen(self, q: Quest, node: str, npc: str, talk: Talk | None) -> None:
        if node == give_node(q) and self.progress(q.id).state == "open":
            self._fact(q, "story_quest_given")
        elif node == done_node(q) and self.progress(q.id).state == "given":
            self._fact(q, "story_quest_done")
            self._outcome(q, npc, "amended" if self._terms(q) else "done", "full")
        if talk is not None and talk.remember and self.dialogue is not None:
            self.dialogue.remember(npc.upper(), q.name(npc.upper()), talk.remember)

    def _fired(self, q: Quest, id: str) -> None:
        for k in range(1, len(q.stages)):
            if id == stage_trigger(q, k):
                self._fact(q, "story_quest_stage", stage=k + 1)
        if id == late_trigger(q) and self.progress(q.id).state == "given":
            self._fact(q, "story_quest_failed")

    def _fact(self, q: Quest, kind: str, **data: Any) -> None:
        fact: Fact = {"fact": kind, "id": q.id, **data}
        self.chronicle.record(fact)
        self.journal.add(kind, npc=q.giver, name=q.name(q.giver), quest=q.id, title=q.title, **data)
        log.info("questbook: %s «%s»: %s %s", q.id, q.title, kind, data or "")

    async def settle(self, npc: str | None = None) -> None:
        """The truth from the save: the quests' variables in the game, and what the chronicle missed of them
        (a message lost while the server was away)."""
        mine = [
            q
            for q in self.all().values()
            if npc is None or q.giver == npc.upper() or any(r["npc"] == npc.upper() for r in q.repairs)
        ]
        if not mine:
            return
        await self._settle_blame(mine)
        names = [v for q in mine for v in (q.id, stage_var(q), *self._goals(q))]
        generation = self._generation
        try:
            vars = (await self.link.story_state(vars=names)).get("vars") or {}
        except (GameError, TimeoutError) as err:
            log.warning("questbook: the quests' state: %s", err)
            return
        if generation != self._generation:
            return
        self._completed_goals.update(
            gid for q in mine for gid in self._goals(q) if vars.get(gid, 0) >= 1
        )
        for q in mine:
            p, status, stage = self.progress(q.id), vars.get(q.id), vars.get(stage_var(q))
            if status in (LOG_RUNNING, LOG_SUCCESS, LOG_FAILED, LOG_OBSOLETE) and p.state == "open":
                self._fact(q, "story_quest_given")
            if isinstance(stage, int) and 1 < stage <= len(q.stages) and stage > p.stage:
                self._fact(q, "story_quest_stage", stage=stage)
            if status == LOG_SUCCESS and p.state != "done":
                self._fact(q, "story_quest_done")
            elif status == LOG_FAILED and p.state != "failed":
                self._fact(q, "story_quest_failed")
            elif status == LOG_OBSOLETE and p.state != "obsolete":
                self._fact(q, "story_quest_obsolete")

    # --- the conversation (TurnExtension) ------------------------------------------------------

    async def prepare(self, npc: str) -> None:
        await self.settle(npc)

    def _terms(self, q: Quest) -> list[Fact]:
        return [f for f in self.chronicle.facts("quest_terms") if f.get("quest") == q.id]

    def _goals(self, q: Quest) -> dict[str, Goal]:
        return {
            goal_id(q, k, j): goal
            for k, stage in enumerate(q.stages, 1)
            for j, goal in enumerate(stage.goals, 1)
        }

    def _done_goals(self, q: Quest) -> set[str]:
        done = self._completed_goals | {
            f["id"] for f in self.chronicle.facts("triggered") if f.get("count", 0) >= 1
        }
        done.update(f["goal"] for f in self.chronicle.facts("story_quest_goal") if f.get("id") == q.id)
        told = Patches.of(self.patches.ops()).told
        done.update(gid for gid in self._goals(q) if goal_node(gid) in told)
        return done

    def _removable(self, q: Quest) -> dict[str, Goal]:
        dropped = {id for f in self._terms(q) for id in f["drop"]}
        done = self._done_goals(q)
        return {
            goal_id(q, k, j): goal
            for k, stage in enumerate(q.stages, 1)
            for j, goal in enumerate(stage.goals, 1)
            if k >= self.progress(q.id).stage
            and goal.kind != VANILLA
            and goal is not q.turn_in()
            and goal_id(q, k, j) not in dropped
            and goal_id(q, k, j) not in done
        }

    def _goal_name(self, q: Quest, goal: Goal) -> str:
        item = q.name(goal.item)
        if goal.count > 1:
            item += f" ×{goal.count}"
        match goal.kind:
            case "reach":
                place = self.places.of(goal.point) if self.places and goal.point else None
                return f"дойти до {place.ru if place else goal.point or q.name(goal.npc)}"
            case "bring":
                return f"принести {item} ({q.name(goal.npc)})"
            case "have":
                return f"добыть {item}"
            case "talk":
                return f"поговорить с {q.name(goal.npc)}: {goal.talk.description if goal.talk else ''}"
            case "kill":
                return f"убить: {', '.join(q.name(npc) for npc in goal.npcs)}"
            case "wait":
                return f"подождать {goal.days} дн."
            case "blame":
                return f"назвать виновного в деле {goal.crime}"
            case _:
                raise ValueError(f"Goal cannot be amended: {goal.kind}")

    def _outcome(self, q: Quest, npc: str, how: str, pay: str, why: str | None = None) -> None:
        terms = self._terms(q)
        self.chronicle.record(
            {
                "fact": "quest_outcome",
                "quest": q.id,
                "npc": npc,
                "how": how,
                "drop": list(dict.fromkeys(id for f in terms for id in f["drop"])),
                "why": [f["why"] for f in terms] + ([why] if why is not None else []),
                "pay": pay,
            }
        )

    def situation(self, npc: str) -> str | None:
        npc = npc.upper()
        out = []
        for q in self.all().values():
            inherited = [r for r in q.repairs if r["npc"] == npc]
            if q.giver != npc and not inherited:
                continue
            p = self.progress(q.id)
            if p.state == "given":
                if inherited:
                    dead = ", ".join(dict.fromkeys(q.name(r["dead"]) for r in inherited))
                    pending = [
                        g.talk.description
                        for k, stage in enumerate(q.stages, 1)
                        for j, g in enumerate(stage.goals, 1)
                        if g.talk is not None and any(r["goal"] == goal_id(q, k, j) for r in inherited)
                    ]
                    line = (
                        f"Ты принял поручение умершего ({dead}) «{q.title}» (id {q.id}): "
                        f"{q.asked or q.stages[0].journal} Ждёшь героя: {'; '.join(pending)}."
                    )
                else:
                    line = f"Ты дал герою поручение «{q.title}» (id {q.id}): {q.asked or q.stages[0].journal}"
                if q.turn_in() is not None and q.turn_in().npc == npc:
                    line += f" Сделает — заплатишь: {q.reward()}."
                if q.days:
                    line += f" Срок — {q.days} дн."
                out.append(line)
                goals = self._goals(q)
                for term in self._terms(q):
                    names = ", ".join(self._goal_name(q, goals[id]) for id in term["drop"])
                    out.append(f"Снято тобой: {names} — «{term['why']}»")
                removable = self._removable(q)
                if removable:
                    out.append(
                        "Можешь снять цели (amend_quest): "
                        + "; ".join(f"{id} — {self._goal_name(q, goal)}" for id, goal in removable.items())
                    )
            elif p.state == "done":
                line = f"Поручение «{q.title}» герой выполнил"
                if q.turn_in() is not None and q.turn_in().npc == npc:
                    line += f", ты заплатил: {q.reward()}"
                out.append(line + ".")
            elif p.state == "failed":
                repaired = any(f.get("quest") == q.id for f in self.chronicle.facts("repair"))
                out.append(f"Поручение «{q.title}» герой не выполнил{'' if repaired else ' в срок'}.")
            elif p.state == "obsolete":
                out.append(f"Поручение «{q.title}» потеряло смысл и закрыто.")
        return "\n".join(out) or None

    all_tools: tuple[Tool, ...] = (AMEND_TOOL, DROP_TOOL)

    def tools(self, npc: str) -> list[Tool]:
        npc = npc.upper()
        quests = [
            q for q in self.all().values()
            if self.progress(q.id).state == "given"
            and q.id not in self.dropping
            and (q.giver == npc or any(r["npc"] == npc for r in q.repairs))
        ]
        tools = [AMEND_TOOL] if any(self._removable(q) for q in quests) else []
        if quests:
            tools.append(DROP_TOOL)
        return tools

    async def handle(
        self, dialogue: DialogueManager, conv: Conversation, call: ToolCall, snap: Snapshot | None
    ) -> ToolOutcome | None:
        if call.name not in (AMEND_TOOL.name, DROP_TOOL.name):
            return None
        q = self.all().get(str(call.arguments.get("quest", "")).upper())
        if q is None or (q.giver != conv.npc and not any(r["npc"] == conv.npc for r in q.repairs)):
            return ToolOutcome("Отклонено: такого дела ты от героя не ждёшь.", "rejected")
        if self.progress(q.id).state != "given":
            return ToolOutcome("Отклонено: ты не давал герою этого поручения.", "rejected")
        if q.id in self.dropping:
            return ToolOutcome("Отклонено: поручение уже отменяется.", "rejected")
        why = str(call.arguments.get("why", ""))
        if call.name == AMEND_TOOL.name:
            return await self._amend(q, conv, call.arguments.get("drop", []), why)

        async def drop() -> tuple[str, bool]:
            return await self._drop(q, conv, why)

        self.dropping.add(q.id)
        dialogue.after_speech(conv, drop)
        return ToolOutcome("Принято: поручение закроется без награды после реплики.", "accepted")

    async def _amend(self, q: Quest, conv: Conversation, drop: Any, why: str) -> ToolOutcome:
        if not isinstance(drop, list) or not drop or not all(isinstance(d, str) for d in drop):
            return ToolOutcome("Отклонено: укажи id снимаемых пунктов из обстановки.", "rejected")
        ids = list(dict.fromkeys(d.upper() for d in drop))
        goals, removable = self._goals(q), self._removable(q)
        done = self._done_goals(q)
        for id in ids:
            if id not in removable:
                why_not = "этот пункт не снимается" if id in goals else "такого пункта нет в уговоре"
                if id in goals and id in done:
                    why_not = "эта цель уже выполнена"
                return ToolOutcome(f"Отклонено: {id} — {why_not}.", "rejected")
        try:
            await self.link.story_effect(conv.npc, [{"do": "set", "var": id, "value": 1} for id in ids])
        except (GameError, TimeoutError) as err:
            return ToolOutcome(f"Отклонено: игра не приняла правку: {err}", "rejected")
        fact = {
            "fact": "quest_terms", "quest": q.id, "npc": conv.npc,
            "name": conv.persona.name, "drop": ids, "why": why,
        }
        self.chronicle.record(fact)
        self.journal.add("quest_amended", **{k: v for k, v in fact.items() if k != "fact"})
        return ToolOutcome(
            "Принято: ты снял пункты: " + ", ".join(self._goal_name(q, goals[id]) for id in ids) + ".",
            "accepted",
        )

    async def _drop(self, q: Quest, conv: Conversation, why: str) -> tuple[str, bool]:
        effects = [
            {"do": "set", "var": q.id, "value": LOG_OBSOLETE},
            {"do": "journal", "topic": q.title, "text": "", "status": "LOG_OBSOLETE"},
        ]
        try:
            await self.link.story_effect(conv.npc, effects)
        except (GameError, TimeoutError) as err:
            self.dropping.discard(q.id)
            return f"Ты не смог отменить поручение «{q.title}»: {err}", False
        self._outcome(q, conv.npc, "dropped", "none", why)
        self.journal.add(
            "quest_dropped", npc=conv.npc, name=conv.persona.name, quest=q.id, title=q.title, why=why
        )
        return f"Ты отменил поручение «{q.title}»: оно закрыто без награды.", True
