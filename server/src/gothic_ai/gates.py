"""A gatekeeper's admission: native grounds, a paid bribe, or the hero's believed word."""

from __future__ import annotations

import asyncio
from typing import TYPE_CHECKING, Any

from .chronicle import Chronicle
from .crime_law_graph import (
    CITY_GATE_NPCS,
    city_entry_blocked,
    city_entry_program,
    law_var,
    projection_values,
)
from .game import GameError, GameLink
from .gamedata.storygraph import Node, StoryGraph, walk
from .llm import Tool, ToolCall
from .protocol import GameMessage, Session, Snapshot, TriggerFired
from .turn import ToolOutcome

if TYPE_CHECKING:
    from .dialogue import Conversation, DialogueManager

KEEPERS = {
    "MIL_310_STADTWACHE": (100, 60),
    "MIL_333_STADTWACHE": (100, 60),
    "MIL_305_TORWACHE": (0, 0),
}
GROUNDS = {
    "PASSIERSCHEIN": "у героя пропуск",
    "ZUMSCHMIED": "на герое одежда фермера, он знает про кузнеца от Малета",
    "CONSTANTINO": "у героя травы для Константино, поручение выполняется",
    "PASSASCITIZEN": "герой — ученик городского мастера",
    "PASSASMIL": "герой служит в ополчении",
    "PASSASMAGE": "герой — маг Огня",
    "PASSASSLD": "герой — наёмник",
}
_RUNNING: set[tuple[GameLink, str]] = set()
LET_PASS = Tool(
    "let_pass",
    "Разрешить герою проход. basis — id настоящего основания из блока «Ворота», либо claim, "
    "если ты поверил его словам и отвечаешь за это сам. Для claim запиши его конкретное заявление "
    "(до 200 знаков) и символ NPC, на которого он сослался, либо пустую строку. "
    "Одни сказки про драконов не дают права входа. Взятку оформляй propose_deal с let_pass=true.",
    {
        "type": "object",
        "properties": {
            "basis": {"type": "string"},
            "claim": {"type": "string", "maxLength": 200},
            "about": {"type": "string"},
        },
        "required": ["basis", "claim", "about"],
        "additionalProperties": False,
    },
)


def _post(graph: StoryGraph, npc: str) -> list[Any]:
    node = graph.nodes.get(f"DIA_{npc}_FIRSTWARN")
    for expr in walk(node.condition if node else None):
        if expr[:2] == ["call", "HLP_STRCMP"] and expr[2:3] == [
            ["call", "NPC_GETNEARESTWP", ["inst", "SELF"]]
        ]:
            return expr
    return ["int", 0]


def _ground(node: Node) -> list[Any]:
    # Some dialogs offer a refusal too. Only their branch actually granting admission is a ground.
    granting = next(
        e
        for e in node.effects
        if e.get("do") == "set_field" and e.get("field") == "AIVAR" and e.get("index", [])[:2] == ["int", 14]
    )
    return ["&&", node.condition or ["int", 1], granting.get("when") or ["int", 1]]


