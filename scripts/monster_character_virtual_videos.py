#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["pillow>=11,<13"]
# ///
"""Compose native monster CV takes and inspect their numerical evidence.

uv run scripts/monster_character_virtual_videos.py compose --off OFF.mp4 --on ON.mp4 --out pair.mp4
python3 scripts/monster_character_virtual_videos.py metrics --off OFF_DIR --on ON_DIR
python3 scripts/monster_character_virtual_videos.py perf --off OFF_DIR --on ON_DIR

Composition preserves both complete images and normal playback speed. The contact
sheet samples each second with OFF above ON, in blocks of five seconds. Geometry
penetrations require viewing those images; numerical output never assumes zero.
"""

import argparse
import csv
import json
import math
import subprocess
import sys
import tempfile
from collections import defaultdict
from pathlib import Path


TITLES = {
    "wolf_terrain": "Wolf chase: slope and steps",
    "wolf_pack": "Three wolves: rocky ground",
    "troll_props": "Troll: barrel and crate",
    "cliff": "Molerats: walkable slope and cliff",
    "wolf_water": "Wolf: enter, swim, leave water",
    "fieldraider_bloodfly": "Fieldraider charge + Bloodfly control",
}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def run(command):
    result = subprocess.run([str(part) for part in command], capture_output=True, text=True)
    if result.returncode:
        raise ValueError(f"{command[0]} failed: {result.stderr.strip()[-3000:]}")
    return result.stdout


def foliage(args):
    """Transparent leaf-only texture overrides; collision meshes stay intact."""
    from PIL import Image
    names = (
        "TREE_NEEDLE_02", "TREE_NEEDLE_03", "BRANCH_01", "BRANCH_02",
        "BRANCH_03", "BRANCH_06", "BRANCH_07", "FARN_01", "LEAVE_01",
        "LEAVE_03", "GRASS_DECAL_01", "HOUSEGRASS_DECAL_01",
    )
    args.out.mkdir(parents=True, exist_ok=True)
    for name in names:
        Image.new("RGBA", (4, 4), (0, 0, 0, 0)).save(args.out / f"NW_NATURE_{name}.png")
    return {"directory": str(args.out.resolve()), "transparent_textures": len(names),
            "purpose": "Leaf/grass render overrides for matching OFF/ON bench takes; physics unchanged"}


def probe(path):
    data = json.loads(run([
        "ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
        "stream=width,height,r_frame_rate,duration:format=duration", "-of", "json", path,
    ]))
    require(data.get("streams"), f"No video stream: {path}")
    video = data["streams"][0]
    return {"width": int(video["width"]), "height": int(video["height"]),
            "duration_s": float(video.get("duration") or data["format"]["duration"]),
            "frame_rate": video["r_frame_rate"]}


def font(size, supplied=None):
    from PIL import ImageFont
    candidates = [supplied] if supplied else [
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf",
    ]
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return ImageFont.truetype(str(candidate), size)
    require(supplied is None, f"Font does not exist: {supplied}")
    return ImageFont.load_default(size=size)


def centered(draw, box, text, face, color="white"):
    left, top, right, bottom = box
    bounds = draw.textbbox((0, 0), text, font=face)
    draw.text(((left + right - bounds[2] - bounds[0]) / 2,
               (top + bottom - bounds[3] - bounds[1]) / 2), text, font=face, fill=color)


