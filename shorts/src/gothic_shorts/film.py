"""A take: the shot played on the test bench with real AI services and separate effects and npc voices.

The services are the launcher's (its settings and keys, gothic-launcher config), on free ports so a play
session can run alongside; the AI server takes the hero's recordings instead of the microphone
(--voice-files) and keeps its memory in the take. The game runs the bench in film mode: a window of the
shot's size, the sound mixed into the recording instead of the speakers (TEMPEST_AUDIO_LOOPBACK), the
fixed time step kept to the wall clock (OPENGOTHIC_BENCH_REALTIME).
Game music is off: the cut adds one music bed for the whole short.

A take folder (.shorts/<shot>/take-NN/):
    scenario.bench   what the bench ran
    main.mp4 main.wav  the recorded segment (one per `rec`), video and the game's effects
    main.voice-NN.wav  each AI npc line, separate from the effects
    events.jsonl     what was said when, in frames of the recording (cut.py)
    journal.jsonl    the AI server's journal: player and npc lines, actions
    bench.log, logs/ the bench, the services and the game
"""

from __future__ import annotations

import asyncio
import json
import shutil
import subprocess
import time
from pathlib import Path

from gothic_launcher.commands import Layout, Ports, Proc, services, texture_env
from gothic_launcher.settings import Config
from gothic_launcher.state import STATE_DIR, SessionFile
from gothic_launcher.supervisor import Event, Supervisor

from .shot import Shot
from .voice import voice_lines

STARTUP_S = 240  # services (a first `uv run` installs), the game's start and its shader cache


class FilmError(RuntimeError):
    pass


def shorts_dir(repo: Path) -> Path:
    return repo / ".shorts"


def default_save(repo: Path) -> Path:
    """save_slot_1.sav of the main checkout (worktrees share it), as scripts/bench.sh."""
    common = subprocess.run(
        ["git", "-C", str(repo), "rev-parse", "--path-format=absolute", "--git-common-dir"],
        capture_output=True,
        text=True,
        check=True,
    ).stdout.strip()
    return Path(common).parent / "save_slot_1.sav"


def new_take(root: Path) -> Path:
    root.mkdir(parents=True, exist_ok=True)
    n = 1 + max((int(p.name[5:]) for p in root.glob("take-*") if p.name[5:].isdigit()), default=0)
    take = root / f"take-{n:02d}"
    take.mkdir()
    return take


def film(
    shot: Shot,
    config: Config,
    layout: Layout,
    *,
    decider_url: str | None = None,
    verbose: bool = False,
) -> Path:
    if not config.settings.voice.enabled:
        raise FilmError("a short needs speech recognition: gothic-launcher config set voice.enabled true")
    root = shorts_dir(layout.repo)
    voiced = voice_lines(config, root / "voice", shot.hero_voice, shot.hero_lines)
    save = shot.save or default_save(layout.repo)
    if not save.is_file():
        raise FilmError(f"no save {save} (set save = ... in the shot)")

    take = new_take(root / shot.name)
    scenario = take / "scenario.bench"
    scenario.write_text(shot.scenario(voiced))
    run = take / "run"  # the game's working directory: its settings and the save
    run.mkdir()
    shutil.copy(save, run / "save_slot_1.sav")
    (run / "Gothic.ini").write_text(
        # no music and no looping sounds of the map's places (zCVobSound): the cut lays one bed
        f"[INTERNAL]\nvidResIndex=0\n[SOUND]\nsoundVolume={shot.sound:g}\nmusicVolume=0\nmusicEnabled=0\n"
        "worldSounds=0\n"
    )
    logs = take / "logs"
    logs.mkdir()

    ports = Ports.free()
    procs = services(config, layout, ports, cwd=logs)
    for proc in procs:
        if proc.name == "tts" and "--no-original-voices" not in proc.argv:
            proc.argv.append("--no-original-voices")
    procs[-1] = _server(procs[-1], take, decider_url, shot.server_args)
    game = Proc(
        "game",
        [str(layout.game_bin), "-g", str(layout.game_dir), "-window", "-devmode", "-save", "1"],
        run,
        {
            "GOTHIC_AI_URL": f"ws://127.0.0.1:{ports.server}",
            "TEMPEST_BACKGROUND": "1",
            "TEMPEST_WINDOW_SIZE": shot.size,
            "TEMPEST_AUDIO_LOOPBACK": "48000",
            "OPENGOTHIC_BENCH": str(scenario),
            "OPENGOTHIC_BENCH_OUT": str(take),
            "OPENGOTHIC_BENCH_REALTIME": "1",
            "OPENGOTHIC_BENCH_VOICE_STEM": "1",
            "OPENGOTHIC_BENCH_ENCODER": "-c:v h264_videotoolbox -b:v 24M",  # x264 slows the game down
            **texture_env(layout),
        },
        restart=False,
        stamp=True,
    )
    journal = (take / "journal.jsonl").open("w")
    started = time.monotonic()

    def on_event(e: Event) -> None:
        if e.kind == "journal" and e.data is not None:
            journal.write(json.dumps(e.data, ensure_ascii=False) + "\n")
            journal.flush()
            if e.data.get("kind") in ("player", "npc", "action", "llm_error", "voice_error", "tts_off"):
                print(f"  {_journal_line(e.data)}")
        elif e.kind in ("ready", "failed", "exited", "restarting"):
            print(f"[{time.monotonic() - started:5.0f}s] {e.proc} {e.kind} {e.detail}".rstrip())

    sup = Supervisor(
        procs,
        game,
        logs,
        on_event,
        session=SessionFile(STATE_DIR / "shorts.json"),
        echo=(lambda name, line: print(f"{name}: {line}")) if verbose else None,
    )
    print(f"take {take}")
    try:
        code = asyncio.run(_run(sup, STARTUP_S + shot.timeout))
    finally:
        journal.close()
    if code not in (0, None):
        print(f"the game exited with code {code}")
    return take


def _server(proc: Proc, take: Path, decider_url: str | None, extra: tuple[str, ...] = ()) -> Proc:
    argv = [
        *proc.argv,
        "--voice-files",
        "--memory-db", str(take / "memory.sqlite3"),
        "--trace-dir", str(take / "traces"),
        *extra,
    ]  # fmt: skip
    if decider_url:
        argv += ["--decider-url", decider_url]
    return Proc(proc.name, argv, proc.cwd, proc.env, proc.health, proc.restart, proc.stamp)


async def _run(sup: Supervisor, timeout: float) -> int:
    task = asyncio.create_task(sup.run())
    try:
        return await asyncio.wait_for(asyncio.shield(task), timeout)
    except TimeoutError:
        print(f"timeout after {timeout:.0f}s: stopping")
        sup.stop()
        return await task


def _journal_line(d: dict) -> str:
    match d.get("kind"):
        case "player":
            return f"hero -> {d.get('name') or '?'}: {d.get('text')}"
        case "npc":
            return f"{d.get('name')}: {d.get('text')}"
        case "action":
            return f"{d.get('name')} does {d.get('action')} {d.get('arguments') or ''}".rstrip()
        case _:
            return f"{d.get('kind')}: {d.get('detail')}"
