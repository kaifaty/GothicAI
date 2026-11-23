"""The story layer of the game as a graph: dialogs, choices, quests and the variables between them.

`storygraph` (OpenGothic/ai_bridge/storygraph) lifts the imported program record into JSON, see
docs/story-graph.md. Nodes are dialogs (C_INFO) and the choices they offer; a dialog has a condition
(when it is offered) and both have effects (lines, variables, items, journal, choices...), each effect
with the guard it runs under.

Expressions are nested lists, the first element names the form:
`["int", 1, "LOG_RUNNING"]`, `["str", "text", "TOPIC_X"]`, `["var", "MIS_X"]`, `["var", "ARR", 3]`,
`["field", inst, "AIVAR", 12, "AIV_X"]`, `["inst", "OTHER"]`, `["sym", "DIA_X"]`, `["param", 0]`,
`["call", "NPC_KNOWSINFO", ...args]` (engine), `["fn", "C_BEKLAUEN", ...args]` (script function, its body
is in `functions`), `["cases", guard, value, ...]` (the first true guard picks the value, else 0),
`["unknown", why]`, and operators: `["==", a, b]`, `["&&", ...]`, `["!", a]`, `["neg", a]`, ...
"""

from __future__ import annotations

import hashlib
import json
import logging
import os
import subprocess
from collections.abc import Iterable, Iterator
from dataclasses import dataclass, field
from pathlib import Path

from gothic_ai.gamedata import base

log = logging.getLogger(__name__)

VERSION = 1
ALGORITHM_VERSION = 1
# the converter, built in the repository (docs/story-graph.md, stage 1)
REPO = Path(__file__).resolve().parents[4]
TOOL = REPO / "OpenGothic" / "ai_bridge" / "storygraph" / "build" / "storygraph"

Expr = list

QUEST_STATES = {1: "running", 2: "success", 3: "failed", 4: "obsolete"}  # LOG_RUNNING... in the scripts

_BINARY = {"==", "!=", "<", ">", "<=", ">=", "+", "-", "*", "/", "%", "&", "|", "<<", ">>", "&&", "||"}


@dataclass
class Node:
    id: str
    kind: str  # "dialog" or "choice"
    npc: str
    description: str
    effects: list[dict]
    writes: set[str]
    condition: Expr | None = None  # dialogs only
    lifted: bool = True
    not_lifted: str = ""
    nr: int = 0
    important: bool = False
    permanent: bool = False
    trade: bool = False
    reads: set[str] = field(default_factory=set)
    knows: set[str] = field(default_factory=set)
    condition_effects: list[dict] = field(default_factory=list)
    parents: list[str] = field(default_factory=list)  # choices only

    @property
    def is_choice(self) -> bool:
        return self.kind == "choice"

    def lines(self) -> list[tuple[str, str, str]]:
        """Spoken lines in code order: (speaker, line id, subtitle); the speaker is "self" or "other"."""
        out = []
        for e in self.effects:
            if e["do"] == "line":
                out.append((render(e["speaker"]), e["id"], e.get("text", "")))
        return out

    def choices(self) -> list[str]:
        return [e["node"] for e in self.effects if e["do"] == "choice"]


@dataclass
class Quest:
    var: str
    topics: dict[str, str]  # TOPIC_* -> journal title
    running: set[str]
    success: set[str]
    failed: set[str]
    obsolete: set[str]
    other: set[str]

    @property
    def title(self) -> str:
        return next(iter(self.topics.values()), "")

    def movers(self) -> set[str]:
        """Nodes that set the quest variable."""
        return self.running | self.success | self.failed | self.obsolete | self.other


@dataclass
class Var:
    readers: set[str]  # nodes whose conditions read it
    writers: set[str]  # nodes that may write it
    others: set[str]  # other script functions that write it (ZS_DEAD, B_KAPITELWECHSEL...)


