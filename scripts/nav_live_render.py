#!/usr/bin/env python3
"""Render factual nav-live diagnostics from one recorded game run.

Requires numpy, Pillow and ffmpeg. The maps and eye rays are the recorder's
actual samples; this script never advances the game or evaluates a policy.
"""
from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess

import numpy as np
from PIL import Image, ImageDraw, ImageFont


MAGIC = b"NAVLIVE\0"
HEADER = struct.Struct("<8sI")
FRAME = struct.Struct("<QQ")
RAY_DTYPE = np.dtype([
    ("origin_cm", "<f4", (3,)), ("point_cm", "<f4", (3,)),
    ("direction", "<f4", (3,)), ("distance_cm", "<f4"),
    ("normal_y", "<f4"), ("kind", "u1"), ("hit", "u1"),
    ("complete", "u1"), ("age_ms", "<u4"), ("near_age_ms", "<u4"),
])
CLASS_NAMES = ("unknown", "free", "wall", "water", "cliff / ledge", "familiar", "movable")
CLASS_COLORS = np.array([
    (42, 46, 54), (162, 187, 163), (195, 158, 132), (61, 133, 205),
    (201, 97, 112), (166, 135, 205), (237, 186, 70),
], dtype=np.uint8)
PATH_COLORS = {"NPC path": "#f9f2bf", "teacher": "#55d9c8", "Detour": "#f291d4"}
KEY_NAMES = ("forward", "strafe", "turn", "walk", "jump", "climb")
RAW_FILES = ("episode.json", "live-format.json", "live-decisions.jsonl", "live-observations.bin.gz")
THRESHOLDS = {"standing_speed_cm_s": 20, "circling_speed_cm_s": 20,
              "circling_turn_abs_min": .01, "memory_class_probability_min": .1}


def read_json(path: Path):
    return json.loads(path.read_text(encoding="utf-8"))


def json_rows(path: Path) -> list[dict]:
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines() if line.strip()]


def load_trace(directory: Path) -> tuple[dict, list[dict]]:
    metadata = read_json(directory / "live-format.json")
    if metadata.get("schema") != 1:
        raise ValueError("Unsupported nav-live trace schema")
    decisions = json_rows(directory / "live-decisions.jsonl")
    if not decisions:
        raise ValueError("Nav-live trace has no decisions")
    for i, row in enumerate(decisions):
        if row["decision_index"] != i or (i and row["elapsed_ms"] <= decisions[i-1]["elapsed_ms"]):
            raise ValueError("Nav-live decisions are not strictly ordered")
        for field in ("position_cm", "goal_cm", "carrot_cm"):
            values = np.asarray(row[field], dtype=float)
            if values.shape != (3,) or not np.isfinite(values).all():
                raise ValueError(f"Invalid nav-live {field}")
        for name, probabilities in row["probabilities"].items():
            p = np.asarray(probabilities, dtype=float)
            selected = row["selected"][name]
            if p.ndim != 1 or not len(p) or not np.isfinite(p).all() or (p < 0).any() or not np.isclose(p.sum(), 1, atol=2e-5):
                raise ValueError(f"Invalid nav-live probabilities for {name}")
            if not isinstance(selected, int) or selected < 0 or selected >= len(p):
                raise ValueError(f"Invalid nav-live selected index for {name}")
            if not np.isclose(p[selected], row["chosen_probability"][name], atol=2e-5):
                raise ValueError(f"Nav-live selected probability differs for {name}")
    return metadata, decisions


def _read_exact(stream, count: int) -> bytes:
    value = stream.read(count)
    if len(value) != count:
        raise ValueError("Truncated nav-live observation stream")
    return value


