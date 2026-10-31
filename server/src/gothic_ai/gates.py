"""A gatekeeper's admission: native grounds, a paid bribe, or the hero's believed word."""

from __future__ import annotations

import asyncio
import json
import logging
import uuid
from pathlib import Path
from typing import TYPE_CHECKING, Any

from .chronicle import Chronicle, Fact
from .crime_law import DAY_MINUTES, game_minutes
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
from .quests import Lie, caught
from .turn import ToolOutcome

if TYPE_CHECKING:
    from .crimes import Crimes
    from .dialogue import Conversation, DialogueManager

KEEPERS = {
    "MIL_310_STADTWACHE": (100, 60),
    "MIL_333_STADTWACHE": (100, 60),
    "MIL_305_TORWACHE": (0, 0),
}
log = logging.getLogger(__name__)
VOUCHERS = json.loads((Path(__file__).parent / "gamedata" / "gates.json").read_text())
APPRENTICES = {"VLK_413_BOSPER": 1, "VLK_412_HARAD": 2, "VLK_417_CONSTANTINO": 3}
VOUCH_FOR_HERO = Tool(
    "vouch_for_hero",
    "Поручиться за героя перед городскими воротами на двое игровых суток. Решай по своему характеру: "
    "ты отвечаешь за него, и известное городу преступление подорвёт твоё доверие к нему и твою репутацию. "
    "Стражник узнает, если ты рядом с ним (до 10 м), или сходишь рассказать именно ему через plan tell. "
    "note — почему ты согласился поручиться, от 1 до 200 знаков.",
    {"type": "object", "properties": {"note": {"type": "string", "maxLength": 200}},
     "required": ["note"], "additionalProperties": False},
)
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


class GateResultUnknown(GameError):
    """The gate program may have run; retrying its payment would be unsafe."""