class Gates:
    all_tools: tuple[Tool, ...] = (LET_PASS,)

    def __init__(
        self, link: GameLink, dialogue: DialogueManager, chronicle: Chronicle, graph: StoryGraph
    ) -> None:
        self.link, self.dialogue, self.chronicle, self.graph = link, dialogue, chronicle, graph
        self._posts: set[str] = set()
        self.grounds: dict[str, dict[str, Node]] = {}
        self.blocked: set[str] = set()
        dialogue.extensions.append(self)
        link.listeners.append(self._on_message)

    def _on_message(self, msg: GameMessage) -> None:
        if isinstance(msg, Session):
            self._posts.clear()
            self.grounds.clear()
            self.blocked.clear()

    def on_post(self, npc: str) -> bool:
        return npc.upper() in self._posts

    async def prepare(self, npc: str) -> None:
        npc = npc.upper()
        self._posts.discard(npc)
        self.grounds.pop(npc, None)
        self.blocked.discard(npc)
        if npc not in KEEPERS:
            return
        nodes = [n for n in self.graph.dialogs_of(npc) if n.id.rsplit("_", 1)[-1] in GROUNDS]
        values = await self.link.story_eval(
            npc, [_post(self.graph, npc), city_entry_blocked(), *(_ground(n) for n in nodes)]
        )
        if not values or not values[0]:
            return
        self._posts.add(npc)
        if values[1] is None or values[1]:
            self.blocked.add(npc)
        self.grounds[npc] = {n.id: n for n, value in zip(nodes, values[2:], strict=True) if value}

    def situation(self, npc: str) -> str | None:
        npc = npc.upper()
        if not self.on_post(npc):
            return None
        lines = ["Ворота:"]
        if npc in self.blocked:
            lines.append("Известно неоплаченное преступление героя: проход закрыт по любому основанию.")
        else:
            lines.extend(
                f"{GROUNDS[n.id.rsplit('_', 1)[-1]]} (basis {n.id})."
                for n in self.grounds.get(npc, {}).values()
            )
            if not self.grounds.get(npc):
                lines.append("Подтверждённых оснований сейчас нет.")
        start, minimum = KEEPERS[npc]
        lines.append(
            f"Ты жадный: начинаешь со {start} золотых за проход, ниже {minimum} не согласишься."
            if start
            else "Взяток за проход ты не берёшь."
        )
        lines.append(
            "Можешь пустить на слово, если поверил конкретному основанию героя; "
            "отвечаешь за это сам. "
            "Истории про драконов сами по себе не основание, как и в родном разговоре."
        )
        return "\n".join(lines)

    def tools(self, npc: str) -> list[Tool]:
        return [LET_PASS] if self.on_post(npc) else []

    async def _allowed(self, npc: str) -> str:
        if npc not in KEEPERS:
            return "you are not a gatekeeper for these gates"
        values = await self.link.story_eval(npc, [_post(self.graph, npc), city_entry_blocked()])
        if len(values) != 2 or not values[0]:
            return "you are not at your post"
        if values[1] is None or values[1]:
            return "the hero has a known unpaid crime; passage is blocked"
        return ""

    async def handle(
        self, dialogue: DialogueManager, conv: Conversation, call: ToolCall, snap: Snapshot | None
    ) -> ToolOutcome | None:
        if call.name != "let_pass":
            return None
        npc = conv.npc.upper()
        if reason := await self._allowed(npc):
            return ToolOutcome(f"Rejected: {reason}.", "rejected")
        basis = str(call.arguments.get("basis", ""))
        claim = str(call.arguments.get("claim", "")).strip()
        about = str(call.arguments.get("about", "")).strip().upper()
        node = None
        if basis.lower() == "claim":
            if not claim or len(claim) > 200:
                return ToolOutcome("Rejected: claim must contain 1 to 200 characters.", "rejected")
            if about:
                symbols = (await self.link.story_state(symbols=[about])).get("symbols", {})
                if symbols.get(about) != "npc":
                    return ToolOutcome(
                        "Rejected: about must be an NPC symbol or an empty string.", "rejected"
                    )
        else:
            node = self.grounds.get(npc, {}).get(basis.upper())
            if node is None or not (await self.link.story_eval(npc, [_ground(node)]))[0]:
                return ToolOutcome("Rejected: this ground is not currently verified by code.", "rejected")

        async def admit() -> tuple[str, bool]:
            try:
                if reason := await self._allowed(npc):
                    return f"Passage refused: {reason}.", False
                state = await self.link.story_state()
                if node is not None:
                    program = await self.link.story_program(node.id, own=True)
                    await _run(
                        self.link, npc, _quiet(program["steps"]), _ground(node), _post(self.graph, npc)
                    )
                    await self.link.story_effect(npc, [{"do": "knows", "info": node.id}])
                else:
                    await _run(self.link, npc, _admission(npc), ["int", 1], _post(self.graph, npc))
                    self.chronicle.record(
                        {
                            "fact": "gate_claim",
                            "npc": npc,
                            "claim": claim,
                            "about": about,
                            "believed": True,
                            **{k: int(state[k]) for k in ("day", "hour", "minute")},
                        }
                    )
            except (GameError, TimeoutError) as error:
                return f"Passage refused: {error}", False
            return "Ты разрешил герою проход.", True

        dialogue.after_speech(conv, admit)
        return ToolOutcome("Принято: разрешишь проход после реплики.", "accepted")

    async def bribe_error(self, npc: str, amount: int) -> str:
        npc = npc.upper()
        if reason := await self._allowed(npc):
            return reason
        _, minimum = KEEPERS[npc]
        if not minimum:
            return "you do not take bribes for passage"
        if amount < minimum:
            return f"you require at least {minimum} gold for passage"
        return ""

    async def bribe(self, npc: str, effects: list[dict[str, Any]] | None = None) -> None:
        npc = npc.upper()
        effects = effects or []
        amount = sum(
            e.get("count", 0)
            for e in effects
            if e.get("do") == "give" and e.get("item") == "ITMI_GOLD" and e.get("to") == "self"
        )
        if reason := await self.bribe_error(npc, amount):
            raise GameError(reason)
        steps, enough, after = _effects(effects)
        if after:
            raise GameError("a gate bribe supports only an immediate gold transfer")
        number = npc.split("_", 2)[1]
        steps += _admission(npc)
        steps.append(
            {
                "do": "set",
                "target": ["var", f"MIL_{number}_PERSONAL_ABSOLUTIONLEVEL"],
                "value": ["+", ["fn", "B_GETCURRENTABSOLUTIONLEVEL", ["inst", "SELF"]], ["int", 1]],
            }
        )
        await _run(self.link, npc, steps, enough, _post(self.graph, npc))


