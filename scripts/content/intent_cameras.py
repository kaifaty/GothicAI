#!/usr/bin/env python3
"""Place inspection cameras from the offline city geometry census.

uv run --no-project --with numpy --with scipy --with ijson python \
  scripts/content/intent_cameras.py --input ~/.local/share/gothic-ai/texture-intent \
  --scenario scripts/content/city-intent.bench \
  --metadata ~/.local/share/gothic-ai/texture-intent/cameras.json \
  --debug-scenario ~/.local/share/gothic-ai/texture-intent/debug.bench

All triangle coordinates and camera metadata stay outside git. The scenario
contains positions and commands only; it uses original game resources.
Semantics come from the committed city-materials.csv mapping; the census's
context.json supplies waypoints as well as occlusion geometry.
"""

import argparse
import csv
import json
import math
from pathlib import Path

import numpy as np
from scipy.spatial import cKDTree


DEBUG_TEXTURES = (
    "NW_CITY_WALL_03.TGA", "NW_CITY_BODENDIELEN_02.TGA", "NW_CITY_ROOF_04.TGA",
    "NW_CITY_INSIDEWALL_13.TGA", "NW_CITY_DOOR_01.TGA", "NW_HARBOUR_CANON_01.TGA",
    "NW_HARBOUR_FISHINGNET_04.TGA", "NW_HARBOUR_WOODCRATE_02.TGA",
)

# These representative objects were verified against the census and scene review.
# The textures are reused on other objects: this limits the photographed example,
# and does not assign the texture's meaning from either a texture or visual name.
REPRESENTATIVE_VISUALS = {
    "NW_HARBOUR_CANONHOLE.TGA": {"NW_HARBOUR_CANON_01.3DS"},
    "NW_HARBOUR_CANON_01.TGA": {"NW_HARBOUR_CANON_01.3DS"},
    "NW_HARBOUR_WOODCRATE_01.TGA": {"NW_HARBOUR_CRATE_01.3DS", "NW_HARBOUR_CRATEPILE_01.3DS"},
    "NW_HARBOUR_WOODCRATE_02.TGA": {"NW_HARBOUR_CRATE_01.3DS", "NW_HARBOUR_CRATEPILE_01.3DS"},
}


def json_items(path, prefix):
    # Import only for census reading, so geometry unit tests need numpy/scipy only.
    import ijson
    with path.open("rb") as stream:
        yield from ijson.items(stream, prefix, use_float=True)


class Occluders:
    """Two-sided triangle ray tests with conservative radius-binned KD trees."""

    def __init__(self, triangles):
        self.triangles = np.asarray(triangles, dtype=np.float32).reshape((-1, 3, 3))
        centers = self.triangles.mean(axis=1)
        radii = np.linalg.norm(self.triangles - centers[:, None], axis=2).max(axis=1)
        levels = np.ceil(np.log2(np.maximum(radii, 32))).astype(int)
        self.bins = []
        for level in np.unique(levels):
            indices = np.flatnonzero(levels == level)
            self.bins.append((cKDTree(centers[indices]), indices, float(2.0 ** level)))

    def first_hit(self, origin, target, endpoint_margin=3.0):
        origin, target = np.asarray(origin), np.asarray(target)
        direction = target - origin
        length = np.linalg.norm(direction)
        if length < endpoint_margin * 2:
            return None
        candidates = []
        midpoint = (origin + target) / 2
        for tree, indices, radius in self.bins:
            found = tree.query_ball_point(midpoint, length / 2 + radius)
            if found:
                candidates.append(indices[found])
        if not candidates:
            return None
        triangles = self.triangles[np.concatenate(candidates)]
        lo, hi = np.minimum(origin, target), np.maximum(origin, target)
        keep = np.all(triangles.max(axis=1) >= lo - 0.01, axis=1) & np.all(triangles.min(axis=1) <= hi + 0.01, axis=1)
        triangles = triangles[keep].astype(np.float64)
        if not len(triangles):
            return None
        e1, e2 = triangles[:, 1] - triangles[:, 0], triangles[:, 2] - triangles[:, 0]
        p = np.cross(direction, e2)
        determinant = np.einsum("ij,ij->i", e1, p)
        keep = np.abs(determinant) > 1e-8
        triangles, e1, e2, p, determinant = (v[keep] for v in (triangles, e1, e2, p, determinant))
        tvec = origin - triangles[:, 0]
        u = np.einsum("ij,ij->i", tvec, p) / determinant
        q = np.cross(tvec, e1)
        v = q @ direction / determinant
        t = np.einsum("ij,ij->i", e2, q) / determinant
        hits = t[(u >= -1e-6) & (v >= -1e-6) & (u + v <= 1 + 1e-6)
                 & (t > endpoint_margin / length) & (t < 1 - endpoint_margin / length)]
        return float(hits.min() * length) if len(hits) else None


