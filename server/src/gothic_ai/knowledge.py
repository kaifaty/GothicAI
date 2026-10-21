"""What an npc knows of the world (M6, docs/npc-agent.md «Знания»), from the scripts' story graph
(gamedata/storygraph.py), as far as the story has got.

An npc knows its own dialogs, the quests they move or read, and the talk of its guild: the ambient dialogs
the scripts give nameless people («Что новенького?», lines by chapter). What it may know is filtered by
where the story is, not by the situation. The chapter, quest variables, the other npc's dialogs the hero
has heard and who is dead come from the game (story_state). Items, distances, the time, aivars and the
like are unknown and hide nothing. So each of its dialogs is
- told: the hero has heard it, the npc said it to him;
- open: by the story its condition is not false, the npc could tell it if asked;
- hidden: the future (a later chapter, the reward for a quest not begun).

Whether the hero has heard one of the npc's own dialogs is the flow of their talk, not knowledge: Lobart
knows of his turnip field before the hero says hello. That he has heard another npc's dialog is the story.

The conversational model sees the gist in the situation of every turn (a TurnExtension) and gets the
rest through recall.
"""

from __future__ import annotations

import logging
import re
from collections import Counter
from collections.abc import Callable, Collection
from dataclasses import dataclass, field
from typing import TYPE_CHECKING, Any

from .gamedata.storygraph import QUEST_STATES, Expr, Node, Quest, StoryGraph, walk
from .npc_tools import guild_name
from .validator import Patches

if TYPE_CHECKING:
    from .dialogue import Conversation, DialogueManager
    from .game import GameLink
    from .llm import Tool, ToolCall
    from .protocol import Snapshot
    from .story import StoryPatches
    from .turn import ToolOutcome

log = logging.getLogger(__name__)

HERO = ("OTHER", "HERO")
AMBIENT_NPC = "$INSTANCE_HELP"  # ambient dialogs: the scripts hand them to people at runtime
FN_DEPTH = 6

# the prefixes of the ambient dialogs the scripts give a guild's people, by world (B_AssignAmbientInfos)
AMBIENT = {
    "NEWWORLD": {
        1: ["PAL"],
        2: ["MIL"],
        3: ["VLK"],
        5: ["NOV"],
        7: ["SLD"],
        8: ["BAU"],
        9: ["BDT"],
        12: ["OUT"],
    },
    "OLDWORLD": {1: ["OCPAL", "OWPAL"], 3: ["OCVLK"], 9: ["BDT"], 10: ["STRF"]},
    "ADDONWORLD": {9: ["BL_BDT", "TAL_BDT"]},
}
_AMBIENT_ID = re.compile(r"DIA_(?:ADDON_)?([A-Z_]+?)_(\d+)_")
_WORDS = re.compile(r"\w+")

STATE_WORDS = {
    "running": "идёт",
    "success": "выполнено",
    "failed": "провалено",
    "obsolete": "больше не нужно",
}

TOLD_MAX, OPEN_MAX, QUESTS_MAX, RUMORS_MAX = 10, 12, 6, 2
GIST_TOLD, GIST_OPEN = 150, 90
RECALL_NODES, RECALL_NODE_CHARS = 3, 900


FIELDS = [("SELF", "GUILD"), ("SELF", "VOICE"), ("OTHER", "GUILD")]  # read from the game every turn


@dataclass
class StoryState:
    """The story as the game has it now: what the filter reads."""

    vars: dict[str, int] = field(default_factory=dict)
    knows: dict[str, bool] = field(default_factory=dict)  # dialog -> the hero has heard it
    npcs: dict[str, dict[str, Any]] = field(default_factory=dict)  # symbol -> {"state": alive|dead|absent}
    fields: dict[tuple[str, str], int] = field(default_factory=dict)  # ("SELF" | "HERO", "GUILD") -> value

    @property
    def chapter(self) -> int | None:
        return self.vars.get("KAPITEL")


Value = int | str | None


