from __future__ import annotations

import asyncio
import logging
import time
from collections.abc import Sequence
from typing import Any

from ..bodies import Bodies
from ..brain import Decider, NpcPlanner
from ..brain.reflex import Reflexes
from ..budget import Budget
from ..dialogue import DialogueManager
from ..game import GameError, GameLink
from ..incidents import IncidentDesk
from ..journal import Journal
from ..knowledge import Knowledge, Topic
from ..llm import LLMBackend, Message, ToolCall, ToolResult
from ..llm.fake import FakeBackend, FakeStep
from ..llm.openai_responses import OpenAIResponsesBackend
from ..npc_tools import GESTURES as NPC_GESTURES
from ..perception import BEASTS
from ..persona import PersonaProvider, habits_of
from ..protocol import (
    ActionResult,
    Detached,
    FollowEnded,
    GameMessage,
    LineStarted,
    NpcActResult,
    NpcActType,
    NpcEvent,
    NpcIdle,
    NpcRoster,
    Perception,
    PerceptionHeld,
    Saved,
    SayCmd,
    Session,
    Snapshot,
    ThreatControlCmd,
)
from ..talk import TalkDesk
from ..trace import TraceWriter
from ..trace import read as read_trace
from ..voice import VoiceInput
from .common import (
    HILDA,
    LOBART,
    FixedDecider,
    Step,
    _logged,
    _m5b_player_weapon,
    _until,
    _until_async,
    attach_when_free,
    wait_message,
)
from .responses_server import ResponsesServer, reply

log = logging.getLogger("gothic_ai.scenarios")

GESTURES = ("T_COMEOVERHERE", "T_GREETNOV", "T_YES")
QUICK_LINE_S = (
    1.5  # m5 with the quick model: its line starts this soon (the conversational model takes 3-5 s)
)


async def m0_smoke(link: GameLink, npc: str = "NONE_100_XARDAS") -> list[Step]:
    """M0 check: take the npc over, walk him around, gesture, bring him home and back to his routine."""
    world = await link.wait_world()
    log.info("world loaded: %s", world)
    await attach_when_free(link, npc)

    snap = await link.snapshot(npc)
    home = snap.npc.waypoint or snap.npc.nearest_waypoint
    away = max(snap.nearby_waypoints, key=lambda w: w.distance, default=None)
    log.info("%s (%s) home=%s away=%s", snap.npc.id, snap.npc.name, home, away and away.name)

    steps: list[Step] = []

    async def step(name: str, coro) -> ActionResult:
        res = await coro
        steps.append(Step(name, res))
        extra = ", ".join(
            x for x in (res.detail, f"{res.distance:.0f}cm" if res.distance is not None else None) if x
        )
        log.info("%-24s %s%s", name, res.status, f" ({extra})" if extra else "")
        return res

    if away is not None:
        await step(f"go_to_waypoint {away.name}", link.act(npc, "go_to_waypoint", waypoint=away.name))
    await step("turn_to PLAYER", link.act(npc, "turn_to", npc="PLAYER"))
    for gesture in GESTURES:
        res = await step(f"play_ani {gesture}", link.act(npc, "play_ani", name=gesture))
        if res.status != "rejected":
            break
    await step("wait 1500", link.act(npc, "wait", ms=1500))
    await step("go_to_npc PLAYER", link.act(npc, "go_to_npc", npc="PLAYER"))
    if home is not None:
        await step(f"go_to_waypoint {home}", link.act(npc, "go_to_waypoint", waypoint=home))
    await step("return_to_routine", link.act(npc, "return_to_routine"))
    return steps


async def m1_chat(
    link: GameLink, dialogue: DialogueManager, npc: str = "NONE_100_XARDAS", text: str = "Кто ты такой?"
) -> list[str]:
    """M1 check without the keyboard: player text injected on the server, the npc must speak every line."""
    await link.wait_world()
    await attach_when_free(link, npc)
    snap = await link.snapshot(npc)
    conv = dialogue.open(npc, snap.npc.name)

    started: list[str] = []
    link.listeners.append(lambda msg: started.append(msg.line) if isinstance(msg, LineStarted) else None)

    await dialogue.say_as_player(npc, text)
    lines = conv.last_lines
    log.info("%s said %d line(s), waiting for the npc to speak them", npc, len(lines))
    for _ in range(20 * max(1, len(lines))):  # ~10 s per line
        if all(line in started for line in lines):
            break
        await asyncio.sleep(0.5)
    await link.act(npc, "return_to_routine")
    missing = [line for line in lines if line not in started]
    if not lines or missing:
        raise RuntimeError(f"npc did not speak lines: {missing or 'no lines generated'}")
    return lines


