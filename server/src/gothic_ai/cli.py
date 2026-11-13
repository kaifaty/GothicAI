"""`gothic-ai`: AI server for the game, ChatGPT sign-in and diagnostics."""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
import math
import os
import sys
import threading
import time
import tomllib
from array import array
from contextlib import nullcontext
from pathlib import Path
from typing import Any
from urllib.parse import urlsplit

from . import (
    characters,
    lore,
    npc_tools,
    places,
)
from .beasts import TONE_MAX_TOKENS
from .brain import DeciderClient
from .budget import Budget
from .console import Console
from .dialogue import REACT_HINT, SVM_HINT
from .embeddings import EmbedClient
from .face_track import A2F_PORT, a2f_url, emotion_table, make_source
from .face_track import DEFAULT_TIMEOUT_PER_S as DEFAULT_A2F_TIMEOUT_PER_S
from .face_track import DEFAULT_TIMEOUT_S as DEFAULT_A2F_TIMEOUT_S
from .game import GameLink, GameServer
from .gamedata import storygraph
from .gamedata.ou import load_npc_lines
from .journal import Journal, print_sink
from .llm import (
    BACKENDS,
    Done,
    Item,
    LLMBackend,
    LLMError,
    Message,
    Reasoning,
    TextDelta,
    ToolCall,
    ToolResult,
    make_backend,
    make_quick,
    quick_url,
)
from .llm.siwc import AuthError, TokenStore, login
from .memory import DEFAULT_PATH as DEFAULT_MEMORY_DB
from .memory import MemoryStore
from .persona import MOOD_DELIVERY, PersonaProvider
from .protocol import MOODS
from .replay import replay as replay_turn
from .replay import replay_first_words as replay_first_words_turn
from .replay import replay_svm_lines as replay_svm_lines_turn
from .scenarios.groups import GIVEN, LIVE, OFF
from .session import ServerState, SessionEnv, run_session
from .trace import TraceWriter, find_turn, latest, read
from .tts import DEFAULT_TTS_URL, TtsClient, TtsError
from .voice import (
    DEFAULT_ASR_URL,
    MIC_DENIED,
    AsrClient,
    AudioSource,
    Microphone,
    VoiceError,
    WavSource,
)

log = logging.getLogger("gothic_ai")

DEFAULT_TRACE_DIR = DEFAULT_MEMORY_DB.parent / "traces"

DEFAULT_GAME_DIR = "/Volumes/DevSSD/Dev/games/Gothic II"


def _story_graph(args: argparse.Namespace, game_dir: Path | None) -> storygraph.StoryGraph | None:
    """The scripts' story graph (storygraph's JSON): a given file, or made from the game's scripts."""
    path = Path(args.story_graph) if args.story_graph else None
    if path is None and game_dir is not None:
        try:
            path = storygraph.ensure(game_dir, DEFAULT_MEMORY_DB.parent / "story.json")
        except RuntimeError as error:
            log.warning("story graph unavailable: %s", error)
            return None
    if path is None or not path.exists():
        log.warning(
            "no story graph, so no director and no npc knowledge "
            "(build OpenGothic/ai_bridge/storygraph or --story-graph)"
        )
        return None
    return storygraph.load(path)


def _backend(args: argparse.Namespace) -> LLMBackend:
    return make_backend(
        args.llm,
        args.model,
        base_url=args.base_url,
        reasoning_effort=args.reasoning_effort,
        websocket=not args.no_websocket,
    )


def _audio_source(args: argparse.Namespace) -> AudioSource:
    if args.voice_file:
        return WavSource(args.voice_file)
    device = int(args.mic_device) if args.mic_device and args.mic_device.isdigit() else args.mic_device
    return Microphone(device)


def _a2f_emotion(item: str) -> str:
    try:
        emotion_table([item])
    except ValueError as err:
        raise argparse.ArgumentTypeError(str(err)) from err
    return item


def embed_url(url: str | None, decider_url: str | None) -> str | None:
    """Explicit embedding service, or HTTP port 47837 on the decider's host."""
    if url:
        return url
    host = urlsplit(decider_url).hostname if decider_url else None
    if host and ":" in host:
        host = f"[{host}]"
    return f"http://{host}:47837" if host else None


async def serve(args: argparse.Namespace) -> int:
    soak_control = os.environ.get("GOTHIC_SOAK_CONTROL") == "1"
    if soak_control and args.scenario != "neural-soak":
        log.error("GOTHIC_SOAK_CONTROL requires --scenario neural-soak")
        return 2
    if soak_control and args.neural_body:
        log.error("scenario neural-soak control requires --no-neural-body")
        return 2
    if args.scenario in {
        "neural",
        "neural-companion",
        "neural-group",
        "neural-talk",
        "neural-load",
        "neural-soak",
        "neural-still",
    } and not soak_control and (
        not args.neural_body or (args.body_policy == "service" and not args.body_url)
    ):
        log.error("scenario %s requires --neural-body and, for service policy, GOTHIC_BODY_URL "
                  "(or --body-url)", args.scenario)
        return 2
    done = asyncio.Event()
    exit_code = 0
    state = ServerState()
    llm = _backend(args)
    game_dir = Path(args.game_dir) if args.game_dir else None
    personas = PersonaProvider(game_dir if game_dir and game_dir.is_dir() else None)
    threading.Thread(target=personas.warm, name="original-lines", daemon=True).start()
    embed_at = None if args.no_embeddings else embed_url(args.embed_url, args.decider_url)
    embeddings = EmbedClient(embed_at) if embed_at else None
    memory = MemoryStore(args.memory_db, embeddings=embeddings)
    asr = None if args.no_voice else AsrClient(args.asr_url)
    decider = None if args.no_brain or not args.decider_url else DeciderClient(args.decider_url)
    # npc lines in a fight: a local model, in a fraction of a second (dialogue.py)
    quick_at = None if args.no_quick else quick_url(args.quick_url, args.decider_url)
    quick = make_quick(quick_at) if quick_at else None
    # how the hero's words sound to a wolf, in a word (beasts.py)
    tone_quick = make_quick(quick_at, max_tokens=TONE_MAX_TOKENS) if quick_at and args.beasts else None
    source = None if args.no_voice else _audio_source(args)
    tts = None if args.no_tts else TtsClient(args.tts_url)
    # a line's face from its voice (v63): no voice, no face
    face_at = a2f_url(args.a2f_url, args.decider_url)
    face = None if tts is None else make_source(args.a2f, face_at, args.a2f_timeout, args.a2f_timeout_per_s)
    journal = Journal(print_sink if args.journal else None)
    trace = None if args.no_trace else TraceWriter(Path(args.trace_dir))
    budget = Budget(per_npc_per_min=args.llm_per_npc_min, total_per_min=args.llm_total_min)
    log.info("LLM backend: %s (%s)", llm.name, getattr(llm, "model", "-"))
    log.info("memory: %s", args.memory_db)
    log.info("memory search: %s", f"semantic ({embed_at}), lexical fallback" if embeddings else "lexical")
    log.info("turn traces: %s", "off" if trace is None else args.trace_dir)
    log.info("voice: %s", "off" if asr is None else f"{args.voice_file or 'microphone'} -> {args.asr_url}")
    log.info("npc voices: %s", "off" if tts is None else args.tts_url)
    log.info("npc faces from voice: %s", "off" if face is None else getattr(face, "url", face.name))
    log.info("npc planner: %s", "off" if decider is None else args.decider_url)
    log.info(
        "neural body: policy=%s service=%s",
        args.body_policy if args.neural_body else "off", args.body_url,
    )
    log.info("lines in a fight: %s", quick_at or "the conversational model")
    log.info("wolves hear the hero: %s", "off" if not args.beasts else f"tone by {quick_at or 'the rules'}")
    graph = (
        None
        if args.no_director and args.no_knowledge and args.no_questlog and args.no_crimes and args.no_gates
        else _story_graph(args, game_dir)
    )
    place_book = places.PlaceBook.load()
    log.info("places: %s", f"{len(place_book.places)} named" if place_book else "no book, waypoints only")
    log.info(
        "story graph: %s; director %s, npc knowledge %s",
        "none" if graph is None else f"{len(graph.nodes)} nodes, {len(graph.quests)} quests",
        "off" if graph is None or args.no_director else "on",
        "off" if graph is None or args.no_knowledge else "on",
    )

    def set_exit_code(code: int) -> None:
        nonlocal exit_code
        exit_code = code

    env = SessionEnv(
        args=args,
        state=state,
        llm=llm,
        memory=memory,
        asr=asr,
        tts=tts,
        face=face,
        decider=decider,
        quick=quick,
        graph=graph,
        place_book=place_book,
        journal=journal,
        trace=trace,
        done=done,
        personas=personas,
        budget=budget,
        tone_quick=tone_quick,
        source=source,
        set_exit_code=set_exit_code,
    )

    async def on_link(link: GameLink) -> None:
        await run_session(env, link)

    server = GameServer(args.host, args.port, on_link)
    tasks = [asyncio.create_task(server.serve_forever()), asyncio.create_task(done.wait())]
    if not args.no_console:
        tasks.append(asyncio.create_task(Console(server, state, args.npc).run()))
    finished, _ = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
    for task in tasks:
        task.cancel()
    await asyncio.gather(*tasks, return_exceptions=True)
    await llm.aclose()
    if asr is not None:
        await asr.aclose()
    if tts is not None:
        await tts.aclose()
    if face is not None:
        await face.aclose()
    if decider is not None:
        await decider.aclose()
    if quick is not None:
        await quick.aclose()
    if tone_quick is not None:
        await tone_quick.aclose()
    memory.close()
    if embeddings is not None:
        await embeddings.close()
    for task in finished:
        if not task.cancelled() and (err := task.exception()) is not None:
            log.error("%s", err)
            return 1
    return exit_code