def observation_frames(directory: Path, metadata: dict):
    maps = metadata["maps"]
    shapes = {name: tuple(maps[name]["shape"]) for name in ("near", "far")}
    sizes = {name: math.prod(shape) for name, shape in shapes.items()}
    for name, shape in shapes.items():
        if len(shape) != 3 or any(d <= 0 for d in shape) or shape[0] != len(maps[name]["channels"]) or sizes[name] > 1_000_000:
            raise ValueError(f"Invalid nav-live {name} map shape")
    ray_count = int(metadata["ray_count"])
    if not 0 < ray_count <= 3000:
        raise ValueError("Invalid nav-live ray count")
    with gzip.open(directory / "live-observations.bin.gz", "rb") as stream:
        if HEADER.unpack(_read_exact(stream, HEADER.size)) != (MAGIC, 1):
            raise ValueError("Unsupported nav-live observation header")
        previous = -1
        while header := stream.read(FRAME.size):
            if len(header) != FRAME.size:
                raise ValueError("Truncated nav-live observation frame")
            elapsed, index = FRAME.unpack(header)
            if elapsed <= previous or (previous >= 0 and elapsed - previous > 500):
                raise ValueError("Nav-live observations are not ordered at >=2Hz")
            if previous < 0 and elapsed != 0:
                raise ValueError("Nav-live observations must start with the first decision")
            previous = elapsed
            result = {"elapsed_ms": elapsed, "decision_index": index}
            for name in ("near", "far"):
                result[name] = np.frombuffer(_read_exact(stream, sizes[name]), dtype=np.uint8).reshape(shapes[name])
            result["rays"] = np.frombuffer(_read_exact(stream, ray_count * RAY_DTYPE.itemsize), dtype=RAY_DTYPE)
            for name in ("origin_cm", "point_cm", "direction", "distance_cm", "normal_y"):
                if not np.isfinite(result["rays"][name]).all():
                    raise ValueError("Non-finite nav-live eye ray")
            yield result


def timed_observations(directory: Path, metadata: dict, elapsed_ms: int):
    previous = None
    for frame in observation_frames(directory, metadata):
        if previous is not None:
            yield previous, frame["elapsed_ms"]-previous["elapsed_ms"]
        previous = frame
    if previous is not None:
        duration = elapsed_ms-previous["elapsed_ms"]
        if duration < 0 or duration > 500:
            raise ValueError("Nav-live observations do not cover the final decision window")
        yield previous, duration


def memory_classes(values: np.ndarray, metadata: dict) -> np.ndarray:
    """Classify recorded probability channels, keeping hazards over familiarity."""
    channels = {name: values[i].astype(float) / 255 for i, name in enumerate(metadata["channels"])}
    zero = np.zeros(values.shape[1:])
    scores = np.stack([channels.get("free", zero), channels.get("wall", zero),
                       channels.get("water", zero),
                       np.maximum(channels.get("ledge", zero), channels.get("drop", zero)),
                       channels.get("movable", zero)])
    result = np.array([1, 2, 3, 4, 6], dtype=np.uint8)[scores.argmax(axis=0)]
    result[scores.max(axis=0) < THRESHOLDS["memory_class_probability_min"]] = 0
    familiar = channels.get("familiar", zero) >= THRESHOLDS["memory_class_probability_min"]
    result[familiar & ((result == 0) | (result == 1))] = 5
    return result


def _path(directory: Path | None, decisions: list[dict] | None = None) -> np.ndarray:
    if directory is None:
        return np.empty((0, 3))
    trajectory = directory / "trajectory.txt"
    if trajectory.exists():
        rows = np.loadtxt(trajectory, ndmin=2)
        if rows.shape[1] >= 4:
            return rows[:, 1:4]
    if decisions is None:
        decisions = json_rows(directory / "live-decisions.jsonl")
    return np.array([row["position_cm"] for row in decisions], dtype=float)


def _path_length(path: np.ndarray) -> float:
    return float(np.linalg.norm(np.diff(path, axis=0), axis=1).sum()) if len(path) > 1 else 0.


def _entries(values) -> int:
    return sum(bool(value) and (i == 0 or not bool(values[i-1])) for i, value in enumerate(values))