def compose(args):
    from PIL import Image, ImageDraw

    off, on, output = (path.resolve() for path in (args.off, args.on, args.out))
    require(output.suffix.lower() == ".mp4", "--out must end in .mp4")
    require(off != on and output not in (off, on), "Input and output paths must differ")
    before, after = probe(off), probe(on)
    duration = args.duration if args.duration is not None else min(20, before["duration_s"], after["duration_s"])
    require(10 <= duration <= 20, "Both takes must supply 10–20 seconds at normal speed")
    require(duration <= min(before["duration_s"], after["duration_s"]) + 0.001,
            "Requested duration exceeds an input; repeated/frozen frames are not allowed")
    crop, crop_filter = None, ""
    views = (before, after)
    if args.crop:
        crop = tuple(int(part) for part in args.crop.split(":"))
        require(len(crop) == 4, "--crop expects X:Y:W:H in integer pixels")
        x, y, width, height = crop
        require(x >= 0 and y >= 0 and width > 0 and height > 0 and width % 2 == height % 2 == 0,
                "--crop needs nonnegative X/Y and positive even W/H")
        require(all(x + width <= row["width"] and y + height <= row["height"] for row in views),
                "--crop rectangle must fit completely inside both source videos")
        crop_filter = f"crop={width}:{height}:{x}:{y}:exact=1,"
        views = ({"width": width, "height": height},) * 2
    panel_width = max(640, *(row["width"] for row in views))
    panel_width += panel_width % 2
    panel_height = max(round(panel_width * row["height"] / row["width"]) for row in views)
    panel_height += panel_height % 2
    title = args.title or TITLES.get(output.stem, output.stem.replace("-", " ").replace("_", " "))
    output.parent.mkdir(parents=True, exist_ok=True)
    sheet = output.with_name(output.stem + "-frames.png")
    with tempfile.TemporaryDirectory(prefix="monster-cv-", dir=output.parent) as temporary:
        temporary = Path(temporary)
        banner = Image.new("RGB", (2 * panel_width, 72), "#111820")
        draw = ImageDraw.Draw(banner)
        face = font(24, args.font)
        while draw.textbbox((0, 0), title, font=face)[2] > banner.width - 32:
            require(face.size > 10, "Title is too long")
            face = font(face.size - 1, args.font)
        centered(draw, (0, 0, banner.width, 36), title, face)
        for index, mode in enumerate(("OFF", "ON")):
            centered(draw, (index * panel_width, 36, (index + 1) * panel_width, 72),
                     mode, font(26, args.font), "#a8cfff" if index == 0 else "#b4eab5")
        banner_path = temporary / "banner.png"
        banner.save(banner_path)
        panels = ";".join(
            f"[{index}:v]{crop_filter}trim=duration={duration:.6f},setpts=PTS-STARTPTS,fps=30,"
            f"scale={panel_width}:{panel_height}:force_original_aspect_ratio=decrease,"
            f"pad={panel_width}:{panel_height}:(ow-iw)/2:(oh-ih)/2,setsar=1[v{index}]"
            for index in range(2)
        )
        graph = (panels + ";[v0][v1]hstack=inputs=2:shortest=1[pair];"
                 "[2:v][pair]vstack=inputs=2:shortest=1,format=yuv420p[out]")
        run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", off,
             "-i", on, "-loop", "1", "-framerate", "30", "-i", banner_path,
             "-filter_complex", graph, "-map", "[out]", "-an", "-t", f"{duration:.6f}",
             "-c:v", "libx264", "-preset", "fast", "-crf", "18", "-movflags", "+faststart",
             "-metadata", f"title={title} (OFF left / ON right)", output])

        # Extract the source frames, without the video title band, once per second.
        samples = math.ceil(duration - 1e-6)
        cell_width, band = 320, 28
        cell_height = round(panel_height * cell_width / panel_width)
        for mode, source in (("off", off), ("on", on)):
            folder = temporary / mode
            folder.mkdir()
            run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", source,
                 "-vf", crop_filter + f"fps=1:start_time=0:round=up,scale={cell_width}:{cell_height}:"
                 "force_original_aspect_ratio=decrease,"
                 f"pad={cell_width}:{cell_height}:(ow-iw)/2:(oh-ih)/2",
                 "-frames:v", str(samples), folder / "%03d.png"])
            require(len(list(folder.glob("*.png"))) == samples,
                    f"Expected {samples} one-second samples from {source}")
        row_height = band + cell_height
        canvas = Image.new("RGB", (cell_width * 5, 44 + 2 * row_height * math.ceil(samples / 5)), "#111820")
        draw = ImageDraw.Draw(canvas)
        centered(draw, (0, 0, canvas.width, 44), title, font(24, args.font))
        for second in range(samples):
            block, column = divmod(second, 5)
            for index, mode in enumerate(("off", "on")):
                x, y = column * cell_width, 44 + (block * 2 + index) * row_height
                centered(draw, (x, y, x + cell_width, y + band),
                         f"{mode.upper()}  {second:02d} s", font(18, args.font))
                with Image.open(temporary / mode / f"{second + 1:03d}.png") as frame:
                    canvas.paste(frame, (x, y + band))
        canvas.save(sheet)
    final = probe(output)
    require(final["width"] >= 1280 and final["height"] >= 480, "Output is below 1280×480")
    require(10 <= final["duration_s"] <= 20.04 and abs(final["duration_s"] - duration) <= 1 / 30 + .002,
            "Encoded duration differs from requested native-speed interval")
    return {"video": str(output), "sheet": str(sheet), "title": title, **final,
            "samples_s": list(range(samples)), "crop_xywh": crop,
            "off": str(off), "on": str(on)}


