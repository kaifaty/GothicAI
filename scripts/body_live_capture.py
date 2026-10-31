#!/usr/bin/env python3
"""Film a live P30 fight on macOS, using the production GameLink and BodyPilot.

Run with the server environment: uv run --project server python scripts/body_live_capture.py
live-takeover --output /absolute/footage/live-takeover. No LLM, ASR or TTS client is created.
The existing body_url is read from launcher settings; the remote policy is never modified.
"""
from __future__ import annotations

import argparse
import asyncio
import contextlib
import json
import logging
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tomllib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "server" / "src"))
sys.path.insert(0, str(ROOT / "scripts"))

SHOTS = ("live-takeover", "live-group", "live-passive")
ARENA = "NW_BIGFARM_VORPOSTEN1_01_RIGHTFIELD_02"
LOG = logging.getLogger("body_live_capture")


def actors(shot: str) -> list[str]:
    if shot not in SHOTS:
        raise ValueError(f"unknown shot: {shot}")
    return [f"DIR_NPC_LIVE_BANDIT_{i}" for i in range(1, 4 if shot == "live-group" else 2)]


def scene(shot: str, output: Path, duration: int) -> str:
    """Recording starts before native ZS_ATTACK; all framing/HP setup happens first."""
    if not 20 <= duration <= 30:
        raise ValueError("duration must be 20..30 seconds")
    npcs = actors(shot)
    lines = [
        "set time 12 0", f"goto {ARENA}", "wait 500", "clear 3000", "release all",
        "overlay off", "bodylive on", f"bodylive wait {output / 'ready'} 120000",
        "stats hpmax 100000 hp 100000", "immortal player on",
        *[f"stats npc {npc} hpmax 1800 hp 1800" for npc in npcs],
        "cam cut", f"cam two {npcs[0]} {850 if len(npcs) == 3 else 650} 170 1",
        "cam smooth 1", "view full", f"track {','.join(npcs)} head", "wait 300",
        "rec shot", *[f"fight {npc} player" for npc in npcs],
    ]
    if shot == "live-passive":
        lines += [f"wait {duration * 1000}"]
    else:
        # Actual player input, not debug damage or a scripted opponent. Short gaps
        # let the hero finish his clips and P30 observe both windups and idle poses.
        remaining = duration * 1000
        while remaining:
            hold = min(600, remaining)
            lines += [f"hold action {hold}", f"wait {hold}"]
            remaining -= hold
            if remaining:
                gap = min(700, remaining)
                lines += [f"wait {gap}"]
                remaining -= gap
    lines += ["release all", "stop", "wait 3000", "bodylive off"]
    return "\n".join(lines) + "\n"


def game_guard() -> None:
    query_env = {**os.environ, "LC_ALL": "C"}
    result = subprocess.run(["pgrep", "-fl", "Gothic2Notr"], capture_output=True, text=True, env=query_env)
    print(result.stdout.strip() or "no game", flush=True)
    if result.returncode not in (0, 1):
        raise RuntimeError(f"pgrep failed: {result.returncode}")
    # The required broad inventory also matches cmake/ninja --target Gothic2Notr.
    # Check the process name before interpreting a build as a running game.
    exact = subprocess.run(["pgrep", "-x", "Gothic2Notr"], capture_output=True, text=True, env=query_env)
    if exact.returncode == 0:
        raise RuntimeError("another Gothic2Notr is running on this Mac")
    if exact.returncode != 1:
        raise RuntimeError(f"pgrep failed: {exact.returncode}")


def read_events(path: Path) -> list[dict]:
    if not path.exists():
        return []
    # event() flushes each line, but tolerate a concurrent incomplete final write.
    return [json.loads(line) for line in path.read_text().splitlines() if line.endswith("}")]


def validate_recording(folder: Path, duration: int) -> None:
    """A finished session (including cancellation) is not proof of a finished take."""
    events = read_events(folder / "events.jsonl")
    starts = [i for i, event in enumerate(events) if event.get("kind") == "rec" and event.get("seg") == "shot"]
    stops = [i for i, event in enumerate(events) if event.get("kind") == "rec_stop" and event.get("seg") == "shot"]
    if len(starts) != 1 or len(stops) != 1 or starts[0] >= stops[0]:
        raise RuntimeError("incomplete recording: expected one rec/rec_stop pair for shot")
    if events[starts[0]].get("fps") != 30 or events[stops[0]].get("frames") != duration * 30:
        raise RuntimeError(f"incomplete recording: expected {duration * 30} frames at 30 fps")
    for name in ("shot.mp4", "shot.wav"):
        path = folder / name
        if not path.is_file() or path.stat().st_size == 0:
            raise RuntimeError(f"incomplete recording: missing or empty {name}")


