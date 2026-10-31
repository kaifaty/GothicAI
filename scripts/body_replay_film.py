#!/usr/bin/env python3
"""Stage 9: film a policy's arena duels in the rendered game.

The headless runner has no picture. A lockstep episode recorded with the RL collector
(--keep-journals) is replayed in Gothic2Notr by the bench command `bodyreplay`
(OpenGothic/common/game/bodyreplay.h): the same arena reset at the same scenario line, the
primary's accepted commands issued after the same number of steps. The rendered run writes the
runner's journals, so scripts/body_schedule_equivalence.py checks the film against the
recording before anyone looks at it.

  prepare  NAME=EPISODE_DIR ... --output DIR   replay files and bench scenes
  render   DIR/NAME ... --game Gothic2Notr     macOS: Metal + game audio; Linux: Xvfb + lavapipe
  labels   DIR [--output actions.json]         head pixels, HP and started policy actions
  compose  LEFT.mp4 RIGHT.mp4 OUT.mp4          side by side, labels and outcomes

Replay file: {"arena": {scenario, seed, side, distance_cm, timeout_ms[, geometry, ...]}, "provider",
"commands": [{step, command_id, action_id, goal_version, target_entity_id, observation_seq,
ttl_ms[, actor: "opponent"]}]}, where step counts the native steps after the reset sample (arena_start
is step 0). A self-play duel (arena opponent_owned) also replays the opponent's commands.
"""
from __future__ import annotations

import argparse
from bisect import bisect_left, bisect_right
import csv
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT/"body-service"/"src"))
from gothic_body.schema import ACTIONS  # noqa: E402; the shared schema imports no models

SCENARIO = ROOT/"scripts"/"bench"/"body-arena.bench"
ARENA_KEYS = ("scenario", "seed", "side", "distance_cm", "timeout_ms")
# A custom arena reset (near_wall, offsets), as the runner reports it under "setup".
SETUP_KEYS = ("geometry", "wall_clearance_cm", "wall_tangent_sign", "yaw_offset_deg", "opponent_yaw_offset_deg",
              "separation_yaw_deg", "center_offset_cm")
COMMAND_KEYS = ("command_id", "action_id", "goal_version", "target_entity_id", "observation_seq", "ttl_ms")
FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"


def rows(path: Path) -> list[dict]:
    return [json.loads(line) for line in path.read_text().splitlines() if line]


def replay_file(episode: Path) -> dict:
    arena = rows(episode/"arena-events.jsonl")
    initial = next(row["arena"] for row in arena if row.get("kind") == "reset_ready")
    terminal = next((row["arena"] for row in arena if row.get("kind") == "terminal"), None)
    primary, opponent = initial["primary_entity_id"], initial.get("opponent_entity_id")
    owned = initial.get("opponent_owned") is True
    # The reset sample is the first observation row: its step is the arena's step zero.
    start = rows(episode/"observations.jsonl")[0]["sim_step"]
    # In a self-play duel the opponent's commands too, in the order the runner accepted them.
    actors = {primary: "primary", **({opponent: "opponent"} if owned else {})}
    accepted = [row for row in rows(episode/"body-control-events.jsonl")
                if row.get("entity_id") in actors and row.get("phase") == "accepted"]
    if initial["scenario"] != "native_duel" or not accepted:
        raise SystemExit(f"{episode}: not a policy-driven native duel")
    return {"schema_version": 1, "source": str(episode), "provider": accepted[0]["provider"],
            "arena": {**{key: initial[key] for key in ARENA_KEYS}, **{key: value for key, value in (initial.get("setup") or {}).items()
                                                                  if key in SETUP_KEYS}, **({"opponent_owned": True} if owned else {})},
            "outcome": {key: terminal.get(key) for key in ("outcome", "reason", "elapsed_game_ms")} if terminal else None,
            "commands": [{"step": row["sim_step"]-start, **{key: row[key] for key in COMMAND_KEYS},
                          **({"actor": "opponent"} if actors[row["entity_id"]] == "opponent" else {})} for row in accepted]}


