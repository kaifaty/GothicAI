#!/usr/bin/env python3
"""Export frame-aligned labels from an actual P30 fight recorded by the bench.

  labels DIR --npc SYMBOL --pilot-log DIR/server.log [--output actions.json]

The scene enables ``bodylive on`` before starting the pilot and keeps it enabled
until the lease is released. Its events.jsonl must contain body_live_control
wrappers and body_live_sample rows for every captured frame. ``track A,B,C head``
adds rendered head positions. Actions are accepted P30 decisions, including
``continue`` and commands cancelled before execution; they are not a claim that
every chosen action started. The one ``neural-body start ... lease=(...)`` line
binds the NPC symbol to its complete lease identity; provisional observation IDs
may change only before acquisition. Full-session accepted decisions must equal
the one ``neural-body end ... decisions=N`` line for that NPC. Only decisions within
the selected recording are exported. Their frame is the engine's capture index and
``t`` is that frame's presentation timestamp in the fixed-rate video.
"""
from __future__ import annotations

import argparse
import csv
import json
import math
from bisect import bisect_left
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT/"body-service"/"src"))
from gothic_body.schema import ACTIONS  # noqa: E402; constants only, no model is loaded


def rows(path: Path) -> list[dict]:
    result = []
    for line_number, line in enumerate(path.read_text().splitlines(), 1):
        if not line.strip():
            continue
        try:
            value = json.loads(line)
        except ValueError as error:
            raise ValueError(f"{path}:{line_number}: invalid JSON") from error
        if not isinstance(value, dict):
            raise ValueError(f"{path}:{line_number}: expected a JSON object")
        result.append(value)
    return result


def integer(value, name: str, *, minimum: int = 0) -> int:
    if type(value) is not int or value < minimum:
        raise ValueError(f"invalid {name}: {value!r}")
    return value


def number(value, name: str) -> float | int:
    if type(value) not in (int, float) or not math.isfinite(value):
        raise ValueError(f"invalid {name}: {value!r}")
    return value


def pilot_decisions(path: Path, npc: str) -> int:
    counts = []
    for line in path.read_text().splitlines():
        match = re.search(r"neural-body end npc=(\S+)\s.*?\bdecisions=(\d+)\b", line)
        if match and match[1] == npc:
            counts.append(int(match[2]))
    if len(counts) != 1:
        raise ValueError(f"{path}: expected one neural-body end decisions line for {npc}, got {len(counts)}")
    return counts[0]


def pilot_lease(path: Path, npc: str) -> tuple[int, int, int, int]:
    """Bind the NPC name to its actual lease, not provisional pre-watch entity IDs."""
    identities = []
    for line in path.read_text().splitlines():
        match = re.search(r"neural-body start npc=(\S+)\s.*?\blease=\((\d+),\s*(\d+),\s*(\d+),\s*(\d+)\)", line)
        if match and match[1] == npc:
            identities.append(tuple(integer(int(value), "pilot lease identity", minimum=1) for value in match.groups()[1:]))
    if len(identities) != 1:
        raise ValueError(f"{path}: expected one neural-body start lease line for {npc}, got {len(identities)}")
    return identities[0]


