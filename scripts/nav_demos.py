#!/usr/bin/env python3
"""Read and check navigation demonstrations without the engine; only numpy is required.

    python3 -I scripts/nav_demos.py check /absolute/path/nav-demos
    python3 -I scripts/nav_demos.py summary /absolute/path/nav-demos
    python3 -I scripts/nav_demos.py render /path/episode /path/episode.svg

The gzip or XZ payload has a 32-byte HEADER followed by packed records. Versions 1 and
2 share the same byte layout; version 2 road guidance describes a forward carrot
on the familiar route instead of the next graph node. Version 3 retains version 2
roads and adds an eighth memory channel, familiar. RECORD_DTYPE, CELL_DTYPE,
CHANNELS and RECORD_BYTES remain version 1 aliases for existing learners; use
RECORD_DTYPES[version] and RECORD_BYTES_BY_VERSION[version] for other formats.
Memory channels are probabilities quantized by round(255*p); divide by 255.
episode.json.navigation_memory_version identifies their interpretation independently
of the binary format: absent means legacy version 1; version 2 uses local terrain
transitions instead of height relative to the observer's feet. Version 4 adds
longer journey memory and familiar-map fallback, marked by the familiar channel.
ground_height_cm=0,height_valid=0 represents an unknown height (never NaN).
Body and road vectors use x=right,y=up,z=forward in the current body frame.
World position/yaw and teacher hints are debug fields, excluded by learner_input().
Memory is [forward,right], near=64x64 at 25 cm, far=100x100 at 200 cm per cell.
Eye hit kinds: 0 none, 1 world, 2 static vob, 3 movable vob, 4 NPC, 5 water.
Body flags: inAir, falling, sliding, climbing, swimming. All times are game time.
"""

from __future__ import annotations

import argparse
import gzip
import html
import json
import lzma
from pathlib import Path
import struct
import sys
from typing import Iterator

import numpy as np


HEADER = struct.Struct("<8s6I")
MAGIC = b"NAVDEMO\0"
VERSION = 1
RAY_COUNT, NEAR_SIZE, FAR_SIZE, PERIOD_MS = 238, 64, 100, 100
CHANNELS = ("confidence", "free", "wall", "ledge", "drop", "water", "movable")
CHANNELS_V3 = CHANNELS + ("familiar",)
BODY_FLAGS = ("in_air", "falling", "sliding", "climbing", "swimming")

KEY_DTYPE = np.dtype([
    ("forward", "<f4"), ("strafe", "<f4"), ("turn", "<f4"),
    ("head_yaw_deg", "<f4"), ("head_pitch_deg", "<f4"),
    ("walk", "u1"), ("jump", "u1"), ("climb", "u1"),
])
RAY_DTYPE = np.dtype([
    ("distance_cm", "<f4"), ("kind", "u1"), ("normal_y", "<f4"),
    ("age_ms", "<u4"), ("near_age_ms", "<u4"),
    ("complete", "u1"), ("hit", "u1"),
])
CELL_DTYPE = np.dtype([
    ("channels", "u1", (7,)), ("ground_height_cm", "<f4"), ("height_valid", "u1"),
])
CELL_V3_DTYPE = np.dtype([
    ("channels", "u1", (8,)), ("ground_height_cm", "<f4"), ("height_valid", "u1"),
])
BODY_DTYPE = np.dtype([
    ("velocity_cm_s", "<f4", (3,)), ("flags", "u1", (5,)),
    ("ground_normal", "<f4", (3,)), ("ground_slope_deg", "<f4"),
    ("head_yaw_deg", "<f4"), ("head_pitch_deg", "<f4"),
])
ROADS_DTYPE = np.dtype([
    ("next_direction", "<f4", (3,)), ("next_distance_cm", "<f4"),
    ("goal_direction", "<f4", (3,)), ("goal_distance_cm", "<f4"),
    ("remaining_visual_cm", "<f4"), ("goal_in_known", "u1"),
])
ROADS_V2_DTYPE = np.dtype([
    ("carrot_direction", "<f4", (3,)), ("carrot_distance_cm", "<f4"),
    ("goal_direction", "<f4", (3,)), ("goal_distance_cm", "<f4"),
    ("remaining_known_cm", "<f4"), ("beyond_known", "u1"),
])
HINT_DTYPE = np.dtype([("next_corner_cm", "<f4", (3,)), ("path_cm", "<f4")])