class Partial:
    """A story graph expression over the story only: three-valued, None is "can't say from the story".
    Whether the hero has heard one of the npc's own dialogs (`own`) is `flow`'s to say: the flow of its talk
    with the hero, not the story (see Knowledge.view)."""

    def __init__(
        self,
        graph: StoryGraph,
        state: StoryState,
        own: Collection[str] = (),
        flow: Callable[[str], int | None] | None = None,
    ) -> None:
        self.graph, self.state, self.own, self.flow = graph, state, set(own), flow

    def truth(self, e: Expr | None) -> bool | None:
        """False only when the story rules it out; no condition is true."""
        if e is None:
            return True
        v = self.value(e)
        return None if not isinstance(v, int) else v != 0

    def value(self, e: Any, params: tuple[Value, ...] = (), depth: int = 0) -> Value:
        if not isinstance(e, list) or not e:
            return None
        match e:
            case ["int", v, *_]:
                return int(v)
            case ["str", text, *_]:
                return str(text)
            case ["var", name]:
                return self.state.vars.get(name)
            case ["inst", name]:
                return self.graph.npc(name)
            case ["sym", name]:
                return str(name)
            case ["field", inst, name]:
                who = self.value(inst, params, depth)
                return self.state.fields.get(("HERO" if who in HERO else str(who), name))
            case ["param", i]:
                return params[i] if 0 <= i < len(params) else None
            case ["call", fn, *args]:
                return self._call(fn, [self.value(a, params, depth) for a in args])
            case ["fn", fn, *args]:
                body = self.graph.functions.get(fn, {})
                if depth >= FN_DEPTH or not body.get("lifted") or "body" not in body:
                    return None
                return self.value(body["body"], tuple(self.value(a, params, depth) for a in args), depth + 1)
            case ["cases", *pairs]:
                for i in range(0, len(pairs) - 1, 2):
                    g = self.value(pairs[i], params, depth)
                    if not isinstance(g, int):
                        return None
                    if g:
                        return self.value(pairs[i + 1], params, depth)
                return 0
            case ["&&", *args]:
                vs = [self.value(a, params, depth) for a in args]
                if any(v == 0 for v in vs if isinstance(v, int)):
                    return 0
                return 1 if all(isinstance(v, int) for v in vs) else None
            case ["||", *args]:
                vs = [self.value(a, params, depth) for a in args]
                if any(isinstance(v, int) and v != 0 for v in vs):
                    return 1
                return 0 if all(isinstance(v, int) for v in vs) else None
            case ["!", a]:
                v = self.value(a, params, depth)
                return int(v == 0) if isinstance(v, int) else None
            case ["neg", a]:
                v = self.value(a, params, depth)
                return -v if isinstance(v, int) else None
            case [op, a, b]:
                return _binary(op, self.value(a, params, depth), self.value(b, params, depth))
        return None  # fields, arrays, locals, unknowns

    def _call(self, fn: str, args: list[Value]) -> Value:
        match fn, args:
            case "NPC_KNOWSINFO", [who, info] if who in HERO and isinstance(info, str):
                if info in self.own:
                    return self.flow(info) if self.flow is not None else None
                return int(self.state.knows[info]) if info in self.state.knows else None
            case "NPC_ISDEAD", [npc] if isinstance(npc, str):
                state = self.state.npcs.get(npc, {}).get("state")
                return {"dead": 1, "alive": 0}.get(state) if isinstance(state, str) else None
        return None


_COMPARE = {
    "==": lambda a, b: a == b,
    "!=": lambda a, b: a != b,
    "<": lambda a, b: a < b,
    ">": lambda a, b: a > b,
    "<=": lambda a, b: a <= b,
    ">=": lambda a, b: a >= b,
}
_ARITHMETIC = {
    "+": lambda a, b: a + b,
    "-": lambda a, b: a - b,
    "*": lambda a, b: a * b,
    "/": lambda a, b: int(a / b) if b else None,
    "%": lambda a, b: a % b if b else None,
    "&": lambda a, b: a & b,
    "|": lambda a, b: a | b,
}