def label_data(folder: Path, *, npc: str, pilot_log: Path, segment: str = "shot",
               size: tuple[int, int] = (1920, 1080), min_decisions: int = 1,
               consecutive_wait: int = 0) -> tuple[dict, dict]:
    """Return the replay-compatible label schema and reconciliation counts."""
    if len(size) != 2 or any(type(value) is not int or value <= 0 for value in size):
        raise ValueError("size must contain two positive pixel dimensions")
    integer(min_decisions, "min_decisions")
    integer(consecutive_wait, "consecutive_wait")
    events = rows(folder/"events.jsonl")
    starts = [row for row in events if row.get("kind") == "rec" and row.get("seg") == segment]
    stops = [row for row in events if row.get("kind") == "rec_stop" and row.get("seg") == segment]
    if len(starts) != 1 or len(stops) != 1 or events.index(starts[0]) >= events.index(stops[0]):
        raise ValueError(f"{folder}: expected one complete {segment} recording")
    fps = integer(starts[0].get("fps"), "recording fps", minimum=1)
    count = integer(stops[0].get("frames"), "captured frames", minimum=1)
    with (folder/"metrics.csv").open(newline="") as stream:
        metrics = [row for row in csv.DictReader(stream) if row.get("seg") == segment]
    if [int(row["frame"]) for row in metrics] != list(range(count)):
        raise ValueError(f"{folder}: metrics do not cover all {count} captured frames")
    offsets = [int(row["t"]) for row in metrics]
    if offsets[0] != 0 or any(a >= b for a, b in zip(offsets, offsets[1:])):
        raise ValueError(f"{folder}: metrics frame times must start at zero and increase")

    samples = [row for row in events if row.get("kind") == "body_live_sample"
               and row.get("npc") == npc and row.get("seg") == segment]
    if [row.get("frame") for row in samples] != list(range(count)):
        raise ValueError(f"{folder}: {npc} samples do not cover all {count} captured frames in order")
    for row in samples:
        integer(row.get("entity_id"), "sample entity_id", minimum=1)
    times = [integer(row.get("t_game_ms"), "sample t_game_ms") for row in samples]
    if any(a >= b for a, b in zip(times, times[1:])):
        raise ValueError(f"{folder}: {npc} captured game clocks must increase")
    hp = [[frame, number(row.get("self_hp"), "self HP"), number(row.get("target_hp"), "target HP")]
          for frame, row in enumerate(samples)]

    track = [[frame, None, None] for frame in range(count)]
    seen = set()
    for row in events:
        if row.get("kind") != "track" or row.get("seg") != segment or row.get("npc") != npc:
            continue
        frame = integer(row.get("frame"), "track frame")
        if frame >= count or frame in seen:
            raise ValueError(f"{folder}: duplicate or invalid track frame {frame} for {npc}")
        seen.add(frame)
        if (row.get("w"), row.get("h")) != size:
            raise ValueError(f"{folder}: track size {row.get('w')}x{row.get('h')} differs from {size[0]}x{size[1]}")
        x, y = row.get("hx"), row.get("hy")
        if x is not None and y is not None:
            number(x, "head x")
            number(y, "head y")
            if 0 <= x < size[0] and 0 <= y < size[1]:
                track[frame] = [frame, x, y]
    if not any(row[1] is not None for row in track):
        raise ValueError(f"{folder}: no visible head track for {npc}")

    lease = pilot_lease(pilot_log, npc)
    controls = []
    for row in events:
        if row.get("kind") != "body_live_control":
            continue
        control = row.get("control")
        if not isinstance(control, dict):
            raise ValueError(f"{folder}: malformed body_live_control wrapper")
        scope = tuple(control.get(key) for key in ("entity_id", "session_epoch", "world_epoch", "control_generation"))
        if scope == lease and control.get("provider") == "neural-body":
            if control.get("mode") != "realtime":
                raise ValueError(f"{folder}: {npc} neural-body control is not realtime")
            controls.append((row, control))
    accepted = [(row, control) for row, control in controls if control.get("phase") == "accepted"]
    expected = pilot_decisions(pilot_log, npc)
    if not accepted or len(accepted) != expected:
        raise ValueError(f"{folder}: {npc} accepted decisions {len(accepted)} != pilot decisions {expected}")
    boundaries = [(row, control) for row, control in controls
                  if control.get("phase") == "accepted" or
                  (control.get("phase") == "ownership" and control.get("reason") == "acquired")]
    lease_frame = min(integer(row.get("frame"), "lease frame") if row.get("seg") == segment else
                      bisect_left(times, integer(control.get("t_game_ms"), "lease t_game_ms"))
                      for row, control in boundaries)
    # Before body_watch establishes its channel, observation IDs can be reassigned.
    # Samples are named by the script symbol; once this lease exists its ID is fixed.
    if any(row["entity_id"] != lease[0] for row in samples[lease_frame:]):
        raise ValueError(f"{folder}: {npc} changes entity_id after lease acquisition")
    command_ids = [integer(control.get("command_id"), "command_id", minimum=1) for _, control in accepted]
    if len(command_ids) != len(set(command_ids)) or any(a >= b for a, b in zip(command_ids, command_ids[1:])):
        raise ValueError(f"{folder}: {npc} duplicate or out-of-order accepted command_id")
    accepted_by_id = {control["command_id"]: control for _, control in accepted}
    previous_time = None
    for _, control in accepted:
        action = integer(control.get("action_id"), "action_id")
        if action >= len(ACTIONS):
            raise ValueError(f"{folder}: unknown action_id {action}")
        game_ms = integer(control.get("t_game_ms"), "control t_game_ms")
        if previous_time is not None and game_ms < previous_time:
            raise ValueError(f"{folder}: {npc} accepted game clocks go backwards")
        previous_time = game_ms
    started, continued = set(), set()
    for _, control in controls:
        phase = control.get("phase")
        if phase not in ("started", "continued"):
            continue
        command = control.get("command_id")
        accepted_control = accepted_by_id.get(command)
        if accepted_control is None or control.get("action_id") != accepted_control["action_id"]:
            raise ValueError(f"{folder}: {npc} {phase} command does not match an accepted decision")
        destination = continued if phase == "continued" else started
        if command in destination or (phase == "continued") != (control.get("action_id") == 0):
            raise ValueError(f"{folder}: {npc} duplicate or invalid {phase} acknowledgement")
        destination.add(command)
    if any(control["action_id"] == 0 and control["command_id"] not in continued for _, control in accepted):
        raise ValueError(f"{folder}: {npc} continue decision lacks its continued acknowledgement")

    actions = []
    for row, control in accepted:
        if row.get("seg") != segment:
            continue
        frame = integer(row.get("frame"), "decision frame")
        if frame >= count:
            # Commands after the last captured picture have no video frame.
            continue
        game_ms = control["t_game_ms"]
        if game_ms > times[frame] or (frame and game_ms < times[frame-1]):
            raise ValueError(f"{folder}: {npc} decision game clock disagrees with capture frame {frame}")
        actions.append({"frame": frame, "t": frame/fps, "action": ACTIONS[control["action_id"]],
                        "id": control["command_id"]})
    if len(actions) < min_decisions:
        raise ValueError(f"{folder}: {npc} has {len(actions)} recorded decisions, need {min_decisions}")
    longest = run = 0
    for action in actions:
        run = run+1 if action["action"] == "wait_200" else 0
        longest = max(longest, run)
    if longest < consecutive_wait:
        raise ValueError(f"{folder}: {npc} has {longest} consecutive wait_200 decisions, need {consecutive_wait}")
    data = {"fps": fps, "frames": count, "size": list(size), "track": track, "hp": hp, "actions": actions}
    stats = {"recorded": len(actions), "accepted": len(accepted), "pilot": expected,
             "started": len(started), "continued": len(continued), "longest_wait_200": longest}
    return data, stats