PROBE_SITUATION = "[Обстановка: твой инвентарь — зелье лечения (ITPO_HEALTH_01) ×2.]"
PROBE_TEXT = "Дай мне зелье лечения, пожалуйста."  # the npc has one: it calls give_item
PROBE_REJECTION = "Отклонено: зелья у тебя больше нет, ты отдал последнее раньше."


async def probe_tools(llm: LLMBackend, instructions: str, text: str) -> dict[str, Any]:
    """Can a turn go on after a tool call? Step 1 offers the tools; every call is answered as rejected and
    step 2 goes on with the response's own items (reasoning included) and tool_choice="none"."""
    messages: list[Item] = [Message("developer", PROBE_SITUATION), Message("user", text)]
    steps: list[dict[str, Any]] = []
    for n in (1, 2):
        started = time.monotonic()
        first: float | None = None
        done: Done | None = None
        async for event in llm.stream(
            instructions=instructions,
            messages=messages,
            tools=npc_tools.TOOLS,
            tool_choice="none" if n == 2 else None,
        ):
            if isinstance(event, TextDelta) and first is None:
                first = time.monotonic() - started
            elif isinstance(event, Done):
                done = event
        assert done is not None
        calls = [i for i in done.items if isinstance(i, ToolCall)]
        steps.append(
            {
                "text": done.text.strip(),
                "calls": [f"{c.name}({c.raw})" for c in calls],
                "reasoning_items": sum(isinstance(i, Reasoning) for i in done.items),
                "first_token_s": round(first or 0.0, 2),
                "total_s": round(time.monotonic() - started, 2),
                "usage": vars(done.usage) if done.usage else None,
            }
        )
        if not calls:
            break
        messages += [*done.items, *(ToolResult(c.call_id, PROBE_REJECTION) for c in calls)]
    ok = len(steps) == 2 and not steps[1]["calls"] and bool(steps[1]["text"])
    return {"ok": ok, "llm": llm.name, "model": getattr(llm, "model", None), "steps": steps}


async def llm_test(args: argparse.Namespace) -> int:
    """One request, prints the reply and latency: compare backends and models. With --json one object
    on stdout ({"text", "llm", "model", "first_token_s", "total_s"} or {"error"}), for the launcher."""
    llm = _backend(args)
    personas = PersonaProvider(Path(args.game_dir) if args.game_dir else None)
    persona = personas.get(args.npc, args.name)
    if args.probe_tools:
        try:
            result = await probe_tools(llm, persona.instructions, args.text or PROBE_TEXT)
        except LLMError as err:
            result = {"ok": False, "error": str(err)}
        finally:
            await llm.aclose()
        print(json.dumps(result, ensure_ascii=False, indent=None if args.json else 2))
        return 0 if result["ok"] else 1
    if not args.text:
        await llm.aclose()
        print("llm-test: the text to send is required", file=sys.stderr)
        return 2
    messages = [Message("user", args.text)]
    if args.situation:
        messages.insert(0, Message("developer", args.situation))
    tools = npc_tools.TOOLS if args.tools else None
    started = time.monotonic()
    first = None
    text = ""
    try:
        async for event in llm.stream(instructions=persona.instructions, messages=messages, tools=tools):
            if isinstance(event, TextDelta):
                if first is None:
                    first = time.monotonic() - started
                text += event.text
                if not args.json:
                    print(event.text, end="", flush=True)
            elif isinstance(event, ToolCall) and not args.json:
                at = time.monotonic() - started
                print(f"\n  -> {event.name}({event.arguments}) at {at:.2f}s", flush=True)
    except LLMError as err:
        if args.json:
            print(json.dumps({"error": str(err)}, ensure_ascii=False))
        else:
            print(f"\nerror: {err}", file=sys.stderr)
        return 1
    finally:
        await llm.aclose()
    total = time.monotonic() - started
    if args.json:
        result = {
            "text": text.strip(),
            "llm": llm.name,
            "model": getattr(llm, "model", None),
            "first_token_s": round(first or 0.0, 2),
            "total_s": round(total, 2),
        }
        print(json.dumps(result, ensure_ascii=False))
    else:
        print(f"\n\n[{llm.name}] first token {first or 0:.2f}s, total {total:.2f}s", file=sys.stderr)
    return 0


