"""Scripted checks that drive an NPC without an LLM."""

from __future__ import annotations

import asyncio
import logging
import re
import time
from collections.abc import AsyncIterator, Callable, Collection, Sequence
from dataclasses import dataclass
from typing import Any, Literal

from ..brain import Outcome
from ..brain.reflex import Reflexes
from ..chronicle import Chronicle, Fact
from ..dialogue import DialogueManager, SceneLine
from ..game import GameError, GameLink
from ..gossip import Scene
from ..llm import Item, LLMBackend, LLMEvent, Message, Tool, ToolCall, ToolChoice, ToolResult
from ..llm.fake import FakeBackend, FakeStep
from ..perception import BEASTS, trace_name
from ..protocol import ActionResult, GameMessage, NpcActResult, NpcIdle, PerceptionHeld, Saved
from ..sightings import Sighting
from ..talk import TalkDesk

log = logging.getLogger("gothic_ai.scenarios")


@dataclass(frozen=True)
class Step:
    name: str
    result: ActionResult


class WitnessReaction:
    """Watch one native deed until its rule and recorded SVM finish, before moving its witness."""

    def __init__(self, link: GameLink, chronicle: Chronicle, npc: str) -> None:
        self.link, self.chronicle, self.npc = link, chronicle, npc
        self.events: list[tuple[float, NpcActResult | NpcIdle | PerceptionHeld]] = []
        self.rows: list[Fact] = []
        self.started = time.monotonic()

    def __enter__(self) -> WitnessReaction:
        self.link.listeners.append(self._record)
        return self

    def __exit__(self, *_: object) -> None:
        self.link.listeners.remove(self._record)

    def _record(self, message: GameMessage) -> None:
        if isinstance(message, NpcActResult | NpcIdle | PerceptionHeld) and message.npc == self.npc:
            self.events.append((time.monotonic(), message))

    @property
    def detail(self) -> dict[str, Any]:
        return {
            "reactions": self.rows,
            "events": [
                {"ms": round((at - self.started) * 1000), **event.model_dump()} for at, event in self.events
            ],
        }

    async def wait(self, crime: str, *, reaction: str = "shout", svm: str = "DIRTYTHIEF") -> bool:
        def decided() -> bool:
            self.rows = [
                fact
                for fact in self.chronicle.facts("crime_reaction")
                if fact.get("crime") == crime and fact.get("npc") == self.npc
            ]
            return bool(self.rows)

        if not await _until(decided, 5):
            return False
        if len(self.rows) != 1 or any(
            self.rows[0].get(key) != value
            for key, value in {"verdict": "server", "reaction": reaction, "svm": svm}.items()
        ):
            return False

        def voiced() -> bool:
            queued = {
                event.id
                for _, event in self.events
                if isinstance(event, NpcActResult) and event.action == "say_svm" and event.status == "queued"
            }
            return any(
                isinstance(event, NpcIdle) and event.act in queued and event.reason == "done"
                for _, event in self.events
            )

        return await _until(voiced, 20)