def primary_symbol(episode: Path) -> str:
    initial = next(row["arena"] for row in rows(episode/"arena-events.jsonl") if row.get("kind") == "reset_ready")
    actor = next((actor for actor in initial.get("actors", []) if actor["entity_id"] == initial["primary_entity_id"]), None)
    if actor is None or not actor.get("symbol"):
        raise ValueError(f"{episode}: reset has no primary script symbol for head tracking")
    return actor["symbol"]


def scene(scenario: Path, replay: Path, *, camera: str, tail_ms: int, track: str | None = None) -> str:
    """The arena scenario up to its `bodyarena` line, which the replay takes over."""
    lines = scenario.read_text().splitlines()
    cut = next(index for index, line in enumerate(lines) if line.split()[:1] == ["bodyarena"])
    return "\n".join(lines[:cut]+[
        "overlay off",
        f"bodyreplay {replay} {tail_ms}",
        f"cam {camera}",
        *([f"track {track} head"] if track else []),
        "rec duel",
        "wait 600100",
        "stop",
    ])+"\n"


def prepare(args) -> int:
    for item in args.episodes:
        name, _, folder = item.partition("=")
        out = (args.output/name).resolve()
        out.mkdir(parents=True, exist_ok=True)
        replay = replay_file(Path(folder))
        (out/"replay.json").write_text(json.dumps(replay, indent=1)+"\n")
        (out/"scene.bench").write_text(scene(args.scenario, out/"replay.json", camera=args.camera, tail_ms=args.tail_ms,
                                          track=primary_symbol(Path(folder)) if args.track_head else None))
        print(f"{name}: seed {replay['arena']['seed']} side {replay['arena']['side']} "
              f"distance {replay['arena']['distance_cm']}: {len(replay['commands'])} commands, {replay['outcome']}")
    return 0


def macos_window_size(size: str) -> str:
    """Tempest's macOS window uses points; --size specifies the captured pixels."""
    scale = float(subprocess.check_output([
        "osascript", "-l", "JavaScript", "-e",
        'ObjC.import("AppKit"); $.NSScreen.mainScreen.backingScaleFactor',
    ], text=True).strip())
    width, height = (int(value) for value in size.split("x"))
    if scale <= 0 or any(value <= 0 or not (value/scale).is_integer() for value in (width, height)):
        raise SystemExit(f"--size {size} cannot use whole window points at display scale {scale:g}")
    return f"{int(width/scale)}x{int(height/scale)}"