LET_PASS = Tool(
    "let_pass",
    "Разрешить герою проход. basis — id настоящего основания из блока «Ворота», либо claim, "
    "если ты поверил его словам и отвечаешь за это сам. Для claim запиши его конкретное заявление "
    "(до 200 знаков) и символ NPC, на которого он сослался, либо пустую строку. "
    "basis vouch — дошедшее до тебя действующее поручительство из блока «Ворота». "
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


def _claim_id(claim: Fact) -> str:
    # Step 1 saves have the guard and clock, but no explicit claim id.
    return claim.get("id") or f"{claim['npc']}_{claim['day']}_{claim['hour']}_{claim['minute']}"


class Gates:
    all_tools: tuple[Tool, ...] = (LET_PASS, VOUCH_FOR_HERO)

    def __init__(
        self, link: GameLink, dialogue: DialogueManager, chronicle: Chronicle, graph: StoryGraph,
        *, crimes: Crimes | None = None,
    ) -> None:
        self.link, self.dialogue, self.chronicle, self.graph = link, dialogue, chronicle, graph
        self.crimes = crimes
        self._posts: set[str] = set()
        self.grounds: dict[str, dict[str, Node]] = {}
        self.blocked: set[str] = set()
        self.vouches: dict[str, list[Fact]] = {}
        self._now = 0
        self._claim_lock = asyncio.Lock()
        dialogue.extensions.append(self)
        link.listeners.append(self._on_message)
        chronicle.on_fact.append(self._on_fact)

    def _on_message(self, msg: GameMessage) -> None:
        if isinstance(msg, Session):
            self._posts.clear()
            self.grounds.clear()
            self.blocked.clear()
            self.vouches.clear()

    def on_post(self, npc: str) -> bool:
        return npc.upper() in self._posts

    async def prepare(self, npc: str) -> None:
        npc = npc.upper()
        self._posts.discard(npc)
        self.grounds.pop(npc, None)
        self.blocked.discard(npc)
        self.vouches.pop(npc, None)
        if npc not in KEEPERS:
            if npc in VOUCHERS:
                self._now = game_minutes(await self.link.story_state()) or 0
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
        self.vouches[npc] = await self._known_vouches(npc)

    def situation(self, npc: str) -> str | None:
        npc = npc.upper()
        if not self.on_post(npc):
            if npc in VOUCHERS:
                return (
                    "Ворота: ты можешь поручиться за героя на двое суток, если он убедит тебя. "
                    "За известное городу преступление героя ты отвечаешь своим добрым именем. "
                    "Сообщи поручительство привратнику лично рядом или через plan tell именно ему."
                )
            return None
        lines = ["Ворота:"]
        if npc in self.blocked:
            lines.append("Известно неоплаченное преступление героя: проход закрыт по любому основанию.")
        else:
            lines.extend(
                f"{GROUNDS[n.id.rsplit('_', 1)[-1]]} (basis {n.id})."
                for n in self.grounds.get(npc, {}).values()
            )
            for vouch in self.vouches.get(npc, []):
                when = "сегодня" if vouch["day"] == self._now // DAY_MINUTES else f"в день {vouch['day'] + 1}"
                lines.append(
                    f"За героя поручился {vouch['name']}, {vouch['role']}, {when} "
                    f"в {vouch['hour']:02d}:{vouch['minute']:02d} (basis vouch)."
                )
            if not self.grounds.get(npc) and not self.vouches.get(npc):
                lines.append("Подтверждённых оснований сейчас нет.")
        start, minimum = KEEPERS[npc]
        if self._caught(npc):
            minimum *= 2
            start *= 2
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
        for claim in self.chronicle.facts("gate_claim"):
            if (claim.get("npc") == npc and claim.get("about")
                    and not any(f.get("claim") == _claim_id(claim) and f.get("truth") is not None
                                for f in self.chronicle.facts("gate_claim_checked"))):
                lines.append(
                    f"Принятое заявление: «{claim['claim']}»; назван {claim['about']}. "
                    "Для его проверки через ask_witness скопируй заявление в said дословно."
                )
        return "\n".join(lines)

    def tools(self, npc: str) -> list[Tool]:
        return ([VOUCH_FOR_HERO] if npc.upper() in VOUCHERS else []) + (
            [LET_PASS] if self.on_post(npc) else []
        )

    async def _vouch(self, conv: Conversation, call: ToolCall) -> ToolOutcome:
        npc = conv.npc.upper()
        if npc not in VOUCHERS:
            return ToolOutcome("Rejected: you cannot vouch for city admission.", "rejected")
        note = str(call.arguments.get("note", "")).strip()
        if not 1 <= len(note) <= 200:
            return ToolOutcome("Rejected: note must contain 1 to 200 characters.", "rejected")

        session = self.link.session
        state = await self.link.story_state()
        if session is not self.link.session:
            return ToolOutcome("Rejected: world reloaded.", "rejected")
        at = game_minutes(state)
        assert at is not None
        self._now = at
        self.chronicle.record({
            "fact": "gate_vouch", "id": uuid.uuid4().hex, "npc": npc, **VOUCHERS[npc],
            "note": note, "at": at, "expires": at + 2 * DAY_MINUTES,
            **{k: int(state[k]) for k in ("day", "hour", "minute")},
        })
        return ToolOutcome("Ты поручился за героя на двое суток; сообщи об этом привратнику.", "accepted")

    def _active(self, now: int) -> list[Fact]:
        return [f for f in self.chronicle.facts("gate_vouch") if f["at"] <= now < f["expires"]]

    async def _known_vouches(self, npc: str) -> list[Fact]:
        session = self.link.session
        state = await self.link.story_state()
        if session is not self.link.session:
            return []
        self._now = game_minutes(state) or 0
        active = self._active(self._now)
        if not active:
            return []
        people = (await self.link.story_state(npcs={v["npc"] for v in active}, near=npc)).get("npcs", {})
        if session is not self.link.session:
            return []
        heard = {f["vouch"] for f in self.chronicle.facts("gate_vouch_heard") if f.get("to") == npc}
        out = []
        for v in active:
            person = people.get(v["npc"], {})
            distance = person.get("distance")
            if (v["id"] not in heard and person.get("state") not in {"dead", "absent"}
                    and distance is not None and 0 <= distance <= 1000):
                self.chronicle.record({
                    "fact": "gate_vouch_heard", "vouch": v["id"], "to": npc,
                    "from": v["npc"], "at": self._now,
                })
                heard.add(v["id"])
            if v["id"] in heard:
                out.append(v)
        return out

    async def claim_truth(self, about: str) -> bool:
        """A relationship established by code, never by the hero's or a model's assertion."""
        about = about.upper()
        for fact in self.chronicle.facts():
            kind = fact.get("fact")
            if (kind in {"gate_vouch", "contract", "quest_given", "deal", "meet"}
                    and fact.get("npc") == about):
                return True
            if kind == "quest" and fact.get("giver") == about:
                return True
        quests = [q.var for q in self.graph.quests_of(about)
                  if any(self.graph.nodes[n].npc == about for n in q.running)]
        state = await self.link.story_state(vars=["PLAYER_ISAPPRENTICE", *quests])
        values = state.get("vars", {})
        if about in APPRENTICES and values.get("PLAYER_ISAPPRENTICE") == APPRENTICES[about]:
            return True
        return any(values.get(q) in (1, 2) for q in quests)

    def _caught(self, npc: str) -> bool:
        return any(f.get("npc") == npc and f.get("gate_claim") for f in self.chronicle.facts("caught"))

    def news(self, npc: str, to: str) -> tuple[str, str] | None:
        """Only the sponsor's word and the claim its actual listener knows can be carried."""
        npc, to = npc.upper(), to.upper()
        if to in KEEPERS:
            heard = {f["vouch"] for f in self.chronicle.facts("gate_vouch_heard") if f.get("to") == to}
            for v in reversed(self._active(self._now)):
                if v["npc"] == npc and v["id"] not in heard:
                    return f"gate:vouch:{v['id']}", f"{v['name']} поручился за героя"
        for claim in reversed(self.chronicle.facts("gate_claim")):
            if (claim.get("about") == to and claim.get("npc") == npc
                    and not any(f.get("claim") == _claim_id(claim) and f.get("truth") is not None
                                for f in self.chronicle.facts("gate_claim_checked"))):
                return f"gate:claim:{_claim_id(claim)}", claim["claim"]
        return None

    async def told(self, key: str, npc: str, to: str) -> None:
        kind, id = key.removeprefix("gate:").split(":", 1)
        if kind == "vouch" and to in KEEPERS:
            session = self.link.session
            state = await self.link.story_state()
            if session is not self.link.session:
                return
            at = game_minutes(state) or 0
            if any(v["id"] == id and v["npc"] == npc for v in self._active(at)):
                self.chronicle.record({
                    "fact": "gate_vouch_heard", "vouch": id, "from": npc, "to": to, "at": at,
                })
        elif kind == "claim":
            claim = next((f for f in self.chronicle.facts("gate_claim") if _claim_id(f) == id), None)
            if claim is not None and claim.get("npc") == npc and claim.get("about") == to:
                await self._check_claim(claim, to, npc)

    async def claim_heard(self, witness: str, asker: str, said: str) -> str | None:
        said = said.strip().strip("«»\"' .")  # ask_witness strips these before calling its hooks
        checked = {f["claim"] for f in self.chronicle.facts("gate_claim_checked")
                   if f.get("truth") is not None}
        claims = [f for f in self.chronicle.facts("gate_claim")
                  if f.get("about") == witness.upper()
                  and f.get("claim", "").strip().strip("«»\"' .") == said
                  and _claim_id(f) not in checked]
        if not claims:
            return None
        answer = None
        for claim in claims:
            answer = await self._check_claim(claim, witness, asker)
        return answer

    async def _check_claim(self, claim: Fact, witness: str, asker: str) -> str:
        async with self._claim_lock:
            session = self.link.session
            if claim not in self.chronicle.facts("gate_claim"):
                return ""
            checked = next((f for f in self.chronicle.facts("gate_claim_checked")
                            if f.get("claim") == _claim_id(claim) and f.get("truth") is not None), None)
            truth = checked["truth"] if checked is not None else claim.get("truth")
            if truth is None:
                truth = await self.claim_truth(witness)
            if session is not self.link.session:
                return ""
            answer = (
                "Подтверждаю: у героя есть настоящее дело со мной."
                if truth else "Отрицаю: у героя нет настоящего дела со мной, он солгал у ворот."
            )
            if checked is not None:
                return answer
            if not truth:
                await self._expose(claim, f"проверил заявление у {witness}")
            if session is not self.link.session:
                return ""
            self.dialogue.remember(
                witness, witness, f"Тебе передали заявление героя «{claim['claim']}». {answer}",
            )
            self.chronicle.record({
                "fact": "gate_claim_checked", "claim": _claim_id(claim), "npc": witness.upper(),
                "to": asker.upper(), "truth": bool(truth),
            })
            return answer

    def _city_person(self, person: dict[str, Any]) -> bool:
        guild, npctype = int(person.get("guild", -1)), int(person.get("npctype", 0))
        if self.crimes is not None:
            return self.crimes.community(guild, npctype) == "city"
        return guild in (1, 2, 3) and npctype not in (3, 4)

    async def _expose(self, claim: Fact, how: str) -> None:
        session = self.link.session
        if claim not in self.chronicle.facts("gate_claim"):
            return
        id = _claim_id(claim)
        if any(f.get("gate_claim") == id for f in self.chronicle.facts("caught")):
            return
        npc = claim["npc"]
        guards = list(CITY_GATE_NPCS) if npc in CITY_GATE_NPCS else [npc]
        steps = [
            {"do": "set", "target": ["field", ["inst", guard], "AIVAR", 14], "value": ["int", 0]}
            for guard in guards
        ] + [
            {"do": "set", "target": ["var", f"MIL_{guard.split('_')[1]}_SCHONMALREINGELASSEN"],
             "value": ["int", 0]} for guard in guards
        ]
        # Revocation is unconditional, including when the hero has an unpaid crime.
        await _run(self.link, "PC_HERO", steps, ["int", 1], ["int", 1])
        if session is not self.link.session:
            return
        lie = Lie(f"SAID_GATE_{id}", npc, "Привратник", claim["claim"], "проход в город", said=True)
        await caught(self.link, self.dialogue, self.chronicle, self.dialogue.journal, lie, how,
                     extra={"gate_claim": id})
        people = await self.link.story_people()
        if session is not self.link.session:
            return
        for guard in people:
            if (guard["npc"] == npc or guard.get("guild") not in (1, 2)
                    or not self._city_person(guard)):
                continue
            self.chronicle.record({
                "fact": "rumor", "kind": "caught", "quest": lie.quest, "said": True,
                "about": npc, "about_name": lie.giver_name, "title": lie.title,
                "from": npc, "from_name": lie.giver_name, "to": guard["npc"], "hops": 1,
                "how": "gate_check", "day": self._now // DAY_MINUTES,
            })

    async def check_claims(self, state: dict[str, Any] | None = None) -> None:
        session = self.link.session
        checked = {f.get("claim") for f in self.chronicle.facts("gate_claim_checked")}
        pending = [f for f in self.chronicle.facts("gate_claim")
                   if f.get("about") and _claim_id(f) not in checked]
        if not pending:
            return
        state = state if state is not None else await self.link.story_state()
        if session is not self.link.session:
            return
        self._now = game_minutes(state) or 0
        claims = [f for f in pending if self._now >= (f["day"] + 1) * DAY_MINUTES + 6 * 60]
        if not claims:
            return
        people = {p["npc"]: p for p in await self.link.story_people()}
        if session is not self.link.session:
            return
        for claim in claims:
            about = claim["about"]
            person = people.get(about, {})
            if (not self._city_person(person)
                    or not any(r.get("waypoint") and r.get("pos") for r in person.get("routine", []))):
                self.chronicle.record({
                    "fact": "gate_claim_checked", "claim": _claim_id(claim), "npc": about,
                    "truth": None, "skipped": "not a reachable city person",
                })
                continue
            live = (await self.link.story_state(npcs=[about])).get("npcs", {}).get(about, {})
            if session is not self.link.session:
                return
            if live.get("state") in (None, "dead", "absent"):
                self.chronicle.record({
                    "fact": "gate_claim_checked", "claim": _claim_id(claim), "npc": about,
                    "truth": None, "skipped": "named person is dead or absent",
                })
                continue
            await self._check_claim(claim, about, claim["npc"])

    async def run(self) -> None:
        while True:
            try:
                checked = {f.get("claim") for f in self.chronicle.facts("gate_claim_checked")}
                if any(f.get("about") and _claim_id(f) not in checked
                       for f in self.chronicle.facts("gate_claim")):
                    await self.check_claims()
            except (GameError, TimeoutError) as error:
                log.warning("gate claim check: %s", error)
            await asyncio.sleep(1)

    def _on_fact(self, fact: Fact) -> None:
        if fact.get("fact") != "crime_known" or fact.get("community") != "city":
            return
        deed = next((f for f in self.chronicle.facts("crime") if f.get("id") == fact.get("crime")), None)
        at = game_minutes(fact)
        if deed is None or deed.get("community") != "city" or at is None:
            return
        for v in self.chronicle.facts("gate_vouch"):
            if not v["at"] <= at < v["expires"] + 3 * DAY_MINUTES:
                continue
            if any(f.get("vouch") == v["id"] and f.get("crime") == deed["id"]
                   for f in self.chronicle.facts("gate_vouch_broken")):
                continue
            title = f"{v['name']} поручился за преступника"
            observers = [npc for npc in ("MIL_311_ANDRE", "PAL_200_HAGEN") if npc != v["npc"]]
            self.chronicle.record({
                "fact": "gate_vouch_broken", "id": uuid.uuid4().hex, "vouch": v["id"],
                "npc": v["npc"], "name": v["name"], "crime": deed["id"], "title": title,
                "crime_kind": deed["kind"],
                "witnesses": observers,
                "witness_names": {npc: VOUCHERS[npc]["name"] for npc in observers},
                "at": at, "day": at // DAY_MINUTES,
            })
            self.dialogue.remember(
                v["npc"], v["name"],
                f"Городу стало известно преступление героя: {deed.get('kind', '')}. "
                "Ты поручился за него и отвечаешь своим именем. Герой подорвал твоё доверие.",
            )

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
        if call.name == "vouch_for_hero":
            return await self._vouch(conv, call)
        if call.name != "let_pass":
            return None
        npc = conv.npc.upper()
        if reason := await self._allowed(npc):
            return ToolOutcome(f"Rejected: {reason}.", "rejected")
        basis = str(call.arguments.get("basis", ""))
        claim = str(call.arguments.get("claim", "")).strip()
        about = str(call.arguments.get("about", "")).strip().upper()
        node = None
        vouch = basis.lower() == "vouch"
        if basis.lower() == "claim":
            if not claim or len(claim) > 200:
                return ToolOutcome("Rejected: claim must contain 1 to 200 characters.", "rejected")
            if about:
                symbols = (await self.link.story_state(symbols=[about])).get("symbols", {})
                if symbols.get(about) != "npc":
                    return ToolOutcome(
                        "Rejected: about must be an NPC symbol or an empty string.", "rejected"
                    )
        elif vouch:
            if not await self._known_vouches(npc):
                return ToolOutcome("Rejected: no active vouch has reached you.", "rejected")
        else:
            node = self.grounds.get(npc, {}).get(basis.upper())
            if node is None or not (await self.link.story_eval(npc, [_ground(node)]))[0]:
                return ToolOutcome("Rejected: this ground is not currently verified by code.", "rejected")

        async def admit() -> tuple[str, bool]:
            session = self.link.session
            claim_fact: Fact | None = None
            try:
                if reason := await self._allowed(npc):
                    return f"Passage refused: {reason}.", False
                state = await self.link.story_state()
                if vouch and not await self._known_vouches(npc):
                    return "Passage refused: the vouch expired or has not reached you.", False
                truth = await self.claim_truth(about) if about and not vouch and node is None else None
                if node is None and not vouch:
                    claim_fact = {
                        "fact": "gate_claim", "id": uuid.uuid4().hex, "npc": npc,
                        "claim": claim, "about": about, "believed": True, "truth": truth,
                        **{k: int(state[k]) for k in ("day", "hour", "minute")},
                    }
                if node is not None:
                    program = await self.link.story_program(node.id, own=True)
                    await _run(
                        self.link, npc, _quiet(program["steps"]), _ground(node), _post(self.graph, npc)
                    )
                    await self.link.story_effect(npc, [{"do": "knows", "info": node.id}])
                else:
                    await _run(self.link, npc, _admission(npc), ["int", 1], _post(self.graph, npc))
                    if claim_fact is not None and session is self.link.session:
                        self.chronicle.record(claim_fact)
            except GateResultUnknown as error:
                kept = ""
                if claim_fact is not None and session is self.link.session:
                    self.chronicle.record({**claim_fact, "admission": "unknown"})
                    kept = " Your accepted claim has been kept."
                return f"Passage outcome is unknown: {error}.{kept}", False
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
        if self._caught(npc):
            minimum *= 2
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
    listening = False
    try:
        # Removing the old program must succeed before resetting its one-shot counter.
        await link.patch_graph([{"op": "trigger", "id": trigger, "steps": None}])
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
        link.listeners.append(hear)
        listening = True
        try:
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
            if values == [0, 0]:
                # The program can still fire between this read and its removal.
                await link.patch_graph([{"op": "trigger", "id": trigger, "steps": None}])
                values = await link.story_eval(npc, [["var", trigger], ["var", result]])
                if session is not link.session:
                    raise GameError("world reloaded")
        except (GameError, TimeoutError) as error:
            raise GateResultUnknown(f"gate admission outcome is unknown: {error}") from error
        if values == [0, 0]:
            raise GameError("passage refused: gate program did not run")
        if not values or values[0] != 1:
            raise GateResultUnknown("gate admission outcome is unknown: program completion unconfirmed")
        if values != [1, 1]:
            raise GameError(
                "passage refused: unpaid crime, invalid ground, guard off post or insufficient gold"
            )
    finally:
        if listening:
            link.listeners.remove(hear)
        try:
            if session is link.session:
                await link.patch_graph([{"op": "trigger", "id": trigger, "steps": None}])
        except (GameError, TimeoutError):
            # The next run must remove it before resetting the counter. Cleanup cannot turn a
            # confirmed transfer into an apparent failure and cause the model to charge again.
            pass
        finally:
            _RUNNING.discard(key)