async def prepare_theft_witness(link: GameLink, npc: str) -> tuple[bool, dict[str, Any]]:
    """Stage the household's free witness and prove native sight immediately before a single take."""
    point, hero_point = "NW_FARM1_INHOUSE_02", "NW_FARM1_INHOUSE_01"
    detail: dict[str, Any] = {"npc": npc, "point": point, "attempts": []}

    async def initialized() -> bool:
        views = await link.npc_view([npc])
        if not views:
            return False
        view = views[0]
        detail["view"] = view.model_dump()
        return (
            view.theft_ready is True
            and view.state == "ZS_STAND_WP"
            and view.body == "stand"
            and view.weapon == "none"
        )

    async def visible() -> bool:
        if not await initialized():
            return False
        hero = detail["view"].get("hero")
        return bool(hero and hero["seen"] and hero["distance"] <= 1500)

    await link.debug("skip_important", near=npc)
    await link.debug("player_goto", waypoint=hero_point)
    for attempt, distance in enumerate((None, 180, 300)):
        if attempt == 0 or not await initialized():
            await link.debug("npc_goto", npc=npc, waypoint=point)
            await link.npc_act(
                npc,
                "start_activity",
                state="ZS_STAND_WP",
                waypoint=point,
                minutes=120,
                place=False,
            )
            if not await _until_async(initialized, bool, 5):
                detail["attempts"].append({"distance": distance, "view": detail.get("view")})
                continue
        # Beds and the stove can occlude the hero. Only test placement changes here; native
        # perceptions stay enabled. No second npc_act replaces the initialized standing activity.
        if distance is None:
            await link.debug("player_goto", waypoint=hero_point)
        else:
            await link.debug("teleport", near=npc, distance=distance)
        await link.debug("npc_goto", npc=npc, waypoint=point, face="PLAYER")
        if await _until_async(visible, bool, 2):
            detail["hero_in_front_cm"] = distance
            return True, detail
        detail["attempts"].append({"distance": distance, "view": detail.get("view")})
    return False, detail


async def prepare_npc_pose(
    link: GameLink,
    npc: str,
    waypoint: str,
    *,
    face: str | None = None,
    to: str | None = None,
    distance: int = 300,
    require_perception_ready: bool = True,
    perception: Literal["theft", "others_damage"] = "theft",
    timeout: float = 15,
) -> tuple[bool, dict[str, Any]]:
    """Place a free, standing actor and wait for its native state, perception and pose.

    A relative placement happens only after ZS_STAND_WP initializes. It clears the old walk while
    keeping that activity; issuing turn_to as a second soft action would replace it. Use one road
    waypoint for both actors when the later AI_GotoNpc must stay on a connected stretch.
    Distant parking may skip perception initialization until the actor returns near the hero.
    """
    detail: dict[str, Any] = {"npc": npc, "waypoint": waypoint}
    receipts: list[NpcActResult | NpcIdle] = []

    def record(message: GameMessage) -> None:
        if isinstance(message, NpcActResult | NpcIdle) and message.npc == npc:
            receipts.append(message)

    async def ready(*, unarmed: bool = True) -> bool:
        views = await link.npc_view([npc])
        view = views[0] if views else None
        detail["view"] = view.model_dump() if view is not None else None
        return (
            view is not None
            and view.state == "ZS_STAND_WP"
            and (not require_perception_ready or getattr(view, f"{perception}_ready") is True)
            and view.body == "stand"
            and (not unarmed or view.weapon == "none")
            and view.hp > 0
        )

    link.listeners.append(record)
    try:
        await link.debug("skip_important", near=npc)
        await link.debug("npc_goto", npc=npc, waypoint=waypoint)
        action = await link.npc_act(
            npc, "start_activity", state="ZS_STAND_WP", waypoint=waypoint, minutes=120, place=False
        )
        detail["action"] = action
        await _until(lambda: any(isinstance(r, NpcActResult) and r.id == action for r in receipts), 5)
        detail["receipts"] = [r.model_dump() for r in receipts]
        if not any(isinstance(r, NpcActResult) and r.id == action and r.status == "queued" for r in receipts):
            return False, detail
        if not await _until_async(lambda: ready(unarmed=False), bool, timeout):
            return False, detail
        if detail["view"]["weapon"] != "none":
            sheathe = await link.npc_act(npc, "remove_weapon")
            if not await _until(
                lambda: any(
                    isinstance(r, NpcIdle) and r.act == sheathe and r.reason == "done" for r in receipts
                ), timeout,
            ):
                detail["receipts"] = [r.model_dump() for r in receipts]
                return False, detail
        placement: dict[str, Any] = {"npc": npc}
        if to is None:
            placement["waypoint"] = waypoint
        else:
            placement.update(to=to, distance=distance)
        if face is not None:
            placement["face"] = face
        await link.debug("npc_goto", **placement)
        detail["placement"] = placement
        return await _until_async(ready, bool, timeout), detail
    finally:
        link.listeners.remove(record)