def finite(value):
    result = float(value)
    require(math.isfinite(result), f"Non-finite number: {value}")
    return result


def position(value):
    if isinstance(value, dict):
        value = [value[key] for key in ("x", "y", "z")]
    require(isinstance(value, list) and len(value) == 3, f"Invalid position: {value}")
    return [finite(part) for part in value]


def frame_summary(frames):
    actors = defaultdict(list)
    for row in frames:
        actors[row["npc_id"]].append(row)
    deltas = []
    for rows in actors.values():
        for a, b in zip(rows, rows[1:]):
            require(finite(b["t_game_ms"]) > finite(a["t_game_ms"]), "Non-monotonic native actor CSV time")
            deltas.append(abs(finite(b["visual_y_cm"]) - finite(a["visual_y_cm"])))
    return {"csv_frames": len(frames), "csv_actors": len(actors),
            "csv_max_visual_jump_cm": max(deltas, default=None),
            "visible_geometry_penetrations": None}


def events(directory):
    return [json.loads(line) for line in (directory / "events.jsonl").read_text().splitlines() if line.strip()]


def complete(directory):
    log = directory / "bench.log"
    return log.is_file() and "done" in log.read_text().splitlines()


def load(directory, enabled):
    journal = events(directory)
    with (directory / "monster-character-frames.csv").open(newline="") as source:
        frames = list(csv.DictReader(source))
    require(frames, f"No native frames: {directory}")
    errors = [row for row in journal if row.get("kind") == "monster_character_error"]
    for row in frames:
        require(int(row["cv_enabled"]) == int(enabled), f"Wrong CSV controller mode: {directory}")
        require(enabled or not int(row["cv_active"]), f"OFF actor uses CV: {directory}")
    ends = {}
    for end in (row for row in journal if row.get("kind") == "monster_character_route_end"):
        name = end["route"]
        require(name not in ends, f"Repeated route end cannot be paired unambiguously: {name}")
        require(end.get("cv_enabled") is enabled, f"Wrong controller mode in {name}: {directory}")
        selected = [row for row in frames if row["route"] == name]
        require(selected, f"Missing CSV frames for {name}: {directory}")
        ends[name] = {key: end.get(key) for key in (
            "success", "elapsed_ms", "window_ms", "stalls_over_1s", "max_visual_jump_cm",
            "max_physical_jump_cm", "falls", "cv_frames", "swim_frames", "saw_swim", "left_water",
            "start_cm", "goal_cm", "end_cm", "attack_distance_cm", "attack_duration_ms",
            "charge_collision_callbacks", "barrel_displacement_cm",
        )}
        begins = [row for row in journal if row.get("kind") == "monster_character_route_begin" and row.get("route") == name]
        ends[name]["actual_start_cm"] = begins[0].get("start_cm") if len(begins) == 1 else None
        ends[name].update(frame_summary(selected))
        primary_id = int(selected[0]["npc_id"])
        ends[name]["ground_cv_actors"] = [{
            "npc_id": primary_id, "roles": ["primary"], "csv_frames": len(selected),
            "cv_frames": sum(int(row["cv_active"]) for row in selected),
        }]
        ends[name]["actors"] = []
        for actor in (row for row in journal if row.get("kind") == "monster_character_actor_end" and row["route"] == name):
            actor = {key: value for key, value in actor.items() if key not in ("kind", "t", "seg", "frame")}
            actor_frames = [row for row in frames if row["route"] == name + "_" + actor["role"]]
            require(actor_frames, f"No native frames for extra actor {name}/{actor['role']}")
            actor.update(frame_summary(actor_frames))
            ends[name]["actors"].append(actor)
            if actor["role"] != "bloodfly":
                if int(actor["npc_id"]) == primary_id:
                    # The pack's first wolf is also recorded as the primary.
                    ends[name]["ground_cv_actors"][0]["roles"].append(actor["role"])
                else:
                    ends[name]["ground_cv_actors"].append({
                        "npc_id": int(actor["npc_id"]), "roles": [actor["role"]],
                        "csv_frames": len(actor_frames),
                        "cv_frames": sum(int(row["cv_active"]) for row in actor_frames),
                    })
        ends[name]["props"] = [{key: value for key, value in row.items() if key not in ("kind", "t", "seg", "frame")}
                                for row in journal if row.get("kind") == "monster_character_prop_end" and row["route"] == name]
    return {"directory": str(directory.resolve()), "routes": ends, "native_errors": errors,
            "events": journal, "frames": frames, "bench_complete": complete(directory)}