def _record_dtype(roads: np.dtype, cell: np.dtype = CELL_DTYPE) -> np.dtype:
    return np.dtype([
        ("time_ms", "<u8"), ("position_cm", "<f4", (3,)), ("body_yaw_deg", "<f4"),
        ("body", BODY_DTYPE), ("roads", roads),
        ("previous_keys", KEY_DTYPE), ("keys", KEY_DTYPE),
        ("eye", RAY_DTYPE, (RAY_COUNT,)),
        ("memory_near", cell, (NEAR_SIZE, NEAR_SIZE)),
        ("memory_far", cell, (FAR_SIZE, FAR_SIZE)),
        ("teacher_hint", HINT_DTYPE),
    ])


RECORD_DTYPE = _record_dtype(ROADS_DTYPE)
RECORD_V2_DTYPE = _record_dtype(ROADS_V2_DTYPE)
RECORD_V3_DTYPE = _record_dtype(ROADS_V2_DTYPE, CELL_V3_DTYPE)
RECORD_DTYPES = {1: RECORD_DTYPE, 2: RECORD_V2_DTYPE, 3: RECORD_V3_DTYPE}
RECORD_BYTES = 173838
RECORD_BYTES_BY_VERSION = {1: RECORD_BYTES, 2: RECORD_BYTES, 3: 187934}
assert all(dtype.itemsize == RECORD_BYTES_BY_VERSION[version]
           for version, dtype in RECORD_DTYPES.items())


def _samples_path(path: str | Path) -> Path:
    path = Path(path)
    if path.is_dir():
        compact = path / "samples.bin.xz"
        return compact if compact.is_file() else path / "samples.bin.gz"
    return path


samples_path = _samples_path


def _open_samples(path: str | Path):
    source = _samples_path(path)
    return (lzma.open if source.suffix == ".xz" else gzip.open)(source, "rb")


def episode_paths(path: str | Path) -> list[Path]:
    """Return all recorded episodes, including unsuccessful attempts."""
    path = Path(path)
    if path.is_file():
        return [path]
    samples = _samples_path(path)
    if samples.is_file():
        return [samples]
    if (path / "episodes").is_dir():
        path = path / "episodes"
    files = {}
    for samples in path.rglob("samples.bin.*"):
        if samples.name not in ("samples.bin.gz", "samples.bin.xz"):
            continue
        if samples.parent not in files or samples.suffix == ".xz":
            files[samples.parent] = samples
    return sorted(files.values())


def _read_header(stream) -> int:
    header = stream.read(HEADER.size)
    if len(header) != HEADER.size:
        raise ValueError("truncated navigation demo header")
    fields = HEADER.unpack(header)
    version = fields[1]
    expected = (MAGIC, version, RECORD_BYTES_BY_VERSION.get(version),
                RAY_COUNT, NEAR_SIZE, FAR_SIZE, PERIOD_MS)
    if version not in RECORD_DTYPES or fields != expected:
        raise ValueError(f"unsupported navigation demo header: {fields!r}")
    return version


def episode_version(path: str | Path) -> int:
    """Read the validated format version without loading sensor records."""
    with _open_samples(path) as stream:
        return _read_header(stream)


def episode_memory_version(path: str | Path) -> int:
    """Read memory semantics separately from NAVDEMO's byte-layout version.

    Existing episodes without a tag retain legacy semantics (version 1).
    The tag belongs to episode.json; loading old samples never rewrites it.
    """
    source = _samples_path(path).parent / "episode.json"
    metadata = json.loads(source.read_text(encoding="utf-8")) if source.is_file() else {}
    version = metadata.get("navigation_memory_version", 1)
    if type(version) is not int or version < 1:
        raise ValueError(f"invalid navigation_memory_version in {source}: {version!r}")
    return version


def iter_chunks(path: str | Path, chunk_records: int = 64) -> Iterator[np.ndarray]:
    """Stream read-only structured arrays; memory use is bounded by chunk_records."""
    if chunk_records < 1:
        raise ValueError("chunk_records must be positive")
    with _open_samples(path) as stream:
        dtype = RECORD_DTYPES[_read_header(stream)]
        record_bytes = dtype.itemsize
        while True:
            raw = stream.read(record_bytes * chunk_records)
            if not raw:
                break
            if len(raw) % record_bytes:
                raise ValueError(f"truncated record: {len(raw)} bytes")
            yield np.frombuffer(raw, dtype=dtype)


def load_episode(path: str | Path) -> np.ndarray:
    """Load one episode. A 240 s V3 episode occupies about 430 MiB uncompressed."""
    chunks = list(iter_chunks(path))
    if not chunks:
        return np.empty(0, dtype=RECORD_DTYPES[episode_version(path)])
    return np.concatenate(chunks)