async def places_cmd(args: argparse.Namespace) -> int:
    """The book of places from a dump of the world (SCENARIO=places): grouped, surveyed (rooms, ways in,
    neighbours), named and its kinds confirmed by the model, written. `--names`: the names of a book read
    through already, kept; only places it lacks are named afresh."""
    data = json.loads(await asyncio.to_thread(Path(args.dump).read_text, encoding="utf-8"))
    groups = places.group(data["points"], data.get("people", []))
    if args.area:
        groups = [g for g in groups if g.area in {a.upper() for a in args.area}]
    surveys = places.survey(groups)
    print(f"{len(data['points'])} points -> {len(groups)} places", flush=True)
    if args.dry_run:
        for g in sorted(groups, key=lambda g: g.id):
            s = surveys[g.id]
            ways = ", ".join(str(w["side"]) for w in s.entrances)
            print(
                f"  {g.id:40s} {len(g.points):4d} points  {s.guess:6s} in {s.inside:4.0%}"
                f"{f'  in from {ways}' if ways else ''}  {'; '.join(g.residents[:2])}"
            )
        return 0
    names: dict[str, dict[str, str]] = {}
    if args.names:
        kept = json.loads(await asyncio.to_thread(Path(args.names).read_text, encoding="utf-8"))
        ids = {g.id for g in groups}
        for p in kept.get("places", []):
            if p.get("id") in ids:
                names[p["id"]] = {k: str(p.get(k, "")) for k in ("ru", "where_ru", "en", "where_en")}
        print(f"{len(names)} names kept from {args.names}", flush=True)
    llm = _backend(args)
    try:
        fresh = [g for g in groups if g.id not in names]
        if fresh:
            names |= await places.label(fresh, llm, among=groups)
        kind = await places.kinds(groups, names, surveys, llm)
    finally:
        await llm.aclose()
    book = places.book(groups, names, data.get("world"), surveys, kind)
    text = json.dumps(book, ensure_ascii=False, indent=1)
    await asyncio.to_thread(Path(args.out).write_text, text, encoding="utf-8")
    print(f"{len(book['places'])} of {len(groups)} places named -> {args.out}")
    return 0 if len(book["places"]) == len(groups) else 1


async def lore_cmd(args: argparse.Namespace) -> int:
    """The book of facts (lore.py): who and what is where, as each npc of the world knows it from its
    dialogs. Picks up where a run stopped; `--who`: only these, afresh."""
    data = json.loads(await asyncio.to_thread(Path(args.dump).read_text, encoding="utf-8"))
    book = places.PlaceBook.load()
    graph = _story_graph(args, Path(args.game_dir) if args.game_dir else None)
    if book is None or graph is None:
        print("lore: needs the book of places and the story graph", file=sys.stderr)
        return 2
    people = lore.people_of(data.get("people", []), book)
    out = Path(args.out)
    done: dict[str, list[dict[str, Any]]] = {}
    if await asyncio.to_thread(out.exists):
        kept = json.loads(await asyncio.to_thread(out.read_text, encoding="utf-8"))
        if kept.get("version") == lore.VERSION:
            done = dict(kept.get("npcs", {}))
    wanted = [n.upper() for n in args.who] if args.who else sorted(people)
    npcs = [n for n in wanted if graph.dialogs_of(n)]
    for n in npcs if args.who else []:
        done.pop(n, None)
    world = str(data.get("world", ""))
    print(f"{len(npcs)} npc's with dialogs, {len([n for n in npcs if n in done])} done already", flush=True)

    def save(facts: dict[str, list[dict[str, Any]]]) -> None:
        text = json.dumps(lore.file(world, facts, people), ensure_ascii=False, indent=1)
        out.write_text(text, encoding="utf-8")

    llm = _backend(args)
    try:
        facts = await lore.build(npcs, graph, book, people, llm, done=done, save=save)
    finally:
        await llm.aclose()
    save(facts)
    print(f"{sum(len(facts.get(n, [])) for n in npcs)} facts of {len(npcs)} npc's -> {out}")
    if args.who:
        for n in npcs:
            for f in facts.get(n, []):
                print(f"  {n} [{f['dialog']}] {f['place'] or '-'} {f['who']}: {f['ru']}")
    return 0


async def cards_cmd(args: argparse.Namespace) -> int:
    """Generate character cards from the connected world's people and their original script lines."""
    graph = _story_graph(args, Path(args.game_dir) if args.game_dir else None)
    if graph is None:
        print("cards: needs the story graph", file=sys.stderr)
        return 2
    only = {npc.strip().upper() for npc in (args.only or "").split(",") if npc.strip()} or None
    cards = characters.load()
    llm = _backend(args)
    server = GameServer(args.host, args.port)
    try:
        async with asyncio.TaskGroup() as tasks:
            serving = tasks.create_task(server.serve_forever())
            try:
                link = await server.wait_link()
                await link.wait_world()
                people = await link.story_people(timeout=30)
                subjects = [
                    subject for subject in characters.subjects(graph, people, only)
                    if only or subject.npc not in cards
                ]
                print(f"{len(subjects)} subjects, {len(cards)} existing cards", flush=True)
                left = list(subjects)
                for attempt in (1, 2):
                    missed = []
                    for i in range(0, len(left), characters.BATCH):
                        batch = left[i : i + characters.BATCH]
                        got, errors = await characters.write(llm, batch)
                        cards.update(got)
                        characters.save(cards)
                        missed.extend(subject for subject in batch if subject.npc not in got)
                        log.info(
                            "cards: %d/%d written (attempt %d)%s", len(got), len(batch), attempt,
                            f": {errors}" if errors else "",
                        )
                    left = missed
                    if not left:
                        break
            finally:
                serving.cancel()
    finally:
        await llm.aclose()
    print(f"{len(subjects) - len(left)} of {len(subjects)} cards written -> {characters.CARDS_FILE}")
    return 0 if not left and (subjects or only is None) else 1


async def replay(args: argparse.Namespace) -> int:
    """A recorded turn, run again: compare models and prompts on real situations from the game."""
    path = Path(args.file) if args.file else latest(Path(args.trace_dir))
    if path is None:
        print(f"no traces in {args.trace_dir}", file=sys.stderr)
        return 2
    try:
        record, instructions = find_turn(path, args.turn)
    except LookupError as err:
        print(err, file=sys.stderr)
        return 2
    if args.persona_now or instructions is None:
        game_dir = Path(args.game_dir) if args.game_dir else None
        personas = PersonaProvider(game_dir if game_dir and game_dir.is_dir() else None)
        instructions = personas.get(record["npc"], args.name).instructions
    llm = _backend(args)
    try:
        result = await replay_turn(llm, record, instructions, after_step=args.after_step)
    except ValueError as err:
        print(err, file=sys.stderr)
        return 2
    finally:
        await llm.aclose()
    if args.json:
        print(json.dumps(result, ensure_ascii=False))
        return 0
    print(
        f"turn {result['turn']} ({result['npc']}), {result['trigger']['kind']}: {result['trigger']['text']}"
    )
    if args.after_step:
        print(f"continuing after recorded step {args.after_step}; already said: {result['said_before']}")
        for output in result["initial_tool_results"]:
            print(f"  tool result: {output}")
    for title, steps in (("recorded", result["original"]), (f"{llm.name}", result["replay"])):
        print(f"\n[{title}]")
        for n, st in enumerate(steps, 1):
            calls = ", ".join(
                c if isinstance(c, str) else f"{c['name']}{c['arguments']}" for c in st["calls"]
            )
            timing = f" ({st['first_token_s']:.2f}s / {st['total_s']:.2f}s)" if "total_s" in st else ""
            print(f"  {n}. {st['text'] or '-'}{f'  -> {calls}' if calls else ''}{timing}")
            for call in st["calls"]:
                if isinstance(call, dict):
                    print(f"     {call['output']}")
    if result["diverged"]:
        print("\n(the replay made calls the recorded turn did not: their results are not known)")
    return 0