def _binary(op: str, a: Value, b: Value) -> Value:
    if op in _COMPARE and isinstance(a, str) and isinstance(b, str):
        return int(_COMPARE[op](a, b)) if op in ("==", "!=") else None
    if not isinstance(a, int) or not isinstance(b, int):
        return None
    if op in _COMPARE:
        return int(_COMPARE[op](a, b))
    if op in _ARITHMETIC:
        return _ARITHMETIC[op](a, b)
    return None


# --- what an npc knows -------------------------------------------------------------------------


@dataclass
class Atoms:
    """What the filter reads from the game for some expressions."""

    vars: set[str] = field(default_factory=set)
    knows: set[str] = field(default_factory=set)
    npcs: set[str] = field(default_factory=set)

    def add(self, graph: StoryGraph, e: Expr | None, seen: set[str] | None = None) -> None:
        seen = set() if seen is None else seen
        for x in walk(e):
            match x:
                case ["var", name]:
                    self.vars.add(name)
                case ["call", "NPC_KNOWSINFO", _, ["sym", info]]:
                    self.knows.add(info)
                case ["call", "NPC_ISDEAD", ["inst", name]]:
                    self.npcs.add(graph.npc(name))
                case ["fn", fn, *_] if fn not in seen:
                    seen.add(fn)
                    self.add(graph, graph.functions.get(fn, {}).get("body"), seen)


Line = tuple[str, str, bool]  # who ("Ты" | "Герой" | "Кто-то"), subtitle, said for sure


@dataclass
class Topic:
    """A dialog as the npc knows it now: its lines (who, subtitle) whose guards the story allows, and the
    answers it offers the hero with theirs."""

    node: Node
    told: bool
    sure: bool  # the story lets it be said now; not sure: it waits for something the story can't tell
    lines: list[Line]
    choices: list[tuple[Node, list[Line]]] = field(default_factory=list)

    def said(self) -> str:
        """What the npc says for sure: no lines that wait for the hero's items, guild, the time..."""
        return " ".join(text for who, text, sure in self.lines if who == "Ты" and sure)


@dataclass
class View:
    """What an npc knows, as of its last turn."""

    npc: str
    state: StoryState
    guild: int | None
    told: list[Topic]
    open: list[Topic]
    quests: list[tuple[str, str, str]]  # title, state, gist (a journal entry of what it told)
    rumors: list[str]
    pool: list[Topic]  # the guild's ambient dialogs