def learner_input(records: np.ndarray) -> dict[str, np.ndarray]:
    """Model inputs only, with the version's road fields exposed without conversion.

    Version 2 roads contain carrot_direction, carrot_distance_cm,
    remaining_known_cm, goal_direction, goal_distance_cm and beyond_known.
    Actions, world pose and privileged teacher hints stay separate.
    """
    return {name: records[name] for name in
            ("body", "roads", "previous_keys", "eye", "memory_near", "memory_far")}


def ray_layout(path: str | Path) -> np.ndarray:
    """Static [238,3] yaw degrees, pitch degrees, max range cm, relative to head."""
    source = _samples_path(path).parent / "episode.json"
    metadata = json.loads(source.read_text(encoding="utf-8"))
    layout = np.asarray(metadata["eye_layout"], dtype=np.float32)
    if layout.shape != (238, 3) or not np.isfinite(layout).all() or not (layout[:, 2] > 0).all():
        raise ValueError(f"invalid eye layout in {source}")
    return layout


def _finite(value: np.ndarray, prefix: str = "record") -> None:
    if value.dtype.names:
        for name in value.dtype.names:
            _finite(value[name], f"{prefix}.{name}")
    elif value.dtype.kind == "f" and not np.isfinite(value).all():
        raise ValueError(f"NaN or infinite value in {prefix}")


def _require(condition: bool | np.bool_, message: str) -> None:
    if not condition:
        raise ValueError(message)


def _validate_chunk(data: np.ndarray, layout: np.ndarray) -> None:
    _finite(data)
    for name in ("keys", "previous_keys"):
        keys = data[name]
        for axis in ("forward", "strafe", "turn"):
            _require((np.abs(keys[axis]) <= 1).all(), f"{name}.{axis} outside [-1,1]")
        for key in ("walk", "jump", "climb"):
            _require((keys[key] <= 1).all(), f"{name}.{key} is not a bit")
        _require((np.abs(keys["head_yaw_deg"]) <= 180).all(), f"{name}: invalid head yaw")
        _require((np.abs(keys["head_pitch_deg"]) <= 90).all(), f"{name}: invalid head pitch")
    eye = data["eye"]
    _require((eye["distance_cm"] >= 0).all(), "negative ray distance")
    max_range = layout[:, 2]
    tolerance = np.maximum(1e-3, max_range * 1e-6)
    _require((eye["distance_cm"] <= max_range + tolerance).all(), "ray distance exceeds its max range")
    _require((eye["kind"] <= 5).all(), "unknown ray hit kind")
    _require((np.abs(eye["normal_y"]) <= 1).all(), "ray normal outside [-1,1]")
    for flag in ("complete", "hit"):
        _require((eye[flag] <= 1).all(), f"eye.{flag} is not a bit")
    for age in ("age_ms", "near_age_ms"):
        _require((eye[age] <= data["time_ms"][:, None]).all(), f"eye.{age} predates game start")
    _require((data["body"]["flags"] <= 1).all(), "body flag is not a bit")
    _require((np.abs(data["body"]["ground_normal"]) <= 1.00001).all(), "invalid ground normal")
    _require(((data["body"]["ground_slope_deg"] >= 0) &
              (data["body"]["ground_slope_deg"] <= 180)).all(), "invalid ground slope")
    roads = data["roads"]
    v2 = roads.dtype == ROADS_V2_DTYPE
    bit = "beyond_known" if v2 else "goal_in_known"
    _require((roads[bit] <= 1).all(), f"road {bit} is not a bit")
    distance_fields = (("carrot_distance_cm", "goal_distance_cm", "remaining_known_cm") if v2 else
                       ("next_distance_cm", "goal_distance_cm", "remaining_visual_cm"))
    for name in distance_fields:
        _require((roads[name] >= 0).all(), f"negative roads.{name}")
    for name in (("carrot_direction" if v2 else "next_direction"), "goal_direction"):
        norms = np.linalg.norm(roads[name], axis=-1)
        _require(((norms <= 1e-5) | (np.abs(norms - 1) <= 1e-3)).all(), f"roads.{name} not unit or zero")
    _require((data["teacher_hint"]["path_cm"] >= 0).all(), "negative teacher path length")
    for name in ("memory_near", "memory_far"):
        memory = data[name]
        _require((memory["height_valid"] <= 1).all(), f"{name}.height_valid is not a bit")
        _require((memory["ground_height_cm"][memory["height_valid"] == 0] == 0).all(),
                 f"{name}: unknown height must be zero")