@dataclass
class StoryGraph:
    nodes: dict[str, Node]
    functions: dict[str, dict]
    instances: dict[str, str]  # global instance variable -> npc instance (HILDA -> BAU_951_HILDA)
    vars: dict[str, Var]
    quests: dict[str, Quest]

    def node(self, id: str) -> Node:
        return self.nodes[id.upper()]

    def quest(self, var: str) -> Quest:
        return self.quests[var.upper()]

    def dialogs_of(self, npc: str) -> list[Node]:
        """The npc's dialogs (not choices) in menu order."""
        npc = npc.upper()
        found = [n for n in self.nodes.values() if n.npc == npc and not n.is_choice]
        return sorted(found, key=lambda n: (n.nr, n.id))

    def quests_of(self, npc: str) -> list[Quest]:
        """Quests the npc's dialogs and their choices move."""
        npc = npc.upper()
        return [q for q in self.quests.values() if any(self.nodes[m].npc == npc for m in q.movers())]

    def things_of(self, npc: str) -> set[str]:
        """Symbols of the npc's story that look like items (IT…): in its dialogs and their choices, and in
        the dialogs of anyone that move or read the quests its own move or read: Cavalorn's stolen letter
        is named only in Vatras's «У меня для тебя письмо.». Which of them are items, the game knows."""
        npc = npc.upper()
        own = [n for n in self.nodes.values() if n.npc == npc]
        quests = {v for n in own for v in n.writes | n.reads if v in self.quests}
        ids = {m for v in quests if (var := self.vars.get(v)) for m in var.readers | var.writers}
        nodes = own + [self.nodes[i] for i in ids if i in self.nodes and self.nodes[i].npc != npc]
        return _things(nodes)

    def things(self) -> set[str]:
        """Item-like symbols (IT…) named by any script dialog or choice, including its conditions."""
        return _things(self.nodes.values())

    def npc(self, instance: str) -> str:
        """Npc instance behind a global instance variable (HILDA -> BAU_951_HILDA); other names unchanged."""
        return self.instances.get(instance.upper(), instance.upper())


def load(path: Path) -> StoryGraph:
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    if data.get("version") != VERSION:
        raise ValueError(f"{path}: story graph version {data.get('version')}, expected {VERSION}")
    nodes = {}
    for n in data["nodes"]:
        nodes[n["id"]] = Node(
            id=n["id"],
            kind=n["kind"],
            npc=n["npc"],
            description=n["description"],
            effects=n["effects"],
            writes=set(n["writes"]),
            condition=n.get("condition"),
            lifted=n.get("lifted", True),
            not_lifted=n.get("not_lifted", ""),
            nr=n.get("nr", 0),
            important=n.get("important", False),
            permanent=n.get("permanent", False),
            trade=n.get("trade", False),
            reads=set(n.get("reads", [])),
            knows=set(n.get("knows", [])),
            condition_effects=n.get("condition_effects", []),
            parents=n.get("parents", []),
        )
    quests = {}
    for q in data["quests"]:
        quests[q["var"]] = Quest(
            var=q["var"],
            topics={t["topic"]: t["title"] for t in q["topics"]},
            running=set(q["running"]),
            success=set(q["success"]),
            failed=set(q["failed"]),
            obsolete=set(q["obsolete"]),
            other=set(q["other"]),
        )
    vars = {
        name: Var(readers=set(v["readers"]), writers=set(v["writers"]), others=set(v["others"]))
        for name, v in data["vars"].items()
    }
    return StoryGraph(
        nodes=nodes, functions=data["functions"], instances=data["instances"], vars=vars, quests=quests
    )


def ensure(game_dir: Path, cache: Path, tool: Path | None = None) -> Path | None:
    """Cache the graph by ready-base identity and converter, without probing installation files."""
    with base.resolve(game_dir) as selected:
        return _ensure(selected, cache, tool)