def render(args) -> int:
    subprocess.run(["bash", str(Path(__file__).with_name("base-ensure.sh")), str(args.assets.resolve())],
                   env=dict(os.environ, GAME_BIN=str(args.game.resolve())), check=True)
    failures = 0
    macos = sys.platform == "darwin"
    window_size = macos_window_size(args.size) if macos else args.size
    for folder in args.scenes:
        out = folder.resolve()
        run = out/"run"
        shutil.rmtree(run, ignore_errors=True)
        run.mkdir()
        shutil.copy(args.save, run/"save_slot_1.sav")
        width, height = args.size.split("x")
        env = {**os.environ, "GOTHIC_AI": "0", "TEMPEST_BACKGROUND": "1",
               "TEMPEST_WINDOW_SIZE": window_size, "OPENGOTHIC_TEXTURES": args.textures,
               "OPENGOTHIC_BODY_TRACE": "1", "OPENGOTHIC_BODY_ANIMATION_RNG": "isolated",
               "OPENGOTHIC_BODY_SCRIPT_RNG": "isolated", "OPENGOTHIC_BENCH_REALTIME": "1",
               "OPENGOTHIC_PHYSICS_THREADS": "0",
               "OPENGOTHIC_BENCH": str(out/"scene.bench"), "OPENGOTHIC_BENCH_OUT": str(out)}
        for key in ("DISPLAY", "WAYLAND_DISPLAY"):
            env.pop(key, None)
        argv = [str(args.game.resolve()), "-g", str(args.assets.resolve()), "-g2", "-window", "-devmode", "-save", "1"]
        if macos:
            # The macOS engine selects Metal itself. Do not inherit Linux's software renderer
            # or null audio device. Full resolution prevents each realtime screenshot from
            # scaling the previous screenshot's already reduced viewport a second time.
            for key in ("VK_DRIVER_FILES", "VK_ICD_FILENAMES", "LP_NUM_THREADS", "ALSOFT_DRIVERS"):
                env.pop(key, None)
            env["TEMPEST_AUDIO_LOOPBACK"] = "48000"
            env.setdefault("OPENGOTHIC_BENCH_ENCODER", "-c:v h264_videotoolbox -b:v 24M")
            (run/"Gothic.ini").write_text("[INTERNAL]\nvidResIndex=0\n[SOUND]\n"
                                         "soundVolume=1\nmusicVolume=0\nmusicEnabled=0\nworldSounds=0\n")
        else:
            env.update({"VK_DRIVER_FILES": "/usr/share/vulkan/icd.d/lvp_icd.json",
                        "VK_ICD_FILENAMES": "/usr/share/vulkan/icd.d/lvp_icd.json", "LP_NUM_THREADS": str(args.threads),
                        "ALSOFT_DRIVERS": "null"})
            argv = ["nice", "-n", "10", "xvfb-run", "-a", "-s", f"-screen 0 {width}x{height}x24", *argv]
        # A failed rerun must not pass using a previous take's video, audio or replay result.
        for name in ("duel.mp4", "duel.wav", "replay.jsonl"):
            (out/name).unlink(missing_ok=True)
        with open(out/"launcher.log", "w") as log:
            code = subprocess.run(argv, cwd=run, env=env, stdout=log, stderr=subprocess.STDOUT,
                                  timeout=args.timeout_s).returncode
        log_rows = rows(out/"replay.jsonl") if (out/"replay.jsonl").exists() else []
        end = next((row for row in log_rows if row["kind"] in ("replay_end", "replay_error")), None)
        video, audio = (out/"duel.mp4").exists(), (out/"duel.wav").exists()
        ok = code == 0 and end is not None and end["kind"] == "replay_end" and end["rejected"] == 0 and video and (audio or not macos)
        failures += not ok
        summary = {key: end.get(key) for key in ("kind", "accepted", "rejected", "observation_mismatches", "reason")} if end else None
        print(f"{out.name}: exit {code}, {summary}, video {video}, audio {audio}", flush=True)
    return 1 if failures else 0


def frame_clock(folder: Path, segment: str = "duel") -> tuple[list[dict], list[int], int]:
    """Captured-frame game clocks, anchored by the arena setup and checked against world ticks.

    Combat events are emitted during the world tick, before the bench advances its own `t`.
    Their frame and t_game are authoritative; their `t` is therefore not a video timestamp.
    """
    events = rows(folder/"events.jsonl")
    starts = [row for row in events if row.get("kind") == "rec" and row.get("seg") == segment]
    stops = [row for row in events if row.get("kind") == "rec_stop" and row.get("seg") == segment]
    if len(starts) != 1 or len(stops) != 1:
        raise ValueError(f"{folder}: expected one complete {segment} recording")
    start, stop = starts[0], stops[0]
    setup = next((row for row in reversed(events[:events.index(start)]) if row.get("kind") == "body_arena_setup"), None)
    if setup is None:
        raise ValueError(f"{folder}: recording has no preceding arena game clock")
    with (folder/"metrics.csv").open(newline="") as stream:
        metrics = [row for row in csv.DictReader(stream) if row["seg"] == segment]
    count = stop["frames"]
    if not count or [int(row["frame"]) for row in metrics] != list(range(count)):
        raise ValueError(f"{folder}: metrics do not cover all {count} captured frames")
    offsets = [int(row["t"]) for row in metrics]
    if offsets[0] != 0 or any(a >= b for a, b in zip(offsets, offsets[1:])):
        raise ValueError(f"{folder}: frame times must start at zero and increase")
    origin = setup["arena"]["game_ms"] + start["t"] - setup["t"]
    times = [origin + offset for offset in offsets]
    for row in events:
        if (row.get("kind") == "body_combat" and row.get("phase") == "physics_step" and
                row.get("seg") == segment and 0 <= row["frame"] < count and row["t_game"] != times[row["frame"]]):
            raise ValueError(f"{folder}: game clock disagrees with frame {row['frame']}")
    fps = start["fps"]
    if not isinstance(fps, int) or fps <= 0:
        raise ValueError(f"{folder}: invalid recording fps {fps}")
    return events, times, fps