async def wait_npc_visibility(
    link: GameLink,
    witness: str,
    target: str,
    *,
    distance: float = 1500,
    perception: Literal["theft", "others_damage"] = "theft",
    state: str = "ZS_STAND_WP",
    bodies: Collection[str] = ("stand",),
    range_target: str | None = None,
    timeout: float = 5,
) -> tuple[bool, dict[str, Any]]:
    """Prove native sight, state and the deed's actual perception anchor without changing a callback."""
    anchor = range_target or target
    detail: dict[str, Any] = {
        "witness": witness, "target": target, "perception": perception,
        "expected_state": state, "range_target": anchor,
    }

    async def ready() -> bool:
        seen, actual, senses_range, anchor_distance = await link.story_eval(witness, [
            ["call", "NPC_CANSEENPC", ["inst", "SELF"], ["inst", target]],
            ["call", "NPC_GETDISTTONPC", ["inst", "SELF"], ["inst", target]],
            ["field", ["inst", "SELF"], "SENSES_RANGE"],
            ["call", "NPC_GETDISTTONPC", ["inst", "SELF"], ["inst", anchor]],
        ])
        views = await link.npc_view([witness])
        view = views[0] if views else None
        native_range = (
            view.others_damage_range if view is not None and perception == "others_damage" else senses_range
        )
        detail.update(
            seen=seen, distance=actual, senses_range=senses_range, perception_range=native_range,
            anchor_distance=anchor_distance,
            view=view.model_dump() if view is not None else None,
        )
        return (
            bool(seen)
            and actual <= min(distance, senses_range)
            and native_range is not None and anchor_distance <= native_range
            and view is not None
            and view.state == state
            and getattr(view, f"{perception}_ready") is True
            and view.body in bodies
            and view.weapon == "none"
        )

    return await _until_async(ready, bool, timeout), detail


async def wait_message[M: GameMessage](
    link: GameLink, kind: type[M], match: Callable[[M], bool] = lambda _: True, timeout: float = 120
) -> M:
    """The next message of `kind` from the game that satisfies `match`."""
    fut: asyncio.Future[M] = asyncio.get_running_loop().create_future()

    def listener(msg: GameMessage) -> None:
        if isinstance(msg, kind) and match(msg) and not fut.done():
            fut.set_result(msg)

    link.listeners.append(listener)
    try:
        return await asyncio.wait_for(fut, timeout)
    finally:
        link.listeners.remove(listener)


async def attach_when_free(link: GameLink, npc: str, *, attempts: int = 60, delay: float = 1.0) -> None:
    """Attach, retrying while the npc is busy (e.g. in a scripted dialog with the hero)."""
    for attempt in range(attempts):
        try:
            await link.attach(npc)
            return
        except GameError as err:
            if attempt == attempts - 1:
                raise
            log.info("attach %s failed (%s), retrying", npc, err)
            await asyncio.sleep(delay)


class FixedDecider:
    """A stand-in for the behavioral model: per question the first of its choices the question offers, and a
    fixed level for score questions (escalate, attitude). With `npcs` it decides so only for those; the others
    keep to what they do: the first option the question offers (continue, keep_routine) and level 0."""

    def __init__(
        self,
        choices: dict[str, str | tuple[str, ...]],
        scores: dict[str, float] | None = None,
        npcs: Collection[str] | None = None,
    ) -> None:
        self.choices = choices
        self.scores = scores or {}
        self.npcs = {n.upper() for n in npcs} if npcs is not None else None

    async def decide(self, rid, lane, priority, deadline_ms, state, questions) -> Outcome:  # noqa: ANN001
        mine = self.npcs is None or rid.split("#", 1)[0].upper() in self.npcs
        answers: dict[str, dict] = {}
        for name, q in questions.items():
            if q["type"] == "score":
                score = self.scores.get(name, 2.0) if mine else 0.0
                answers[name] = {"type": "score", "score": score, "confidence": 0.8}
                continue
            prefs = self.choices.get(name, ()) if mine else ()
            prefs = (prefs,) if isinstance(prefs, str) else prefs
            pick = next((c for c in prefs if c in q["criteria"]), next(iter(q["criteria"])))
            answers[name] = {
                "type": "choice",
                "choice": pick,
                "probabilities": {pick: 0.9},
                "confidence": 0.9,
            }
        return Outcome("answers", answers, 0.0, 1.0)

    async def cancel(self, rid: str) -> None:
        pass