def summarize(directory: Path, metadata: dict, decisions: list[dict], teacher_directory: Path | None = None, scenario: dict | None = None) -> dict:
    episode = read_json(directory / "episode.json")
    elapsed_ms = int(episode["elapsed_ms"])
    times = np.array([row["elapsed_ms"] for row in decisions], dtype=float)
    if elapsed_ms < times[-1]:
        raise ValueError("Nav-live episode ends before its final decision")
    dt = np.diff(np.r_[times, elapsed_ms]) / 1000
    speeds = np.array([row["speed_cm_s"] for row in decisions], dtype=float)
    turns = np.array([row["keys"]["turn"] for row in decisions], dtype=float)
    standing = speeds < THRESHOLDS["standing_speed_cm_s"]
    circling = (speeds < THRESHOLDS["circling_speed_cm_s"]) & (np.abs(turns) > THRESHOLDS["circling_turn_abs_min"])
    travelled = float(episode.get("travel_cm", _path_length(_path(directory, decisions))))
    teacher_length = None
    if teacher_directory is not None:
        teacher_episode = read_json(teacher_directory / "episode.json")
        teacher_length = float(teacher_episode.get("travel_cm", _path_length(_path(teacher_directory))))
    safety_path = directory / "perception-safety-frames.jsonl"
    safety = json_rows(safety_path) if safety_path.exists() else []
    deep = [row["water_depth_cm"] + 1 >= row["deep_water_threshold_cm"] or row.get("swimming", False) for row in safety]
    falling = [bool(row["falling"]) for row in safety]
    contacts = [row.get("contact", False) for row in decisions]
    obstacles = [row["body_blocked"] for row in decisions]
    intervention = [row.get("safety_intervention", False) for row in decisions]
    def minimum(field):
        values = [row[field] for row in decisions if row.get(field) is not None]
        return min(values) if values else None
    def clearance(name):
        values = [(row[f"{name}_clearance_cm"], row[f"{name}_clearance_censored"])
                  for row in decisions if row.get(f"{name}_clearance_cm") is not None]
        if not values:
            return None, None
        nearest = min(value for value, _ in values)
        return nearest, all(censored for value, censored in values if value == nearest)
    edge_distance, edge_censored = clearance("edge")
    water_distance, water_censored = clearance("water")
    following = episode.get("goal_kind") == "follow" or (scenario or {}).get("kind") == "follow"
    # Measure the entire follow window, including first contact and pauses.
    distances = np.array([np.linalg.norm(np.asarray(row["goal_cm"])-row["position_cm"]) for row in decisions])
    in_band = (distances >= 200) & (distances <= 500)
    summary = {
        "schema": 1, "success": bool(episode["success"]), "elapsed_seconds": elapsed_ms / 1000,
        "path_length_m": travelled / 100, "teacher_path_length_m": None if teacher_length is None else teacher_length / 100,
        "path_to_teacher_ratio": travelled / teacher_length if teacher_length else None,
        "standing_seconds": float(dt[standing].sum()), "circling_seconds": float(dt[circling].sum()),
        "obstacle_contacts": _entries(obstacles), "cv_contact_episodes": _entries(contacts),
        "safety_interventions": _entries(intervention), "stuck_seconds": float(sum(duration for duration, row in zip(dt, decisions) if row["stuck"])),
        "deep_water_entries": _entries(deep) if safety else None,
        "falls": _entries(falling) if safety else None,
        "minimum_edge_distance_cm": edge_distance, "minimum_edge_distance_censored": edge_censored,
        "minimum_water_distance_cm": water_distance, "minimum_water_distance_censored": water_censored,
        "clearance_probe": metadata.get("clearance_probe"),
        "minimum_observed_edge_distance_cm": minimum("observed_edge_distance_cm"),
        "minimum_observed_water_distance_cm": minimum("observed_water_distance_cm"),
        "follow_band_fraction": float(dt[in_band].sum() / dt.sum()) if following and dt.sum() else None,
        "decision_count": len(decisions), "thresholds": THRESHOLDS,
        "metric_sources": {
            "path_length": "episode.travel_cm or recorded trajectory (3D)",
            "standing_and_circling": "decision speed; circling also requires nonzero turn",
            "obstacle_contacts": "rising edges of native body_blocked at decision frequency",
            "deep_water_and_falls": "physical perception-safety frames; null when absent",
            "minimum_distances": "minimum sampled native radial probe clearance; censor=true means >=probe radius; null when unavailable; probe geometry/resolution in clearance_probe",
            "follow_band": "whole elapsed run, including initial gap and target pauses; 3D goal distance",
        },
        "trace_sha256": {name: file_hash(directory / name) for name in RAW_FILES},
    }
    return summary