def label_data(folder: Path, *, size: tuple[int, int] = (1920, 1080), segment: str = "duel") -> dict:
    events, times, fps = frame_clock(folder, segment)
    initial = next(row["arena"] for row in rows(folder/"arena-events.jsonl") if row.get("kind") == "reset_ready")
    primary, symbol = initial["primary_entity_id"], primary_symbol(folder)
    track = [[frame, None, None] for frame in range(len(times))]
    seen = set()
    for row in events:
        if row.get("kind") != "track" or row.get("seg") != segment or row.get("npc") != symbol:
            continue
        frame = row["frame"]
        if frame in seen or not 0 <= frame < len(times):
            raise ValueError(f"{folder}: duplicate or invalid track frame {frame}")
        seen.add(frame)
        if (row["w"], row["h"]) != size:
            raise ValueError(f"{folder}: track size {row['w']}x{row['h']} differs from {size[0]}x{size[1]}")
        # hx/hy remain the head even if a hand-written scene requested `track ... body`.
        x, y = row["hx"], row["hy"]
        if x is not None and y is not None and math.isfinite(x) and math.isfinite(y) and 0 <= x < size[0] and 0 <= y < size[1]:
            track[frame] = [frame, x, y]
    if not seen:
        raise ValueError(f"{folder}: no head track for {symbol}; prepare with --track-head")

    observations = [row for row in rows(folder/"observations.jsonl") if row.get("entity_id") == primary]
    observed_times = [row["t_game_ms"] for row in observations]
    if not observations or observed_times[0] > times[0] or any(a > b for a, b in zip(observed_times, observed_times[1:])):
        raise ValueError(f"{folder}: primary HP observations must cover the first frame in game-time order")
    hp = []
    for frame, game_ms in enumerate(times):
        sample = observations[bisect_right(observed_times, game_ms)-1]
        values = [sample["self"]["hp"], sample["target"]["hp"]]
        if not all(isinstance(value, (int, float)) and math.isfinite(value) for value in values):
            raise ValueError(f"{folder}: invalid HP at frame {frame}")
        hp.append([frame, *values])

    controls = [row for row in rows(folder/"body-control-events.jsonl") if row.get("entity_id") == primary]
    accepted = [(row["command_id"], row["action_id"]) for row in controls if row.get("phase") == "accepted"]
    started = [row for row in controls if row.get("phase") == "started"]
    replay = json.loads((folder/"replay.json").read_text())
    expected = [(row["command_id"], row["action_id"]) for row in replay["commands"] if row.get("actor", "primary") == "primary"]
    if not accepted or accepted != expected or [(row["command_id"], row["action_id"]) for row in started] != accepted:
        raise ValueError(f"{folder}: started actions must match all {len(expected)} recorded primary commands in order")
    if len({row[0] for row in accepted}) != len(accepted):
        raise ValueError(f"{folder}: duplicate primary command_id")
    actions = []
    previous = times[0]
    for row in started:
        game_ms, action_id = row["t_game_ms"], row["action_id"]
        if not previous <= game_ms <= times[-1]:
            raise ValueError(f"{folder}: started command {row['command_id']} falls outside the film or out of order")
        if not isinstance(action_id, int) or not 0 <= action_id < len(ACTIONS):
            raise ValueError(f"{folder}: unknown action_id {action_id}")
        frame = bisect_left(times, game_ms)
        actions.append({"frame": frame, "t": (game_ms-times[0])/1000, "action": ACTIONS[action_id], "id": row["command_id"]})
        previous = game_ms
    return {"fps": fps, "frames": len(times), "size": list(size), "track": track, "hp": hp, "actions": actions}