class Knowledge:
    """The npc's knowledge for the conversational model: a TurnExtension with situation lines and recall."""

    def __init__(self, link: GameLink, graph: StoryGraph, patches: StoryPatches | None = None) -> None:
        self.link = link
        self.graph = graph
        self.patches = patches
        self.views: dict[str, View] = {}
        self._topic_uses = Counter(t for q in graph.quests.values() for t in q.topics)
        self._ambient: dict[str, list[Node]] = {}  # "VLK_6" -> its ambient dialogs
        for n in graph.nodes.values():
            if n.npc == AMBIENT_NPC and not n.is_choice and (m := _AMBIENT_ID.match(n.id)):
                self._ambient.setdefault(f"{m.group(1)}_{m.group(2)}", []).append(n)

    # --- TurnExtension ---------------------------------------------------------------------------

    async def prepare(self, npc: str) -> None:
        npc = npc.upper()
        # its guild and voice (which ambient dialogs are its kind's), the hero's guild (lines wait for it)
        fields = [["field", ["inst", who], name] for who, name in FIELDS]
        values = [v if isinstance(v, int) else None for v in await self.link.story_eval(npc, fields)]
        known = {
            (who if who == "SELF" else "HERO", name): v
            for (who, name), v in zip(FIELDS, values, strict=False)
            if v is not None
        }
        guild, voice = known.get(("SELF", "GUILD")), known.get(("SELF", "VOICE"))
        own = self.dialogs(npc)
        pool = self.ambient(guild, voice, named=bool(own))
        quests = self.quests(npc)
        atoms = Atoms(vars={"KAPITEL"}, knows={n.id for n in own})
        for node in [*own, *pool, *(c for n in own for c in self._choices(n))]:
            atoms.add(self.graph, node.condition)
            for e in node.effects:
                atoms.add(self.graph, e.get("when"))
        atoms.vars |= {q.var for q in quests}
        raw = await self.link.story_state(
            vars=sorted(atoms.vars), knows=sorted(atoms.knows), npcs=sorted(atoms.npcs)
        )
        state = StoryState(
            vars={k: int(v) for k, v in raw.get("vars", {}).items() if isinstance(v, int)},
            knows={k: bool(v) for k, v in raw.get("knows", {}).items()},
            npcs=dict(raw.get("npcs", {})),
            fields=known,
        )
        self.views[npc] = self.view(npc, state, guild, own, pool, quests)

    def situation(self, npc: str) -> str | None:
        view = self.views.get(npc.upper())
        return None if view is None else brief(view)

    all_tools: tuple[Tool, ...] = ()

    def tools(self, npc: str) -> list[Tool]:
        return []

    async def handle(
        self, dialogue: DialogueManager, conv: Conversation, call: ToolCall, snap: Snapshot | None
    ) -> ToolOutcome | None:
        return None

    def recall(self, npc: str, query: str) -> str | None:
        """The topics that fit `query` in full: the npc's dialogs it told or could tell, its guild's talk."""
        view = self.views.get(npc.upper())
        if view is None:
            return None
        words = roots(query)
        scored = []
        for i, topic in enumerate([*view.told, *view.open, *view.pool]):
            text = _topic_text(topic).lower()
            score = sum(1 for w in words if w in text)
            if score:
                scored.append((-score, i, topic))
        if not scored:
            return None
        parts = [_recall_text(t) for _, _, t in sorted(scored, key=lambda s: (s[0], s[1]))]
        return "Ты знаешь:\n" + "\n\n".join(parts[:RECALL_NODES])

    # --- the index ----------------------------------------------------------------------------

    def dialogs(self, npc: str) -> list[Node]:
        """The npc's enabled dialogs that say something: no exits, pickpocketing or trade menus."""
        disabled = self._disabled()
        return [n for n in self.graph.dialogs_of(npc) if n.id not in disabled and not _menu_only(n)]

    def _disabled(self) -> set[str]:
        if self.patches is None:
            return set()
        return Patches.of([*self.patches.static, *self.patches.ops()]).disabled

    def ambient(self, guild: int | None, voice: int | None, *, named: bool) -> list[Node]:
        """The ambient dialogs of the npc's kind: its own voice for a nameless one, its whole guild's for a
        named one (it has heard them about)."""
        if guild is None:
            return []
        world = (self.link.world or "NEWWORLD").upper().removesuffix(".ZEN")
        prefixes = AMBIENT.get(world, AMBIENT["NEWWORLD"]).get(guild, [])
        keys = [k for k in self._ambient if any(k.rsplit("_", 1)[0] == p for p in prefixes)]
        own_voice = [k for k in keys if voice is not None and k.endswith(f"_{voice}")]
        nodes = [n for k in sorted(own_voice if own_voice and not named else keys) for n in self._ambient[k]]
        return [n for n in nodes if not _menu_only(n)]

    def quests(self, npc: str) -> list[Quest]:
        """Quests its dialogs move or read."""
        npc = npc.upper()
        reads = {v for n in self.graph.dialogs_of(npc) for v in n.reads}
        found = {q.var: q for q in self.graph.quests_of(npc)}
        found.update({v: self.graph.quests[v] for v in reads if v in self.graph.quests})
        return [found[v] for v in sorted(found)]

    def view(
        self,
        npc: str,
        state: StoryState,
        guild: int | None,
        own: list[Node],
        pool: list[Node],
        quests: list[Quest],
    ) -> View:
        disabled = self._disabled()
        own = [n for n in own if n.id not in disabled]
        pool = [n for n in pool if n.id not in disabled]
        gates: dict[str, bool | None] = {}

        def gate(id: str) -> bool | None:
            """Whether the story lets the npc say its dialog: True if it has, False if not yet."""
            if id not in gates:
                gates[id] = None  # dialogs that wait for each other: can't say
                told = state.knows.get(id, False)
                gates[id] = True if told else partial.truth(self.graph.nodes[id].condition)
            return gates[id]

        def flow(id: str) -> int | None:
            # the hero has heard it; or it is out of reach by the story, and so is all that waits for it
            return 1 if state.knows.get(id, False) else 0 if gate(id) is False else None

        partial = Partial(self.graph, state, {n.id for n in own}, flow)

        def topic(n: Node, told: bool, sure: bool) -> Topic:
            choices = [(c, _lines(c, partial)) for c in self._choices(n, disabled)]
            return Topic(n, told, sure, _lines(n, partial), choices)

        told: list[Topic] = []
        open: list[Topic] = []
        for n in own:
            if state.knows.get(n.id, False):
                told.append(topic(n, True, True))
            elif (can := gate(n.id)) is not False:
                # not sure: it waits for the hero to bring something (the reward for the turnips), or it is
                # what the npc starts by itself when it sees something (its clothes on the hero); what only
                # waits for the talk to get there, the npc can tell
                waits = n.important or self._waits_for_items(n.condition)
                open.append(topic(n, False, can is True or not waits))
        open.sort(key=lambda t: not t.sure)  # stable: menu order within each
        topics = [topic(n, False, True) for n in pool if partial.truth(n.condition) is not False]
        # «what's new» of its kind: this chapter's lines first, then those the scripts pick at random
        news = [
            (not sure, text)
            for t in topics
            if "_STANDARD" in t.node.id
            for who, text, sure in t.lines
            if who == "Ты"
        ]
        rumors = list(dict.fromkeys(text for _, text in sorted(news, key=lambda n: n[0])))
        return View(
            npc=npc.upper(),
            state=state,
            guild=guild,
            told=told,
            open=open,
            quests=self._quest_lines(quests, state, told, partial),
            rumors=rumors[:RUMORS_MAX],
            pool=topics,
        )

    def _waits_for_items(self, e: Expr | None, seen: set[str] | None = None) -> bool:
        """Whether a condition asks what the hero has on him, also in the script functions it calls."""
        seen = set() if seen is None else seen
        for x in walk(e):
            match x:
                case ["call", "NPC_HASITEMS", ["inst", who], *_] if who in HERO:
                    return True
                case ["fn", fn, *_] if fn not in seen:
                    seen.add(fn)
                    if self._waits_for_items(self.graph.functions.get(fn, {}).get("body"), seen):
                        return True
        return False

    def _choices(self, node: Node, disabled: Collection[str] = ()) -> list[Node]:
        """The answers the dialog offers the hero, and theirs, in order."""
        out: list[Node] = []
        todo = list(node.choices())
        while todo:
            id = todo.pop(0)
            if id not in disabled and id in self.graph.nodes and all(c.id != id for c in out):
                choice = self.graph.nodes[id]
                out.append(choice)
                todo += choice.choices()
        return out

    def _quest_lines(
        self, quests: list[Quest], state: StoryState, told: list[Topic], partial: Partial
    ) -> list[tuple[str, str, str]]:
        out = []
        for q in quests:
            word = STATE_WORDS.get(QUEST_STATES.get(state.vars.get(q.var, 0), ""), "")
            if not word:
                continue
            entries = [
                e["text"]
                for t in told
                for n in [t.node, *(c for c, _ in t.choices)]
                for e in n.effects
                if e["do"] == "journal_entry"
                and e.get("topic") in q.topics
                and e.get("text")
                and partial.truth(e.get("when")) is not False
            ]
            out.append((self._title(q), word, entries[-1] if entries else ""))
        return out[:QUESTS_MAX]

    def _title(self, q: Quest) -> str:
        """Its name in the hero's journal: its least shared topic (the militia's is everyone's)."""
        titles = sorted((self._topic_uses.get(t, 0), title) for t, title in q.topics.items() if title)
        return titles[0][1] if titles else q.var