def load_occluders(directory):
    source = directory / "context.json"
    cache = directory / "camera-occluders.npy"
    if not cache.exists() or cache.stat().st_mtime < source.stat().st_mtime:
        values = (float(c) for triangle in json_items(source, "occluders.item") for point in triangle for c in point)
        array = np.fromiter(values, dtype=np.float32).reshape((-1, 3, 3))
        np.save(cache, array)
    triangles = np.load(cache, mmap_mode="r")
    print(f"Occlusion context: {len(triangles)} triangles", flush=True)
    return Occluders(triangles)


def camera_commands(target, eye):
    """Account for cam world using hero origin, and cam look using origin+45."""
    hero = np.rint(np.asarray(target) - [0, 45, 0]).astype(int)
    target = hero.astype(float) + [0, 45, 0]
    delta = np.asarray(eye) - hero
    distance = float(np.hypot(delta[0], delta[2]))
    yaw = math.degrees(math.atan2(delta[2], delta[0]))
    return hero.tolist(), target.tolist(), distance, float(delta[1]), yaw


def patch_rank(patch, name, semantic, city_tree, market):
    center, normal = np.asarray(patch["center"]), np.asarray(patch["normal"])
    city_distance = float(city_tree.query(center[[0, 2]])[0])
    in_city = city_distance < 3500 and -18000 < center[0] < 23000 and -24000 < center[2] < 18000
    score = math.sqrt(max(patch["area_m2"], 1e-6)) / (1 + city_distance / 10000)
    role = semantic.get("object", "") + " " + semantic.get("role", "")
    # Prefer a face that expresses the intended role over a larger reused underside.
    if "floor" in role or "paving" in role or "walking" in role or "lane" in role:
        score *= 1.0 if normal[1] > 0.55 else 0.08
    elif any(word in role for word in ("wall", "door", "window", "crate", "barrel", "gate", "plaster")):
        score *= 1.0 if abs(normal[1]) < 0.65 else 0.25
    elif name in ("NW_CITY_ROOF_01.TGA", "NW_CITY_ROOF_04.TGA"):
        score *= 1.0 if normal[1] > 0.25 else 0.1
    if name == "NW_CITY_WALL_03.TGA":
        score /= 1 + np.linalg.norm((center - market)[[0, 2]]) / 2500
    return in_city, score, city_distance


def sample_targets(patch, triangles):
    """Anchor on actual geometry, avoiding a centroid in a hole of a concave patch."""
    centers = np.asarray([t["center"] for t in triangles], dtype=float)
    areas = np.asarray([t["area_m2"] for t in triangles])
    patch_center = np.asarray(patch["center"])
    order = np.argsort(np.linalg.norm(centers - patch_center, axis=1))
    anchor = centers[order[0]]
    # Keep both the central sightline and a few actual surface samples.
    representative = np.argsort(areas)[-6:]
    samples = np.concatenate(([anchor], centers[representative]))
    return anchor, samples