def labels(args) -> int:
    size = tuple(int(value) for value in args.size.split("x"))
    if len(size) != 2 or any(value <= 0 for value in size):
        raise ValueError("--size must be WIDTHxHEIGHT with positive pixel dimensions")
    data = label_data(args.folder, size=size, segment=args.segment)
    output = args.output or args.folder/"actions.json"
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(data, ensure_ascii=False, separators=(",", ":"), allow_nan=False)+"\n")
    print(f"{args.folder.name}: {data['frames']} frames, {len(data['actions'])} started actions -> {output}")
    return 0


def compose(args) -> int:
    """Two films side by side; each gets its label and, below, its replay's outcome."""
    texts = args.output.parent/(args.output.stem+"-text")
    texts.mkdir(parents=True, exist_ok=True)

    def caption(index, film):
        replay = json.loads((film.parent/"replay.json").read_text()) if (film.parent/"replay.json").exists() else {}
        outcome = replay.get("outcome") or {}
        result = {"opponent_down": "win", "primary_down": "loss"}.get(outcome.get("reason"), outcome.get("reason") or "")
        hp = next((row["arena"]["actors"][0]["hp"] for row in rows(film.parent/"arena-events.jsonl")
                   if row.get("kind") == "terminal"), None) if (film.parent/"arena-events.jsonl").exists() else None
        seconds = (outcome.get("elapsed_game_ms") or 0)/1000
        (texts/f"label{index}.txt").write_text(args.labels[index])
        (texts/f"caption{index}.txt").write_text(f"{result}  {hp} HP  {seconds:.1f} s" if result else "")
        box = "fontcolor=white:box=1:boxcolor=black@0.45:boxborderw=10"
        return (f"drawtext=fontfile={FONT}:textfile={texts}/label{index}.txt:x=24:y=20:fontsize=36:{box},"
                f"drawtext=fontfile={FONT}:textfile={texts}/caption{index}.txt:x=24:y=h-th-24:fontsize=28:{box}")

    graph = (f"[0:v]{caption(0, args.left)}[a];[1:v]{caption(1, args.right)}[b];"
             "[a][b]hstack=inputs=2:shortest=0[v]")
    argv = ["ffmpeg", "-loglevel", "error", "-y", "-i", str(args.left), "-i", str(args.right),
            "-filter_complex", graph, "-map", "[v]", "-r", "30", "-c:v", "libx264", "-preset", "medium", "-crf", "20",
            "-pix_fmt", "yuv420p", "-movflags", "+faststart", str(args.output)]
    return subprocess.run(argv).returncode


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("prepare")
    p.add_argument("episodes", nargs="+", help="NAME=EPISODE_DIR of a collector run with --keep-journals")
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--scenario", type=Path, default=SCENARIO)
    p.add_argument("--camera", default="two arena 320 110 1")
    p.add_argument("--tail-ms", type=int, default=3000)
    p.add_argument("--track-head", action="store_true", help="track the primary NPC head in every captured frame")
    p.set_defaults(run=prepare)
    r = sub.add_parser("render")
    r.add_argument("scenes", nargs="+", type=Path)
    r.add_argument("--game", type=Path, required=True)
    r.add_argument("--assets", type=Path, required=True)
    r.add_argument("--save", type=Path, required=True)
    r.add_argument("--size", default="960x540", help="output video size in pixels, including on Retina displays")
    r.add_argument("--threads", type=int, default=4)
    r.add_argument("--textures", default="")
    r.add_argument("--timeout-s", type=int, default=1800)
    r.set_defaults(run=render)
    l = sub.add_parser("labels")
    l.add_argument("folder", type=Path, help="rendered replay with events, metrics and body journals")
    l.add_argument("--output", type=Path, help="default: FOLDER/actions.json")
    l.add_argument("--size", default="1920x1080", help="captured pixels; must agree with track event dimensions")
    l.add_argument("--segment", default="duel")
    l.set_defaults(run=labels)
    c = sub.add_parser("compose")
    c.add_argument("left", type=Path)
    c.add_argument("right", type=Path)
    c.add_argument("output", type=Path)
    c.add_argument("--labels", nargs=2, default=("BC", "R2u"))
    c.set_defaults(run=compose)
    args = parser.parse_args()
    return args.run(args)


if __name__ == "__main__":
    sys.exit(main())