async def websocket(link: GameLink, dialogue: DialogueManager) -> dict[str, str]:
    """Responses transports without a live model: farm conversations reach the game's line_started.

    Lobart speaks twice with Hilda's turn between his; his second turn reads the offered recall tool.
    Fresh backends then exercise a refused handshake (including the HTTP cooldown) and HTTP only.
    """
    await link.wait_world()
    original = dialogue.llm
    started: set[tuple[str, str]] = set()
    results: dict[str, str] = {}

    def on_message(msg: GameMessage) -> None:
        if isinstance(msg, LineStarted):
            started.add((msg.npc, msg.line))

    def check(name: str, ok: bool, detail: str) -> None:
        results[name] = f"{'done' if ok else 'failed'} ({detail})"

    async def token() -> str:
        return "e2e-placeholder"

    async def say(npc: str, text: str) -> list[str]:
        await link.debug("player_goto", to=npc, distance=250)
        snap = await link.snapshot(npc, fresh=True)
        conv = dialogue.open(npc, snap.npc.name)
        await dialogue.say_as_player(npc, text)
        lines = list(conv.last_lines)
        if lines:
            await _until(lambda: all((npc, line) in started for line in lines), 20)
            await _until(lambda: not dialogue.speaking(npc), 15)
        return lines

    def heard(npc: str, lines: list[str]) -> int:
        return sum((npc, line) in started for line in lines)

    link.listeners.append(on_message)
    try:
        for npc in (LOBART, HILDA):
            await link.debug("skip_important", near=npc)
            await attach_when_free(link, npc)

        turns = {LOBART: 0, HILDA: 0}
        call_id = "e2e_recall"
        call_request: dict[str, Any] = {}
        call_response = ""

        def answer(request: dict[str, Any], n: int) -> list[dict[str, Any]]:
            nonlocal call_response
            key = request.get("prompt_cache_key")
            if any(i.get("call_id") == call_id for i in request["input"]):
                return reply(f"ws_{n}", "Вспомнил, работа найдётся.")
            if key in turns:
                turns[key] += 1
            if key == LOBART and turns[LOBART] == 2:
                call_request.update(request)
                call_response = f"ws_{n}"
                return reply(
                    call_response,
                    "",
                    call={
                        "type": "function_call",
                        "id": "fc_e2e",
                        "call_id": call_id,
                        "name": "recall",
                        "arguments": '{"query":"ферма"}',
                    },
                )
            return reply(f"ws_{n}", "На ферме всё спокойно." if key == HILDA else "Здравствуй, путник.")

        async with ResponsesServer(answer) as server:
            backend = OpenAIResponsesBackend("e2e", token, base_url=server.base_url, websocket=True)
            dialogue.llm = backend
            try:
                first = await say(LOBART, "Здравствуй, Лобарт.")
                hilda = await say(HILDA, "Здравствуй, Хильда.")
                second = await say(LOBART, "Вспомни, какая работа есть на ферме?")
                lobart = [r for c in server.connections for r in c if r.get("prompt_cache_key") == LOBART]
                lines = [*first, *second]
                spoken = heard(LOBART, lines)
                check(
                    "ws_said",
                    bool(first and second) and spoken == len(lines) and len(lobart) == 3
                    and all(r.get("type") == "response.create" for r in lobart)
                    and not server.http_requests,
                    f"Lobart line_started={spoken}/{len(lines)}, response.create={len(lobart)}/3, "
                    f"cache_keys={sum(r.get('prompt_cache_key') == LOBART for r in lobart)}, "
                    f"HTTP={len(server.http_requests)}",
                )
                own = {
                    npc: {i for i, c in enumerate(server.connections)
                          if any(r.get("prompt_cache_key") == npc for r in c)}
                    for npc in (LOBART, HILDA)
                }
                check(
                    "ws_own",
                    len(server.connections) == 2 and len(own[LOBART]) == len(own[HILDA]) == 1
                    and own[LOBART].isdisjoint(own[HILDA]) and turns == {LOBART: 2, HILDA: 1}
                    and bool(hilda) and heard(HILDA, hilda) == len(hilda),
                    f"connections={len(server.connections)}/2, Lobart={len(own[LOBART])} "
                    f"for {turns[LOBART]} turns, Hilda={len(own[HILDA])} for {turns[HILDA]} turn, "
                    f"Hilda line_started={heard(HILDA, hilda)}/{len(hilda)}",
                )
                continued = [r for r in lobart if r.get("previous_response_id") == call_response]
                added = continued[0]["input"] if len(continued) == 1 else []
                choice = call_request.get("tool_choice")
                offered = any(t.get("name") == "recall" for t in call_request.get("tools", []))
                if isinstance(choice, dict):
                    offered = offered and any(t.get("name") == "recall" for t in choice.get("tools", []))
                else:
                    offered = offered and choice != "none"
                same = any(
                    any(r is continued[0] for r in c)
                    and any(r.get("input") == call_request.get("input") for r in c)
                    for c in server.connections
                ) if continued else False
                check(
                    "ws_continued",
                    bool(call_response) and offered and same and len(continued) == len(added) == 1
                    and added[0].get("type") == "function_call_output"
                    and added[0].get("call_id") == call_id and bool(added[0].get("output"))
                    and not added[0]["output"].startswith("Отклонено"),
                    f"offered_recall={int(offered)}, matching_previous_response_id={len(continued)}/1, "
                    f"same_connection={int(same)}, added_items={len(added)}/1 "
                    f"vs first_input={len(call_request.get('input', []))}",
                )
            finally:
                dialogue.llm = original
                await backend.aclose()

        class Warnings(logging.Handler):
            def __init__(self) -> None:
                super().__init__(logging.WARNING)
                self.count = 0

            def emit(self, record: logging.LogRecord) -> None:
                if "no websocket" in record.getMessage() and "over HTTP" in record.getMessage():
                    self.count += 1

        warnings = Warnings()
        transport_log = logging.getLogger("gothic_ai.llm.openai_responses")
        fallback = ResponsesServer(lambda r, n: reply(f"fallback_{n}", "И по старой дороге дойдём."))
        async with fallback as server:
            server.refuse_websocket = True
            backend = OpenAIResponsesBackend("e2e", token, base_url=server.base_url, websocket=True)
            dialogue.llm = backend
            transport_log.addHandler(warnings)
            try:
                first = await say(LOBART, "Лобарт, ты меня слышишь?")
                handshakes = server.handshakes
                second = await say(LOBART, "И ещё раз здравствуй.")
                lines = [*first, *second]
                spoken = heard(LOBART, lines)
                check(
                    "ws_refused",
                    bool(first and second) and spoken == len(lines) and server.refused >= 1
                    and not server.connections and len(server.http_requests) == 2 and warnings.count == 1
                    and server.handshakes == handshakes
                    and all(r.get("prompt_cache_key") == LOBART for r in server.http_requests),
                    f"refused={server.refused}, HTTP={len(server.http_requests)}/2, "
                    f"line_started={spoken}/{len(lines)}, warnings={warnings.count}/1, "
                    f"cooldown_handshakes={server.handshakes - handshakes}/0",
                )
            finally:
                transport_log.removeHandler(warnings)
                dialogue.llm = original
                await backend.aclose()

        async with ResponsesServer(lambda r, n: reply(f"http_{n}", "Да, слышу тебя.")) as server:
            backend = OpenAIResponsesBackend("e2e", token, base_url=server.base_url, websocket=False)
            dialogue.llm = backend
            try:
                lines = await say(LOBART, "Лобарт, ответь без сокета.")
                spoken = heard(LOBART, lines)
                check(
                    "http_only",
                    bool(lines) and spoken == len(lines) and server.handshakes == 0
                    and len(server.http_requests) == 1
                    and server.http_requests[0].get("prompt_cache_key") == LOBART,
                    f"handshakes={server.handshakes}/0, HTTP={len(server.http_requests)}/1, "
                    f"line_started={spoken}/{len(lines)}",
                )
            finally:
                dialogue.llm = original
                await backend.aclose()
    finally:
        dialogue.llm = original
        link.listeners.remove(on_message)
        for npc in (LOBART, HILDA):
            await link.act(npc, "return_to_routine")
    return _logged(results)


async def m2_actions(link: GameLink, npc: str = "NONE_100_XARDAS") -> dict[str, str]:
    """M2 check of the game-side actions: snapshot contents, give_item, follow/stop, gestures."""
    await link.wait_world()
    await attach_when_free(link, npc)
    snap = await link.snapshot(npc)
    log.info("inventory of %s: %s", npc, [(i.item, i.name, i.count, i.equipped) for i in snap.npc.inventory])
    if snap.player is not None:
        p = snap.player
        log.info(
            "player: guild=%s armor=%s melee=%s ranged=%s hp=%s/%s",
            p.guild,
            p.armor,
            p.melee,
            p.ranged,
            p.hp,
            p.hp_max,
        )
    log.info("nearby npcs: %s", [(o.id, o.name, round(o.distance)) for o in snap.nearby_npcs])

    results: dict[str, str] = {}
    potion = "ITPO_HEALTH_01"
    results["debug_add_item"] = (await link.act(npc, "debug_add_item", item=potion, count=2)).status

    def count_of(s: Snapshot) -> int:
        return next((i.count for i in s.npc.inventory if i.item == potion), 0)

    # the whole path: the model speaks and calls give_item, the game transfers, the outcome becomes a note
    llm = FakeBackend(
        "Держи, это поможет тебе.", delay=0, tool_calls=[ToolCall("give_item", {"item": potion, "count": 1})]
    )
    dialogue = DialogueManager(link, llm, PersonaProvider(None))
    conv = dialogue.open(npc, snap.npc.name)
    before = count_of(await link.snapshot(npc, fresh=True))
    await dialogue.say_as_player(npc, "Дай зелье")
    for _ in range(60):
        if any(m.role == "developer" for m in conv.history):
            break
        await asyncio.sleep(0.5)
    after = count_of(await link.snapshot(npc, fresh=True))
    note = next((m.content for m in conv.history if m.role == "developer"), "no note")
    results["give_item"] = (
        f"{'done' if after == before - 1 else 'failed'} ({potion}: {before} -> {after}, {note})"
    )

    res = await link.act(npc, "give_item", item="ITMI_NOT_A_REAL_ITEM", count=1)
    results["give_item_missing"] = res.status

    ended: list[str] = []
    link.listeners.append(lambda m: ended.append(m.reason) if isinstance(m, FollowEnded) else None)
    res = await link.act(npc, "follow", npc="PLAYER")
    following = (await link.snapshot(npc, fresh=True)).npc.following
    results["follow"] = f"{res.status}, following={following}"
    await asyncio.sleep(2)
    res = await link.act(npc, "stop")
    await asyncio.sleep(0.5)
    results["stop"] = f"{res.status}, follow_ended={ended}"

    for gesture, anim in NPC_GESTURES.items():
        results[f"gesture {gesture}"] = (await link.act(npc, "play_ani", name=anim)).status

    await link.act(npc, "return_to_routine")
    for name, outcome in results.items():
        log.info("%-24s %s", name, outcome)
    return results