def fixture_comparison(off, on):
    def setups(journal):
        return [{key: value for key, value in row.items() if key not in
                 ("t", "seg", "frame", "cv_enabled", "ready_actors", "hero_ready_cm")}
                for row in journal if row.get("kind", "").startswith("monster_character_") and
                (row["kind"].endswith("_setup") or row["kind"] == "monster_character_camera")]

    before, after = setups(off), setups(on)
    mismatches = [{"event_index": index, "off": a, "on": b}
                  for index, (a, b) in enumerate(zip(before, after)) if a != b]
    cameras = [row for row in before if row["kind"] == "monster_character_camera"]
    other_cameras = [row for row in after if row["kind"] == "monster_character_camera"]
    paired = bool(cameras) and len(cameras) == len(other_cameras)
    camera_deltas = [math.dist(position(a[key]), position(b[key]))
                     for a, b in zip(cameras, other_cameras)
                     for key in ("camera_cm", "eye_cm", "target_cm") if key in a and key in b]
    hero_deltas = [math.dist(position(a["hero_cm"]), position(b["hero_cm"]))
                   for a, b in zip(cameras, other_cameras) if "hero_cm" in a and "hero_cm" in b]
    ready = []
    raw_cameras = [[row for row in journal if row.get("kind") == "monster_character_camera"]
                   for journal in (off, on)]
    for a, b in zip(*raw_cameras):
        before_ready = {row["npc_id"]: row["root_cm"] for row in a.get("ready_actors", [])}
        after_ready = {row["npc_id"]: row["root_cm"] for row in b.get("ready_actors", [])}
        ready.append({"route": a.get("route"),
                      "off": a.get("ready_actors"), "on": b.get("ready_actors"),
                      "root_differences_cm": {str(actor): math.dist(position(before_ready[actor]), position(after_ready[actor]))
                                               for actor in before_ready.keys() & after_ready.keys()},
                      "hero_difference_cm": math.dist(position(a["hero_ready_cm"]), position(b["hero_ready_cm"]))
                      if "hero_ready_cm" in a and "hero_ready_cm" in b else None})
    time_fields = ("seed", "hour", "minute")
    return {"off_setup_events": len(before), "on_setup_events": len(after),
            "exact_payload_match": bool(before) and before == after,
            "mismatches": mismatches,
            "camera_events": len(cameras),
            "fixed_camera": paired and all(row.get("fixed") is True for row in cameras + other_cameras),
            "follow_camera": paired and all(row.get("mode") == "follow" for row in cameras + other_cameras) and all(
                all(a.get(key) == b.get(key) and key in a for key in
                    ("watch_npc_id", "distance_cm", "height_cm", "angle_deg", "look_height_cm"))
                for a,b in zip(cameras, other_cameras)),
            "same_seed_and_time": paired and all(all(key in a and key in b and a[key] == b[key]
                for key in time_fields) for a, b in zip(cameras, other_cameras)),
            "authored_actor_starts_match": paired and all("actors" in a and a["actors"] == b.get("actors")
                for a, b in zip(cameras, other_cameras)),
            "max_camera_difference_cm": max(camera_deltas, default=None),
            "camera_coordinates_identical": paired and bool(camera_deltas) and max(camera_deltas) == 0,
            "max_hero_start_difference_cm": max(hero_deltas, default=None),
            "settled_starts": ready,
            "camera_proofs": cameras}