def eye_candidates(target, normal, scale, nearby_waypoint):
    normal = np.asarray(normal)
    up = np.array([0.0, 1.0, 0.0])
    if abs(normal[1]) > 0.78:
        # Floors/ceilings need an oblique view; a vertical normal alone has no yaw.
        delta = np.asarray(nearby_waypoint) - target
        base_yaw = math.atan2(delta[2], delta[0])
        for step in (0, 90, -90, 180):
            angle = base_yaw + math.radians(step)
            horizontal = np.array([math.cos(angle), 0, math.sin(angle)])
            direction = normal * 0.82 + horizontal * 0.57
            direction /= np.linalg.norm(direction)
            for ratio in (1, 0.65, 1.3, 0.4):
                yield target + direction * scale * ratio
    else:
        side = np.cross(up, normal)
        side /= np.linalg.norm(side)
        for angle in (0, 25, -25, 45, -45, 70, -70):
            a = math.radians(angle)
            direction = normal * math.cos(a) + side * math.sin(a)
            for elevation in (0.12, 0, 0.35):
                for ratio in (1, 0.65, 1.3, 0.4):
                    yield target + (direction + up * elevation) * scale * ratio


def choose_camera(row, semantic, city_tree, city_positions, waypoints, occluders):
    name = row["texture"]
    market = next((np.asarray(w["position"]) for w in waypoints if w["name"] == "MARKT"), np.array([10044, 320, 4239]))
    ranking = [patch_rank(p, name, semantic, city_tree, market) for p in row["patches"]]
    eligible = list(range(len(ranking)))
    if name in REPRESENTATIVE_VISUALS:
        eligible = [i for i in eligible if row["patches"][i]["visual"] in REPRESENTATIVE_VISUALS[name]]
        if not eligible:
            raise RuntimeError(f"{name}: verified representative visual is missing from census")
    city_available = any(ranking[i][0] for i in eligible)
    order = sorted(eligible, key=lambda i: ranking[i][1], reverse=True)
    chosen, buckets = [], set()
    for index in order:
        if city_available and not ranking[index][0]:
            continue
        p = row["patches"][index]
        bucket = (p["source"], *(round(v / 400) for v in p["center"]), *(round(v * 2) for v in p["normal"]))
        if bucket in buckets:
            continue
        buckets.add(bucket)
        chosen.append(index)
        if len(chosen) >= 64:
            break
    by_patch = {index: [] for index in chosen}
    for triangle in row["triangles"]:
        if triangle["patch"] in by_patch:
            by_patch[triangle["patch"]].append(triangle)
    best = None
    for candidate_number, index in enumerate(chosen):
        if candidate_number >= 12 and best is not None:
            break
        patch = row["patches"][index]
        if not by_patch[index]:
            continue
        target, samples = sample_targets(patch, by_patch[index])
        normal = np.asarray(patch["normal"])
        scale = float(np.clip(math.sqrt(patch["area_m2"]) * 100 * 1.1, 60, 850))
        near = city_positions[int(city_tree.query(target[[0, 2]])[1])]
        for eye in eye_candidates(target, normal, scale, near):
            # The archive normal defines the visible face, including interior faces.
            if np.dot(eye - target, normal) < 18 or occluders.first_hit(eye, target) is not None:
                continue
            visible = sum(occluders.first_hit(eye, sample) is None for sample in samples) / len(samples)
            score = ranking[index][1] * (0.4 + 0.6 * visible)
            # Prefer a nearly straight view and the baseline framing distance.
            direction = (eye - target) / np.linalg.norm(eye - target)
            score *= 0.7 + 0.3 * max(0, np.dot(direction, normal))
            score /= 1 + abs(np.linalg.norm(eye - target) / scale - 1) * 0.4
            if best is None or score > best[0]:
                best = (score, index, target, eye, visible, scale)
            if visible == 1 and np.dot(direction, normal) > 0.95:
                break
    if best is None:
        raise RuntimeError(f"{name}: no unobstructed outward-facing camera among {len(chosen)} patches")
    score, index, target, eye, visible, scale = best
    hero, actual_target, distance, height, yaw = camera_commands(target, eye)
    patch = row["patches"][index]
    nearest = min(waypoints, key=lambda w: np.linalg.norm(np.asarray(w["position"]) - target))
    return {
        "texture": name, "shot": Path(name).stem.lower(), "patch_index": index,
        "patch": patch, "selection": "city" if city_available else "whole-world fallback",
        "nearest_waypoint": nearest, "target": actual_target, "eye": eye.tolist(), "hero": hero,
        "normal": patch["normal"], "camera": {"distance": distance, "height": height, "yaw": yaw, "look": 45},
        "surface_samples_visible": visible, "central_sightline_clear": True,
        "framing_scale_cm": scale, "layout": row["layout"], "layout_share": row["layout_share"],
        "semantic": semantic,
    }


