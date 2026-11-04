"""Acceptance 2 of the nav-sim stand eye and its cost (docs/nav-sim.md, S1).

``parity INPUT RAW`` replays the engine's perception-replay poses (``poses.txt``
with head origins and times from ``rays.bin``) through the stand: every episode
is one agent, so each starts from a fresh eye as the replay's reset. It
compares the fresh rays with ``rays.bin`` (same hit, |Δdistance| < 1 cm, same
kind, |ΔnormalY| < 0.01), the near/far schedule and the far cache slot by slot.

``cost INPUT RAW`` times one eye update of 2 048 agents on the reference poses
at several pool sizes (``cost-<threads>.json``; separate runs may add sizes).

``summary INPUT RAW OUT`` writes the compact eye.json (≤ 100 KB) from all of them.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import sys
import time

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
from formats import KINDS, read_rays  # noqa: E402
from stand import eye  # noqa: E402

EPOCH = 1_000_000_000  # perception-replay's game time origin (navperceptionbench.cpp)
STATIC_KINDS = ("world", "static_vob", "water")
THRESHOLD = 0.999
BOUNDARY_CM = 0.1  # a hit this close to the near end is not attributable to near or far


def read_poses(path: Path):
    labels, elapsed, position, yaw = [], [], [], []
    for line in path.read_text().splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        row = line.split()
        labels.append(row[0])
        elapsed.append(int(row[1]))
        position.append([float(v) for v in row[2:5]])
        yaw.append(float(row[5]))
    return labels, np.array(elapsed, np.uint64), np.array(position, np.float32), np.array(yaw, np.float32)


def episodes(labels):
    starts = [0] + [i for i in range(1, len(labels)) if labels[i] != labels[i - 1]]
    ends = starts[1:] + [len(labels)]
    if len({labels[s] for s in starts}) != len(starts):
        raise ValueError("an episode label appears in two separate runs")
    return np.array(starts), np.array(ends) - np.array(starts)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


FRESH = np.dtype([("distance", "<f4"), ("normal_y", "<f4"), ("kind", "u1"), ("hit", "u1"), ("complete", "u1")])
CACHE = np.dtype([("time", "<u4"), ("distance", "<f4"), ("normal_y", "<f4"), ("kind", "u1"), ("hit", "u1")])


def replay(inputs: Path, raw: Path, threads: int):
    """Stand outputs for every reference update: fresh rays, far cache, schedule."""
    rays = read_rays(inputs / "rays.bin")
    labels, elapsed, position, yaw = read_poses(inputs / "poses.txt")
    updates = rays.updates
    if len(labels) != len(updates) or not np.array_equal(elapsed, updates["elapsed_ms"]):
        raise ValueError("poses.txt and rays.bin disagree on updates or times")
    starts, lengths = episodes(labels)
    agents = len(starts)
    began = time.perf_counter()
    world = eye.EyeWorld(inputs / "static.bin", agents=agents, threads=threads)
    build_s = time.perf_counter() - began
    fresh = np.zeros((len(updates), eye.RAYS), FRESH)
    cache = np.zeros((len(updates), eye.RAYS), CACHE)
    schedule = np.zeros(len(updates), [("updated", "u1"), ("far_updated", "u1"), ("casts", "<u4")])
    direction_max = 0.0
    direction_equal = 0
    out = eye.Slots.empty(agents)
    for step in range(int(lengths.max())):
        active = step < lengths
        index = starts + np.minimum(step, lengths - 1)
        world.set_poses_yaw(updates["origin"][index], position[index], yaw[index], EPOCH + elapsed[index])
        result = world.update(out)
        rows, agent = index[active], np.flatnonzero(active)
        if not result.updated[agent].all():
            raise ValueError("source poses are faster than the eye")
        schedule["updated"][rows] = result.updated[agent]
        schedule["far_updated"][rows] = result.far_updated[agent]
        schedule["casts"][rows] = result.casts[agent]
        now, past = slice(0, eye.RAYS), slice(eye.RAYS, eye.SLOTS)
        for name in ("distance", "normal_y", "kind", "hit", "complete"):
            fresh[name][rows] = getattr(out, name)[agent, now]
        for name in ("time", "distance", "normal_y", "kind", "hit"):
            cache[name][rows] = getattr(out, name)[agent, past]
        delta = np.abs(out.direction[agent, now] - updates["rays"]["direction"][rows])
        direction_max = max(direction_max, float(delta.max()))
        direction_equal += int((delta == 0).all(axis=2).sum())
    world.close()
    np.save(raw / "stand-fresh.npy", fresh)
    np.save(raw / "stand-cache.npy", cache)
    np.save(raw / "stand-schedule.npy", schedule)
    return dict(agents=agents, threads=threads, build_s=build_s, direction_max_abs=direction_max,
                direction_equal_fraction=direction_equal / fresh.size)


def expected_cache(updates, starts, lengths, near_end: float):
    """Far cache implied by rays.bin: the last far cast of each ray in its episode."""
    ref = updates["rays"]
    far = updates["far_updated"].astype(bool)[:, None]
    hit, distance = ref["hit"].astype(bool), ref["distance_cm"]
    boundary = far & hit & (np.abs(distance - near_end) <= BOUNDARY_CM)
    cast = far & ~boundary & (~hit | (distance > near_end))
    event = cast | boundary
    last = np.full(ref.shape, -1, np.int64)
    for start, length in zip(starts, lengths):
        current = np.full(ref.shape[1], -1, np.int64)
        for u in range(start, start + length):
            current = np.where(event[u], u, current)
            last[u] = current
    valid = last >= 0
    ambiguous = valid & boundary[np.maximum(last, 0), np.arange(ref.shape[1])]
    source = ref[np.maximum(last, 0), np.arange(ref.shape[1])]
    time = np.where(valid, EPOCH + updates["elapsed_ms"][np.maximum(last, 0)],
                    EPOCH + updates["elapsed_ms"][:, None]).astype(np.uint64)
    return dict(valid=valid, ambiguous=ambiguous, time=time, hit=np.where(valid, source["hit"], 0),
                kind=np.where(valid, source["kind"], 0), distance=np.where(valid, source["distance_cm"], 0),
                normal_y=np.where(valid, source["normal_y"], 0), far_casts=int(cast.sum()),
                boundary=int(boundary.sum()))


def agreement(ref_hit, ref_distance, ref_kind, ref_normal, hit, distance, kind, normal):
    same_hit = hit.astype(bool) == ref_hit.astype(bool)
    close = np.abs(distance - ref_distance) < 1
    same_kind = kind == ref_kind
    flat = np.abs(normal - ref_normal) < .01
    return same_hit, close, same_kind, flat, same_hit & close & same_kind & flat


def parity(inputs: Path, raw: Path, threads: int):
    run = replay(inputs, raw, threads)
    rays = read_rays(inputs / "rays.bin")
    updates, ref = rays.updates, rays.updates["rays"]
    labels, *_ = read_poses(inputs / "poses.txt")
    starts, lengths = episodes(labels)
    fresh = np.load(raw / "stand-fresh.npy")
    cache = np.load(raw / "stand-cache.npy")
    schedule = np.load(raw / "stand-schedule.npy")
    same_hit, close, same_kind, flat, strict = agreement(
        ref["hit"], ref["distance_cm"], ref["kind"], ref["normal_y"],
        fresh["hit"], fresh["distance"], fresh["kind"], fresh["normal_y"])
    by_kind = {}
    for k, name in enumerate(KINDS):
        sel = ref["kind"] == k
        n = int(sel.sum())
        frac = (lambda m: float(m[sel].mean()) if n else None)
        by_kind[name] = {"rays": n, "all_fraction": frac(strict), "hit_equal_fraction": frac(same_hit),
                         "distance_lt_1cm_fraction": frac(close), "kind_equal_fraction": frac(same_kind),
                         "normal_y_lt_0_01_fraction": frac(flat), "mismatches": int(n - strict[sel].sum())}
    static = np.isin(ref["kind"], [KINDS.index(k) for k in STATIC_KINDS])
    gate = {name: by_kind[name]["all_fraction"] for name in STATIC_KINDS}
    passed = all(v is not None and v >= THRESHOLD for v in gate.values())

    expected = expected_cache(updates, starts, lengths, rays.near_range_cm)
    compared = ~expected["ambiguous"]
    slot_schedule = (cache["time"] == expected["time"]) & ((cache["distance"] > 0) == expected["valid"])
    content = agreement(expected["hit"], expected["distance"], expected["kind"], expected["normal_y"],
                        cache["hit"], cache["distance"], cache["kind"], cache["normal_y"])[-1]
    valid_compared = compared & expected["valid"]
    stand_far = int(schedule["casts"].sum()) - int(schedule["updated"].sum()) * eye.RAYS
    schedule_summary = {
        "updates": len(updates), "far_updated_equal_fraction": float(
            (schedule["far_updated"] == updates["far_updated"]).mean()),
        "updated_all": bool(schedule["updated"].all()),
        "near_casts": int(schedule["updated"].sum()) * eye.RAYS, "far_casts": stand_far,
        "engine_far_casts_inferred": expected["far_casts"], "near_boundary_hits": expected["boundary"],
        "complete_equal_fraction": float((fresh["complete"] == ref["complete"]).mean()),
        "complete_mismatches": int((fresh["complete"] != ref["complete"]).sum()),
        "cache_slots_compared": int(compared.sum()),
        "cache_slots_excluded_boundary": int((~compared).sum()),
        "cache_schedule_equal_fraction": float(slot_schedule[compared].mean()),
        "cache_schedule_mismatches": int((~slot_schedule & compared).sum()),
        "cache_valid_slots": int(valid_compared.sum()),
        "cache_content_equal_fraction": float((content & slot_schedule)[valid_compared].mean()),
        "definition": ("Engine cache from rays.bin: per episode and ray, the last far-updated record whose near "
                       "segment missed (no hit, or hit beyond the near end). Hits within 0.1 cm of the near end "
                       "cannot be attributed and their cache slots are excluded until the ray's next far cast. "
                       "Schedule = same timestamp and validity; content = the four ray criteria."),
    }
    examples = []
    for name in STATIC_KINDS + ("none", "movable_vob"):
        bad = np.argwhere(~strict & (ref["kind"] == KINDS.index(name)))
        for u, r in bad[np.linspace(0, len(bad) - 1, min(5, len(bad)), dtype=int)] if len(bad) else []:
            e, p = ref[u, r], fresh[u, r]
            examples.append({"engine_kind": name, "update": int(u), "ray": int(r),
                             "origin_cm": updates["origin"][u].tolist(), "direction": e["direction"].tolist(),
                             "far_updated": bool(updates["far_updated"][u]),
                             "engine": {"hit": bool(e["hit"]), "distance_cm": float(e["distance_cm"]),
                                        "kind": KINDS[int(e["kind"])], "normal_y": float(e["normal_y"])},
                             "stand": {"hit": bool(p["hit"]), "distance_cm": float(p["distance"]),
                                       "kind": KINDS[int(p["kind"])], "normal_y": float(p["normal_y"])}})
    result = {"passed": passed, "threshold": THRESHOLD, "gate_kinds": list(STATIC_KINDS), "gate": gate,
              "static_rays": int(static.sum()), "static_all_fraction": float(strict[static].mean()),
              "all_rays": int(strict.size), "all_fraction": float(strict.mean()), "by_engine_kind": by_kind,
              "schedule_and_cache": schedule_summary, "replay": run, "mismatch_examples": examples}
    (raw / "parity.json").write_text(json.dumps(result, indent=1) + "\n")
    return result


def cost(inputs: Path, raw: Path, threads_list, agents: int, steps: int, repeats: int):
    rays = read_rays(inputs / "rays.bin")
    labels, elapsed, position, yaw = read_poses(inputs / "poses.txt")
    starts, lengths = episodes(labels)
    origin = np.ascontiguousarray(rays.updates["origin"])
    a = np.arange(agents)
    episode = a % len(starts)
    offset = (a // len(starts) * 37) % lengths[episode]
    stagger = a % 5  # spreads the 500 ms far updates over the 100 ms steps
    cpus = sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else []
    began = time.perf_counter()
    world = eye.EyeWorld(inputs / "static.bin", agents=agents, threads=max(threads_list))
    build_s = time.perf_counter() - began
    out = eye.Slots.empty(agents)
    warmup = 10
    rows = []
    for threads in threads_list:
        pinned = cpus[:threads] if len(cpus) >= threads else None
        world.set_threads(threads, pinned)
        for repeat in range(-1, repeats):
            for write in (False, True):
                world.reset()
                walls, updated, casts = [], 0, 0
                load = os.getloadavg()
                for step in range(warmup + steps):
                    index = starts[episode] + (offset + step) % lengths[episode]
                    ms = EPOCH + 100 * np.maximum(0, step - stagger)
                    world.set_poses_yaw(origin[index], position[index], yaw[index], ms)
                    t0 = time.perf_counter()
                    result = world.update(out if write else None)
                    wall = time.perf_counter() - t0
                    if step >= warmup:
                        if not result.updated.all():
                            raise ValueError("cost poses must update every agent each step")
                        walls.append(wall)
                        updated += int(result.updated.sum())
                        casts += result.total_casts
                if repeat < 0:
                    continue
                total = float(np.sum(walls))
                rows.append({"threads": threads, "repeat": repeat, "write_slots": write, "pinned": pinned is not None,
                             "agents": agents, "steps": steps, "updates": updated, "casts": casts,
                             "wall_s": total, "step_ms_median": float(np.median(walls)) * 1e3,
                             "step_ms_p90": float(np.percentile(walls, 90)) * 1e3,
                             "wall_us_per_update": total * 1e6 / updated,
                             "thread_us_per_update": total * 1e6 * threads / updated,
                             "updates_per_second": updated / total, "load_average": list(load)})
                print(json.dumps(rows[-1]), flush=True)
    world.close()
    result = {"agents": agents, "steps": steps, "warmup_steps": warmup, "repeats": repeats, "build_s": build_s,
              "cpus": cpus, "rows": rows}
    (raw / f"cost-{'_'.join(map(str, threads_list))}.json").write_text(json.dumps(result, indent=1) + "\n")
    return result


def summary(inputs: Path, raw: Path, out: Path, raw_root: str):
    parity_result = json.loads((raw / "parity.json").read_text())
    costs = [json.loads(path.read_text()) for path in sorted(raw.glob("cost-*.json"))]
    rows = {r["threads"]: [] for c in costs for r in c["rows"]}
    for c in costs:  # a later run of the same pool size replaces an earlier one
        for threads in {r["threads"] for r in c["rows"]}:
            rows[threads] = [r for r in c["rows"] if r["threads"] == threads]
    table = {}
    for write in (False, True):
        key = "eye_update" if not write else "eye_update_and_slot_write"
        table[key] = {}
        for threads in sorted(rows):
            group = [r for r in rows[threads] if r["write_slots"] == write]
            pick = lambda k: float(np.median([r[k] for r in group]))  # noqa: E731
            table[key][str(threads)] = {
                "thread_us_per_update": pick("thread_us_per_update"), "wall_us_per_update": pick("wall_us_per_update"),
                "updates_per_second": 1e6 / pick("wall_us_per_update"), "step_ms_median": pick("step_ms_median"),
                "casts_per_update": group[0]["casts"] / group[0]["updates"], "repeats": len(group),
                "load_average_per_repeat": [r["load_average"] for r in group]}
    files = {}
    for path in sorted(raw.iterdir()):
        if path.is_file():
            files[path.name] = {"bytes": path.stat().st_size, "sha256": sha256(path)}
    engine = json.loads((inputs / "engine.json").read_text()) if (inputs / "engine.json").exists() else {}
    result = {
        "status": "passed" if parity_result["passed"] else "failed",
        "acceptance_2": {key: parity_result[key] for key in
                         ("passed", "threshold", "gate_kinds", "gate", "static_rays", "static_all_fraction",
                          "all_rays", "all_fraction", "by_engine_kind", "schedule_and_cache", "replay")},
        "cost": {"agents": sorted({c["agents"] for c in costs}), "steps_per_repeat": sorted({c["steps"] for c in costs}),
                 "warmup_steps": sorted({c["warmup_steps"] for c in costs}),
                 "bvh_build_s": [c["build_s"] for c in costs],
                 "reference_us_per_update": {"raybench_embree_single_thread_median": 43,
                                             "engine_replay_median": engine.get("eye_us_per_update", {}).get(
                                                 "eye", {}).get("median_us")},
                 "definition": ("One update of all agents' eyes on the pool (near every 100 ms, far every 500 ms, "
                                "far phases staggered over five steps). thread_us_per_update = wall x threads / "
                                "updates; wall_us_per_update = wall / updates. eye_update_and_slot_write also "
                                "writes all 476 slots of every agent in the gpumem layout."),
                 **table},
        "mismatch_examples": parity_result["mismatch_examples"][:20],
        "semantics": {
            "poses": "poses.txt position and yaw, head origin from rays.bin; neutral head frame from body yaw; "
                     "game time 1e9 + elapsed_ms; one agent per episode (= replay's eye reset)",
            "categories": "NAVSTAT1 1 World, 2 StaticVob, 3 Water, 4 MovableVob (kinematic C_Object, dump pose)",
            "not_in_dump": "NPCs and loose items (C_Item, C_MovableVob) are absent; doors and mobsi stay in their "
                           "dump pose, so movable_vob rays are reported apart from the gate",
            "criteria": "same hit, |distance| < 1 cm, same kind, |normalY| < 0.01, against rays.bin"},
        "raw_root": raw_root, "files": files,
        "measurements": {path.stem: json.loads(path.read_text()) for path in sorted(raw.glob("meta-*.json"))},
        "host": {"machine": platform.machine(), "python": platform.python_version()},
    }
    payload = json.dumps(result, indent=1, allow_nan=False) + "\n"
    if len(payload.encode()) > 100_000:
        raise ValueError("eye.json exceeds 100 KB")
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(payload)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("parity")
    p.add_argument("input", type=Path)
    p.add_argument("raw", type=Path)
    p.add_argument("--threads", type=int, default=8)
    c = sub.add_parser("cost")
    c.add_argument("input", type=Path)
    c.add_argument("raw", type=Path)
    c.add_argument("--threads", default="1,8,28")
    c.add_argument("--agents", type=int, default=2048)
    c.add_argument("--steps", type=int, default=50)
    c.add_argument("--repeats", type=int, default=3)
    s = sub.add_parser("summary")
    s.add_argument("input", type=Path)
    s.add_argument("raw", type=Path)
    s.add_argument("out", type=Path)
    s.add_argument("--raw-root", default=None, help="where the raw files live (default: RAW)")
    args = parser.parse_args()
    if args.command == "parity":
        args.raw.mkdir(parents=True, exist_ok=True)
        result = parity(args.input, args.raw, args.threads)
        print(json.dumps({k: result[k] for k in ("passed", "gate", "static_all_fraction")}))
    elif args.command == "cost":
        args.raw.mkdir(parents=True, exist_ok=True)
        cost(args.input, args.raw, [int(t) for t in args.threads.split(",")], args.agents, args.steps, args.repeats)
    else:
        result = summary(args.input, args.raw, args.out, args.raw_root or str(args.raw))
        print(json.dumps({"gate": result["acceptance_2"]["gate"], "cost": result["cost"]["eye_update"]}))


if __name__ == "__main__":
    main()