def bloodfly_comparison(off, on):
    before = [row for row in off["frames"] if row["route"].endswith("_bloodfly")]
    after = [row for row in on["frames"] if row["route"].endswith("_bloodfly")]
    if not before and not after:
        return None
    coordinates = ("x_cm", "y_cm", "z_cm", "visual_x_cm", "visual_y_cm", "visual_z_cm")
    same_count = bool(before) and len(before) == len(after)
    delta = max((abs(finite(a[key]) - finite(b[key])) for a, b in zip(before, after)
                 for key in coordinates), default=None)
    excluded = all(int(row["cv_active"]) == 0 for row in before + after)
    animations = same_count and all(a["animation"] == b["animation"] for a, b in zip(before, after))
    timing = same_count and all(a["dt_ms"] == b["dt_ms"] for a, b in zip(before, after))
    coverage = {}
    for mode, evidence, fly in (("off", off, before), ("on", on, after)):
        primary = [row for row in evidence["frames"] if row["route"] == "fieldraider_attack"]
        aligned = bool(fly) and len(fly) == len(primary) and all(
            (a["t_game_ms"], a["dt_ms"]) == (b["t_game_ms"], b["dt_ms"])
            for a, b in zip(fly, primary))
        measured_ms = sum(finite(row["dt_ms"]) for row in fly)
        window_ms = evidence["routes"].get("fieldraider_attack", {}).get("window_ms")
        allowance = max((finite(row["dt_ms"]) for row in fly), default=0)
        coverage[mode] = {"primary_frames": len(primary), "aligned_with_primary": aligned,
                          "sampled_ms": measured_ms, "route_window_ms": window_ms,
                          "complete": aligned and window_ms is not None and window_ms > 0 and
                          abs(measured_ms - window_ms) <= allowance}
    return {"off_frames": len(before), "on_frames": len(after), "cv_excluded_both": excluded,
            "max_coordinate_difference_cm": delta, "same_animation_frames": animations,
            "same_tick_intervals": timing, "full_window_coverage": coverage,
            "identical": same_count and excluded and delta == 0 and animations and timing and
            all(row["complete"] for row in coverage.values())}


def difference(before, after):
    if before is None or after is None:
        return {"percent": None, "within_10_percent": None}
    before, after = finite(before), finite(after)
    require(before >= 0 and after >= 0, "Attack range/duration cannot be negative")
    delta = 100 * (after / before - 1) if before else (0.0 if after == 0 else None)
    return {"percent": delta, "within_10_percent": abs(after - before) <= before * .1 + 1e-9}


def metrics(args):
    off, on = load(args.off, False), load(args.on, True)
    routes = []
    for name in sorted(off["routes"].keys() | on["routes"].keys()):
        before, after = off["routes"].get(name), on["routes"].get(name)
        pair = {"route": name, "off": before, "on": after, "matched_start_and_goal": None}
        pair["actual_start_difference_cm"] = None
        if before and after:
            if before["actual_start_cm"] is not None and after["actual_start_cm"] is not None:
                pair["actual_start_difference_cm"] = math.dist(position(before["actual_start_cm"]), position(after["actual_start_cm"]))
            pair["start_difference_cm"] = math.dist(position(before["start_cm"]), position(after["start_cm"]))
            pair["goal_difference_cm"] = math.dist(position(before["goal_cm"]), position(after["goal_cm"]))
            pair["matched_start_and_goal"] = (pair["start_difference_cm"] == pair["goal_difference_cm"] == 0 and
                                                before["window_ms"] == after["window_ms"])
            if name.endswith("_attack"):
                pair["distance_comparison"] = difference(before["attack_distance_cm"], after["attack_distance_cm"])
                pair["duration_comparison"] = difference(before["attack_duration_ms"], after["attack_duration_ms"])
                if "fieldraider" in name:
                    pair["native_collision_both"] = all((row["charge_collision_callbacks"] or 0) > 0 for row in (before, after))
        routes.append(pair)
    fixtures = fixture_comparison(off["events"], on["events"])
    bloodfly = bloodfly_comparison(off, on)
    errors = []
    for mode, evidence in (("off", off), ("on", on)):
        if not evidence["bench_complete"]:
            errors.append(f"{mode}: bench.log has no completed run")
        if evidence["native_errors"]:
            errors.append(f"{mode}: native monster preparation/measurement errors")
    if not off["routes"] or off["routes"].keys() != on["routes"].keys():
        errors.append("Missing or mismatched native route ends")
    if not fixtures["exact_payload_match"]:
        errors.append("Authored setup payloads differ between OFF and ON")
    recorded = [any(row.get("kind") == "rec" for row in evidence["events"]) for evidence in (off, on)]
    if any(recorded):
        if not all(recorded):
            errors.append("Only one take contains a native recording")
        for key in ("same_seed_and_time", "authored_actor_starts_match"):
            if not fixtures[key]:
                errors.append(f"Recorded pair lacks matching {key}")
        if not (fixtures["follow_camera"] or (fixtures["fixed_camera"] and fixtures["camera_coordinates_identical"])):
            errors.append("Recorded pair lacks identical follow parameters or matching fixed cameras")
        if fixtures["max_hero_start_difference_cm"] != 0:
            errors.append("Authored hero starts differ or are missing")
    for pair in routes:
        name = pair["route"]
        for actor in (pair.get("on") or {}).get("ground_cv_actors", []):
            if actor["cv_frames"] == 0:
                errors.append(f"{name}: ON ground actor {actor['npc_id']} ({'/'.join(actor['roles'])}) has no active CV frames")
        # The reverse leg starts at the actual wet endpoint of each take. Its
        # divergence is a measured consequence of the first leg, not a reset.
        if name == "wolf_water_exit":
            if (pair.get("goal_difference_cm") != 0 or pair.get("off") is None or pair.get("on") is None or
                    pair["off"]["window_ms"] != pair["on"]["window_ms"]):
                errors.append(f"{name}: dry goal or reverse duration differs")
        elif not pair["matched_start_and_goal"]:
            errors.append(f"{name}: initial route start/goal or duration differs")
        if name.endswith("_attack"):
            if any(pair.get(mode) is None or pair[mode].get("success") is not True for mode in ("off", "on")):
                errors.append(f"{name}: native attack did not complete successfully in both takes")
            for key in ("distance_comparison", "duration_comparison"):
                if pair.get(key, {}).get("within_10_percent") is not True:
                    errors.append(f"{name}: {key} lacks evidence or exceeds 10%")
            if "fieldraider" in name and not pair.get("native_collision_both"):
                errors.append(f"{name}: missing native charge collision in OFF or ON")
    if "fieldraider_attack" in off["routes"] or "fieldraider_attack" in on["routes"]:
        if bloodfly is None or not bloodfly["identical"]:
            errors.append("Bloodfly is not identical and CV-excluded throughout both complete attack windows")
    return {"off_directory": off["directory"], "on_directory": on["directory"], "routes": routes,
            "native_errors": {"off": off["native_errors"], "on": on["native_errors"]},
            "bench_complete": {"off": off["bench_complete"], "on": on["bench_complete"]},
            "fixtures": fixtures, "bloodfly_control": bloodfly,
            "validation_errors": errors, "pass": not errors,
            "geometry_review": "Pending actual frame inspection; missing data is not zero penetrations."}