async def tts_lines(
    link: GameLink, dialogue: DialogueManager, npc: str = "NONE_100_XARDAS"
) -> dict[str, str]:
    """TTS check: every line of a reply reaches the game with its audio and is spoken aloud."""
    await link.wait_world()
    await attach_when_free(link, npc)
    snap = await link.snapshot(npc)
    conv = dialogue.open(npc, snap.npc.name)
    voiced: dict[str, bool] = {}
    link.listeners.append(
        lambda m: voiced.__setitem__(m.line, m.voiced) if isinstance(m, LineStarted) else None
    )
    started = asyncio.get_running_loop().time()
    await dialogue.say_as_player(npc, "Расскажи о драконах.")
    lines = list(conv.last_lines)
    for _ in range(240):  # each line is voiced, then spoken for as long as its audio lasts
        if lines and all(line in voiced for line in lines):
            break
        await asyncio.sleep(0.25)
    first = asyncio.get_running_loop().time() - started
    await asyncio.sleep(3)  # let the last line play
    await link.act(npc, "return_to_routine")
    ok = bool(lines) and all(voiced.get(line) for line in lines)
    aloud = sum(voiced.get(line, False) for line in lines)
    results = {"voice": f"{'done' if ok else 'failed'} ({aloud}/{len(lines)} voiced)"}
    results["timing"] = f"done (all lines started within {first:.1f}s)"
    for name, outcome in results.items():
        log.info("%-10s %s", name, outcome)
    return results


async def m3_memory_voice(
    link: GameLink,
    dialogue: DialogueManager,
    voice: VoiceInput | None,
    npc: str = "NONE_100_XARDAS",
    slot: int = 9,
) -> dict[str, str]:
    """M3 check: memories survive save/load and follow the save, push-to-talk becomes a player line.

    Talk, save into `slot`, talk more, load the slot: the npc must remember the first talk only.
    Then one push-to-talk utterance (the server's audio source, e.g. a wav file) goes through ASR.
    """
    results: dict[str, str] = {}
    await link.wait_world()
    if link.session is None:
        await wait_message(link, Session, timeout=10)
    assert link.session is not None
    first = link.session.branch
    results["session"] = f"done ({first}, {link.session.reason})"

    await attach_when_free(link, npc)
    snap = await link.snapshot(npc)
    dialogue.open(npc, snap.npc.name)
    await dialogue.say_as_player(npc, "Запомни: меня зовут Тестер.")
    head = dialogue.memory.head(first)
    await asyncio.sleep(0.5)  # the game has the head by now

    for attempt in range(30):  # the game refuses to save during the opening dialog
        try:
            saving = asyncio.ensure_future(wait_message(link, Saved, timeout=60))
            await link.debug("save", slot)
            break
        except GameError as err:
            saving.cancel()
            if attempt == 29:
                raise
            log.info("save refused (%s), retrying", err)
            await asyncio.sleep(2)
    saved = await saving
    results["save"] = f"{'done' if (saved.branch, saved.seq) == (first, head) else 'failed'} ({saved.seq})"

    await dialogue.say_as_player(npc, "А это я сказал уже после сохранения.")
    loaded = asyncio.ensure_future(wait_message(link, Session, lambda m: m.reason == "load"))
    await link.debug("load", slot)
    session = await loaded
    ok = (session.parent, session.parent_seq) == (first, head) and session.branch != first
    results["load"] = (
        f"{'done' if ok else 'failed'} ({session.branch} from {session.parent}@{session.parent_seq})"
    )

    await link.wait_world()
    await attach_when_free(link, npc)
    snap = await link.snapshot(npc, fresh=True)
    conv = dialogue.open(npc, snap.npc.name)
    said = [m.content for m in conv.history if m.role == "user"]
    ok = said == ["Запомни: меня зовут Тестер."]
    results["memory"] = f"{'done' if ok else 'failed'} (remembers {said})"

    if voice is not None:
        started: list[str] = []
        link.listeners.append(lambda m: started.append(m.line) if isinstance(m, LineStarted) else None)
        before = len(conv.memory.entries)
        voice.start(focus=npc)  # the hero looks at him: talk.py gives him the phrase
        await asyncio.sleep(getattr(voice.source, "duration", 0.3) + 0.2)  # a wav is "spoken" in real time
        voice.stop(recognize=True)
        for _ in range(120):
            if len(conv.memory.entries) >= before + 2 and conv.last_lines and conv.last_lines[0] in started:
                break
            await asyncio.sleep(0.25)
        heard = next((e.content for e in conv.memory.entries[before:] if e.role == "user"), None)
        results["voice"] = f"{'done' if heard else 'failed'} (heard {heard!r})"

    await link.act(npc, "return_to_routine")
    for name, outcome in results.items():
        log.info("%-10s %s", name, outcome)
    return results