def key_masks(keys: np.ndarray) -> dict[str, np.ndarray]:
    moving = (keys["forward"] != 0) | (keys["strafe"] != 0)
    return {
        "forward": keys["forward"] > 0, "back": keys["forward"] < 0,
        "walk": moving & (keys["walk"] != 0),
        "run": moving & (keys["walk"] == 0),
        "turn": keys["turn"] != 0, "turn_left": keys["turn"] < 0,
        "turn_right": keys["turn"] > 0, "strafe": keys["strafe"] != 0,
        "jump": keys["jump"] != 0, "climb": keys["climb"] != 0,
        "idle": ~moving & (keys["turn"] == 0) & (keys["jump"] == 0) & (keys["climb"] == 0),
    }


def summarize(path: str | Path, validate: bool = False) -> dict:
    files = episode_paths(path)
    if not files:
        raise ValueError(f"no samples.bin.gz or samples.bin.xz episodes found in {path}")
    counts: dict[str, int] = {}
    total_records = 0
    episodes = []
    for file in files:
        version = episode_version(file)
        memory_version = episode_memory_version(file)
        if validate:
            layout = ray_layout(file)
            metadata = json.loads((file.parent / "episode.json").read_text(encoding="utf-8"))
            expected_records = metadata["decisions"]
            elapsed_ms = metadata["elapsed_ms"]
            _require(type(expected_records) is int and expected_records > 0,
                     f"invalid episode decisions in {file.parent}")
            _require(type(elapsed_ms) is int and elapsed_ms > 0,
                     f"invalid episode elapsed_ms in {file.parent}")
            actions = np.loadtxt(file.parent / "actions.txt", dtype=np.float64, ndmin=2)
            _require(actions.shape == (expected_records + 1, 9),
                     f"actions must have decisions+1 rows and 9 columns in {file.parent}")
            _require(np.isfinite(actions).all(), f"non-finite action in {file.parent}")
            action_times = actions[:, 0]
            _require(action_times[0] == 0, f"actions must start at zero in {file.parent}")
            _require((action_times == np.floor(action_times)).all(), "action times must be integer milliseconds")
            _require((np.diff(action_times[:-1]) == PERIOD_MS).all(),
                     "nonterminal actions must be exactly 100 ms apart")
            _require(0 < action_times[-1] - action_times[-2] <= PERIOD_MS,
                     "terminal action interval must be within (0,100] ms")
            _require(action_times[-1] == elapsed_ms, "terminal action time differs from episode elapsed_ms")
            _require((actions[-1, 1:] == [0, 0, 0, 1, 0, 0, 0, 0]).all(),
                     "terminal action must release movement, jump, climb and head input")
            # In policy drives NAVDEMO keys remain the teacher's labels, while
            # actions.txt records what the learner actually applied. The format
            # and learner_input are unchanged; only action provenance differs.
            drive = metadata.get("drive", False)
            applied_keys = np.zeros(len(actions), dtype=KEY_DTYPE)
            action_fields = ("forward", "strafe", "turn", "walk", "jump", "climb",
                             "head_yaw_deg", "head_pitch_deg")
            for column, name in enumerate(action_fields, start=1):
                if name in ("forward", "strafe", "turn"):
                    if drive:
                        _require(np.isin(actions[:, column], [-1, 0, 1]).all(), f"actions.{name} is not a digital axis")
                elif name in ("walk", "jump", "climb"):
                    _require(np.isin(actions[:, column], [0, 1]).all(), f"actions.{name} is not a bit")
                else:
                    bound = 180 if name == "head_yaw_deg" else 90
                    _require((np.abs(actions[:, column]) <= bound).all(), f"actions.{name} outside head limits")
                    if drive:
                        _require((actions[:, column] == 0).all(), f"policy actions.{name} must look straight")
                applied_keys[name] = actions[:, column]
        record_count = 0
        first_ms = last_ms = None
        last_keys = None
        last_debug = {name: None for name in
                      ("last_teacher_hint", "last_body_flags", "last_keys", "last_position_cm", "body_yaw_deg")}
        for data in iter_chunks(file):
            if first_ms is None:
                first_ms = int(data["time_ms"][0])
            if validate:
                _validate_chunk(data, layout)
                end_record = record_count + len(data)
                _require(end_record <= expected_records, "more records than episode decisions")
                chunk_actions = actions[record_count:end_record]
                relative_times = data["time_ms"] - first_ms
                _require((relative_times == chunk_actions[:, 0]).all(),
                         "record times differ from relative action times")
                if not drive:
                    for column, name in enumerate(action_fields, start=1):
                        _require((chunk_actions[:, column].astype(np.float32) == data["keys"][name]).all(),
                                 f"actions.{name} differs from recorded keys")
                if last_ms is not None:
                    _require(int(data["time_ms"][0]) - last_ms == PERIOD_MS, "time gap between chunks")
                    _require(bool(data["previous_keys"][0] == last_keys), "previous keys mismatch between chunks")
                if len(data) > 1:
                    times = data["time_ms"]
                    _require((times[1:] > times[:-1]).all(), "time is not strictly increasing")
                    _require((np.diff(times) == PERIOD_MS).all(), "decision interval is not 100 ms")
                    prior = applied_keys[record_count:end_record - 1] if drive else data["keys"][:-1]
                    _require((data["previous_keys"][1:] == prior).all(), "previous keys mismatch")
            last_ms = int(data["time_ms"][-1])
            last_keys = (applied_keys[record_count + len(data) - 1] if validate and drive else data["keys"][-1]).copy()
            last = data[-1]
            last_debug = {
                "last_teacher_hint": {
                    "next_corner_cm": last["teacher_hint"]["next_corner_cm"].tolist(),
                    "path_cm": float(last["teacher_hint"]["path_cm"]),
                },
                "last_body_flags": last["body"]["flags"].tolist(),
                "last_keys": {name: last_keys[name].item() for name in KEY_DTYPE.names},
                "last_position_cm": last["position_cm"].tolist(),
                "body_yaw_deg": float(last["body_yaw_deg"]),
            }
            record_count += len(data)
            for key, mask in key_masks(data["keys"]).items():
                counts[key] = counts.get(key, 0) + int(np.count_nonzero(mask))
            for index, key in enumerate(BODY_FLAGS):
                name = f"body_{key}"
                counts[name] = counts.get(name, 0) + int(np.count_nonzero(data["body"]["flags"][:, index]))
        if validate:
            _require(record_count > 0, f"empty episode: {file}")
            _require(record_count == expected_records, "record count differs from episode decisions")
        compressed_bytes = file.stat().st_size
        episodes.append({"episode": str(file.parent), "decisions": record_count,
                         "format_version": version,
                         "record_bytes": RECORD_BYTES_BY_VERSION[version],
                         "navigation_memory_version": memory_version,
                         "first_ms": first_ms, "last_ms": last_ms,
                         "compressed_bytes": compressed_bytes, **last_debug})
        total_records += record_count
    compressed_bytes = sum(episode["compressed_bytes"] for episode in episodes)
    root = Path(path)
    if (root / "episodes").is_dir():
        root = root / "episodes"
    dataset_bytes = sum(file.stat().st_size for file in root.rglob("*") if file.is_file()) if root.is_dir() else compressed_bytes
    versions = sorted({episode["format_version"] for episode in episodes})
    memory_versions = sorted({episode["navigation_memory_version"] for episode in episodes})
    record_sizes = sorted({episode["record_bytes"] for episode in episodes})
    if validate:
        _require(len(memory_versions) == 1,
                 f"mixed navigation memory versions: {memory_versions}; use separate datasets")
    return {
        "format_version": versions[0] if len(versions) == 1 else None,
        "format_versions": versions, "episodes": len(files), "decisions": total_records,
        "navigation_memory_version": memory_versions[0] if len(memory_versions) == 1 else None,
        "navigation_memory_versions": memory_versions,
        "record_bytes": record_sizes[0] if len(record_sizes) == 1 else None,
        "uncompressed_bytes": sum(episode["decisions"] * episode["record_bytes"] + HEADER.size
                                  for episode in episodes),
        "compressed_samples_bytes": compressed_bytes, "dataset_bytes": dataset_bytes,
        "dataset_gb": dataset_bytes / 1e9,
        "key_counts": counts,
        "key_fractions": {key: value / total_records if total_records else 0 for key, value in counts.items()},
        "validated": validate, "episode_details": episodes,
    }


