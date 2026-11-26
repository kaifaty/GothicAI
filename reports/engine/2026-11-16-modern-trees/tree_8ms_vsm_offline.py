#!/usr/bin/env python3
"""Reproject archived solar clip windows and count isolated tile requests."""

import argparse
from bisect import bisect_right
from collections import defaultdict
import csv
import json
import math
from pathlib import Path
import re
import struct


WIDTH = 32
MIPS = 16
PAGE_CM = 64
DAY_MS = 86_400_000
DEPTH_CM = 51_200
CLOCK_RATE = 14.5
BATCH_FRAME_MS = 11.901
INTERVALS = (4, 8, 16)
SUBMIT = re.compile(
    r"^vsm sun submit (\d+) ns=(\d+) .* x=([\da-f,]+) y=([\da-f,]+) z=([\da-f,]+) "
    r"xy_basis=(\d) z_basis=(\d)"
)
GEOMETRY = re.compile(r"^vsm sun mip (\d+) m=(\d+) pairs=(\d+) tris=(\d+)")
GROUP = re.compile(r"^(.*)-(\d+)(?:-a1)?-requests\.bin$")


def dot(a, b):
    return sum(x * y for x, y in zip(a, b, strict=True))


def cross(a, b):
    ax, ay, az = a
    bx, by, bz = b
    return ay * bz - az * by, az * bx - ax * bz, ax * by - ay * bx


def normalize(v):
    length = math.sqrt(dot(v, v))
    return tuple(x / length for x in v)


def sun_dir(ms):
    rise, meridian, sunset = (4 * 60 + 45) * 60_000, (13 * 60 + 9) * 60_000, (21 * 60 + 33) * 60_000
    ms %= DAY_MS
    if ms < rise:
        pulse = -1 + ms / rise
    elif ms < meridian:
        pulse = (ms - rise) / (meridian - rise)
    elif ms < sunset:
        pulse = (sunset - ms) / (sunset - meridian)
    else:
        pulse = -(ms - sunset) / (DAY_MS - sunset)
    angle = -2 * math.pi * (ms / DAY_MS + 0.25)
    return normalize((-math.sin(angle) * 0.56, pulse, math.cos(angle) * 0.56))


def decode_camera(rows):
    x, y, z = [struct.unpack(">4f", bytes.fromhex(row.replace(",", ""))) for row in rows]
    axis_x, axis_y, axis_z = [normalize(row[:3]) for row in (x, y, z)]
    target = tuple(-x[3] * 1024 * a - y[3] * 1024 * b + (0.5 - z[3]) * DEPTH_CM * c
                   for a, b, c in zip(axis_x, axis_y, axis_z, strict=True))
    angle = math.atan2(-axis_z[0], axis_z[2])
    time_ms = ((-angle / (2 * math.pi) - 0.25) % 1) * DAY_MS
    if dot(axis_z, sun_dir(time_ms)) < 0.999999:
        raise ValueError("Archived direction does not match the solar model")
    return time_ms, target


def read_archive(path):
    with (path / "frameprofile.csv").open() as file:
        windows = [(int(row["begin_ns"]), int(row["end_ns"]), row["segment"])
                   for row in csv.DictReader(file) if row["outcome"] == "1" and row["segment"]]
    bounds = [start for start, _, _ in windows]
    frames = defaultdict(list)
    counts = defaultdict(dict)
    with (path / "game.log").open() as file:
        for line in file:
            if match := SUBMIT.match(line):
                frame, ns = map(int, match.groups()[:2])
                i = bisect_right(bounds, ns) - 1
                if i >= 0 and ns <= windows[i][1]:
                    time_ms, camera = decode_camera(match.groups()[2:5])
                    basis_matches = match[6] == match[7] == "1"
                    frames[windows[i][2]].append((frame, time_ms, camera, basis_matches))
            elif match := GEOMETRY.match(line):
                frame, mip, pairs, tris = map(int, match.groups())
                counts[frame][mip] = pairs, tris
    if sum(map(len, frames.values())) != len(windows):
        raise ValueError("Solar submits do not match the archived profiler frame intervals")
    if any(frame not in counts for rows in frames.values() for frame, *_ in rows):
        raise ValueError("Missing solar geometry counters")
    return frames, counts


def clip_window(camera, direction, mip):
    reference = (0, 0, 1) if abs(direction[2]) < 0.999 else (1, 0, 0)
    axis_x = normalize(cross(direction, reference))
    axis_y = cross(axis_x, direction)
    pitch = PAGE_CM * (1 << mip)
    return math.floor(dot(camera, axis_x) / pitch), math.floor(dot(camera, axis_y) / pitch)


def overlap(previous, current):
    dx = abs(previous[0] - current[0])
    dy = abs(previous[1] - current[1])
    return max(0, WIDTH - dx) * max(0, WIDTH - dy) / (WIDTH * WIDTH)