async def m4_planner_bridge(
    link: GameLink, npc: str = "NONE_100_XARDAS", planner: NpcPlanner | None = None
) -> dict[str, str]:
    """M4 check of what the npc planner needs from the bridge (protocol v5): the roster, soft actions
    queued like script actions and their end (npc_idle), and that a taken-over npc is left alone."""
    events: list[NpcEvent] = []
    link.listeners.append(lambda m: events.append(m) if isinstance(m, NpcEvent) else None)
    await link.wait_world()
    results: dict[str, str] = {}
    roster = await wait_message(link, NpcRoster, lambda m: bool(m.npcs), timeout=120)
    if not any(e.id == npc for e in roster.npcs):  # e.g. a save elsewhere: the nearest npc on its routine
        free = sorted((e for e in roster.npcs if e.activity == "routine"), key=lambda e: e.distance)
        npc = (free or roster.npcs)[0].id
    me = next(e for e in roster.npcs if e.id == npc)
    log.info("roster: %s", [(e.id, e.name, round(e.distance), e.activity, e.in_view) for e in roster.npcs])
    results["roster"] = (
        f"done ({len(roster.npcs)} npc, {npc}: {me.distance:.0f} cm, {me.activity}, in_view={me.in_view}, "
        f"time {roster.hour}:{roster.minute:02d})"
    )

    async def soft(action: NpcActType, target: str = npc, wait_busy: bool = True, **args: object) -> str:
        for _ in range(60 if wait_busy else 1):  # the npc may be in a scripted dialog at a new game
            # the bridge works in the world tick: while the game pauses (menu, video) it answers later
            res = asyncio.ensure_future(
                wait_message(link, NpcActResult, lambda m: m.npc == target, timeout=15)
            )
            idle = asyncio.ensure_future(wait_message(link, NpcIdle, lambda m: m.npc == target, timeout=40))
            await asyncio.sleep(0)  # both listen before the command goes out
            await link.npc_act(target, action, **args)
            r = await res
            if r.status == "queued":
                i = await idle
                return f"{'done' if i.reason == 'done' else 'failed'} ({i.reason})"
            idle.cancel()
            if not wait_busy or "busy" not in (r.detail or ""):
                return f"rejected ({r.detail})"
            await asyncio.sleep(1)
        return "rejected (busy for a minute)"

    results["turn_to"] = await soft("turn_to")
    results["say_svm"] = await soft("say_svm", svm="NOTNOW")
    results["play_ani"] = await soft("play_ani", name="T_GREETNOV")
    results["look_at"] = await soft("look_at", ms=1000)
    results["unknown_npc"] = await soft("turn_to", target="NOT_AN_NPC")

    await attach_when_free(link, npc)
    busy = await wait_message(
        link, NpcRoster, lambda m: any(e.id == npc and e.activity == "attached" for e in m.npcs), timeout=5
    )
    results["attached_in_roster"] = "done" if busy else "failed"
    results["attached_soft_act"] = await soft("turn_to", wait_busy=False)
    await link.act(npc, "return_to_routine")

    near = [e for e in events if e.npc == npc and e.kind == "player_near"]
    if me.distance <= 500:  # the bridge reports the player coming within 5 m
        results["player_near"] = f"done ({near[0].distance:.0f} cm)" if near else "failed (no event)"
    if planner is not None:
        await asyncio.sleep(1)
        snap = planner.snapshot()
        asked = sum(snap["sent"].values())
        ok = snap["npcs"] and (asked > 0 or not near)  # an event near the player makes it ask the model
        results["planner"] = f"{'done' if ok else 'failed'} ({snap})"
    for name, outcome in results.items():
        log.info("%-20s %s", name, outcome)
    return results


async def m5_turn_loop(
    link: GameLink,
    npc: str = "NONE_100_XARDAS",
    decider: Decider | None = None,
    trace: TraceWriter | None = None,
    quick: LLMBackend | None = None,
) -> dict[str, str]:
    """M5a on the game: the turn loop with checks, intents moving the body, and the two loops on an
    incident (the body reacts before the words). The conversational model is scripted; the behavioral
    model is the live service when given, a fixed stand-in otherwise. The game cannot hit an npc on
    command, so the incident's perception is injected on the server side. With `quick` (the quick model
    of fights, live) the hero's phrases get its first words and the incident's words are its line, said
    at once; the scripted model only acts."""
    await link.wait_world()
    await attach_when_free(link, npc)
    snap = await link.snapshot(npc, fresh=True)
    results: dict[str, str] = {}
    potion = "ITPO_HEALTH_01"
    results["debug_add_item"] = (await link.act(npc, "debug_add_item", item=potion, count=2)).status

    def count_of(s: Snapshot) -> int:
        return next((i.count for i in s.npc.inventory if i.item == potion and not i.equipped), 0)

    intent = {"target": None, "attitude": "friendly", "agenda": "wants the hero to visit Lord Hagen"}
    llm = FakeBackend(
        script=[
            FakeStep(
                "Держи свиток огненного шара.",
                [ToolCall("give_item", {"item": "ITSC_NOT_THERE", "count": 1})],
            ),
            FakeStep("Хотя нет, такого свитка у меня нет."),
            FakeStep("Вот, возьми зелье.", [ToolCall("give_item", {"item": potion, "count": 1})]),
            FakeStep("Хорошо, пойдём вместе.", [ToolCall("set_intent", {"goal": "follow_hero", **intent})]),
            FakeStep(
                "Довольно, у меня свои дела.", [ToolCall("set_intent", {"goal": "keep_routine", **intent})]
            ),
            FakeStep("Проклятая тварь! Ко мне, герой!"),
        ],
        delay=0,
    )
    budget = Budget()
    spoken: list[dict[str, Any]] = []  # the journal's entries: who said a line, how soon
    dialogue = DialogueManager(
        link,
        llm,
        PersonaProvider(None),
        trace=trace,
        budget=budget,
        journal=Journal(spoken.append),
        quick=quick,
    )
    reflexes = Reflexes(decider or FixedDecider({"react": "run_to_hero", "gesture": "nod"}))
    desk = IncidentDesk(link, dialogue, reflexes, budget, trace)
    desk.attached.add(npc)  # it was attached before the desk was listening
    conv = dialogue.open(npc, snap.npc.name)

    async def following(expected: bool) -> bool:
        for _ in range(20):
            if (await link.snapshot(npc, fresh=True)).npc.following == expected:
                return True
            await asyncio.sleep(0.5)
        return False

    try:
        # 1. a commitment the npc cannot keep is rejected before the game and corrected in the same turn
        before = count_of(await link.snapshot(npc, fresh=True))
        await dialogue.say_as_player(npc, "Дай мне свиток огненного шара")
        said = conv.history[-1].content if conv.history else ""
        unchanged = count_of(await link.snapshot(npc, fresh=True)) == before
        ok = len(llm.calls) == 2 and "Хотя нет" in said and unchanged
        results["rejected_give"] = f"{'done' if ok else 'failed'} ({len(llm.calls)} calls: {said})"

        # 2. a commitment it can keep: the body gives after the line
        await dialogue.say_as_player(npc, "Тогда дай зелье")
        for _ in range(60):
            if any(m.role == "developer" and "отдал" in m.content for m in conv.history):
                break
            await asyncio.sleep(0.5)
        after = count_of(await link.snapshot(npc, fresh=True))
        results["give"] = f"{'done' if after == before - 1 else 'failed'} ({potion}: {before} -> {after})"

        # 3. intents move the body: follow, then back to its own business
        await dialogue.say_as_player(npc, "Пойдём со мной")
        results["intent_follow"] = "done" if await following(True) else "failed (not following)"
        await dialogue.say_as_player(npc, "Можешь остаться")
        results["intent_routine"] = "done" if await following(False) else "failed (still following)"

        if quick is not None and trace is not None:  # the quick model's first words to the hero's phrases
            words = [
                e["text"]
                for r in read_trace(trace.path())
                if r.get("type") == "turn" and r.get("npc") == npc
                for e in r.get("events", [])
                if e.get("kind") == "first_words"
            ]
            results["first_words"] = f"{'done' if words else 'failed'} ({len(words)} of 4: {words})"

        # 4. an incident: the body reacts at once, the words come after
        calls = len(llm.calls)
        reacted_at: float | None = None
        started = time.monotonic()
        link._dispatch(
            Perception(type="perception", npc=npc, perc="ASSESSDAMAGE", other="WOLF", distance=300.0)
        )
        for _ in range(100):
            if reacted_at is None and any(r.question == "react" for r in desk.reactions):
                reacted_at = time.monotonic() - started
            if len(llm.calls) > calls and conv.reply is not None and conv.reply.done():
                break
            await asyncio.sleep(0.1)
        words_at = time.monotonic() - started
        reaction = next((r for r in reversed(desk.reactions) if r.question == "react"), None)
        spoke = conv.history[-1].content if conv.history and conv.history[-1].role == "assistant" else ""
        results["incident_reflex"] = (
            f"{'done' if reaction else 'failed'} "
            f"({reaction.choice if reaction else '-'}, applied={reaction.applied if reaction else '-'}, "
            f"{(reacted_at or 0) * 1000:.0f} ms)"
        )
        results["incident_words"] = (
            f"{'done' if spoke and (reacted_at or 99) < words_at else 'failed'} ({spoke})"
        )
        if quick is not None:
            line = next((e for e in reversed(spoken) if e["kind"] == "npc"), {})
            ok = line.get("llm") == quick.name and line.get("first_token_s", 99) < QUICK_LINE_S
            results["incident_quick"] = (
                f"{'done' if ok else 'failed'} ({line.get('llm')}, first token {line.get('first_token_s')} s)"
            )
    finally:
        await link.act(npc, "return_to_routine")
    for name, outcome in results.items():
        log.info("%-24s %s", name, outcome)
    return results