def _admission(npc: str) -> list[dict[str, Any]]:
    number = npc.split("_", 2)[1]
    targets = ["SELF"]
    if npc in CITY_GATE_NPCS:
        targets.append("STADTWACHE_333" if number == "310" else "STADTWACHE_310")
    return [
        *(
            {"do": "set", "target": ["field", ["inst", who], "AIVAR", 14], "value": ["int", 1]}
            for who in targets
        ),
        {"do": "set", "target": ["var", f"MIL_{number}_SCHONMALREINGELASSEN"], "value": ["int", 1]},
        {"do": "call", "call": ["fn", "B_CHECKLOG"]},
    ]


def _quiet(steps: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """The native program's state changes, without its menu, voices or queued animations."""
    out = []
    for step in steps:
        if step["do"] in ("say", "choice", "clear_choices"):
            continue
        if step["do"] == "call" and str(step["call"][1]).startswith(("AI_", "SND_", "PRINT")):
            continue
        if step["do"] == "if":
            step = {**step, "then": _quiet(step.get("then", [])), "else": _quiet(step.get("else", []))}
        out.append(step)
    return out


def _effects(effects: list[dict[str, Any]]) -> tuple[list[dict[str, Any]], list[Any], list[dict[str, Any]]]:
    steps, checks, after = [], [], []
    for e in effects:
        match e["do"]:
            case "gate_pass":
                steps.append(
                    {
                        "do": "set",
                        "target": ["field", ["inst", e["npc"]], "AIVAR", 14],
                        "value": ["int", e["value"]],
                    }
                )
            case "gate_checklog":
                steps.append({"do": "call", "call": ["fn", "B_CHECKLOG"]})
            case "set":
                steps.append({"do": "set", "target": ["var", e["var"]], "value": ["int", e["value"]]})
            case "xp":
                steps.append({"do": "call", "call": ["fn", "B_GIVEPLAYERXP", ["int", e["amount"]]]})
            case "give":
                source, target = ("OTHER", "SELF") if e.get("to") == "self" else ("SELF", "OTHER")
                checks.append(
                    [
                        ">=",
                        ["call", "NPC_HASITEMS", ["inst", source], ["sym", e["item"]]],
                        ["int", e["count"]],
                    ]
                )
                steps.append(
                    {
                        "do": "call",
                        "call": [
                            "fn",
                            "B_GIVEINVITEMS",
                            ["inst", source],
                            ["inst", target],
                            ["sym", e["item"]],
                            ["int", e["count"]],
                        ],
                    }
                )
            case "knows":
                after.append(e)
            case "journal":
                topic = ["str", e["topic"]]
                steps.extend(
                    [
                        {"do": "call", "call": ["call", "LOG_CREATETOPIC", topic, ["int", 1]]},
                        {
                            "do": "call",
                            "call": [
                                "call",
                                "LOG_SETTOPICSTATUS",
                                topic,
                                ["var", e.get("status", "LOG_RUNNING")],
                            ],
                        },
                    ]
                )
                if e.get("text"):
                    steps.append({"do": "call", "call": ["fn", "B_LOGENTRY", topic, ["str", e["text"]]]})
            case _:
                raise GameError(f"unsupported gate effect: {e['do']}")
    enough = ["int", 1]
    if checks:
        enough = checks[0] if len(checks) == 1 else ["&&", *checks]
    return steps, enough, after


async def play_gate_effects(link: GameLink, npc: str, effects: list[dict[str, Any]]) -> None:
    """The small gate subset lifted by vanilla._bridge, over the existing graph protocol."""
    steps, enough, after = _effects(effects)
    await _run(link, npc, steps, enough, ["int", 1])
    if after:
        await link.story_effect(npc, after)


async def _run(
    link: GameLink, npc: str, steps: list[dict[str, Any]], condition: list[Any], post: list[Any]
) -> None:
    # A transient state trigger runs without opening a native menu, also in a normal (non-dev) game.
    # Its program checks law and funds in the same game tick as the stores and the transfer.
    trigger = "DIR_GATE_" + npc.upper()
    result = trigger + "_RESULT"
    key = (link, trigger)
    if key in _RUNNING:
        raise GameError("gate admission is already in progress for this guard")
    _RUNNING.add(key)
    done: asyncio.Future[TriggerFired] = asyncio.get_running_loop().create_future()
    session = link.session

    def hear(message: GameMessage) -> None:
        if isinstance(message, TriggerFired) and message.id == trigger and not done.done():
            done.set_result(message)
        elif isinstance(message, Session) and not done.done():
            done.set_exception(GameError("world reloaded"))

    body = [
        {
            "do": "if",
            "cond": ["&&", condition, post],
            "then": [{"do": "set", "target": ["var", result], "value": ["int", 1]}, *steps],
        }
    ]
    if npc in KEEPERS:
        body = city_entry_program("DIR_GATE_PASS", "information", body)
    link.listeners.append(hear)
    try:
        await link.patch_graph(
            [
                {"op": "vars", "vars": {
                    law_var("city"): 0, **projection_values("city", []), trigger: 0, result: 0,
                }},
            ]
        )
        if session is not link.session:
            raise GameError("world reloaded")
        # Vars only supplies initial values: explicitly reset the two reused globals.
        await link.story_effect(npc, [
            {"do": "set", "var": trigger, "value": 0},
            {"do": "set", "var": result, "value": 0},
        ])
        if session is not link.session:
            raise GameError("world reloaded")
        await link.patch_graph([
            {"op": "trigger", "id": trigger, "on": "state", "npc": npc, "steps": body},
        ])
        try:
            fired = await asyncio.wait_for(done, 5)
        except TimeoutError:
            fired = None  # The event can be lost; the save's counter and result are authoritative.
        if fired is not None and fired.error:
            raise GameError(fired.error)
        if session is not link.session:
            raise GameError("world reloaded")
        values = await link.story_eval(npc, [["var", trigger], ["var", result]])
        if values != [1, 1]:
            raise GameError(
                "passage refused: unpaid crime, invalid ground, guard off post or insufficient gold"
            )
    finally:
        link.listeners.remove(hear)
        try:
            if session is link.session:
                await link.patch_graph([{"op": "trigger", "id": trigger, "steps": None}])
        finally:
            _RUNNING.discard(key)