def write_scenario(path, cameras):
    lines = ["# Original resources; geometry-selected texture inspection cameras.",
             "cheat god", "set time 12 0", "overlay off", "view hide animated morph pfx", "cam lift off", "cam look 45"]
    for camera in cameras:
        p, c = camera["hero"], camera["camera"]
        lines += ["", f"# {camera['texture']} | {camera['nearest_waypoint']['name']} | {camera['selection']}"]
        if camera.get("camera_note"):
            lines += ["# Camera: " + str(camera["camera_note"]).replace("\n", " ")]
        lines += [f"view {camera.get('view', 'hide animated morph pfx')}",
                  f"cam look {c.get('look', 45):.3f}",
                  f"goto pos {p[0]} {p[1]} {p[2]}",
                  f"cam world {c['distance']:.3f} {c['height']:.3f} {c['yaw']:.3f}",
                  "wait 34", "cam hold", "wait 600", f"shot {camera['shot']}",
                  # shot is saved after the frame, so the next goto must wait.
                  "wait 300"]
    lines += ["wait 100"]
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n")


def main():
    repository = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--mapping", type=Path, default=repository / "data/content/city-materials.csv")
    parser.add_argument("--scenario", type=Path, required=True)
    parser.add_argument("--metadata", type=Path, required=True)
    parser.add_argument("--debug-scenario", type=Path)
    parser.add_argument("--only", nargs="*", help="texture basenames, for camera preparation only")
    args = parser.parse_args()
    directory = args.input.expanduser().resolve()
    args.metadata = args.metadata.expanduser().resolve()
    if args.metadata.is_relative_to(repository):
        parser.error("--metadata must be outside git")
    fields = ("object", "role", "depicts", "colour", "weathering", "orientation")
    with args.mapping.expanduser().resolve().open(newline="") as stream:
        semantics = {row["texture"]: {field: row[field] for field in fields} for row in csv.DictReader(stream)}
    waypoints = list(json_items(directory / "context.json", "waypoints.item"))
    city_positions = np.asarray([w["position"] for w in waypoints if w["name"].startswith(("NW_CITY", "SHIP_")) or w["name"] == "MARKT"])
    city_tree = cKDTree(city_positions[:, [0, 2]])
    occluders = load_occluders(directory)
    cameras = []
    for row in json_items(directory / "intent.json", "textures.item"):
        if args.only and row["texture"] not in args.only:
            continue
        camera = choose_camera(row, semantics[row["texture"]], city_tree, city_positions, waypoints, occluders)
        cameras.append(camera)
        print(f"{camera['texture']}: patch={camera['patch_index']} {camera['selection']} visible={camera['surface_samples_visible']:.2f}", flush=True)
        # Persist progress outside git; a far-only texture failure must not lose prior choices.
        args.metadata.parent.mkdir(parents=True, exist_ok=True)
        args.metadata.write_text(json.dumps(cameras, indent=2) + "\n")
        if args.debug_scenario:
            debug = [c for c in cameras if c["texture"] in DEBUG_TEXTURES]
            if len(debug) == len(DEBUG_TEXTURES):
                write_scenario(args.debug_scenario, debug)
    write_scenario(args.scenario, cameras)
    print(f"Wrote {len(cameras)} cameras: {args.scenario}")


if __name__ == "__main__":
    main()