async def m5b_bridge(link: GameLink, npc: str = "NONE_100_XARDAS") -> dict[str, str]:
    """M5b on the game: threats from others reach the server, the player's drawn weapon is perceived,
    and the new body actions work for free and attached npc's. A wolf is spawned next to the npc."""
    await link.wait_world()
    await _m5b_player_weapon(link, "none")
    events: list[NpcEvent] = []
    link.listeners.append(lambda m: events.append(m) if isinstance(m, NpcEvent) else None)
    results: dict[str, str] = {}

    async def event(kinds: set[str], timeout: float) -> NpcEvent | None:
        for _ in range(int(timeout / 0.2)):
            hit = next((e for e in events if e.kind in kinds), None)
            if hit is not None:
                return hit
            await asyncio.sleep(0.2)
        return None

    # 0. a free npc speaks a line, queued like its own (it waits for what the npc is busy with, e.g.
    # walking back to its routine spot: the engine does not take speech from the queue while walking)
    started_lines: list[LineStarted] = []
    link.listeners.append(lambda m: started_lines.append(m) if isinstance(m, LineStarted) else None)
    t0 = time.monotonic()
    await link.send(SayCmd(npc=npc, line="AIB_900001", text="Опять ты здесь.", overlay=None))
    for _ in range(150):
        if any(m.line == "AIB_900001" for m in started_lines):
            break
        await asyncio.sleep(0.1)
    queued = any(m.line == "AIB_900001" for m in started_lines)
    results["free_say"] = (
        f"{'done' if queued else 'failed'} (line started after {time.monotonic() - t0:.1f} s)"
    )

    # _091 puts Xardas beside a rock that blocks spells from his hand even when
    # the wolf is visible. Use _09's flat ground and its open southeast corridor.
    arena = "NW_XARDAS_TOWER_VALLEY_09"
    # Clear both neighbourhoods: a relocation can retain the stock encounter's
    # enemy, and animals by the new spot must not join either test fight.
    # debug kill drops victims by the hero; leave that food away from the wolves.
    async def clear_animals() -> None:
        around = (await link.snapshot(npc, fresh=True)).nearby_npcs
        for other in around:
            if not other.dead and other.guild >= BEASTS:
                await link.debug("kill", instance=other.id, near=npc)
        # Perception reaches beyond snapshot's 15 m, but only within its field of view.
        for view in await link.npc_view([npc]):
            for seen in view.sees:
                if seen.guild >= BEASTS and seen.body != "dead":
                    await link.debug("kill", instance=seen.id, near=npc)

    await link.debug("player_goto", waypoint="NW_XARDAS_TOWER_VALLEY_01")
    for clearing in ("NW_XARDAS_TOWER_VALLEY_091", arena):
        await link.debug("npc_goto", npc=npc, waypoint=clearing)
        await clear_animals()
    # npc_goto resolves script instances, so face needs PC_HERO, not the PLAYER alias.
    # This position fixes the direction for both wolf spawns, away from the rocks.
    await link.debug("player_goto", pos=[23472, 1032, -10816])
    await link.debug("npc_goto", npc=npc, waypoint=arena, face="PC_HERO")
    await link.debug("teleport", near=npc, distance=300)

    # 1. the player draws a weapon next to the npc: the npc perceives it (it is free, scripts run)
    await attach_when_free(link, npc)
    await link.act(npc, "go_to_npc", npc="PLAYER", radius=300)
    # The approach changes the standing pose and can expose animals hidden from
    # the relocated book-reading pose. Clear that view before native perceptions resume.
    await link.debug("player_goto", waypoint="NW_XARDAS_TOWER_VALLEY_01")
    await clear_animals()
    await link.debug("teleport", near=npc, distance=300)
    await link.act(npc, "return_to_routine")
    # Returning control starts the old tower routine. Hold an ordinary native idle
    # here and wait for its initialization, including its normal enemy perceptions.
    bodies = Bodies(link)
    ready = await bodies.act(
        npc, "start_activity", state="ZS_STAND_ARMSCROSSED", waypoint=arena,
        minutes=60, arrive=True, radius=100, timeout=15,
    )
    if ready.status != "arrived":
        current = (await link.routine(npc)).current
        snap = await link.snapshot(npc, fresh=True)
        views = await link.npc_view([npc])
        raise GameError(
            f"m5b arena preparation: {ready.status} {ready.detail}; state={current}; "
            f"pos={snap.npc.pos}; rot={snap.npc.rot}; nearby={snap.nearby_npcs}; "
            f"sees={[v.sees for v in views]}"
        )
    events.clear()
    await _m5b_player_weapon(link, "melee")
    e = await event({"weapon_drawn"}, 5)
    results["weapon_drawn"] = f"done ({e.npc}, {e.distance:.0f} cm)" if e else "failed (no weapon_drawn)"
    await _m5b_player_weapon(link, "none")
    # Keep the hero beyond the 400/500 cm wolf spawns so he cannot intercept
    # Xardas's projectiles. The weapon perception above still runs at 300 cm.
    await link.debug("player_goto", pos=[23967, 1032, -11311])

    # 2. a wolf next to the npc: the npc senses it or sees it attack
    events.clear()
    await link.debug("spawn", instance="WOLF", near=npc, distance=400)
    e = await event({"monster_near", "monster_attack", "hero_attacked"}, 20)
    results["monster_event"] = (
        f"done ({e.npc}: {e.kind} {e.other} '{e.other_name}' {e.distance or 0:.0f} cm {e.detail or ''})"
        if e
        else "failed (no threat event)"
    )

    # 3. a free npc fights it with the game's own combat; npc_idle once it is over
    idles: list[NpcIdle] = []
    link.listeners.append(lambda m: idles.append(m) if isinstance(m, NpcIdle) else None)
    started = time.monotonic()
    act = await link.npc_act(npc, "fight", npc="WOLF")
    for _ in range(300):
        if any(i.act == act for i in idles):
            break
        await asyncio.sleep(0.2)
    idle = next((i for i in idles if i.act == act), None)
    took = time.monotonic() - started
    ok = idle is not None and idle.reason == "done"
    results["free_fight"] = (
        f"{'done' if ok else 'failed'} ({idle.reason if idle else 'no npc_idle'}, {took:.1f} s)"
    )

    # 4. an attached npc: weapons, its own voiced line, and fight/flee handed to the scripts and back
    detached: list[str] = []
    link.listeners.append(lambda m: detached.append(m.reason) if isinstance(m, Detached) else None)
    await attach_when_free(link, npc)

    def wolves(sn: Snapshot) -> str:
        return ", ".join(
            f"{o.name} {o.distance / 100:.0f} m{' dead' if o.dead else ''}" for o in sn.nearby_npcs
        )

    results["after_free_fight"] = f"done (around: {wolves(await link.snapshot(npc, fresh=True)) or 'nobody'})"
    for name, action, args in (
        ("attached_draw", "draw_weapon", {}),
        ("attached_remove", "remove_weapon", {}),
        ("attached_svm", "say_svm", {"svm": "NOTNOW"}),
    ):
        res = await link.act(npc, action, **args)  # type: ignore[arg-type]
        results[name] = f"{res.status} {res.detail or ''}".strip()
    await link.debug("spawn", instance="WOLF", near=npc, distance=500)
    started = time.monotonic()
    res = await link.act(npc, "fight", npc="WOLF", timeout=90)
    took = time.monotonic() - started
    snap = await link.snapshot(npc, fresh=True)
    results["attached_fight"] = (
        f"{'done' if res.status == 'done' and not detached else 'failed'} "
        f"({res.status} {res.detail or ''} in {took:.1f} s, still attached: {not detached}, "
        f"hp {snap.npc.hp}/{snap.npc.hp_max}, around: {wolves(snap)})"
    )
    await link.debug("spawn", instance="WOLF", near=npc, distance=800)
    res = await link.act(npc, "flee", npc="WOLF", timeout_ms=4000)
    results["attached_flee"] = (
        f"{'done' if res.status in ('done', 'timeout') and not detached else 'failed'} "
        f"({res.status} {res.detail or ''}, still attached: {not detached})"
    )
    # The flee wolf may stay with the hero outside the fleeing NPC's snapshot.
    # Remove that encounter before restoring the routine, even if it is out of view.
    try:
        await link.debug("kill", instance="WOLF", near="PC_HERO")
    except GameError as err:
        if str(err) != "no such creature alive":
            raise
    around = (await link.snapshot(npc, fresh=True)).nearby_npcs
    for other in around:
        if not other.dead and other.guild >= BEASTS:
            await link.debug("kill", instance=other.id, near=npc)
    await link.act(npc, "return_to_routine")

    # 5. a free npc speaks: queued like its own lines, or on the move (overlay) without stopping
    # right after return_to_routine the npc walks back to its spot: queued speech would wait for that,
    # a line on the move starts at once
    long_line = "Слушай внимательно, у нас мало времени, и я не стану повторять дважды: драконы уже здесь."
    t0 = time.monotonic()
    await link.send(SayCmd(npc=npc, line="AIB_900002", text=long_line, overlay=True))
    for _ in range(50):
        if any(m.line == "AIB_900002" for m in started_lines):
            break
        await asyncio.sleep(0.05)
    took = time.monotonic() - t0
    said = any(m.line == "AIB_900002" for m in started_lines)
    results["free_say_on_the_move"] = (
        f"{'done' if said and took < 1.0 else 'failed'} (started after {took:.2f} s while the npc walks back)"
    )

    # 6. the routine: read it, put the npc to an activity for a while, then back to a routine variant
    info = await link.routine(npc)
    entries = "; ".join(f"{e.start}-{e.end} {e.state} {e.waypoint}" for e in info.entries)
    results["routine"] = (
        f"{'done' if info.entries else 'failed'} (now {info.current}, variants {info.routines}, {entries})"
    )
    if info.entries:
        spot = info.entries[0].waypoint
        act = await link.npc_act(npc, "start_activity", state="ZS_SIT_CAMPFIRE", waypoint=spot, minutes=30)
        await asyncio.sleep(1.0)
        now = (await link.routine(npc)).current
        results["start_activity"] = (
            f"{'done' if now == 'ZS_SIT_CAMPFIRE' else 'failed'} (now {now} at {spot})"
        )
        variant = "START" if "START" in info.routines or not info.routines else info.routines[0]
        await link.npc_act(npc, "set_routine", name=variant)
        await asyncio.sleep(1.0)
        now = (await link.routine(npc)).current
        back = now == info.entries[0].state or now != "ZS_SIT_CAMPFIRE"
        results["set_routine"] = f"{'done' if back else 'failed'} ({variant}: now {now})"
    for name, outcome in results.items():
        log.info("%-24s %s", name, outcome)
    return results