async def replay_first_words(args: argparse.Namespace) -> int:
    """Compare quick first reactions to the hero's recorded phrases, without running the game."""
    if args.runs < 1:
        print("--runs must be at least 1", file=sys.stderr)
        return 2
    quick_at = quick_url(args.quick_url, os.environ.get("GOTHIC_DECIDER_URL"))
    if not quick_at:
        settings_path = Path.home() / ".config/gothic-ai/settings.toml"
        if await asyncio.to_thread(settings_path.is_file):
            settings = tomllib.loads(await asyncio.to_thread(settings_path.read_text, encoding="utf-8"))
            llm_settings = settings.get("llm", {})
            quick_at = quick_url(llm_settings.get("quick_url"), llm_settings.get("decider_url"))
    if not quick_at:
        print(
            "set --quick-url, GOTHIC_QUICK_LLM_URL/GOTHIC_DECIDER_URL or [llm] in settings.toml",
            file=sys.stderr,
        )
        return 2
    records = await asyncio.to_thread(read, Path(args.file))
    instructions = {r["hash"]: r["text"] for r in records if r.get("type") == "instructions"}
    turns = [
        r
        for r in records
        if r.get("type") == "turn"
        and r.get("trigger", {}).get("kind") == "player"
        and r.get("steps")
        and not r["steps"][0].get("llm")
        and (not args.npc or r["npc"].upper() == args.npc.upper())
    ]
    if not turns:
        print("no matching player turns with a conversational first step", file=sys.stderr)
        return 2
    missing = [r["id"] for r in turns if r["instructions"] not in instructions]
    if missing:
        print(f"missing recorded instructions for turns: {', '.join(missing)}", file=sys.stderr)
        return 2
    hint = (
        (await asyncio.to_thread(Path(args.hint_file).read_text, encoding="utf-8")).strip()
        if args.hint_file else REACT_HINT
    )
    previous: dict[str, list[str]] = {}
    branch: str | None = None
    llm = make_quick(quick_at)
    try:
        with (
            await asyncio.to_thread(Path(args.out).open, "w", encoding="utf-8")
            if args.out else nullcontext()
        ) as output:
            for turn in turns:
                if turn.get("branch") != branch:
                    previous.clear()
                    branch = turn.get("branch")
                last = previous.setdefault(turn["npc"], [""] * args.runs)
                result = await replay_first_words_turn(
                    llm,
                    turn,
                    instructions[turn["instructions"]],
                    hint=hint,
                    runs=args.runs,
                    temperature=args.temperature,
                    previous=last,
                )
                rendered = []
                for n, run in enumerate(result["runs"]):
                    if run["outcome"] == "said":
                        last[n] = run["words"]
                    sound = ", pure_sound" if run["pure_sound"] else ""
                    rendered.append(
                        f"{n + 1}: {run['words'] or '—'} [{run['outcome']}{sound}, {run['elapsed_ms']} ms]"
                    )
                phrase = " ".join(result["phrase"].split())[:80]
                print(f"{result['npc']} | {phrase} | {' | '.join(rendered)}", flush=True)
                if output is not None:
                    output.write(json.dumps(result, ensure_ascii=False) + "\n")
                    output.flush()
    finally:
        await llm.aclose()
    return 0


async def replay_svm_lines(args: argparse.Namespace) -> int:
    """Compare SVM lines in the last recorded situation of each NPC, without a game."""
    if args.runs < 1:
        print("--runs must be at least 1", file=sys.stderr)
        return 2
    quick_at = quick_url(args.quick_url, os.environ.get("GOTHIC_DECIDER_URL"))
    if not quick_at:
        settings_path = Path.home() / ".config/gothic-ai/settings.toml"
        if await asyncio.to_thread(settings_path.is_file):
            settings = tomllib.loads(await asyncio.to_thread(settings_path.read_text, encoding="utf-8"))
            llm_settings = settings.get("llm", {})
            quick_at = quick_url(llm_settings.get("quick_url"), llm_settings.get("decider_url"))
    if not quick_at:
        print(
            "set --quick-url, GOTHIC_QUICK_LLM_URL/GOTHIC_DECIDER_URL or [llm] in settings.toml",
            file=sys.stderr,
        )
        return 2
    records = await asyncio.to_thread(read, Path(args.file))
    instructions = {r["hash"]: r["text"] for r in records if r.get("type") == "instructions"}
    turns = {}
    names = {npc: card.name for npc, card in characters.load().items()}
    for record in records:
        snapshot = record.get("snapshot") or {}
        for npc in [snapshot.get("npc", {}), *snapshot.get("nearby_npcs", [])]:
            if npc.get("id") and npc.get("name"):
                names[npc["id"].upper()] = npc["name"]
        if (
            record.get("type") == "turn"
            and record.get("steps")
            and any(m.get("role") == "developer" for m in record["steps"][0]["added"])
            and (not args.npc or record["npc"].upper() == args.npc.upper())
        ):
            turns[record["npc"]] = record
    names.update(PLAYER="герой", PC_HERO="герой")
    if not turns:
        print("no matching turns with a recorded situation", file=sys.stderr)
        return 2
    missing = [r["id"] for r in turns.values() if r["instructions"] not in instructions]
    if missing:
        print(f"missing recorded instructions for turns: {', '.join(missing)}", file=sys.stderr)
        return 2
    lines = await asyncio.to_thread(read, Path(args.svm_file))
    hint = (
        (await asyncio.to_thread(Path(args.hint_file).read_text, encoding="utf-8")).strip()
        if args.hint_file else SVM_HINT
    )
    llm = make_quick(quick_at)
    try:
        with (
            await asyncio.to_thread(Path(args.out).open, "w", encoding="utf-8")
            if args.out else nullcontext()
        ) as output:
            for turn in turns.values():
                for line in lines:
                    [result] = await replay_svm_lines_turn(
                        llm, turn, instructions[turn["instructions"]], [line],
                        names=names, hint=hint, history=args.history, runs=args.runs,
                    )
                    rendered = [
                        f"{n + 1}: {run['words'] or '—'} [{run['outcome']}, {run['first_phrase_ms']} ms]"
                        for n, run in enumerate(result["runs"])
                    ]
                    native = " ".join(result["text"].split())
                    print(
                        f"{result['npc']} | {result['svm']} | {native} | {' | '.join(rendered)}", flush=True,
                    )
                    if output is not None:
                        output.write(json.dumps(result, ensure_ascii=False) + "\n")
                        output.flush()
    finally:
        await llm.aclose()
    return 0


async def login_chatgpt() -> int:
    store = TokenStore()
    try:
        creds = await login(store)
    except (AuthError, TimeoutError) as err:
        print(f"sign-in failed: {err}", file=sys.stderr)
        return 1
    print(f"signed in{f' as {creds.email}' if creds.email else ''}; credentials saved to {store.path}")
    return 0