def performance(directory, enabled):
    metadata = directory / "appcadence.csv.json"
    meta = json.loads(metadata.read_text())
    require(meta.get("clock") == "normal" and meta.get("complete") is True and meta.get("overflow") == 0,
            f"Incomplete native normal-clock cadence: {metadata}")
    require(meta.get("mode") == ("on" if enabled else "off"), f"Cadence controller mode mismatch: {metadata}")
    start, window_end = int(meta["start_ns"]), int(meta["window_end_ns"])
    require(window_end - start == 30_000_000_000, "Performance must measure a full 30-second window")
    with (directory / "appcadence.csv").open(newline="") as source:
        cadence = list(csv.DictReader(source))
    require(cadence and len(cadence) == int(meta["frames"]), "App cadence sample count differs from metadata")
    prior = start
    for index, row in enumerate(cadence):
        end, interval = int(row["end_ns"]), int(row["interval_ns"])
        require(int(row["frame"]) == index and end > prior and interval == end - prior, "Non-monotonic app cadence")
        require(index + 1 == len(cadence) or end < window_end, "App cadence includes frames after its final frame")
        prior = end
    require(prior == int(meta["final_end_ns"]) and prior >= window_end, "Missing final app-frame tail")
    with (directory / "monster-character-performance.csv").open(newline="") as source:
        rows = [{key: finite(value) for key, value in row.items()} for row in csv.DictReader(source)]
    require(rows, f"No native monster physics samples: {directory}")
    require(all(row["normal_clock"] == 1 and row["captured"] == 0 and row["dt_ms"] > 0 and row["step_ms"] >= 0
                for row in rows), "Physics timing must use uncaptured positive native normal-clock steps")
    require(all(b["t_game_ms"] > a["t_game_ms"] for a, b in zip(rows, rows[1:])), "Non-monotonic physics sample time")
    journal = events(directory)
    begins = [row for row in journal if row.get("kind") == "monster_character_measure_begin"]
    ends = [row for row in journal if row.get("kind") == "monster_character_measure_end"]
    require(len(begins) == len(ends) == 1 and begins[0].get("cv_enabled") is enabled,
            "Need exactly one native measure_begin/end in the expected controller mode")
    end = ends[0]
    require(int(end["frames"]) == len(rows), "Native measure_end physics sample count mismatch")
    crowds = [row for row in journal if row.get("kind") == "monster_character_crowd_setup"]
    species_ok = (len(crowds) == 1 and crowds[0].get("actors") == 20 and
                  (crowds[0].get("wolves"), crowds[0].get("molerats"), crowds[0].get("scavengers")) == (7, 7, 6))
    native_errors = [row for row in journal if row.get("kind") == "monster_character_error"]
    host = (directory / "host.txt").read_text().strip()
    fixture_ok = (end["fixture_actors"] == end["fixture_min_nearby"] == end["fixture_max_nearby"] == 20 and
                  finite(end["fixture_max_distance_cm"]) < 2000 and
                  all(row["nearby_monsters"] == 20 and row["maximum_distance_cm"] < 2000 for row in rows))
    cv_ok = all(row["nearby_cv"] == (20 if enabled else 0) for row in rows)
    costs = sorted(row["step_ms"] for row in rows)
    return {"directory": str(directory.resolve()), "host": host, "normal_clock": True,
            "samples": len(rows), "app_frames": len(cadence), "app_window_ms": (prior - start) / 1e6,
            "mean_fps": 1e9 * len(cadence) / (prior - start),
            "physics_p95_ms": costs[math.ceil(.95 * len(costs)) - 1],
            "physics_mean_ms": sum(costs) / len(costs),
            "fixture_20_within_20m": fixture_ok, "species_fixture_matches": species_ok,
            "cv_population_matches_mode": cv_ok,
            "moving_min": min(row["moving"] for row in rows),
            "moving_mean": sum(row["moving"] for row in rows) / len(rows),
            "maximum_distance_cm": max(row["maximum_distance_cm"] for row in rows),
            "native_measure_end": end, "native_errors": native_errors,
            "pass": host == "mac" and complete(directory) and not native_errors and fixture_ok and species_ok and cv_ok and
                    any(row["moving"] > 0 for row in rows)}