async def m5c_free_npc(link: GameLink, npc: str = "NONE_100_XARDAS") -> dict[str, str]:
    """M5c on the game: a free npc meets a wolf. The threat is held from the npc's scripts; the behavioral
    model (a fixed stand-in here: fight, escalate 3.6 — the live model is measured by evals) takes the body
    over with the game's own combat; the moment calls for words, and the conversational model (scripted)
    speaks for the npc on the move."""
    from ..brain import GameApplier
    from ..brain.day import Habit
    from ..brain.planner import PlannerConfig

    await link.wait_world()
    current = (await link.routine(npc)).current
    # The native starting pose already is Xardas's only usual habit. The day menu correctly excludes
    # it, so give this fixed-decider fixture one distinct, executable option for its h1 choice.
    habit = Habit(
        "stand quietly and think", "ZS_STAND_WP" if current != "ZS_STAND_WP" else "ZS_STAND_ARMSCROSSED"
    )
    results: dict[str, str] = {}
    started_lines: list[LineStarted] = []
    link.listeners.append(lambda m: started_lines.append(m) if isinstance(m, LineStarted) else None)
    held: list[PerceptionHeld] = []
    link.listeners.append(lambda m: held.append(m) if isinstance(m, PerceptionHeld) else None)

    applier = GameApplier(link)
    planner = NpcPlanner(
        FixedDecider({"react": "fight", "plan": "h1"}, {"escalate": 3.6}),
        applier,
        config=PlannerConfig(ask_attitude=False, plan_period_s=3.0),
        routine_of=link.routine,
        habits_of=lambda who: [habit] if who == npc else habits_of(who),
    )
    link.listeners.append(planner.on_message)
    runner = asyncio.create_task(planner.run())
    llm = FakeBackend("Ещё одна тварь из тьмы. Прочь, или сгоришь!", delay=0)
    dialogue = DialogueManager(link, llm, PersonaProvider(None))
    budget = Budget()
    IncidentDesk(link, dialogue, None, budget, None, planner=planner)
    await link.send(ThreatControlCmd(hold=True))
    try:
        for _ in range(50):  # the roster brings the npc to the planner
            if npc in planner.npcs:
                break
            await asyncio.sleep(0.2)

        # 1. a quiet moment: the npc thinks of its next hour, from its own day and habits
        plan = None
        for _ in range(300):
            plan = next((d for d in applier.recent if d.npc == npc and d.question == "plan"), None)
            if plan is not None:
                break
            await asyncio.sleep(0.1)
        await asyncio.sleep(1.0)
        now = (await link.routine(npc)).current
        results["day_plan"] = (
            f"{'done' if plan and plan.args and now == plan.args['state'] else 'failed'} "
            f"({plan.choice + ' ' + str(plan.args) if plan else 'no plan'}; now {now})"
        )
        await link.npc_act(npc, "set_routine", name="START")

        # 2. a wolf
        t0 = time.monotonic()
        await link.debug("spawn", instance="WOLF", near=npc, distance=400)
        decision = None
        for _ in range(150):
            decision = next((d for d in applier.recent if d.npc == npc and d.target), None)
            if decision is not None:
                break
            await asyncio.sleep(0.1)
        verdicts = list(applier.verdicts)
        results["threat_held"] = (
            f"done ({len(held)} held: {', '.join(f'{h.kind}/{h.other}' for h in held)}; verdicts {verdicts})"
            if held and any(to == "server" for _, to in verdicts)
            else f"failed (held {len(held)}, verdicts {verdicts})"
        )
        results["body_reacts"] = (
            f"done ({decision.choice} against {decision.target}, "
            f"{time.monotonic() - t0:.1f} s after the spawn)"
            if decision
            else "failed (no decision)"
        )
        words = None
        for _ in range(150):
            words = next((m for m in started_lines if m.npc == npc), None)
            if words is not None:
                break
            await asyncio.sleep(0.1)
        results["words_on_the_move"] = (
            f"done ({time.monotonic() - t0:.1f} s after the spawn: {llm.calls[0][1][-1].content})"
            if words and llm.calls
            else "failed (no words)"
        )
    finally:
        await link.send(ThreatControlCmd(hold=False))
        runner.cancel()
    for name, outcome in results.items():
        log.info("%-24s %s", name, outcome)
    return results