async def voice_test(args: argparse.Namespace) -> int:
    """Record from the microphone (or play a wav), streaming it to the ASR service like push-to-talk;
    prints partial text as it comes and the final one with the latency after the end of speech.
    With --json, JSON lines on stdout for the launcher: {"level": 0..1} while recording (~10 a second),
    {"partial": text}, and last {"text", "provider", "audio_s", "latency_s"} or {"error"}."""
    source = _audio_source(args)
    asr = AsrClient(args.asr_url)
    queue: asyncio.Queue[bytes | None] = asyncio.Queue()
    loop = asyncio.get_running_loop()
    seconds = getattr(source, "duration", args.seconds)

    def out(msg: dict[str, object]) -> None:
        print(json.dumps(msg, ensure_ascii=False), flush=True)

    def partial(text: str) -> None:
        if args.json:
            out({"partial": text})
        else:
            print(f"  … {text}", file=sys.stderr)

    def fail(detail: str) -> int:
        if args.json:
            out({"error": detail})
        else:
            print(f"error: {detail}", file=sys.stderr)
        return 1

    try:
        session = await asr.open(source.rate, on_partial=partial)
        source.start(lambda data: loop.call_soon_threadsafe(queue.put_nowait, data))
        if not args.voice_file and not args.json:
            print(f"говорите {seconds:g} с...", file=sys.stderr, flush=True)
        loop.call_later(seconds + 0.1, lambda: (source.stop(), queue.put_nowait(None)))
        size, heard, peak, last_level = 0, False, 0.0, 0.0
        while (chunk := await queue.get()) is not None:
            size += len(chunk)
            heard = heard or chunk.count(0) != len(chunk)
            if args.json:
                peak = max(peak, _level(chunk))
                if (now := time.monotonic()) - last_level >= 0.1:
                    out({"level": round(peak, 3)})
                    peak, last_level = 0.0, now
            await session.feed(chunk)
        if size and not heard:
            await session.cancel()
            return fail(MIC_DENIED)
        result = await session.finish()
    except VoiceError as err:
        return fail(str(err))
    finally:
        await asr.aclose()
    audio_s = size / 2 / source.rate
    if args.json:
        out(
            {
                "text": result.text,
                "provider": result.provider,
                "audio_s": round(audio_s, 2),
                "latency_s": round(result.latency_s, 2),
            }
        )
        return 0
    print(result.text or "(ничего не распознано)")
    took = f"{audio_s:.1f}s audio, {result.latency_s:.2f}s after it ended"
    print(f"[{result.provider}] {took}", file=sys.stderr)
    return 0