def compare_performance(args):
    off, on = performance(args.off, False), performance(args.on, True)
    delta = on["physics_p95_ms"] - off["physics_p95_ms"]
    loss = 100 * (1 - on["mean_fps"] / off["mean_fps"])
    return {"off": off, "on": on, "physics_p95_delta_ms": delta, "physics_p95_limit_delta_ms": .5,
            "fps_drop_percent": loss, "fps_drop_limit_percent": 5,
            "physics_pass": delta <= .5, "fps_pass": loss <= 5,
            "pass": off["pass"] and on["pass"] and delta <= .5 and loss <= 5}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    video = commands.add_parser("compose", help="Create a paired MP4 and one-second PNG contact sheet")
    video.add_argument("--off", type=Path, required=True)
    video.add_argument("--on", type=Path, required=True)
    video.add_argument("--out", type=Path, required=True)
    video.add_argument("--title")
    video.add_argument("--duration", type=float, help="Native-speed seconds (10–20); defaults to the shorter take, capped at 20")
    video.add_argument("--font", type=Path)
    video.add_argument("--crop", help="Same fixed X:Y:W:H pixel rectangle for both videos and sheets; W/H must be even")
    data = commands.add_parser("metrics", help="Print native route metrics and attack comparisons as JSON")
    data.add_argument("--off", type=Path, required=True)
    data.add_argument("--on", type=Path, required=True)
    timing = commands.add_parser("perf", help="Check Mac native 30-second physics p95 and app FPS")
    timing.add_argument("--off", type=Path, required=True)
    timing.add_argument("--on", type=Path, required=True)
    leaves = commands.add_parser("foliage", help="Create leaf-only transparent texture overrides for a bench")
    leaves.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = {"compose": compose, "metrics": metrics, "perf": compare_performance,
                  "foliage": foliage}[args.command](args)
        print(json.dumps(result, ensure_ascii=False, indent=2, allow_nan=False))
        if args.command in ("metrics", "perf") and not result["pass"]:
            return 1
    except (ValueError, OSError, KeyError, TypeError) as error:
        if args.command in ("metrics", "perf"):
            print(json.dumps({"pass": False, "validation_errors": [str(error)]}, ensure_ascii=False))
        parser.exit(1, f"error: {error}\n")


if __name__ == "__main__":
    sys.exit(main())