def _ensure(selected: base.Base, cache: Path, tool: Path | None) -> Path | None:
    tool = tool or Path(os.environ.get("STORYGRAPH_TOOL", TOOL))
    stamp = cache.with_suffix(".base.json")
    source = {"base_fingerprint": selected.fingerprint, "algorithm_version": ALGORITHM_VERSION}
    try:
        previous = json.loads(stamp.read_text())
    except (OSError, ValueError):
        previous = {}
    if not tool.exists():
        if (
            cache.is_file()
            and isinstance(previous, dict)
            and all(previous.get(key) == value for key, value in source.items())
        ):
            log.warning("story graph: converter unavailable, converter freshness unverified: %s", tool)
            return cache
        log.warning("story graph: converter unavailable: %s", tool)
        return None
    source["converter_sha256"] = hashlib.sha256(tool.read_bytes()).hexdigest()
    if cache.is_file() and previous == source:
        return cache
    cache.parent.mkdir(parents=True, exist_ok=True)
    tmp = cache.with_suffix(".tmp")
    try:
        cmd = [str(tool), str(selected.path), "--json", str(tmp)]
        subprocess.run(
            cmd,
            env={**os.environ, "OPENGOTHIC_BASE": str(selected.path)},
            check=True,
            capture_output=True,
            timeout=1800,
        )
    except (OSError, subprocess.SubprocessError) as err:
        log.warning("story graph: %s failed: %s", tool, err)
        return None
    tmp.replace(cache)
    temporary = stamp.with_suffix(".tmp")
    temporary.write_text(json.dumps(source), encoding="utf-8")
    temporary.replace(stamp)
    log.info("story graph: %s from base %s", cache, selected.fingerprint)
    return cache


def walk(expr: Expr | None) -> Iterator[Expr]:
    """The expression and all its subexpressions."""
    if not isinstance(expr, list) or not expr:
        return
    yield expr
    tag = expr[0]
    if tag == "field":
        yield from walk(expr[1])
    elif tag in ("call", "fn"):
        for a in expr[2:]:
            yield from walk(a)
    elif tag not in ("int", "str", "var", "inst", "sym", "param", "unknown"):
        for a in expr[1:]:
            yield from walk(a)


def _things(nodes: Iterable[Node]) -> set[str]:
    return {
        str(e[1])
        for n in nodes
        for e in _exprs([n.condition, n.condition_effects, n.effects])
        if e[0] == "sym" and len(e) > 1 and str(e[1]).startswith("IT")
    }


def _exprs(data: object) -> Iterator[Expr]:
    """Every expression in a node's parts: conditions, and effects (dicts whose values hold them)."""
    if isinstance(data, dict):
        for v in data.values():
            yield from _exprs(v)
    elif isinstance(data, list) and data and isinstance(data[0], str):
        yield from walk(data)
    elif isinstance(data, list):
        for v in data:
            yield from _exprs(v)


def variables(expr: Expr | None) -> set[str]:
    """Global variables the expression reads (script functions it calls are not followed)."""
    return {e[1] for e in walk(expr) if e[0] == "var"}


def is_known(expr: Expr | None) -> bool:
    return not any(e[0] == "unknown" for e in walk(expr))


def render(expr: Expr | None) -> str:
    """Readable text, like the converter's --node view."""
    if expr is None:
        return "always"
    tag = expr[0]
    if tag == "int":
        return expr[2] if len(expr) > 2 else str(expr[1])
    if tag == "str":
        return f'"{expr[1]}"'
    if tag == "var":
        return expr[1] + (f"[{expr[2]}]" if len(expr) > 2 else "")
    if tag == "field":
        index = ""
        if len(expr) > 3:
            index = f"[{expr[4] if len(expr) > 4 else expr[3]}]"
        return f"{render(expr[1])}.{expr[2].lower()}{index}"
    if tag == "inst":
        return expr[1].lower() if expr[1] in ("SELF", "OTHER", "HERO") else expr[1]
    if tag == "sym":
        return expr[1]
    if tag == "param":
        return f"${expr[1]}"
    if tag in ("call", "fn"):
        return f"{expr[1]}({', '.join(render(a) for a in expr[2:])})"
    if tag == "cases":
        pairs = [f"{_wrap(expr[i])} -> {_wrap(expr[i + 1])}" for i in range(1, len(expr) - 1, 2)]
        return "case " + "; ".join(pairs) + "; else 0"
    if tag == "unknown":
        return f"?{expr[1]}"
    if tag == "!":
        return "!" + _wrap(expr[1])
    if tag == "neg":
        return "-" + _wrap(expr[1])
    if tag in _BINARY:
        return f" {tag} ".join(_wrap(a) for a in expr[1:])
    return f"{tag}({', '.join(_wrap(a) for a in expr[1:])})"


def _wrap(expr: Expr) -> str:
    text = render(expr)
    if expr[0] == "cases" or (expr[0] in _BINARY):
        return f"({text})"
    return text
