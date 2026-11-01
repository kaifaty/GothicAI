"""Minimal stand-in for OpenGothic's ai_bridge, for tests."""

from __future__ import annotations

import asyncio
import json
import time
from collections.abc import AsyncIterator, Collection, Sequence
from contextlib import asynccontextmanager
from typing import Any

from websockets.asyncio.client import ClientConnection, connect

from gothic_ai.game import GameLink, GameServer
from gothic_ai.llm import Item, LLMBackend, LLMEvent, Message, Tool, ToolChoice, ToolResult
from gothic_ai.llm.fake import FakeBackend, FakeStep
from gothic_ai.protocol import PROTOCOL_VERSION

KNOWN_ANIMS = {
    "T_GREETNOV",
    "T_YES",
    "T_NO",
    "T_DONTKNOW",
    "T_FORGETIT",
}  # not T_COMEOVERHERE: m0 tries it first
WAYPOINTS = {"NW_XARDAS_TOWER_IN1_31": 0.0, "NW_XARDAS_TOWER_04": 850.0, "NW_XARDAS_STAIRS_01": 1200.0}


class FakeGame:
    def __init__(
        self, busy_attaches: int = 0, branch: str | None = "test-branch", protocol: int = PROTOCOL_VERSION
    ) -> None:
        self.protocol = protocol  # what hello reports (an older bridge: an older version)
        self.attached: set[str] = set()
        self.received: list[dict[str, Any]] = []
        self.busy_attaches = busy_attaches  # first N attach requests fail as "npc is in dialog"
        self.attach_delay = 0.0  # seconds before an attach is answered (to cut something mid-attach)
        self.ws: ClientConnection | None = None
        self.said: list[str] = []
        self.voiced: list[bool] = []  # said with audio
        # every say the server sent (dropped ones too) and when it came, by time.monotonic()
        self.says: list[tuple[dict[str, Any], float]] = []
        self.drop_says: set[str] = set()  # npc's whose says are dropped silently, as the bridge refuses one
        self.start_delay = 0.0  # seconds before line_started (0: at once)
        self.say_chunks: dict[str, int] = {}  # line -> say_audio chunks
        self.say_ends: dict[str, float | None] = {}  # line -> audio_s of its say_end
        self.fillers: list[str] = []  # said as fillers ("Хм..." while the model thinks)
        self.statuses: list[str] = []
        self.branch = branch  # reported as the session on connect (None: an old game without sessions)
        self.heads: list[tuple[str, int]] = []  # memory heads the server reported
        self.transcripts: list[str] = []  # final ones
        self.partials: list[str] = []
        self.graph: list[dict[str, Any]] = []  # graph_patch messages; ops on node "BAD" are rejected
        # what story_program answers (v23): the scripts' programs of (node, part); missing ones are errors
        self.programs: dict[tuple[str, str], list[dict[str, Any]]] = {}
        self.story: list[dict[str, Any]] = []  # story_spawn / story_watch / story_effect / raise_dead
        self.cast: dict[str, dict[str, Any]] = {}  # story_npc (v34): the director's npc's made, by id
        self.cast_asked: list[str] = []  # story_npc, every one asked, in order
        self.people_near: list[dict[str, Any]] = []  # what story_nearby answers
        self.nearby_of: dict[str, list[dict[str, Any]]] = {}  # ...around this npc (else people_near)
        self.nearby_error: set[str] = set()  # npc's story_nearby does not know (ok=false: not in the world)
        # what story_state answers: globals, npc's ({"state", "name", "distance" from `near`}), dialogs the
        # hero heard, script names
        self.vars: dict[str, int] = {}
        self.npc_states: dict[str, dict[str, Any]] = {}
        self.known: set[str] = set()
        self.symbols: dict[str, str] = {}
        self.in_dialog = False
        self.in_cutscene = False
        self.nearest_waypoint: str | None = "NW_XARDAS_TOWER_IN1_31"
        self.waypoint_distances: dict[str, float] = {}  # story_eval: hero's distance to a waypoint, cm
        self.day = 1
        self.hour, self.minute = 8, 0
        self.people: list[dict[str, Any]] = []  # what story_people answers: the world's people and their days
        self.item_names: dict[str, str] = {}
        self.fns: dict[str, int] = {}  # story_eval: what helper predicates of the scripts come to (C_X())
        self.attitudes: dict[str, int] = {}  # NPC_GETATTITUDE towards HERO; default ATT_NEUTRAL
        self.guild_attitudes: dict[tuple[int, int], int] = {}  # WLD_GETGUILDATTITUDE, npc -> hero guild
        # story_eval: expressions over vars, npc_states, the hero's items (instances: global -> npc symbol)
        self.hero_items: dict[str, int] = {}
        # story_effect with `check`: the npc's things (the hero's are hero_items), moved by the gives
        self.npc_items: dict[str, dict[str, int]] = {}
        # v17: the hero's learning points and experience; what teaching costs (per point for attribute and
        # fight, else for the lesson), default 1; what was taught
        self.hero_lp = 0
        self.hero_xp = 0
        self.learn_cost: dict[tuple[str, str], int] = {}
        self.taught: list[tuple[str, str, int]] = []
        self.deals: list[dict[str, Any]] = []  # deal_show / deal_hide messages
        self.views: dict[str, dict[str, Any]] = {}  # what npc_view answers: npc -> its view (NpcView)
        self.fields: dict[tuple[str, str], int] = {}  # ("SELF" | "OTHER", "GUILD") -> value; others read 0
        self.instances: dict[str, str] = {}
        self.nearby = [{"id": "PC_THIEF", "name": "Диего", "distance": 650.0, "guild": 0, "dead": False}]
        self.hp = 999
        self.soft: list[tuple[str, str]] = []  # npc_act's: (npc, action)
        self.acts: list[dict[str, Any]] = []  # npc_act messages as sent
        self.busy_npcs: set[str] = set()  # their npc_act's are rejected
        self.auto_idle = False  # answer every queued npc_act with npc_idle at once
        self.idle_delay = 0.0  # ...or this many seconds later (a walk still under way meanwhile)
        self.idle_reason = "done"
        # as the bridge tracks soft acts: one per npc, a newer one drops the older (no npc_idle for it), an
        # npc taken over (attached) ends its act as "gone"
        self.track_acts = False
        self.running: dict[str, str] = {}  # npc -> its act being tracked
        self.important: set[str] = set()  # npc's with an important dialog to tell (NPC_CHECKINFO)
        self.hushed: list[dict[str, Any]] = []
        self.heard: list[dict[str, Any]] = []  # final transcripts with whom they were said to
        self.debug_calls: list[dict[str, Any]] = []  # debug messages, each answered ok
        # what `routine` answers: npc -> {"current", "entries": [{start, end, state, waypoint}], "routines"};
        # an npc not in it is not in the world ("npc not found", as the bridge says)
        self.routines: dict[str, dict[str, Any]] = {}
        self.routine_errors: dict[str, str] = {}  # npc -> the error its routine is answered with
        self.routine_hang: set[str] = set()  # npc's whose routine is never answered (the server times out)
        self.routine_calls: list[str] = []  # every npc asked for, in order
        self.beast_cmds: list[dict[str, Any]] = []  # the server's words on wolves (v21)
        self._later: set[asyncio.Task[None]] = set()
        self.slow_actions: set[str] = set()  # actions that go on until the npc's next action cancels them
        # v28: what npc_chests answers, npc -> its chests (protocol.Chest as JSON); use_chest moves items
        self.chests: dict[str, list[dict[str, Any]]] = {}
        self.long_running: dict[str, str] = {}  # npc -> the id of its action going on

    async def perception(
        self, npc: str, perc: str, other: str, distance: float = 300.0, victim: str | None = None
    ):
        """What a controlled npc noticed (the game sends these for attached npc's only)."""
        extra = {"victim": victim} if victim else {}
        await self.send(type="perception", npc=npc, perc=perc, other=other, distance=distance, **extra)

    async def push_views(self, *views: dict[str, Any]) -> None:
        """What the npc's nearest to the player perceive, as the game pushes it once a second (v11)."""
        await self.send(type="npc_views", t=1000, views=list(views))

    async def beasts(self, *wolves: dict[str, Any], hero_weapon: str = "none") -> None:
        """The wolves near the hero (v21): id, name, distance, state, hp, hp_max, focus..."""
        await self.send(type="beasts", t=1000, hero_weapon=hero_weapon, wolves=list(wolves))

    async def roster(self, *people: dict[str, Any]) -> None:
        """The people around the player (npc_roster): id, name, distance, in_view, state, moving..."""
        npcs = [{"guild": 0, "in_view": False, "activity": "routine", **p} for p in people]
        await self.send(type="npc_roster", t=1000, hour=self.hour, minute=self.minute, full=True, npcs=npcs)

    async def send(self, **msg: Any) -> None:
        assert self.ws is not None
        await self.ws.send(json.dumps(msg))

    def snapshot(self, npc: str) -> dict[str, Any]:
        return {
            "type": "snapshot",
            "t": 1000,
            "world": {"name": "NEWWORLD.ZEN", "day": 0, "hour": 8, "minute": 0},
            "npc": {
                "id": npc,
                "name": "Ксардас",
                "pos": [1.0, 2.0, 3.0],
                "rot": 90.0,
                "hp": self.hp,
                "hp_max": 999,
                "busy": False,
                "action": None,
                "waypoint": "NW_XARDAS_TOWER_IN1_31",
                "nearest_waypoint": "NW_XARDAS_TOWER_IN1_31",
                "following": False,
                "inventory": [
                    {"item": "ITPO_HEALTH_01", "name": "Эликсир лечения", "count": 3, "equipped": False},
                    {"item": "ITMW_ZWEIHAENDER1", "name": "Двуручник", "count": 1, "equipped": True},
                ],
            },
            "player": {
                "name": "Я",
                "pos": [100.0, 2.0, 3.0],
                "ahead": [1.0, 0.0],  # he faces +x, his right is -z (v32)
                "right": [0.0, -1.0],
                "distance": 99.0,
                "guild": 0,
                "hp": 40,
                "hp_max": 40,
                "armor": None,
                "melee": "Палка",
                "ranged": None,
            },
            "nearby_npcs": self.nearby,
            "nearby_waypoints": [{"name": n, "distance": d} for n, d in WAYPOINTS.items()],
        }

    async def run(self, url: str) -> None:
        async with connect(url) as ws:
            self.ws = ws
            await self.send(type="hello", protocol=self.protocol, client="fake", codepage="cp1251")
            if self.branch is not None:
                await self.send(type="session", branch=self.branch, reason="new_game")
            await self.send(type="world_loaded", world="NEWWORLD.ZEN")
            async for raw in ws:
                msg = json.loads(raw)
                self.received.append(msg)
                await self.handle(msg)

    async def handle(self, msg: dict[str, Any]) -> None:
        match msg["type"]:
            case "detach":
                self.attached.discard(msg["npc"])
                await self.send(type="result", req=msg["req"], ok=True)
                await self.send(type="detached", npc=msg["npc"], reason="server")
            case "npc_act":
                npc, args = msg["npc"], msg.get("args", {})
                self.soft.append((npc, msg["action"]))
                self.acts.append(msg)
                if npc in self.busy_npcs:
                    busy = {"status": "rejected", "detail": "npc is busy (attached or in a dialog)"}
                    await self.send(type="npc_act_result", id=msg["id"], npc=npc, **busy)
                    return
                await self.send(type="npc_act_result", id=msg["id"], npc=npc, status="queued")
                reason, failed = self.idle_reason, None
                if msg["action"] == "give":  # v15: to another npc
                    failed = self.hand_over(npc, args)
                elif msg["action"] == "start_activity" and args.get("arrive") and reason == "done":
                    reason = "arrived"
                if self.auto_idle:
                    idle = {"type": "npc_idle", "npc": npc, "act": msg["id"]}
                    idle |= {"reason": "failed", "detail": failed} if failed else {"reason": reason}
                    if self.idle_delay > 0:
                        self._spawn(self.send_later(self.idle_delay, **idle))
                    else:
                        await self.send(**idle)
                elif self.track_acts:
                    self.running[npc] = msg["id"]
            case "hush":
                self.hushed.append(msg)
            case "attach":
                if self.attach_delay > 0:
                    self._spawn(self.attach(msg))  # the rest of the messages go on meanwhile
                else:
                    await self.attach(msg)
            case "snapshot":
                await self.send(**self.snapshot(msg["npc"]))
            case "action":
                await self.handle_action(msg)
            case "say":
                self.says.append((msg, time.monotonic()))
                if msg["npc"] in self.drop_says:
                    return  # the bridge drops a say for an npc that cannot speak, without a word
                self.said.append(msg["text"])
                self.voiced.append(bool(msg.get("audio") or msg.get("stream")))
                if msg.get("filler"):
                    self.fillers.append(msg["text"])
                started = {"type": "line_started", "npc": msg["npc"], "line": msg["line"]}
                if self.start_delay > 0:
                    self._spawn(self.send_later(self.start_delay, **started))
                else:
                    await self.send(**started)
            case "say_audio":
                self.say_chunks[msg["line"]] = self.say_chunks.get(msg["line"], 0) + 1
            case "say_end":
                self.say_ends[msg["line"]] = msg.get("audio_s")
            case "debug":
                self.debug_calls.append(msg)
                await self.send(type="result", req=msg["req"], ok=True)
            case "chat_status":
                self.statuses.append(msg["status"])
            case "memory":
                self.heads.append((msg["branch"], msg["seq"]))
            case "transcript":
                (self.transcripts if msg.get("final", True) else self.partials).append(msg["text"])
                if msg.get("final", True):
                    self.heard.append(msg)
            case "story_spawn":
                self.story.append(msg)
                await self.send(type="result", req=msg["req"], ok=True, value=msg.get("count", 1))
            case "story_watch":
                self.story.append(msg)
            case "story_npc":
                # the director's npc (v34): made once, then left as the save has it
                self.cast_asked.append(msg["npc"])
                npc = msg["npc"]
                if npc in self.cast:
                    state = "dead" if self.npc_states.get(npc, {}).get("state") == "dead" else "there"
                else:
                    self.cast[npc] = msg
                    self.npc_states[npc] = {"state": "alive", "name": msg["name"]}
                    self.symbols[npc] = "npc"
                    if msg.get("routine"):
                        self.routines[npc] = {"entries": msg["routine"]}
                    state = "made"
                index = 74650 + list(self.cast).index(npc)
                await self.send(
                    type="result", req=msg["req"], ok=True, value={"npc": npc, "state": state, "index": index}
                )
            case "raise_dead":
                self.story.append(msg)
                await self.send(type="result", req=msg["req"], ok=True)
            case "story_effect":
                self.story.append(msg)
                # a dialog counts as heard (v31)
                self.known |= {e["info"] for e in msg["effects"] if e.get("do") == "knows"}
                error = self.give(msg) if msg.get("check") else None
                await self.send(
                    type="result", req=msg["req"], ok=error is None, **({"error": error} if error else {})
                )
            case "deal_show" | "deal_hide":
                self.deals.append(msg)
            case "npc_chests":
                await self.send(type="result", req=msg["req"], ok=True, value=self.chests.get(msg["npc"], []))
            case "story_nearby":
                if msg["npc"] in self.nearby_error:
                    await self.send(type="result", req=msg["req"], ok=False, error=f"no npc {msg['npc']}")
                    return
                near = self.nearby_of.get(msg["npc"], self.people_near)
                await self.send(type="result", req=msg["req"], ok=True, value=near)
            case "routine":
                await self.routine(msg)
            case "story_state":
                value = {
                    "vars": {v: self.vars[v] for v in msg.get("vars", []) if v in self.vars},
                    "npcs": {
                        n: {
                            k: v
                            for k, v in self.npc_states.get(n, {"state": "absent"}).items()
                            if k not in ("distance", "seen") or msg.get("near")
                        }
                        for n in msg.get("npcs", [])
                    },
                    "knows": {k: k in self.known for k in msg.get("knows", [])},
                    "symbols": {n: self.symbols[n] for n in msg.get("symbols", []) if n in self.symbols},
                    "dialog": self.in_dialog,
                    "cutscene": self.in_cutscene,
                    "items": {i: self.item_names[i] for i in msg.get("items", []) if i in self.item_names},
                    "day": self.day,
                    "hour": self.hour,
                    "minute": self.minute,
                }
                await self.send(type="result", req=msg["req"], ok=True, value=value)
            case "npc_view":
                value = [self.views[n] for n in msg["npcs"] if n in self.views]
                await self.send(type="result", req=msg["req"], ok=True, value=value)
            case "story_people":
                await self.send(type="result", req=msg["req"], ok=True, value=self.people)
            case "story_eval":
                self.story.append(msg)
                values = [self.evaluate(e, msg["npc"]) for e in msg["exprs"]]
                await self.send(type="result", req=msg["req"], ok=True, value=values)
            case "story_tag":
                self.story.append(msg)
                await self.send(type="result", req=msg["req"], ok=True, value=len(msg["npcs"]))
            case "beast":
                self.beast_cmds.append(msg)
                await self.send(type="result", req=msg["req"], ok=True)
            case "story_program":
                key = (msg["node"], msg.get("part", "information"))
                if key not in self.programs:
                    await self.send(type="result", req=msg["req"], ok=False, error=f"no dialog {key[0]}")
                else:
                    value = {"node": key[0], "fn": key[0], "returns": key[1] == "condition", "patched": False}
                    await self.send(
                        type="result", req=msg["req"], ok=True, value={**value, "steps": self.programs[key]}
                    )
            case "graph_patch":
                self.graph.append(msg)
                for op in msg["ops"]:
                    for name, value in (op.get("vars") or {}).items() if op.get("op") == "vars" else []:
                        # the director's story variables (v29): kept if the save has them
                        self.vars.setdefault(name, value)
                        self.symbols.setdefault(name, "var")
                    if op.get("op") == "trigger" and op.get("steps") is not None:
                        # a trigger's count (v35): a variable of the director's by its id
                        self.vars.setdefault(op["id"], 0)
                        self.symbols.setdefault(op["id"], "var")
                if msg.get("req"):
                    bad = [i for i, op in enumerate(msg["ops"]) if op.get("node") == "BAD"]
                    errors = {"error": "; ".join(f"op {i}: no dialog BAD" for i in bad)} if bad else {}
                    await self.send(type="result", req=msg["req"], ok=not bad, **errors)

    async def routine(self, msg: dict[str, Any]) -> None:
        """`routine` (v6): the npc's day as `routines` has it, an error, or no answer at all."""
        npc = msg["npc"]
        self.routine_calls.append(npc)
        if npc in self.routine_hang:
            return
        error = self.routine_errors.get(npc) or (None if npc in self.routines else "npc not found")
        if error is not None:
            await self.send(type="routine", req=msg["req"], npc=npc, error=error)
            return
        day = {"current": "", "entries": [], "routines": [], **self.routines[npc]}
        await self.send(type="routine", req=msg["req"], npc=npc, **day)

    async def attach(self, msg: dict[str, Any]) -> None:
        if self.attach_delay > 0:
            await asyncio.sleep(self.attach_delay)
        if self.busy_attaches > 0:
            self.busy_attaches -= 1
            await self.send(type="result", req=msg["req"], ok=False, error="npc is in dialog")
            return
        self.attached.add(msg["npc"])
        if (act := self.running.pop(msg["npc"], None)) is not None:
            await self.send(type="npc_idle", npc=msg["npc"], act=act, reason="gone")
        await self.send(type="result", req=msg["req"], ok=True)
        await self.send(type="attached", npc=msg["npc"], name="Ксардас")
        await self.send(**self.snapshot(msg["npc"]))

    async def send_later(self, delay: float, **msg: Any) -> None:
        await asyncio.sleep(delay)
        await self.send(**msg)

    def _spawn(self, coro: Any) -> None:
        async def run() -> None:
            try:
                await coro
            except Exception:  # the server is gone: nobody to tell
                pass

        task = asyncio.create_task(run())
        self._later.add(task)
        task.add_done_callback(self._later.discard)

    def hand_over(self, npc: str, args: dict[str, Any]) -> str | None:
        """npc_act give (v15): the npc's item to another npc (at least one, as the bridge); why it failed, or
        None."""
        item, count = args.get("item", ""), max(1, int(args.get("count", 1)))
        giver = self.npc_items.setdefault(npc, {})
        has = giver.get(item, 0)
        if has <= 0:
            return f"npc has no item {item}"
        moved = min(has, count)
        giver[item] = has - moved
        taker = self.npc_items.setdefault(args.get("npc", ""), {})
        taker[item] = taker.get(item, 0) + moved
        return None

    def give(self, msg: dict[str, Any]) -> str | None:
        """story_effect with `check`: all the gives or none, as the bridge does; what is missing, or None.
        What the npc creates (v17) is its own to give; teaching takes the hero's learning points."""
        npc = self.npc_items.setdefault(msg["npc"], {})
        need: dict[tuple[str, str], int] = {}
        made: dict[str, int] = {}
        lp = 0
        for e in msg["effects"]:
            if e.get("do") == "give":
                who = "npc" if e.get("to") != "self" else "hero"
                need[(who, e["item"])] = need.get((who, e["item"]), 0) + int(e.get("count", 1))
            elif e.get("do") == "create":
                made[e["item"]] = made.get(e["item"], 0) + int(e.get("count", 1))
            elif e.get("do") == "teach":
                lp += self.cost(e)
        for (who, item), count in need.items():
            has = (self.hero_items if who == "hero" else npc).get(item, 0) + (
                made.get(item, 0) if who == "npc" else 0
            )
            if has < count:
                return f"missing: {who} {item} {count} (has {has})"
        if lp > self.hero_lp:
            return f"missing: hero LP {lp} (has {self.hero_lp})"
        for item, count in made.items():
            npc[item] = npc.get(item, 0) + count
        for (who, item), count in need.items():
            giver, taker = (self.hero_items, npc) if who == "hero" else (npc, self.hero_items)
            giver[item] -= count
            taker[item] = taker.get(item, 0) + count
        for e in msg["effects"]:
            if e.get("do") == "teach":
                self.hero_lp -= self.cost(e)
                self.taught.append((e["teach"], e.get("arg", ""), int(e.get("count", 1))))
            elif e.get("do") == "xp":
                self.hero_xp += int(e.get("amount", e.get("count", 0)))
        return None

    def cost(self, e: dict[str, Any]) -> int:
        each = self.learn_cost.get((e["teach"], e.get("arg", "")), 1)
        return each * int(e.get("count", 1)) if e["teach"] in ("attribute", "fight") else each

    def evaluate(self, e: Any, npc: str | None = None) -> Any:
        """A story graph expression, as far as the fake world knows: ints, vars, comparisons, logic, the
        hero's items, who is dead and whether self (`npc`) has something important to tell; fields read 0,
        anything else is null (can't be computed)."""
        ops = {
            "==": lambda a, b: a == b,
            "!=": lambda a, b: a != b,
            "<": lambda a, b: a < b,
            ">": lambda a, b: a > b,
            "<=": lambda a, b: a <= b,
            ">=": lambda a, b: a >= b,
        }
        match e:
            case ["inst", "HERO"]:
                return "PC_HERO"
            case ["int", v, *_]:
                return v
            case ["var", name]:
                return self.vars.get(name, {"ATT_HOSTILE": 0, "GIL_BDT": 9}.get(name, 0))
            case ["field", ["inst", who], name]:
                return self.fields.get((who, name), 0)
            case ["field", *_]:
                return 0
            case ["!", a]:
                v = self.evaluate(a, npc)
                return None if v is None else int(not v)
            case ["&&", *args]:
                vs = [self.evaluate(a, npc) for a in args]
                return None if None in vs else int(all(vs))
            case ["||", *args]:
                vs = [self.evaluate(a, npc) for a in args]
                return None if None in vs else int(any(vs))
            case [op, a, b] if op in ops:
                x, y = self.evaluate(a, npc), self.evaluate(b, npc)
                return None if x is None or y is None else int(ops[op](x, y))
            case ["call", "NPC_HASITEMS", ["inst", "OTHER" | "HERO"], ["sym", item]]:
                return self.hero_items.get(item, 0)
            case ["call", "NPC_GETNEARESTWP", ["inst", "HERO"]]:
                return self.nearest_waypoint
            case ["call", "NPC_GETDISTTOWP", ["inst", "HERO"], ["str", waypoint]]:
                return self.waypoint_distances.get(waypoint, 0)
            case ["call", "NPC_GETATTITUDE", ["inst", who], ["inst", "HERO"]]:
                return self.attitudes.get(who, 2)
            case ["call", "WLD_GETGUILDATTITUDE", first, second]:
                guilds = (self.evaluate(first, npc), self.evaluate(second, npc))
                return self.guild_attitudes.get(guilds, 2)
            case ["call", "WLD_GETDAY"]:
                return self.day
            case ["call", "NPC_ISDEAD", ["inst", name]]:
                dead = self.instances.get(name, name)
                return int(self.npc_states.get(dead, {}).get("state") == "dead")
            case ["call", "NPC_CHECKINFO", ["inst", "SELF"], ["int", 1, *_]]:
                return int(npc in self.important)
            case ["call", "NPC_KNOWSINFO", ["inst", "OTHER" | "HERO"], ["sym", info]]:
                return int(info in self.known)
            case ["fn", name] if name in self.fns:
                return self.fns[name]
        return None

    async def handle_action(self, msg: dict[str, Any]) -> None:
        npc, action, args = msg["npc"], msg["action"], msg.get("args", {})
        if (running := self.long_running.pop(npc, None)) is not None:  # preempted, as the bridge does
            await self.send(type="action_result", id=running, npc=npc, status="cancelled", detail="preempted")
        if action in self.slow_actions and npc in self.attached:
            self.long_running[npc] = msg["id"]  # goes on until the next action of this npc
            return
        status, detail = "done", None
        if npc not in self.attached:
            status, detail = "rejected", "npc is not attached"
        elif action == "play_ani" and args.get("name") not in KNOWN_ANIMS:
            status, detail = "rejected", "unknown animation"
        elif action == "go_to_waypoint" and args.get("waypoint") not in WAYPOINTS:
            status, detail = "rejected", "unknown waypoint"
        elif action == "use_chest":
            status, detail = self.use_chest(npc, args)
        elif action == "go_to_npc" and args.get("npc") in self.npc_states:
            # it gets there: the target is as far from it as it was to stop, and in sight
            self.npc_states[args["npc"]]["distance"] = args.get("radius", 400)
            if "seen" in self.npc_states[args["npc"]]:
                self.npc_states[args["npc"]]["seen"] = True
        await self.send(
            type="action_result",
            id=msg["id"],
            npc=npc,
            status=status,
            **({"detail": detail} if detail else {}),
        )
        if action == "return_to_routine" and status == "done":
            self.attached.discard(npc)
            await self.send(type="detached", npc=npc, reason="routine")

    def use_chest(self, npc: str, args: dict[str, Any]) -> tuple[str, str | None]:
        """As the bridge does: what is there moves, as much as there is; `detail` says what moved."""
        chest = next((c for c in self.chests.get(npc, []) if c["id"] == args.get("chest")), None)
        if chest is None:
            return "rejected", "not its chest"
        mine = self.npc_items.setdefault(npc, {})
        moved: dict[str, list[str]] = {"took": [], "put": []}
        for m in args.get("take", []):
            it = next((i for i in chest["items"] if i["item"] == m["item"]), None)
            n = min(m["count"], it["count"]) if it else 0
            if it is not None and n > 0:
                it["count"] -= n
                mine[m["item"]] = mine.get(m["item"], 0) + n
                moved["took"].append(f"{m['item']}:{n}")
        for m in args.get("put", []):
            n = min(m["count"], mine.get(m["item"], 0)) or m["count"]
            it = next((i for i in chest["items"] if i["item"] == m["item"]), None)
            if it is None:
                chest["items"].append(it := {"item": m["item"], "name": m["item"], "count": 0})
            it["count"] += n
            moved["put"].append(f"{m['item']}:{n}")
        chest["items"] = [i for i in chest["items"] if i["count"] > 0]
        return "done", f"took={','.join(moved['took'])};put={','.join(moved['put'])}"