async def m5e_talk(
    link: GameLink,
    dialogue: DialogueManager,
    talk: TalkDesk,
    voice: VoiceInput | None,
    npc: str = "NONE_100_XARDAS",
    witness: str = "VLK_408_JORA",
) -> dict[str, str]:
    """M5e on the game: the hero talks without a window. Lines go in as the player makes them (the talk
    key and the input line, through the bridge's own paths, with the hero's own focus); the server picks
    the addressee, the npc's body follows the behavioral model (a fixed stand-in here), the hero's doings,
    an interruption and a witness go into memory, the conversation ends when the hero walks away, and a
    walking npc answers on the move. The conversational model is scripted."""
    await link.wait_world()
    results: dict[str, str] = {}
    started: list[tuple[float, LineStarted]] = []
    link.listeners.append(
        lambda m: started.append((time.monotonic(), m)) if isinstance(m, LineStarted) else None
    )
    stand_in = FixedDecider({"posture": "listen"})
    talk.reflexes = Reflexes(stand_in)
    talk.end_silence_s = 3.0

    def lines_of(who: str, since: float = 0.0) -> list[LineStarted]:
        return [m for t, m in started if m.npc.upper() == who and t >= since]

    def notes(who: str) -> list[str]:
        conv = dialogue.conversations.get(who)
        return [m.content for m in conv.history if m.role == "developer"] if conv else []

    def heard() -> str:
        if talk.last is None:
            return "nothing heard"
        to, text = talk.last
        return f"{text!r} -> {to.id + ' by ' + to.why if to else 'nobody'}"

    if not await _until(lambda: npc in talk.people, 20):
        return {"roster": f"failed ({npc} is not around the hero)"}

    # 1. a typed line: the bridge adds the hero's focus, the server picks whom it is said to
    dialogue.llm = FakeBackend("Ты снова здесь. Говори, что нужно.", delay=0)
    t0 = time.monotonic()
    await link.debug("type", text="Что мне делать дальше?")
    ok = await _until(lambda: talk.partner is not None and bool(lines_of(npc, t0)), 20)
    results["typed_line"] = (
        f"{'done' if ok and talk.partner and talk.partner.npc == npc else 'failed'} "
        f"({heard()}; body: {talk.partner.posture if talk.partner else '-'}, held {talk.held.get(npc)})"
    )

    # 2. the talk key held: the voice (a wav through the ASR service) names the npc
    if voice is not None:
        await _until(lambda: not dialogue.speaking(npc), 10)  # else the key would cut it off (step 4)
        dialogue.llm = FakeBackend("Я слышу тебя.", delay=0)
        t0 = time.monotonic()
        await link.debug("ptt", state="start")
        await asyncio.sleep(getattr(voice.source, "duration", 1.0) + 0.3)  # a wav is spoken in real time
        await link.debug("ptt", state="stop")
        ok = await _until(lambda: bool(lines_of(npc, t0)) and talk.last is not None, 30)
        results["voice"] = f"{'done' if ok else 'failed'} ({heard()})"

    # 3. the hero draws his weapon mid-conversation
    await link.debug("player_weapon", state="melee")
    ok = await _until(lambda: any("достал оружие" in n for n in notes(npc)), 8)
    for _ in range(10):  # not while the drawing animation plays
        await asyncio.sleep(0.5)
        try:
            await link.debug("player_weapon", state="none")
            break
        except GameError:
            pass
    results["weapon_mid_talk"] = f"{'done' if ok else 'failed'} ({notes(npc)[-1:]})"

    # 4. the hero interrupts a long answer
    story = "Слушай внимательно. " + "Драконы пришли в долину рудников, и их ведёт сила Белиара. " * 5
    await _until(lambda: not dialogue.speaking(npc), 10)
    dialogue.llm = FakeBackend(story, delay=0.03)
    t0 = time.monotonic()
    await link.debug("type", text="Расскажи всё о драконах")
    await _until(lambda: bool(lines_of(npc, t0)), 20)
    await asyncio.sleep(0.5)
    await link.debug("ptt", state="start")  # he starts to speak
    t_cut = time.monotonic()
    await asyncio.sleep(0.5)
    await link.debug("ptt", state="cancel")
    ok = await _until(lambda: any("перебил" in n for n in notes(npc)), 8)
    await asyncio.sleep(3.0)
    after = len(lines_of(npc, t_cut + 0.3))
    conv = dialogue.conversations[npc]
    said = next((m.content for m in reversed(conv.history) if m.role == "assistant"), "")
    results["interrupt"] = (
        f"{'done' if ok and after == 0 and len(said) < len(story) else 'failed'} "
        f"(said {len(said)} of {len(story)} chars, {after} line(s) after the cut)"
    )

    # 5. a witness hears what the hero says to the npc
    await link.debug("spawn", instance=witness, distance=300)
    near = await _until(lambda: witness in talk.people and talk.people[witness].distance <= 1000, 10)
    dialogue.llm = FakeBackend("Не знаю его.", delay=0)
    await link.debug("type", text="Ксардас, кто этот человек?")
    ok = await _until(lambda: any("слышал" in n for n in notes(witness)), 10)
    results["witness"] = f"{'done' if near and ok else 'failed'} ({notes(witness)[-1:] or heard()})"

    # 6. the hero walks away: after a silence the conversation is over and the npc goes back to its routine
    await _until(lambda: not dialogue.speaking(npc), 10)
    await link.debug("player_goto", near=npc, distance=1500)
    away = await _until(lambda: npc in talk.people and talk.people[npc].distance > 1000, 5)
    dist = talk.people[npc].distance / 100 if npc in talk.people else -1
    ok = await _until(lambda: talk.partner is None and npc not in talk.held, 15)
    results["walk_away"] = (
        f"{'done' if away and ok else 'failed'} ({dist:.0f} m away; held {talk.held.get(npc)}; "
        f"{notes(npc)[-1:]})"
    )

    # 7. a walking npc answers on the move, without stopping
    stand_in.choices["posture"] = "keep_working"
    await link.npc_act(npc, "go_to_npc")  # walks to the hero
    moving = await _until(lambda: npc in talk.people and talk.people[npc].moving, 10)
    dialogue.llm = FakeBackend("Иду я, иду.", delay=0)
    t0 = time.monotonic()
    await link.debug("type", text="Ксардас, ты куда?")
    ok = await _until(lambda: bool(lines_of(npc, t0)), 20)
    still = npc in talk.people and talk.people[npc].moving
    results["answer_on_the_move"] = (
        f"{'done' if moving and ok and still and npc not in talk.held else 'failed'} "
        f"(walking {moving}, still walking at its line {still}, body: "
        f"{talk.partner.posture if talk.partner else '-'})"
    )

    for name, outcome in results.items():
        log.info("%-24s %s", name, outcome)
    return results