def render_episode(path: str | Path, destination: str | Path) -> None:
    """Generate a shareable SVG with a top-down trajectory and digital-key timeline."""
    # Keep only small fields; do not load the large memory views for rendering.
    chunks = [(data["time_ms"].copy(), data["position_cm"].copy(), data["keys"].copy())
              for data in iter_chunks(path)]
    if not chunks:
        raise ValueError("cannot render an empty episode")
    time_ms = np.concatenate([chunk[0] for chunk in chunks])
    position = np.concatenate([chunk[1] for chunk in chunks])
    keys = np.concatenate([chunk[2] for chunk in chunks])
    xz = position[:, [0, 2]].astype(np.float64) / 100
    lo, hi = xz.min(axis=0), xz.max(axis=0)
    span = np.maximum(hi - lo, 1.0)
    scale = min(850 / span[0], 390 / span[1])
    xy = (xz - (lo + hi) / 2) * [scale, -scale] + [500, 265]
    stride = max(1, len(xy) // 5000)
    points = " ".join(f"{x:.2f},{y:.2f}" for x, y in xy[::stride])
    duration_s = (int(time_ms[-1]) - int(time_ms[0]) + PERIOD_MS) / 1000
    title = html.escape(_samples_path(path).parent.name)
    svg = ['<svg xmlns="http://www.w3.org/2000/svg" width="1000" height="840" viewBox="0 0 1000 840">',
           '<style>text{font-family:system-ui,sans-serif;fill:#e2e8f0} .small{font-size:12px} .label{font-size:14px}</style>',
           '<rect width="1000" height="840" fill="#0f172a"/>',
           f'<text x="36" y="32" font-size="21">{title}</text>',
           f'<text x="36" y="54" class="small">{len(keys)} decisions · {duration_s:.1f} s · X/Z metres</text>',
           f'<polyline points="{points}" fill="none" stroke="#38bdf8" stroke-width="2"/>']
    for point, color, label in ((xy[0], "#4ade80", "start"), (xy[-1], "#fb7185", "end")):
        svg.append(f'<circle cx="{point[0]:.2f}" cy="{point[1]:.2f}" r="5" fill="{color}"/>')
        svg.append(f'<text x="{point[0]+8:.2f}" y="{point[1]-8:.2f}" class="small">{label}</text>')
    length_m = np.linalg.norm(np.diff(xz, axis=0), axis=1).sum()
    svg.append(f'<text x="36" y="487" class="small">XZ path {length_m:.2f} m · extent {span[0]:.1f} × {span[1]:.1f} m</text>')
    bar_x, bar_w, row_h = 130, 830, 29
    masks = key_masks(keys)
    palette = {"forward": "#38bdf8", "walk": "#4ade80", "run": "#fbbf24",
               "turn_left": "#c084fc", "turn_right": "#a78bfa", "jump": "#fb7185", "climb": "#fb923c"}
    for row, (name, color) in enumerate(palette.items()):
        y = 520 + row * row_h
        svg.append(f'<text x="36" y="{y+15}" class="label">{name}</text>')
        svg.append(f'<rect x="{bar_x}" y="{y}" width="{bar_w}" height="19" fill="#1e293b"/>')
        mask = masks[name]
        edges = np.diff(np.concatenate(([False], mask, [False])).astype(np.int8))
        for begin, end in zip(np.flatnonzero(edges == 1), np.flatnonzero(edges == -1)):
            x = bar_x + bar_w * begin / len(mask)
            width = bar_w * (end - begin) / len(mask)
            svg.append(f'<rect x="{x:.2f}" y="{y}" width="{width:.2f}" height="19" fill="{color}"/>')
    for fraction in np.linspace(0, 1, 6):
        x = bar_x + bar_w * fraction
        svg.append(f'<text x="{x:.2f}" y="750" text-anchor="middle" class="small">{duration_s*fraction:.1f}s</text>')
    svg.extend(['<text x="36" y="790" class="small">Green = start · pink = end · decisions every 100 ms</text>', '</svg>'])
    Path(destination).write_text("\n".join(svg) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    for command in ("check", "summary"):
        sub = commands.add_parser(command)
        sub.add_argument("dataset", type=Path)
        sub.add_argument("--output", type=Path, help="also save full JSON summary")
    render = commands.add_parser("render")
    render.add_argument("episode", type=Path)
    render.add_argument("output", type=Path)
    args = parser.parse_args()
    try:
        if args.command == "render":
            render_episode(args.episode, args.output)
            print(args.output)
        else:
            result = summarize(args.dataset, validate=args.command == "check")
            if args.output:
                args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
            public = {key: value for key, value in result.items() if key != "episode_details"}
            print(json.dumps(public, indent=2))
    except (OSError, ValueError, EOFError, KeyError) as error:
        print(f"nav_demos: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