async def _m5b_player_weapon(link: GameLink, state: Literal["none", "melee"], *, timeout: float = 5) -> None:
    """Wait out a saved or newly started weapon transition, with one deadline for all retries."""
    try:
        async with asyncio.timeout(timeout):
            await link.body_watch(["PLAYER"], rate_hz=10)
            while True:
                # Weapon mode changes inside the clip: an already matching mode can report
                # success before the transition ends. Wait for the animation gate as well.
                if any(row.self.interruptible for row in link.body_latest.values()):
                    try:
                        await link.debug("player_weapon", state=state)
                        return
                    except GameError as err:
                        if str(err) != "the player cannot switch weapons now":
                            raise
                        log.info("m5b player_weapon %s refused (%s), retrying", state, err)
                await asyncio.sleep(0.1)
    except TimeoutError as err:
        raise GameError(f"m5b player_weapon {state}: not ready within {timeout:g} s") from err
    finally:
        await link.body_watch([], timeout=1)


async def _clear_beasts(link: GameLink, npc: str) -> int:
    """Remove nearby beasts without crediting an NPC as their killer."""
    cleared = 0
    for other in (await link.snapshot(npc, fresh=True)).nearby_npcs:
        if not other.dead and other.guild >= BEASTS:
            await link.debug("kill", instance=other.id, near=npc)
            cleared += 1
    for view in await link.npc_view([npc]):
        for seen in view.sees:
            if seen.guild >= BEASTS and seen.body != "dead":
                await link.debug("kill", instance=seen.id, near=npc)
                cleared += 1
    return cleared


async def _until(check: Callable[[], object], timeout: float) -> bool:
    """Poll `check` until it is true or `timeout` seconds pass."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if check():
            return True
        await asyncio.sleep(0.1)
    return bool(check())


class _OnWords:
    """A scripted model that answers by what it is asked: the step of the first words found in the last
    message (the hero's phrase or an event's cue), `default` else."""

    name = "fake"

    def __init__(self, replies: list[tuple[str, FakeStep]], default: FakeStep) -> None:
        self.replies, self.default = replies, default

    async def aclose(self) -> None:
        pass

    def stream(
        self,
        *,
        instructions: str,
        messages: Sequence[Item],
        tools: list[Tool] | None = None,
        tool_choice: ToolChoice | None = None,
        allowed: Collection[str] | None = None,
        cache_key: str | None = None,
    ) -> AsyncIterator[LLMEvent]:
        last = messages[-1] if messages else None
        text = last.content if isinstance(last, Message) else ""
        step = next((s for words, s in self.replies if words.lower() in text.lower()), self.default)
        return FakeBackend(script=[step], delay=0).stream(
            instructions=instructions,
            messages=messages,
            tools=tools,
            tool_choice=tool_choice,
            allowed=allowed,
            cache_key=cache_key,
        )


class _Recorded:
    """A model as it is, and what it was given: each request's messages, for a scenario to read."""

    def __init__(self, llm: Any) -> None:
        self.llm = llm
        self.name = llm.name
        self.asked: list[str] = []

    async def aclose(self) -> None:
        await self.llm.aclose()

    def stream(self, *, messages: Sequence[Item], **kw: Any) -> AsyncIterator[LLMEvent]:
        self.asked.append("\n".join(m.content for m in messages if isinstance(m, Message)))
        return self.llm.stream(messages=messages, **kw)

    def __getattr__(self, name: str) -> Any:
        return getattr(self.llm, name)