@asynccontextmanager
async def connected(fake: FakeGame):
    """Server on a free port with the fake game connected; yields the game link."""
    server = GameServer(port=0)
    serving = asyncio.create_task(server.serve_forever())
    await server.started.wait()
    game = asyncio.create_task(fake.run(f"ws://127.0.0.1:{server.port}"))
    try:
        link: GameLink = await asyncio.wait_for(server.wait_link(), 5)
        await link.wait_world(5)
        yield link
    finally:
        game.cancel()
        serving.cancel()
        await asyncio.gather(game, serving, return_exceptions=True)


class ClaimScript:
    """A giver's conversational model, scripted by what it hears: the step for a phrase in the hero's words
    or the cue (`said`), or in the outcome of its last tool call (`told`); else a plain reply."""

    name = "claim-script"

    def __init__(
        self,
        said: list[tuple[str, FakeStep]],
        told: list[tuple[str, FakeStep]],
        default: str = "Ну?",
        only: str | None = None,
        others: LLMBackend | None = None,
    ) -> None:
        """`only`: the scripted npc's name; the other npc's turns go to `others` (the live model)."""
        self.said, self.told, self.default = said, told, default
        self.only, self.others = only, others
        self.calls: list[tuple[str, list[Item], list[Tool]]] = []
        self.outputs: list[str] = []  # what came of its tool calls

    async def aclose(self) -> None:
        pass

    async def stream(
        self,
        *,
        instructions: str,
        messages: Sequence[Item],
        tools: list[Tool] | None = None,
        tool_choice: ToolChoice | None = None,
        allowed: Collection[str] | None = None,
        cache_key: str | None = None,
    ) -> AsyncIterator[LLMEvent]:
        if self.others is not None and self.only is not None and f"по имени {self.only}" not in instructions:
            async for event in self.others.stream(
                instructions=instructions,
                messages=messages,
                tools=tools,
                tool_choice=tool_choice,
                allowed=allowed,
                cache_key=cache_key,
            ):
                yield event
            return
        self.calls.append(
            (instructions, list(messages), [t for t in tools or [] if allowed is None or t.name in allowed])
        )
        last = messages[-1] if messages else None
        if isinstance(last, ToolResult):
            self.outputs.append(last.output)
            rules, text = self.told, last.output
        else:
            rules, text = self.said, last.content if isinstance(last, Message) else ""
        step = next((s for k, s in rules if k in text), FakeStep(self.default))
        fake = FakeBackend(script=[step], delay=0)
        async for event in fake.stream(
            instructions=instructions,
            messages=messages,
            tools=tools,
            tool_choice=tool_choice,
            allowed=allowed,
            cache_key=cache_key,
        ):
            yield event