async def prepare(link, shot: str) -> None:
    from gothic_ai.bodies import Bodies
    from gothic_ai.scenarios.common import _m5b_player_weapon

    await link.wait_world()
    # Let the bench clear the field and reach its readiness gate first.
    await asyncio.sleep(1)
    await link.debug("player_goto", waypoint="NW_XARDAS_TOWER_VALLEY_01")
    for index, npc in enumerate(actors(shot)):
        await link.own_npc(npc, like="BDT_1002_BANDIT_L", waypoint=ARENA)
        await link.debug("skip_important", near=npc)
        ready = await Bodies(link).act(npc, "start_activity", state="ZS_STAND_ARMSCROSSED",
                                       waypoint=ARENA, minutes=60, arrive=True, radius=100, timeout=15)
        if ready.status != "arrived":
            raise RuntimeError(f"preparation {npc}: {ready.status} {ready.detail}")
        weapon = ["call", "NPC_GETEQUIPPEDMELEEWEAPON", ["inst", "SELF"]]
        sword, name = await link.story_eval(npc, [
            ["&", ["field", weapon, "FLAGS"], ["var", "ITEM_SWD"]], ["field", weapon, "NAME"],
        ])
        if not sword:
            raise RuntimeError(f"preparation {npc}: not a one-handed sword ({name})")
        draw = await Bodies(link).act(npc, "draw_weapon", kind="melee")
        if draw.status != "done":
            raise RuntimeError(f"preparation {npc}: draw {draw.status}")
        # Park each prepared clone separately, leaving the waypoint clear.
        await link.debug("npc_goto", npc=npc, to=npc, distance=700 + index * 300)
        LOG.info("prepared npc=%s sword=%s", npc, name)
    await link.debug("player_equip", item="ITMW_1H_BAU_MACE")
    await _m5b_player_weapon(link, "melee")
    npcs = actors(shot)
    await link.debug("npc_goto", npc=npcs[0], waypoint=ARENA)
    for index, npc in enumerate(npcs[1:], 1):
        await link.debug("npc_goto", npc=npc, to=npcs[0], distance=-230 * index)
    await link.debug("teleport", near=npcs[0], distance=450)
    for npc in npcs:
        await link.debug("npc_goto", npc=npc, to=npc, distance=0, face="PC_HERO")