class _ByNpc:
    """A scripted model selected by its npc cache key, then its persona prefix; `default` else."""

    name = "fake"

    def __init__(self, by: dict[str, Any], default: Any) -> None:
        self.by, self.default = by, default

    async def aclose(self) -> None:
        pass

    def stream(
        self,
        *,
        instructions: str,
        messages: Sequence[Item],
        tools: list[Tool] | None = None,
        tool_choice: ToolChoice | None = None,
        allowed: Collection[str] | None = None,
        cache_key: str | None = None,
    ) -> AsyncIterator[LLMEvent]:
        llm = self.by.get(cache_key) if cache_key is not None else None
        if llm is None:
            llm = next((b for name, b in self.by.items() if f"Ты — {name}" in instructions), self.default)
        return llm.stream(
            instructions=instructions,
            messages=messages,
            tools=tools,
            tool_choice=tool_choice,
            allowed=allowed,
            cache_key=cache_key,
        )


LOBART, HILDA = "BAU_950_LOBART", "BAU_951_HILDA"
RUEBEN, RUEBEN_TO_HILDA = "MIS_LOBART_RUEBEN", "MIS_LOBART_RUEBENTOHILDA"


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


ANDRE = "MIL_311_ANDRE"


async def _caught_by(
    link: GameLink, dialogue: DialogueManager, talk: TalkDesk, giver: str, facts: Callable[..., list[Fact]]
) -> tuple[bool, ClaimScript | None]:
    """The hero tells `giver` the turnips are in the barn and is caught (the giver scripted; with LLM=chatgpt
    the others are the live model)."""
    await link.story_effect(giver, [{"do": "set", "var": RUEBEN, "value": 1}])
    await link.debug("player_goto", to=giver, distance=250)
    talk.reflexes = Reflexes(FixedDecider({"posture": "listen"}))
    # by the roster after the move: one from before it may still have him farther than a name is heard
    if not await _until(lambda: giver in talk.people and talk.people[giver].distance <= 600, 20):
        return False, None
    name = talk.people[giver].name
    live = None if isinstance(dialogue.llm, FakeBackend) else dialogue.llm
    expose = FakeStep("Врёшь! Нет у тебя никакой репы.", [ToolCall("expose_lie", {"quest": RUEBEN})])
    script = ClaimScript(
        said=[("репа", expose)], told=[], default="Ну, здравствуй. Чего тебе?", only=name, others=live
    )
    dialogue.llm = script
    await link.debug("type", text=f"{name}, репа уже в амбаре, все двадцать штук. Давай плату.")
    return await _until(lambda: any(f.get("quest") == RUEBEN for f in facts("caught")), 60), script


def _trace_evidence_line(prompt: str, observation: Sighting) -> tuple[str, str]:
    """Find this trace's actual evidence id, even when a new hero sighting has renumbered the В lines."""
    if not observation.trace_kind or not observation.places:
        return "", ""
    stamp = f"{observation.start % 1440 // 60:02d}:{observation.start % 60:02d}"
    for line in prompt.splitlines():
        match = re.match(r"- (В\d+): ", line)
        if (
            match
            and stamp in line
            and trace_name(observation.trace_kind, observation.name) in line
            and observation.places[0] in line
        ):
            return match[1], line
    return "", ""


VINO = "BAU_952_VINO"
# the scene as the scripted model writes it: (0 the teller, 1 the listener), text, mood
GOSSIP_LINES = (
    (0, "Слыхала? Этот бродяга мне про репу наврал, глазом не моргнул.", "angry"),
    (1, "Да ну? А с виду смирный.", "neutral"),
    (0, "Смирный… Глаз с него не спускай.", "neutral"),
)