# --- rendering ---------------------------------------------------------------------------------


def brief(view: View) -> str:
    """The gist for the situation: kept short, the topics in full come through recall."""
    chapter = view.state.chapter
    out = [
        "Что ты знаешь"
        + (f" (сейчас глава {chapter}; что будет дальше, ты не знаешь)" if chapter else "")
        + ":"
    ]
    if view.guild:
        out.append(f"Ты — {guild_name(view.guild)}.")
    if view.told:
        out.append("Ты уже говорил с героем (он это слышал):")
        out += [_topic_line(t, GIST_TOLD) for t in view.told[:TOLD_MAX]]
    if view.open:
        out.append("Если герой спросит или скажет такое, тебе есть что ответить:")
        topics = [t for t in view.open if t.sure or t.node.description]  # what it might start: unsaid
        out += [_topic_line(t, GIST_OPEN) for t in topics[:OPEN_MAX]]
    if view.quests:
        out.append("Дела, о которых ты знаешь:")
        for title, word, gist in view.quests:
            out.append(f"- «{title}»: {word}" + (f". Суть: {gist}" if gist else ""))
    if view.rumors:
        out.append("Что говорят вокруг: " + " ".join(f"«{r}»" for r in view.rumors))
    if len(out) == 1:
        return out[0] + " ничего особенного, только то, что видишь вокруг."
    out.append("Подробности тем — через recall.")
    return "\n".join(out)