async def m6_knows(
    link: GameLink,
    dialogue: DialogueManager,
    talk: TalkDesk,
    knowledge: Knowledge,
    npc: str = "NONE_100_XARDAS",
    farmer: str = LOBART,
) -> dict[str, str]:
    """M6 on the game: what an npc knows comes from the scripts' dialogs, as far as the story has got.
    Xardas in chapter 1 knows his talk of chapters 1-2 and nothing of chapter 3. A dialog of his played from
    the menu becomes one the hero has heard. At chapter 3 his talk of chapter 3 opens. Lobart, the hero put
    next to him, knows his field work and what the people around the city say. The knowledge is in the
    request to the model, and recall gives a topic in full. The conversational model is scripted: its
    requests are checked."""
    results: dict[str, str] = {}

    def check(name: str, ok: bool, detail: object = "") -> None:
        results[name] = f"{'done' if ok else 'failed'}{f' ({detail})' if detail != '' else ''}"

    def ids(topics: Sequence[Topic]) -> set[str]:
        return {t.node.id for t in topics}

    def situation(llm: FakeBackend) -> str:
        return next(
            (
                str(m.content)
                for m in llm.calls[0][1]
                if isinstance(m, Message) and m.role == "developer" and "Что ты знаешь" in str(m.content)
            ),
            "",
        )

    await link.wait_world()
    if not await _until(lambda: npc in talk.people, 20):
        return _logged({"roster": f"failed ({npc} is not around the hero)"})

    # 1. chapter 1: his talk of chapters 1-2 (the Eye, the city), none of chapter 3
    await knowledge.prepare(npc)
    view = knowledge.views[npc]
    known = ids(view.told) | ids(view.open)
    later = sorted(known & XARDAS_CH3)
    check(
        "chapter_1",
        view.state.chapter == 1 and "DIA_XARDAS_ARTEFAKT" in known and not later,
        f"chapter {view.state.chapter}; heard {sorted(ids(view.told))}; open {len(view.open)}; later {later}",
    )

    # 2. a dialog of his played from the menu: the hero has heard it
    offered = await link.offered_dialogs(npc)
    pick = next((t.node.id for t in view.open if t.sure and t.node.id in offered), None)
    if pick is None:
        check("heard_in_menu", False, f"nothing open is offered: {' '.join(offered)}")
    else:
        await link.choose_dialog(npc, pick)

        async def heard() -> set[str]:
            await knowledge.prepare(npc)
            return ids(knowledge.views[npc].told)

        ok = await _until_async(heard, lambda told: pick in told, 30)
        check("heard_in_menu", ok, pick)
        await asyncio.sleep(3)  # the dialog's lines

    # 3. a turn: the knowledge is in the request, recall gives a topic in full
    llm = FakeBackend(
        script=[
            FakeStep("Хм…", [ToolCall("recall", {"query": "Глаз Инноса"}, "c1")]),
            FakeStep("Это амулет, в нём сила самого Инноса."),
        ],
        delay=0,
    )
    dialogue.llm = llm
    await link.debug("type", text="Ксардас, что такое Глаз Инноса?")
    await _until(lambda: len(llm.calls) >= 2, 30)
    result = next((i.output for c in llm.calls[1:] for i in c[1] if isinstance(i, ToolResult)), "")
    text = situation(llm) if llm.calls else ""
    check(
        "in_the_turn",
        "Что ты знаешь (сейчас глава 1" in text and "Ты знаешь:" in result and "Глаз Инноса" in result,
        f"{len(text)} chars known, recall {len(result)} chars: {result[:120]!r}",
    )
    await _until(lambda: not dialogue.speaking(npc), 15)

    # 4. chapter 3: his chapter 3 talk opens, that of chapters 1-2 closes (what the hero heard stays)
    await link.story_effect(npc, [{"do": "set", "var": "KAPITEL", "value": 3}])
    await knowledge.prepare(npc)
    ch3 = knowledge.views[npc]
    opened = sorted(ids(ch3.open) & XARDAS_CH3)
    check(
        "chapter_3",
        ch3.state.chapter == 3
        and "DIA_XARDAS_BACKFROMOW" in opened
        and "DIA_XARDAS_WEAPON" not in ids(ch3.open),
        f"open now {opened}",
    )
    await link.story_effect(npc, [{"do": "set", "var": "KAPITEL", "value": 1}])

    # 5. Lobart on his farm: his work and what farmers say
    await link.debug("teleport", near=farmer, distance=300)
    if not await _until(lambda: farmer in talk.people and talk.people[farmer].distance < 800, 20):
        check("farmer", False, f"{farmer} is not around the hero")
        return _logged(results)
    llm = FakeBackend("Работа есть: поле репы за амбаром.", delay=0)
    dialogue.llm = llm
    await link.debug("type", text="Лобарт, есть для меня работа?")
    await _until(lambda: bool(llm.calls), 30)
    text = situation(llm) if llm.calls else ""
    check(
        "farmer",
        "Ты — житель окрестностей." in text and "«Я ищу работу.»" in text and "Что говорят вокруг" in text,
        f"{len(text)} chars known",
    )
    log.info("what %s knows:\n%s", farmer, text)
    return _logged(results)


# chapter 3 dialogs of Xardas's (KAPITEL == 3 in their conditions, or waiting for one that has it)
XARDAS_CH3 = {
    "DIA_XARDAS_BACKFROMOW",
    "DIA_XARDAS_DMTSINDDA",
    "DIA_XARDAS_INNOSEYEBROKEN",
    "DIA_XARDAS_RITUALREQUEST",
    "DIA_XARDAS_BINGESPANNT",
    "DIA_XARDAS_WASNUN",
    "DIA_XARDAS_PYROWILLNICHT",
    "DIA_XARDAS_BEREIT",
    "DIA_XARDAS_WARUMNICHTJETZT",
}