class GossipScript:
    """The conversational model of the gossip e2e (and the day e2e's), routed by what is asked: a scene (the
    tool `lines`) gets `lines` (GOSSIP_LINES) with the two names the tool allows, Hilda's answer to the hero
    is hers, anything else goes to Lobart's script (which sends the others to the live model itself). With
    `live` (LLM=chatgpt) the scene and Hilda are the live model."""

    name = "gossip-script"

    def __init__(
        self,
        claim: LLMBackend,
        hilda: LLMBackend,
        live: LLMBackend | None = None,
        *,
        lines: Sequence[tuple[int, str, str]] = GOSSIP_LINES,
        quiet: bool = True,
        reason: str = "сплетничают о герое, пока он рядом",
    ) -> None:
        self.claim, self.hilda, self.live = claim, hilda, live
        self.lines = lines
        self.quiet, self.reason = quiet, reason
        self.lines_calls = 0  # how often a scene was written

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
        lines = next((t for t in tools or [] if t.name == "lines"), None)
        backend: LLMBackend
        if lines is not None:
            self.lines_calls += 1
            call = ToolCall("lines", _gossip_lines(lines, self.lines, self.quiet, self.reason), "gossip")
            backend = self.live or FakeBackend(script=[FakeStep("", [call])], delay=0)
        elif "по имени Хильда" in instructions:
            backend = self.live or self.hilda
        else:
            backend = self.claim
        async for event in backend.stream(
            instructions=instructions,
            messages=messages,
            tools=tools,
            tool_choice=tool_choice,
            allowed=allowed,
            cache_key=cache_key,
        ):
            yield event


def _gossip_lines(
    tool: Tool,
    lines: Sequence[tuple[int, str, str]] = GOSSIP_LINES,
    quiet: bool = True,
    reason: str = "сплетничают о герое, пока он рядом",
) -> dict[str, Any]:
    """The scripted lines (0 the teller, 1 the listener) as the tool `lines` takes them: its schema lists the
    teller's name first."""
    try:
        names = tool.parameters["properties"]["lines"]["items"]["properties"]["speaker"]["enum"]
        teller, listener = str(names[0]), str(names[1])
    except (KeyError, IndexError, TypeError):
        teller, listener = "Лобарт", "Хильда"
    return {
        "lines": [
            {"speaker": (teller, listener)[who], "text": text, "mood": mood} for who, text, mood in lines
        ],
        "quiet": quiet,
        "reason": reason,
    }


async def _save(link: GameLink, slot: int, attempts: int = 15) -> Saved | None:
    """Save into the slot, again while the game refuses (in a dialog); None if it never saved."""
    for _ in range(attempts):
        saving = asyncio.ensure_future(wait_message(link, Saved, timeout=60))
        try:
            await link.debug("save", slot)
        except GameError as err:
            saving.cancel()
            log.info("save refused (%s), retrying", err)
            await asyncio.sleep(2)
            continue
        try:
            return await saving
        except TimeoutError:
            return None
    return None


async def _save_or_fail(link: GameLink, slot: int) -> Saved:
    """Save with retries, failing the scenario if the game never saves."""
    saved = await _save(link, slot)
    if saved is None:
        raise RuntimeError(f"Game did not save to slot {slot}")
    return saved


def _passes_text(facts: list[Fact]) -> str:
    """Rumor facts in short, for a check's detail."""
    passes = [f"from {f.get('from')}, how {f.get('how')}, hops {f.get('hops')}" for f in facts]
    return f"{len(facts)} fact(s){': ' if passes else ''}{'; '.join(passes)}"


def _scene_lines(scene: Scene | None) -> list[SceneLine]:
    """The lines of a scene as they were given to the game."""
    return list(scene.play.lines) if scene is not None and scene.play is not None else []


async def _until_async(get: Callable[[], Any], ok: Callable[[Any], bool], timeout: float) -> bool:
    """Poll an async getter until its value is ok or `timeout` seconds pass."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if ok(await get()):
            return True
        await asyncio.sleep(0.25)
    return False


def _logged(results: dict[str, str]) -> dict[str, str]:
    for key, outcome in results.items():
        log.info("%-15s %s", key, outcome)
    return results