def labels(args) -> int:
    try:
        size = tuple(int(value) for value in args.size.split("x"))
    except ValueError as error:
        raise ValueError("--size must be WIDTHxHEIGHT") from error
    data, stats = label_data(args.folder, npc=args.npc, pilot_log=args.pilot_log or args.folder/"server.log",
                             segment=args.segment, size=size, min_decisions=args.min_decisions,
                             consecutive_wait=args.consecutive_wait)
    output = args.output or args.folder/"actions.json"
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(data, ensure_ascii=False, separators=(",", ":"), allow_nan=False)+"\n")
    print(f"{args.npc}: {stats['recorded']} recorded / {stats['accepted']} accepted = {stats['pilot']} pilot decisions; "
          f"{stats['started']} started, {stats['continued']} continued, longest wait_200={stats['longest_wait_200']}; "
          f"{data['frames']} frames -> {output}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    command = sub.add_parser("labels", help="export live P30 decisions, HP and head track")
    command.add_argument("folder", type=Path)
    command.add_argument("--npc", required=True)
    command.add_argument("--pilot-log", type=Path)
    command.add_argument("--segment", default="shot")
    command.add_argument("--size", default="1920x1080")
    command.add_argument("--min-decisions", type=int, default=1)
    command.add_argument("--consecutive-wait", type=int, default=0)
    command.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        return labels(args)
    except (OSError, ValueError, KeyError) as error:
        parser.exit(1, f"body_live_film: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