async def capture(args) -> int:
    from body_replay_film import macos_window_size
    from gothic_ai.bodypilot import BodyPilot
    from gothic_ai.game import GameServer

    if sys.platform != "darwin":
        raise RuntimeError("live capture with game audio requires macOS")
    if any(os.environ.get(name, "fake") != "fake" for name in ("LLM", "ASR", "TTS")):
        raise RuntimeError("LLM, ASR and TTS must be fake")
    body_url = tomllib.loads(args.settings.read_text()).get("llm", {}).get("body_url")
    if not body_url:
        raise RuntimeError("settings contain no body_url")
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    if any(out.iterdir()):
        raise RuntimeError("output already contains a take; use a fresh directory")
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s %(levelname)s %(message)s",
                        handlers=[logging.FileHandler(out / "server.log"), logging.StreamHandler()])
    (out / "scene.bench").write_text(scene(args.shot, out, args.duration))
    (out / "ready").unlink(missing_ok=True)
    run = out / "run"
    run.mkdir()
    shutil.copy(args.save, run / "save_slot_1.sav")
    (run / "Gothic.ini").write_text("[INTERNAL]\nvidResIndex=0\n[GAME]\nUSEGOTHIC1CONTROLS=0\n[SOUND]\n"
                                   "soundVolume=1\nmusicVolume=0\nmusicEnabled=0\nworldSounds=0\n")
    subprocess.run(["bash", str(Path(__file__).with_name("base-ensure.sh")), str(args.assets)],
                   env=dict(os.environ, GAME_BIN=str(args.game.resolve())), check=True)
    session_done = asyncio.Event()
    errors: list[Exception] = []

    async def session(link):
        pilot = task = None
        try:
            await prepare(link, args.shot)
            (out / "ready").touch()
            # Preparation and native fight commands must finish before the pilot
            # can lease anyone: a later fixture fight would revoke that lease.
            while not link.closed.is_set():
                events = read_events(out / "events.jsonl")
                if any(e.get("kind") == "rec" and e.get("seg") == "shot" for e in events):
                    break
                await asyncio.sleep(0.05)
            if link.closed.is_set():
                raise RuntimeError("game disconnected before recording")
            pilot = BodyPilot(link, body_url)
            task = asyncio.create_task(pilot.run())
            while not link.closed.is_set():
                events = read_events(out / "events.jsonl")
                if any(e.get("kind") == "rec_stop" and e.get("seg") == "shot" for e in events):
                    break
                await asyncio.sleep(0.1)
        except Exception as error:
            LOG.exception("capture session failed")
            errors.append(error)
        finally:
            try:
                if pilot is not None:
                    await pilot.aclose()
                    await task
            except Exception as error:
                LOG.exception("capture cleanup failed")
                errors.append(error)
            finally:
                session_done.set()

    server = GameServer(port=0, on_link=session)
    service = asyncio.create_task(server.serve_forever())
    game = None
    try:
        await server.started.wait()
        env = {**os.environ, "LLM": "fake", "ASR": "fake", "TTS": "fake",
               "GOTHIC_AI_URL": f"ws://127.0.0.1:{server.port}", "GOTHIC_AI": "1",
               "TEMPEST_BACKGROUND": "1", "TEMPEST_WINDOW_SIZE": macos_window_size("1920x1080"),
               "TEMPEST_AUDIO_LOOPBACK": "48000", "OPENGOTHIC_BENCH_REALTIME": "1",
               "OPENGOTHIC_BENCH_ENCODER": "-c:v h264_videotoolbox -b:v 24M",
               "OPENGOTHIC_BENCH": str(out / "scene.bench"), "OPENGOTHIC_BENCH_OUT": str(out),
               "OPENGOTHIC_PHYSICS_THREADS": "0"}
        textures = Path.home() / ".local/share/gothic-ai/hd-textures/textures"
        if textures.is_dir():
            env.setdefault("OPENGOTHIC_TEXTURES", str(textures))
        for name in ("ALSOFT_DRIVERS", "VK_DRIVER_FILES", "VK_ICD_FILENAMES"):
            env.pop(name, None)
        # Required immediately before every game launch, even after server setup.
        game_guard()
        with (out / "game.log").open("w") as log:
            game = await asyncio.create_subprocess_exec(str(args.game.resolve()), "-g", str(args.assets),
                "-g2", "-window", "-devmode", "-save", "1", cwd=run, env=env,
                stdout=log, stderr=asyncio.subprocess.STDOUT)
            await asyncio.wait_for(game.wait(), timeout=180)
        await asyncio.wait_for(session_done.wait(), timeout=5)
        if errors:
            raise errors[0]
        if game.returncode:
            raise RuntimeError(f"game exit {game.returncode}")
        validate_recording(out, args.duration)
        LOG.info("capture complete: %s", out)
        return 0
    finally:
        if game is not None and game.returncode is None:
            game.terminate()
            await game.wait()
        service.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await service


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("shot", choices=SHOTS)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--duration", type=int, default=25)
    parser.add_argument("--settings", type=Path, default=Path.home() / ".config/gothic-ai/settings.toml")
    parser.add_argument("--game", type=Path, default=ROOT / "OpenGothic/build/opengothic/Gothic2Notr")
    parser.add_argument("--assets", type=Path, default=Path("/Volumes/DevSSD/Dev/games/Gothic II"))
    common = subprocess.check_output(["git", "rev-parse", "--path-format=absolute", "--git-common-dir"],
                                     cwd=ROOT, text=True).strip()
    parser.add_argument("--save", type=Path, default=Path(common).parent / "save_slot_1.sav")
    args = parser.parse_args()
    return asyncio.run(capture(args))


if __name__ == "__main__":
    raise SystemExit(main())