def _level(chunk: bytes) -> float:
    """Loudness of 16-bit PCM as 0..1: -60 dBFS and below is 0, full scale is 1."""
    samples = array("h", chunk[: len(chunk) // 2 * 2])
    if not samples:
        return 0.0
    rms = math.sqrt(sum(x * x for x in samples) / len(samples))
    if rms < 1:
        return 0.0
    return max(0.0, min(1.0, (20 * math.log10(rms / 32768) + 60) / 60))


def mics(args: argparse.Namespace) -> int:
    """Input devices, for the launcher's microphone setting."""
    try:
        import sounddevice as sd

        devices = sd.query_devices()
        default = sd.default.device[0]
    except Exception as err:  # PortAudio missing or broken
        print(json.dumps({"error": f"нет доступа к аудио: {err}"}, ensure_ascii=False))
        return 1
    found = [
        {"index": i, "name": d["name"], "default": i == default}
        for i, d in enumerate(devices)
        if d["max_input_channels"] > 0
    ]
    print(json.dumps(found, ensure_ascii=False))
    return 0


async def speech_test(args: argparse.Namespace) -> int:
    """One line in an npc's voice through the TTS service, played as it arrives, the way the game gets
    it (same voice id and description, so the provider reuses the npc's voice). With --json one object:
    {"text", "provider", "first_chunk_s", "audio_s", "latency_s"} or {"error"}."""
    personas = PersonaProvider(Path(args.game_dir) if args.game_dir else None)
    persona = personas.get(args.npc, args.name)
    tts = TtsClient(args.tts_url)
    player: Any = None
    try:
        import sounddevice as sd

        async def start(rate: int, _provider: str) -> None:
            nonlocal player
            player = sd.RawOutputStream(samplerate=rate, channels=1, dtype="int16")
            player.start()

        async def chunk(data: bytes) -> None:
            await asyncio.to_thread(player.write, data[: len(data) // 2 * 2])

        style = MOOD_DELIVERY[args.mood] if args.mood else ""
        speech = await tts.stream(args.text, persona.voice_id, persona.voice, start, chunk, style)
        if player is not None:
            await asyncio.to_thread(player.stop)  # waits until the last chunk is heard
    except Exception as err:  # TtsError, or sounddevice's own errors when playing
        detail = str(err) if isinstance(err, TtsError) else f"звук не проигрался: {err}"
        print(json.dumps({"error": detail}, ensure_ascii=False) if args.json else f"error: {detail}")
        return 1
    finally:
        if player is not None:
            player.close()
        await tts.aclose()
    result = {
        "text": args.text,
        "provider": speech.provider,
        "first_chunk_s": round(speech.first_chunk_s, 2),
        "audio_s": round(speech.audio_s, 2),
        "latency_s": round(speech.latency_s, 2),
    }
    print(json.dumps(result, ensure_ascii=False) if args.json else result)
    return 0


def show_memory(args: argparse.Namespace) -> int:
    store = MemoryStore(args.memory_db)
    if args.branch is None:
        for b in store.branches():
            when = time.strftime("%Y-%m-%d %H:%M", time.localtime(b.created))
            parent = f" from {b.parent}@{b.parent_seq}" if b.parent else ""
            print(f"{b.id}  {when}  head {b.head}, {b.npcs} npc(s){parent}")
        return 0
    mem = store.load(args.branch, args.npc.upper())
    if mem.summary:
        print(f"summary (up to {mem.summary_seq}): {mem.summary}")
    for e in mem.entries:
        print(f"{e.seq:5d} {e.role:9s} {e.content}")
    return 0


def lines(args: argparse.Namespace) -> int:
    found = load_npc_lines(Path(args.game_dir), args.npc)
    for name, text in found:
        print(f"{name}: {text}")
    print(f"{len(found)} line(s)", file=sys.stderr)
    return 0


def voices(args: argparse.Namespace) -> int:
    """The TTS voices of npc's as the game speaks them: {"<npc>": {"voice_id", "voice"}} on stdout, the voice
    a description in English (the shorts' dub designs the English voices from it)."""
    game_dir = Path(args.game_dir) if args.game_dir and Path(args.game_dir).is_dir() else None
    personas = PersonaProvider(game_dir)
    out = {}
    for npc in args.npc:
        persona = personas.get(npc, npc)
        out[npc.upper()] = {"voice_id": persona.voice_id, "voice": persona.voice}
    print(json.dumps(out, ensure_ascii=False, indent=1))
    return 0


def _add_voice_args(p: argparse.ArgumentParser) -> None:
    p.add_argument("--asr-url", default=os.environ.get("GOTHIC_ASR_URL", DEFAULT_ASR_URL))
    p.add_argument("--mic-device", help="input device name or index (default: system input)")
    p.add_argument("--no-player-face", action="store_true", help="do not stream microphone PCM to the hero")
    p.add_argument("--voice-file", help="use this 16-bit mono wav instead of the microphone (tests)")
    p.add_argument(
        "--voice-files",
        action="store_true",
        help=("the game may name a wav the hero says instead of the microphone "
              "(the test bench, gothic-shorts)"),
    )


def _add_llm_args(p: argparse.ArgumentParser) -> None:
    p.add_argument("--llm", choices=(*BACKENDS, "auto"), default=os.environ.get("GOTHIC_AI_LLM", "chatgpt"))
    p.add_argument("--model", help="model id (default depends on the backend)")
    p.add_argument("--base-url", help="API base url (openai/local backends)")
    p.add_argument("--reasoning-effort", default="low", help="reasoning effort for Responses models")
    p.add_argument(
        "--no-websocket",
        action="store_true",
        help="npc requests of Responses models over HTTP, not a WebSocket of each npc's own (no prompt cache "
        "on the ChatGPT plan, first text ~1 s later)",
    )
    p.add_argument("--game-dir", default=os.environ.get("GOTHIC_DIR", DEFAULT_GAME_DIR))
    p.add_argument("--npc", default="NONE_100_XARDAS", help="default npc for commands and scenarios")


def main() -> None:
    parser = argparse.ArgumentParser(prog="gothic-ai")
    parser.add_argument("-v", "--verbose", action="store_true")
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("serve", help="wait for the game and drive npc's")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=47817)
    p.add_argument(
        "--scenario",
        choices=[
            "m0",
            "m1",
            "websocket",
            "m2",
            "m3",
            "m4",
            "m5",
            "m5b",
            "neural",
            "neural-companion",
            "neural-group",
            "neural-talk",
            "neural-still",
            "bandit_parley",
            "road_bandits",
            "bandit_hold",
            "bandit_bounty",
            "neural-load",
            "neural-soak",
            "memprobe",
            "load_rewind",
            "m5c",
            "m5e",
            "svm_lines",
            "combat_barks",
            "mercy",
            "hero_yield",
            "humiliate",
            "surrender_live",
            "live_lies",
            "live_npc",
            "live_director",
            "live_deals",
            "orders",
            "fight_reactions_wolf",
            "fight_reactions_sight",
            "fight_reactions_bandits",
            "fight_reactions_flee",
            "companion",
            "wolf",
            "wolf_load",
            "calm",
            "ground",
            "hearing",
            "cave",
            "knows",
            "lead",
            "meet",
            "meet_missed",
            "errand",
            "errand_fork",
            "errand_far",
            "errand_give",
            "chests",
            "chests_lobart",
            "group",
            "interject",
            "remembers",
            "m6",
            "see",
            "seen",
            "trace",
            "broken",
            "witness",
            "places",
            "graph",
            "programs",
            "lie",
            "wrath",
            "repair",
            "death_load",
            "feats",
            "feat_fame",
            "authored",
            "occasions",
            "questlog",
            "crimes",
            "crime_react",
            "npc_deeds",
            "npc_director",
            "npc_will",
            "npc_far",
            "body_found",
            "blood_found",
            "frame",
            "crime_meet",
            "crime_ally",
            "check_walk",
            "follow_back",
            "crime_talk",
            "trace_crime",
            "cover",
            "cover_subject",
            "cheat",
            "crime_director",
            "crime_law",
            "quest_law",
            "showrunner",
            "showrunner_live",
            "vanilla_line",
            "vanilla_line_live",
            "own",
            "trigger",
            "stages",
            "own_amend",
            "requires",
            "chain",
            "own_repair",
            "cast",
            "person",
            "character",
            "vlie",
            "vgive",
            "vspare",
            "gate",
            "gate_vouch",
            "greg",
            "vcheck",
            "rumor",
            "amends",
            "deal",
            "deal_demo",
            "contract",
            "debt",
            "xardas_money",
            "initiative",
            "gossip",
            "gossip_demo",
            "face_cues_demo",
            "face_eyes_demo",
            "face_brows_demo",
            "face_listen_demo",
            "day",
            "noise",
            "amends_deal",
            "haggle",
            "rescue",
            "rescue_self",
            "tts",
        ],
        help="run a scenario when the game connects (the ordinary showrunner loop stays off)",
    )
    p.add_argument("--exit-after", action="store_true", help="exit when the scenario finishes")
    p.add_argument("--no-console", action="store_true", help="do not read commands from stdin")
    p.add_argument("--memory-db", default=str(DEFAULT_MEMORY_DB), help="SQLite file with npc memories")
    p.add_argument("--no-voice", action="store_true", help="disable push-to-talk")
    p.add_argument("--tts-url", default=os.environ.get("GOTHIC_TTS_URL", DEFAULT_TTS_URL))
    p.add_argument("--no-tts", action="store_true", help="npc's do not speak aloud (subtitles only)")
    p.add_argument(
        "--a2f",
        choices=("service", "fake", "off"),
        default=os.environ.get("GOTHIC_A2F", "service"),
        help="face tracks of voiced lines (v63): the Audio2Face service, fake (from loudness, no network) "
        "or off (GOTHIC_A2F; default: service)",
    )
    p.add_argument(
        "--a2f-url",
        default=os.environ.get("GOTHIC_A2F_URL"),
        help=f"gothic-a2f service (GOTHIC_A2F_URL; default: the decider's host, http://<gpu>:{A2F_PORT})",
    )
    p.add_argument(
        "--a2f-timeout", type=float, default=DEFAULT_A2F_TIMEOUT_S,
        help="seconds a line waits for its face track besides --a2f-timeout-per-s: the network and the "
        "queue (default: %(default)s)",
    )
    p.add_argument(
        "--a2f-timeout-per-s", type=float, default=DEFAULT_A2F_TIMEOUT_PER_S,
        help="and this much per second of the line's voice; a line that gets no track in time goes without "
        "one, and the service is not asked for 30 s (default: %(default)s)",
    )
    p.add_argument(
        "--a2f-emotion", action="append", default=[], type=_a2f_emotion, metavar="MOOD=LABEL",
        help="the Audio2Face emotion of a line's mood, e.g. friendly=joy:0.6; empty LABEL: neutral",
    )
    p.add_argument("--journal", action="store_true", help="print lines and actions as JOURNAL {json} lines")
    p.add_argument("--trace-dir", default=str(DEFAULT_TRACE_DIR), help="where turn traces go (a file a day)")
    p.add_argument("--no-trace", action="store_true", help="do not write turn traces")
    p.add_argument("--no-name-fix", action="store_true", help="disable correction of misheard hero names")
    p.add_argument(
        "--llm-per-npc-min",
        type=float,
        default=6.0,
        help="calls of the conversational model per npc and minute nobody asked for (incidents, summaries)",
    )
    p.add_argument("--llm-total-min", type=float, default=20.0, help="the same for all npc's together")
    p.add_argument(
        "--decider-url",
        default=os.environ.get("GOTHIC_DECIDER_URL"),
        help="gothic-decider service for npc decisions outside conversations (without it the planner is off)",
    )
    p.add_argument(
        "--embed-url",
        default=os.environ.get("GOTHIC_EMBED_URL"),
        help="gothic-embed service for NPC memory (default: the decider's host, http://<gpu>:47837)",
    )
    p.add_argument(
        "--no-embeddings", action="store_true", help="use word search for NPC memory"
    )
    p.add_argument(
        "--body-policy",
        choices=("engine", "service"),
        default=os.environ.get("GOTHIC_BODY_POLICY", "engine"),
        help="combat policy location (GOTHIC_BODY_POLICY; default: engine)",
    )
    p.add_argument(
        "--body-url",
        default=os.environ.get("GOTHIC_BODY_URL"),
        help="backup body policy service at tcp://host:47835 (GOTHIC_BODY_URL)",
    )
    p.add_argument(
        "--neural-body",
        action=argparse.BooleanOptionalAction,
        default=False,
        help="let the body policy fight the hero or companions with eligible npcs (default: off)",
    )
    p.add_argument(
        "--neural-body-max",
        type=int,
        choices=range(1, 5),
        default=4,
        help="maximum simultaneous body policy fights (1-4; default: 4)",
    )
    p.add_argument(
        "--quick-url",
        default=os.environ.get("GOTHIC_QUICK_LLM_URL"),
        help="the quick model's server (llama.cpp, server/deploy/gemma): npc lines in a fight at once "
        "(default: the decider's host, http://<gpu>:47834/v1)",
    )
    p.add_argument(
        "--no-quick", action="store_true", help="npc lines in a fight from the conversational model"
    )
    p.add_argument(
        "--no-svm-lines", action="store_true", help="keep the humans' native SVM voices and subtitles"
    )
    p.add_argument(
        "--debug-leak", nargs="?", const="any", choices=("any", "upper", "lower"),
        help="debug, for filming: every [felt:...] tag leaks, without a known lie or the trial; "
        "on this half of the face (default: the drawn one)",
    )
    p.add_argument("--no-combat-barks", action="store_true", help="disable quick-model combat cries")
    p.add_argument("--no-mercy", action="store_true", help="disable NPC and hero surrender and mercy")
    p.add_argument("--no-brain", action="store_true", help="do not plan npc decisions outside conversations")
    p.add_argument("--no-director", action="store_true", help="do not repair quests left without an ending")
    p.add_argument("--no-gates", action="store_true", help="disable conversational gate admission")
    p.add_argument("--no-questlog", action="store_true", help="do not track the scripts' quest states")
    p.add_argument("--no-parley", action="store_true", help="bandits may attack the hero without a cause")
    p.add_argument("--no-bounty", action="store_true", help="disable bandit bounty secrets")
    p.add_argument(
        "--no-crimes", action="store_true", help="disable crimes and law patches; scripts own law entirely"
    )
    p.add_argument(
        "--no-npc-cheat", action="store_true", help="all NPCs keep deferred deal promises"
    )
    p.add_argument(
        "--no-npc-crimes", action="store_true", help="reject marked NPC theft and attack; record no NPC deeds"
    )
    p.add_argument(
        "--no-cover", action="store_true", help="disable witness cover deals and fear of testifying"
    )
    p.add_argument(
        "--no-found-traces",
        action="store_true",
        help="a body or blood first seen is only a trace: no trace_found stimulus and no suspicion",
    )
    p.add_argument(
        "--no-occasions", action="store_true", help="do not watch for the director's story occasions"
    )
    p.add_argument(
        "--no-showrunner",
        action="store_true",
        help="disable the director's new plot lines (enabled by default outside scenarios)",
    )
    p.add_argument(
        "--no-feats",
        action="store_true",
        help="disable the chronicler's feats (enabled by default outside scenarios and in feats)",
    )
    p.add_argument(
        "--no-fate-card", action="store_true", help="disable the final fate card",
    )
    p.add_argument(
        "--no-feat-challenges",
        action="store_true",
        help="disable the director's hidden line challenges (enabled by default)",
    )
    p.add_argument(
        "--no-feat-fame",
        action="store_true",
        help="disable rumors about the hero's public feats",
    )
    p.add_argument(
        "--no-feat-images",
        action="store_true",
        help="disable generated feat pictures; use category icons for toasts",
    )
    p.add_argument(
        "--no-knowledge", action="store_true", help="npc's do not know the world from the scripts (M6)"
    )
    p.add_argument("--story-graph", help="storygraph JSON (default: made from the game's scripts and cached)")
    p.add_argument(
        "--no-hold-threats",
        action="store_true",
        help="threats to free npc's go straight to the game's scripts, not to the behavioral model first",
    )
    # what we built is on by default; --no-wrath, --no-apply-threats, --no-apply-day turn it off
    p.add_argument(
        "--wrath",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="npc's take offense and answer insolence with force (docs/xardas-wrath.md)",
    )
    p.add_argument(
        "--apply-threats",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="carry out the behavioral model's fights and flights (off: traced only, in shadow)",
    )
    p.add_argument(
        "--beasts",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="wolves hear the tone of the hero's words: they let him pass, run or bite (docs/beasts.md)",
    )
    p.add_argument(
        "--chests",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="npc's know what lies in their chests and go to take it out (docs/npc-tools.md «Сундуки»)",
    )
    p.add_argument(
        "--lead",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="npc's lead the hero to a place, a term of a deal (docs/npc-tools.md, wave 2 step 4)",
    )
    p.add_argument(
        "--meet",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="npc's set the hero meetings, a term of a deal, and come there by the hour (docs/npc-tools.md, "
        "wave 2 step 4; needs --lead)",
    )
    p.add_argument(
        "--errands",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="npc's set themselves errands: go and tell someone of the hero, take someone a thing, now or at "
        "an hour (docs/npc-tools.md, wave 2 step 5; needs --lead)",
    )
    p.add_argument(
        "--interject",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="who speaks up in the conversation after the one the hero spoke to, by character: the "
        "behavioral model's `interject` (docs/npc-tools.md, step B3)",
    )
    p.add_argument(
        "--free-bandits",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="the bandits below Xardas's tower without their scripted ambush: our models decide (story.py)",
    )
    p.add_argument(
        "--apply-day",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="carry out the behavioral model's day plans from the new menu (off: `do` traced in shadow, "
        "the old `plan` applied)",
    )
    _add_voice_args(p)
    _add_llm_args(p)

    p = sub.add_parser("cards", help="generate character cards; wait for the game to connect")
    p.add_argument("--only", help="only these NPC symbols, comma separated; rewrite their existing cards")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=47817)
    p.add_argument(
        "--story-graph", help="storygraph JSON (default: built from the game's scripts and cached)"
    )
    _add_llm_args(p)

    p = sub.add_parser("llm-test", help="send one message to the LLM and measure latency")
    p.add_argument("text", nargs="?", default="")
    p.add_argument("--name", default="Ксардас", help="npc display name for the persona")
    p.add_argument("--tools", action="store_true", help="offer the npc action tools")
    p.add_argument("--situation", help="situation note sent before the message")
    p.add_argument("--json", action="store_true", help="one JSON object on stdout instead of the stream")
    p.add_argument(
        "--probe-tools",
        action="store_true",
        help="check that a turn goes on after a tool result (reasoning carried, tool_choice none, cache)",
    )
    _add_llm_args(p)

    p = sub.add_parser("replay", help="run a recorded npc turn again, with another model or prompt")
    p.add_argument("file", nargs="?", help="trace file (default: the newest in --trace-dir)")
    p.add_argument("--turn", help="turn id or its prefix (default: the last turn in the file)")
    p.add_argument("--trace-dir", default=str(DEFAULT_TRACE_DIR))
    p.add_argument("--name", default="Ксардас", help="npc display name for --persona-now")
    p.add_argument("--persona-now", action="store_true", help="today's persona instead of the recorded one")
    p.add_argument(
        "--after-step",
        type=int,
        default=0,
        help="continue after this many recorded steps, keeping their results",
    )
    p.add_argument("--json", action="store_true", help="one JSON object on stdout")
    _add_llm_args(p)

    p = sub.add_parser("replay-first-words", help="replay quick first reactions to recorded player phrases")
    p.add_argument("file", help="recorded traces.jsonl")
    p.add_argument("--hint-file", help="use this file instead of the current first-reaction hint")
    p.add_argument("--runs", type=int, default=1, help="independent runs per turn (default: 1)")
    p.add_argument("--temperature", type=float, default=0.6)
    p.add_argument("--quick-url", default=os.environ.get("GOTHIC_QUICK_LLM_URL"))
    p.add_argument("--npc", help="only this npc id")
    p.add_argument("--out", help="write turn results to a JSONL file")

    p = sub.add_parser("replay-svm-lines", help="compare SVM replacements in recorded NPC situations")
    p.add_argument("file", help="recorded traces.jsonl")
    p.add_argument("--svm-file", required=True, help="JSONL native SVM lines: svm, text, target id")
    p.add_argument("--hint-file", help="replacement SVM hint template with {event}, {text} and {to}")
    p.add_argument("--history", action="store_true", help="include the recorded NPC history for comparison")
    p.add_argument("--runs", type=int, default=1, help="independent runs per native line (default: 1)")
    p.add_argument("--quick-url", default=os.environ.get("GOTHIC_QUICK_LLM_URL"))
    p.add_argument("--npc", help="only this npc id")
    p.add_argument("--out", help="write comparison results to a JSONL file")

    p = sub.add_parser(
        "places", help="the book of places: group a world dump (SCENARIO=places) and name them"
    )
    p.add_argument("dump", help="world_points.json from SCENARIO=places")
    p.add_argument("--out", default=str(places.BOOK), help="the book to write")
    p.add_argument("--area", action="append", help="only these parts of the world (FARM1, CITY...)")
    p.add_argument("--dry-run", action="store_true", help="only show the groups, no model calls")
    p.add_argument("--names", help="keep the names of this book (read through already), name only the rest")
    _add_llm_args(p)

    p = sub.add_parser("lore", help="who and what is where, as each npc knows it from its dialogs (lore.py)")
    p.add_argument("dump", help="world_points.json from SCENARIO=places")
    p.add_argument("--out", default=str(lore.LORE), help="the book of facts to write")
    p.add_argument("--who", action="append", help="only these npc's, afresh")
    p.add_argument("--story-graph", help="storygraph JSON (default: made from the game's scripts and cached)")
    _add_llm_args(p)

    p = sub.add_parser("login", help="sign in to a provider")
    p.add_argument("provider", choices=["chatgpt"])

    p = sub.add_parser("logout", help="forget stored credentials")
    p.add_argument("provider", choices=["chatgpt"])

    p = sub.add_parser("voice-test", help="record from the microphone and print what ASR heard")
    p.add_argument("--seconds", type=float, default=4.0)
    p.add_argument("--json", action="store_true", help="JSON lines: levels, partials, the result")
    _add_voice_args(p)

    sub.add_parser("mics", help="input devices as JSON")

    p = sub.add_parser("speech-test", help="say a line in an npc's voice through the TTS service, aloud")
    p.add_argument("--text", default="Я ждал тебя. Нам нужно поговорить.")
    p.add_argument("--npc", default="NONE_100_XARDAS")
    p.add_argument("--name", default="Ксардас", help="npc display name for the persona")
    p.add_argument("--mood", choices=MOODS, help="say it in this mood (as a line with the model's tag)")
    p.add_argument("--tts-url", default=os.environ.get("GOTHIC_TTS_URL", DEFAULT_TTS_URL))
    p.add_argument("--game-dir", default=os.environ.get("GOTHIC_DIR", DEFAULT_GAME_DIR))
    p.add_argument("--json", action="store_true", help="one JSON object on stdout")

    p = sub.add_parser("memory", help="list save branches, or show what an npc remembers in one")
    p.add_argument("branch", nargs="?")
    p.add_argument("--npc", default="NONE_100_XARDAS")
    p.add_argument("--memory-db", default=str(DEFAULT_MEMORY_DB))

    p = sub.add_parser("lines", help="print an npc's original dialog lines")
    p.add_argument("npc")
    p.add_argument("--game-dir", default=os.environ.get("GOTHIC_DIR", DEFAULT_GAME_DIR))

    p = sub.add_parser("voices", help="the TTS voices of npc's as JSON: their ids and descriptions")
    p.add_argument("npc", nargs="+")
    p.add_argument("--game-dir", default=os.environ.get("GOTHIC_DIR", DEFAULT_GAME_DIR))

    args = parser.parse_args()
    scenario = getattr(args, "scenario", None)
    if getattr(args, "llm", None) == "auto":
        if scenario is None:
            parser.error("--llm auto requires --scenario")
        if scenario in OFF | GIVEN:
            args.llm = "off"
        elif scenario not in LIVE:
            parser.error(f"scenario {scenario} has no model group (docs/e2e.md)")
    if scenario in LIVE and args.llm in {"auto", "off"}:
        parser.error(f"scenario {scenario} is live: set LLM=chatgpt|openai|local")
    if args.command == "cards" and args.llm == "off":
        parser.error("cards requires --llm chatgpt|openai|local")
    if scenario in OFF | GIVEN and args.llm != "off":
        parser.error(f"scenario {scenario} requires --llm off")
    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname).1s %(name)s: %(message)s",
        datefmt="%H:%M:%S",
    )
    for noisy in ("httpx", "httpx2", "httpcore", "openai", "websockets"):
        logging.getLogger(noisy).setLevel(logging.WARNING)

    try:
        match args.command:
            case "serve":
                code = asyncio.run(serve(args))
            case "llm-test":
                code = asyncio.run(llm_test(args))
            case "replay":
                code = asyncio.run(replay(args))
            case "replay-first-words":
                code = asyncio.run(replay_first_words(args))
            case "replay-svm-lines":
                code = asyncio.run(replay_svm_lines(args))
            case "places":
                code = asyncio.run(places_cmd(args))
            case "lore":
                code = asyncio.run(lore_cmd(args))
            case "cards":
                code = asyncio.run(cards_cmd(args))
            case "login":
                code = asyncio.run(login_chatgpt())
            case "logout":
                TokenStore().delete()
                print("ChatGPT credentials removed")
                code = 0
            case "lines":
                code = lines(args)
            case "voices":
                code = voices(args)
            case "voice-test":
                code = asyncio.run(voice_test(args))
            case "mics":
                code = mics(args)
            case "speech-test":
                code = asyncio.run(speech_test(args))
            case "memory":
                code = show_memory(args)
            case _:
                code = 2
    except LLMError as err:
        print(f"error: {err}", file=sys.stderr)
        code = 1
    except KeyboardInterrupt:
        code = 130
    sys.exit(code)