def replay(rows, counts, step_ms):
    direction_matches = 0
    per_mip = defaultdict(lambda: [0, 0, 0.0, 0.0])
    previous_bin, previous_camera = None, None
    for frame, time_ms, camera, _ in rows:
        solar_bin = int(time_ms // step_ms)
        same_bin = solar_bin == previous_bin
        direction_matches += same_bin
        direction = sun_dir(solar_bin * step_ms)
        for mip, (pairs, triangles) in counts[frame].items():
            if not pairs:
                continue
            row = per_mip[mip]
            row[0] += pairs
            row[1] += triangles
            if same_bin:
                common = overlap(clip_window(previous_camera, direction, mip), clip_window(camera, direction, mip))
                row[2] += pairs * common
                row[3] += triangles * common
        previous_bin, previous_camera = solar_bin, camera
    return direction_matches, per_mip


def report_reprojection(path):
    segments, counts = read_archive(path)
    for name, rows in sorted(segments.items()):
        matching_bases = sum(match for _, _, _, match in rows)
        print(f"\n{name}: {len(rows)} submitted frames; original matching bases {matching_bases}")
        for step in INTERVALS:
            step_ms = round(BATCH_FRAME_MS * CLOCK_RATE * step)
            matches, mips = replay(rows, counts, step_ms)
            pairs, tris, pair_weight, tri_weight = map(sum, zip(*mips.values(), strict=True))
            print(f"{step} frames / {step_ms} game ms: same direction {100 * matches / len(rows):.2f}%, "
                  f"clip overlap weighted by pairs/triangles {100 * pair_weight / pairs:.2f}%/"
                  f"{100 * tri_weight / tris:.2f}%")
            print("  mip: current pairs/triangles per frame; weighted clip overlap, pairs/triangles")
            for mip, (p, t, w_p, w_t) in sorted(mips.items()):
                print(f"  m{mip}: {p / len(rows):.0f}/{t / len(rows):.0f}; {100*w_p/p:.2f}%/{100*w_t/t:.2f}%")


def shadow_end(ms):
    x, y, z = sun_dir(ms)
    return -18 * x / y, -18 * z / y


def report_solar_steps():
    print("\n18 m caster, flat ground: solar height; shadow endpoint jump for each step")
    for hour, minute in ((12, 3), (5, 30)):
        ms = (hour * 60 + minute) * 60_000
        height = math.degrees(math.asin(sun_dir(ms)[1]))
        jumps = [math.dist(shadow_end(ms), shadow_end(ms + round(BATCH_FRAME_MS * CLOCK_RATE * step)))
                 for step in INTERVALS]
        print(f"  {hour:02}:{minute:02}: {height:.2f}°, " + ", ".join(f"{jump * 1000:.2f} mm" for jump in jumps))


def request_cells(path):
    data = path.read_bytes()
    if len(data) != WIDTH * WIDTH * MIPS * 8:
        raise ValueError(f"Unexpected request map size: {path}")
    return {i for i, (mark, _) in enumerate(struct.iter_unpack("<II", data)) if mark}


def allocated_cells(path):
    data = path.read_bytes()
    offset = 32 + (MIPS + 1) * 3 * 16
    pairs, _, _, _, entries = struct.unpack_from("<5I", data)
    if entries > 4096 or len(data) != offset + 4096 * 4 + pairs * 16:
        raise ValueError(f"Unexpected atlas proof size: {path}")
    result = set()
    for (packed,) in struct.iter_unpack("<I", data[offset:offset + entries * 4]):
        if not packed or packed & 1:  # Empty allocation and lamp entries.
            continue
        x, y, mip = (packed >> 1) & 255, (packed >> 9) & 255, (packed >> 17) & 127
        width, height = (packed >> 24) & 15, (packed >> 28) & 15
        if width == height == 0:  # Spare stripe cells reserved by the allocator.
            continue
        if mip >= MIPS or x + width > WIDTH or y + height > WIDTH or not min(width, height):
            raise ValueError(f"Unexpected solar rectangle: {path}")
        result.update((mip * WIDTH + row) * WIDTH + col
                      for row in range(y, y + height) for col in range(x, x + width))
    return result


def report_snapshot_requests(path):
    print("\nBatch 1, isolated snapshots: pre-grouping requests / covered by allocated entries")
    for source in sorted(path.glob("*-requests.bin")):
        if not GROUP.match(source.name):
            continue
        name = source.name.removesuffix("-requests.bin")
        combined = request_cells(source)
        surface = request_cells(path / f"{name}-surface.bin")
        allocated = allocated_cells(path / f"{name}-with-fog.bin")
        mips = [sum(i // (WIDTH * WIDTH) == mip for i in combined) for mip in range(MIPS)]
        print(f"  {name}: {len(combined)}/{len(combined & allocated)}, surface {len(surface)}, "
              f"fog-only {len(combined - surface)}, mips {mips}")
    summary = json.loads((path / "summary.json").read_text())
    print("Batch 1, stock inside geometry by mip: pairs/triangles")
    for row in summary["inside"]:
        geometry = row["stock"]["counts_by_mip"][:MIPS]
        pairs, triangles = map(sum, zip(*geometry, strict=True))
        far_pairs, far_triangles = map(sum, zip(*geometry[5:], strict=True))
        print(f"  {row['frame']}: {geometry[:9]}; m5+ {100 * far_pairs / pairs:.2f}%/{100 * far_triangles / triangles:.2f}%")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("diagnosis", type=Path, help="forest-dense-16 basis diagnosis, diag/trees/repeat-1")
    parser.add_argument("snapshots", type=Path, help="tree-8ms-batch1 depth diagnostics, diagnostic")
    args = parser.parse_args()
    report_reprojection(args.diagnosis)
    report_solar_steps()
    report_snapshot_requests(args.snapshots)


if __name__ == "__main__":
    main()