def file_hash(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024*1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _font(size: int):
    for path in ("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/System/Library/Fonts/Supplemental/Arial.ttf"):
        if Path(path).exists():
            return ImageFont.truetype(path, size)
    return ImageFont.load_default(size=size)


def _polyline(draw, points, color, width=2):
    if len(points) >= 2:
        draw.line([tuple(point) for point in points], fill=color, width=width)


def render_timeline(directory: Path, decisions: list[dict], elapsed_ms: int) -> None:
    width, left, right = 1280, 150, 1240
    heads = list(decisions[0]["probabilities"])
    labels = ["forward / back", "strafe", "turn", "walk", "jump", "climb"] + [f"P({head})" for head in heads] + ["speed (m/s)", "stuck", "safety", "obstacle / contact"]
    row_h, top = 45, 65
    image = Image.new("RGB", (width, top + row_h * len(labels) + 50), "#171c25")
    draw = ImageDraw.Draw(image)
    font, small = _font(16), _font(13)
    draw.text((20, 14), "Recorded actions and selected head probabilities", fill="white", font=_font(21))
    times = np.array([row["elapsed_ms"] for row in decisions])
    x = left + times * (right-left) / max(elapsed_ms, 1)
    for i, label in enumerate(labels):
        y = top + i*row_h
        draw.text((15, y+7), label, fill="#d9e1ec", font=small)
        draw.line((left, y+row_h-3, right, y+row_h-3), fill="#333b49")
    values = [np.array([float(row["keys"][key]) for row in decisions]) for key in KEY_NAMES]
    values += [np.array([row["chosen_probability"][head] for row in decisions]) for head in heads]
    values += [np.array([row["speed_cm_s"] / 100 for row in decisions])]
    for i, data in enumerate(values):
        y = top + i*row_h
        if i < 3:
            normalized = (data+1) / 2
        elif i == len(values)-1:
            normalized = data/max(1., float(data.max()))
            draw.text((right-65, y+1), f"max {data.max():.2f}", fill="#a5bacd", font=small)
        else:
            normalized = data
        points = list(zip(x.tolist(), (y+row_h-8 - normalized*(row_h-16)).tolist()))
        _polyline(draw, points, "#a4dcf2", 2)
    for offset, key in enumerate(("stuck", "safety_intervention", "body_blocked")):
        y = top + (len(values)+offset)*row_h
        for i, row in enumerate(decisions):
            if row.get(key) or (key == "body_blocked" and row.get("contact")):
                end = left + (times[i+1] if i+1 < len(times) else elapsed_ms) * (right-left) / max(elapsed_ms, 1)
                draw.rectangle((float(x[i]), y+7, max(float(x[i])+1, end), y+row_h-8), fill=("#edac5c" if key == "stuck" else "#e47883"))
    for tick in np.linspace(0, elapsed_ms, 7):
        px = left+tick*(right-left)/max(elapsed_ms, 1)
        draw.line((px, top-10, px, image.height-35), fill="#4a5362", width=1)
        draw.text((px-15, image.height-27), f"{tick/1000:.1f}s", fill="#d9e1ec", font=small)
    temporary = directory / "timeline.tmp.png"
    image.save(temporary)
    temporary.replace(directory / "timeline.png")


def _map_image(values, map_metadata):
    return Image.fromarray(CLASS_COLORS[memory_classes(values, map_metadata)], "RGB").convert("RGBA")


def _world_map(values, map_metadata, row, centre, scale, size):
    image = _map_image(values, map_metadata)
    yaw = math.radians(row["yaw_deg"])
    sine, cosine = math.sin(yaw), math.cos(yaw)
    cell = map_metadata["cell_cm"]
    width, height = size
    dx, dz = centre[0] - row["position_cm"][0], centre[1] - row["position_cm"][2]
    # Inverse affine transform: each screen pixel -> world X/Z -> body right/forward -> cell.
    a, b = -sine / scale / cell, -cosine / scale / cell
    d, e = cosine / scale / cell, -sine / scale / cell
    c = (-dx*sine+dz*cosine-map_metadata["min_right_cm"])/cell-.5-a*width/2-b*height/2
    f = (dx*cosine+dz*sine-map_metadata["min_forward_cm"])/cell-.5-d*width/2-e*height/2
    return image.transform(size, Image.Transform.AFFINE, (a,b,c,d,e,f), Image.Resampling.NEAREST, fillcolor=(0,0,0,0))


def render_topdown(directory: Path, metadata: dict, decisions: list[dict], teacher_directory: Path | None = None) -> int:
    trajectory = _path(directory, decisions)
    teacher = _path(teacher_directory)
    detour = np.asarray(metadata.get("detour_cm", []), dtype=float).reshape(-1, 3)
    points = np.vstack([path[:, [0, 2]] for path in (trajectory, teacher, detour) if len(path)] + [np.array([row["goal_cm"] for row in decisions])[:, [0, 2]]])
    centre = (points.min(axis=0)+points.max(axis=0))/2
    span = max(2400., float(np.ptp(points, axis=0).max())+1200)
    map_size = (720, 640)
    scale = min(map_size)/span
    def screen(point):
        return (float((point[0]-centre[0])*scale+map_size[0]/2), float((centre[1]-point[2])*scale+map_size[1]/2+50))
    output = directory / "topdown.tmp.mp4"
    command = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", "960x720", "-r", "20", "-i", "-", "-an", "-c:v", "libx264", "-preset", "veryfast", "-crf", "25", "-pix_fmt", "yuv420p", "-movflags", "+faststart", str(output)]
    frames = 0
    elapsed_ms = int(read_json(directory / "episode.json")["elapsed_ms"])
    with subprocess.Popen(command, stdin=subprocess.PIPE) as encoder:
        try:
            for frame, duration in timed_observations(directory, metadata, elapsed_ms):
                index = frame["decision_index"]
                if index >= len(decisions) or decisions[index]["elapsed_ms"] != frame["elapsed_ms"]:
                    raise ValueError("Nav-live observation does not match its decision")
                row = decisions[index]
                image = Image.new("RGB", (960,720), "#151a23")
                canvas = Image.new("RGBA", map_size, (*CLASS_COLORS[0],255))
                for name in ("far", "near"):
                    canvas.alpha_composite(_world_map(frame[name], metadata["maps"][name], row, centre, scale, map_size))
                image.paste(canvas.convert("RGB"), (0,50))
                draw = ImageDraw.Draw(image)
                draw.text((14, 13), f"{frame['elapsed_ms']/1000:.1f}s   x2   yaw {row['yaw_deg']:.0f} deg", fill="white", font=_font(19))
                for name, path in (("Detour", detour), ("teacher", teacher), ("NPC path", np.array([item["position_cm"] for item in decisions[:index+1]]))):
                    _polyline(draw, [screen(p) for p in path], PATH_COLORS[name], 3 if name == "NPC path" else 2)
                for ray in frame["rays"]:
                    if ray["hit"]:
                        color = "#6cb3f0" if ray["kind"] == 5 else "#c4cbd5"
                        draw.line([screen(ray["origin_cm"]), screen(ray["point_cm"])], fill=color, width=1)
                px, py = screen(row["position_cm"])
                yaw = math.radians(row["yaw_deg"])
                forward = np.array([math.cos(yaw), -math.sin(yaw)])
                right = np.array([-forward[1], forward[0]])
                triangle = [np.array([px,py])+forward*10, np.array([px,py])-forward*6+right*5, np.array([px,py])-forward*6-right*5]
                draw.polygon([tuple(p) for p in triangle], fill="#ffffff", outline="#111111")
                for key, color in (("goal_cm", "#ff694f"), ("carrot_cm", "#ffdd55")):
                    gx, gy = screen(row[key])
                    draw.ellipse((gx-4,gy-4,gx+4,gy+4), fill=color)
                draw.text((745,15), "Actual near input", fill="white", font=_font(17))
                near = _map_image(frame["near"], metadata["maps"]["near"])
                near = near.transpose(Image.Transpose.FLIP_TOP_BOTTOM).resize((216,216), Image.Resampling.NEAREST)
                image.paste(near.convert("RGB"), (735,50))
                draw.text((740,273), "Body forward = up", fill="#becada", font=_font(13))
                y = 303
                for name, color in zip(CLASS_NAMES, CLASS_COLORS):
                    draw.rectangle((741,y,754,y+13), fill=tuple(color))
                    draw.text((763,y-1),name,fill="#e0e5ec",font=_font(14)); y += 27
                y += 8
                for name, color in PATH_COLORS.items():
                    draw.line((741,y+7,756,y+7),fill=color,width=3)
                    draw.text((763,y),name,fill="#e0e5ec",font=_font(14)); y += 27
                draw.text((740,y+5),"Goal: red / carrot: yellow",fill="#e0e5ec",font=_font(13))
                draw.text((15,697),f"World X/Z; span {span/100:.1f}m; rays: recorded input hit positions",fill="#becada",font=_font(13))
                # Hold each actual sample until the next timestamp. Ten 20fps
                # output frames represent one second of simulation (speed x2).
                begin_frame = math.ceil(frame["elapsed_ms"]/100)
                end_frame = math.ceil((frame["elapsed_ms"]+duration)/100)
                for _ in range(end_frame-begin_frame):
                    encoder.stdin.write(image.tobytes())
                frames += 1
            if not frames:
                raise ValueError("Nav-live trace has no observation frames")
        except BaseException:
            encoder.stdin.close()
            encoder.terminate()
            encoder.wait()
            output.unlink(missing_ok=True)
            raise
        encoder.stdin.close()
        if encoder.wait() != 0:
            output.unlink(missing_ok=True)
            raise RuntimeError("ffmpeg failed to encode nav-live topdown")
    output.replace(directory / "topdown.mp4")
    return frames


def render_run(directory, teacher_directory=None, scenario=None) -> dict:
    directory = Path(directory)
    teacher_directory = Path(teacher_directory) if teacher_directory is not None else None
    metadata, decisions = load_trace(directory)
    summary = summarize(directory, metadata, decisions, teacher_directory, scenario)
    frames = render_topdown(directory, metadata, decisions, teacher_directory)
    summary["observation_frames"] = frames
    render_timeline(directory, decisions, int(summary["elapsed_seconds"]*1000))
    temporary = directory / "summary.tmp.json"
    temporary.write_text(json.dumps(summary, indent=2, allow_nan=False)+"\n", encoding="utf-8")
    temporary.replace(directory / "summary.json")
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--teacher", type=Path)
    args = parser.parse_args()
    print(json.dumps(render_run(args.directory, args.teacher), allow_nan=False))


if __name__ == "__main__":
    main()