def _topic_line(topic: Topic, limit: int) -> str:
    node = topic.node
    head = f"«{_asked(node)}»" if node.description else "(ты заговариваешь сам)"
    said = topic.said() if topic.sure else ""
    return f"- {head} — {_cut(said, limit)}" if said else f"- {head}"


def _recall_text(topic: Topic) -> str:
    node = topic.node
    kind = "герой это уже слышал от тебя" if topic.told else "ты можешь это рассказать"
    if node.npc == AMBIENT_NPC:
        kind = "так говорят такие, как ты"
    head = f"[«{_asked(node)}» — {kind}]" if node.description else f"[ты заговариваешь сам — {kind}]"
    lines = [f"{who}: «{text}»" for who, text, _ in topic.lines]
    for choice, said in topic.choices:
        words = [
            f"«{text}»" if who == "Ты" else f"{who}: «{text}»"
            for who, text, _ in said
            if not (who == "Герой" and text == choice.description)
        ]
        if words:
            lines.append(f"Если герой ответит «{choice.description}», ты: " + " ".join(words))
    return _cut(head + "\n" + "\n".join(lines), RECALL_NODE_CHARS)


def _topic_text(topic: Topic) -> str:
    parts = []
    for n, lines in [(topic.node, topic.lines), *topic.choices]:
        parts += [n.description, *(t for _, t, _ in lines)]
        parts += [e.get("text", "") for e in n.effects if e["do"] == "journal_entry"]
    return " ".join(p for p in parts if p)


def _lines(node: Node, partial: Partial) -> list[Line]:
    """Spoken lines whose guards the story does not rule out."""
    out = []
    for e in node.effects:
        if e["do"] == "line" and e.get("text") and (said := partial.truth(e.get("when"))) is not False:
            out.append((_speaker(e.get("speaker")), e["text"], said is True))
    return out


def _speaker(e: Any) -> str:
    match e:
        case ["inst", "OTHER" | "HERO"]:
            return "Герой"
        case ["inst", "SELF"]:
            return "Ты"
    return "Кто-то"


def _asked(node: Node) -> str:
    """The hero's line in the menu, without the menu's marks: «Я немедленно отправляюсь в путь! (КОНЕЦ)»."""
    return re.sub(r"\s*\((КОНЕЦ|END)\)\s*$", "", node.description)


def _menu_only(node: Node) -> bool:
    return (
        node.trade
        or node.id.endswith("_EXIT")
        or "PICKPOCKET" in node.id
        or node.description.upper().startswith(("КОНЕЦ", "END"))
    )


def roots(text: str) -> set[str]:
    """Words without their endings, roughly: «репа», «репу» and «репы» meet in «реп»."""
    out = set()
    for w in _WORDS.findall(text.lower()):
        if len(w) >= 3:
            out.add(w[:-2] if len(w) >= 7 else w[:-1] if len(w) >= 4 else w)
    return out


def _cut(text: str, limit: int) -> str:
    if len(text) <= limit:
        return text
    return text[:limit].rsplit(" ", 1)[0].rstrip(",;:—-") + "…"
