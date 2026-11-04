#!/usr/bin/env python3
"""Measure 100-ms key-only motion tables on read-only NAVDEMO recordings.

On Ryzen, after copying scripts/nav_demos.py and this file into the task source:
    scripts/gq run --class batch --threads 1 --mem 3G --max 30m \
      --archive /home/kaifaty/gothic-ai/nav-sim-research/motion/output \
      --name navsim-motion -- /home/kaifaty/.local/bin/uv run --no-project \
      --with numpy python /home/kaifaty/gothic-ai/nav-sim-research/motion/source/scripts/navsim/motion_table.py \
      --corpus /mnt/shared/gothic-ai/nav-demos-v5 \
      --output /home/kaifaty/gothic-ai/nav-sim-research/motion/output/results.json

Only numpy is required. The default reads all episodes, including failed teachers.
No game, model, physics simulation or corpus writes are involved. Safety-frame
logs supply actual support and water depth, which NAVDEMO normals cannot prove.
Accuracy quantiles describe steps/windows, not repeated random experiments.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import platform
import random
import subprocess
import sys
import sysconfig
import time

os.environ["OMP_NUM_THREADS"] = "1"
os.environ["OPENBLAS_NUM_THREADS"] = "1"
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from nav_demos import iter_chunks


AGE_LOWER_STEPS = np.array([0, 1, 2, 3, 5, 10, 20, 50])
SEED = 20261104


def wrap(degrees):
    return (degrees + 180) % 360 - 180


def targets(pose):
    """World X/Y/Z, yaw -> forward/right/up cm and signed yaw degrees."""
    delta = np.diff(pose[:, :3], axis=0)
    angle = np.deg2rad(pose[:-1, 3])
    return np.column_stack((delta[:, 0] * np.cos(angle) + delta[:, 2] * np.sin(angle),
                            -delta[:, 0] * np.sin(angle) + delta[:, 2] * np.cos(angle),
                            delta[:, 1], wrap(np.diff(pose[:, 3]))))


def key_ids(keys):
    """Return current-only (-1) and age/history (0..3) packed table keys."""
    if not np.isin(keys[:, :3], [-1, 0, 1]).all() or not np.isin(keys[:, 3], [0, 1]).all():
        raise ValueError("Motion tables require digital forward/strafe/turn and binary walk")
    code = (((keys[:, 0] + 1) * 3 + keys[:, 1] + 1) * 3 + keys[:, 2] + 1) * 2 + keys[:, 3]
    code = code.astype(np.int64)
    changed = np.r_[True, np.any(keys[1:] != keys[:-1], axis=1)]
    age = np.arange(len(keys)) - np.maximum.accumulate(np.where(changed, np.arange(len(keys)), 0))
    age_bin = np.searchsorted(AGE_LOWER_STEPS, age, side="right") - 1
    packed = code.copy()
    result = [code, age_bin + len(AGE_LOWER_STEPS) * packed]
    for history in range(1, 4):
        previous = np.full(len(keys), 54, dtype=np.int64)
        previous[history:] = code[:-history]
        packed = packed + 55**history * previous
        result.append(age_bin + len(AGE_LOWER_STEPS) * packed)
    return np.column_stack(result)


@dataclass
class Episode:
    identifier: str
    family: str
    pose: np.ndarray
    keys: np.ndarray
    ids: np.ndarray
    target: np.ndarray
    slope: np.ndarray
    supported: np.ndarray
    unsafe: np.ndarray
    wet: np.ndarray
    deep: np.ndarray
    swimming: np.ndarray
    near_jump: np.ndarray
    near_climb: np.ndarray
    weapon: str
    partial_steps: int


def interval_any(values, starts, ends):
    prefix = np.r_[0, np.cumsum(values, dtype=np.int64)]
    return prefix[ends] > prefix[starts]


def around(mask, radius=5):
    # Slicing 'full' also works when an episode is shorter than the kernel.
    return np.convolve(mask.astype(int), np.ones(2 * radius + 1, dtype=int), "full")[
        radius:radius + len(mask)] > 0


def load_episode(directory, root):
    metadata = json.loads((directory / "episode.json").read_text())
    trajectory = np.loadtxt(directory / "trajectory.txt", ndmin=2)
    actions = np.loadtxt(directory / "actions.txt", ndmin=2)
    if (metadata.get("drive") or metadata.get("replay") or
            trajectory.shape != (metadata["decisions"] + 1, 5) or
            actions.shape != (len(trajectory), 9) or
            not np.array_equal(trajectory[:, 0], actions[:, 0]) or
            not np.isfinite(trajectory).all() or not np.isfinite(actions).all()):
        raise ValueError(f"Invalid teacher trajectory/actions: {directory}")
    # Copy only the small physical fields, never retain the large eye/memory arrays.
    fields = []
    for chunk in iter_chunks(directory):
        fields.append((chunk["time_ms"].copy(), chunk["position_cm"].copy(),
                       chunk["body_yaw_deg"].copy(), chunk["body"]["flags"].copy(),
                       chunk["body"]["ground_slope_deg"].copy()))
    stamps, position, yaw, flags, slope = [np.concatenate(parts) for parts in zip(*fields)]
    if (len(stamps) != metadata["decisions"] or
            not np.array_equal(stamps - stamps[0], actions[:-1, 0]) or
            not np.allclose(position, trajectory[:-1, 1:4], atol=.03, rtol=0) or
            not np.allclose(wrap(yaw - trajectory[:-1, 4]), 0, atol=.001)):
        raise ValueError(f"NAVDEMO/text pose or timestamp mismatch: {directory}")
    durations = np.diff(trajectory[:, 0])
    if not (np.all(durations[:-1] == 100) and 0 < durations[-1] <= 100):
        raise ValueError(f"Non-100-ms interior interval: {directory}")
    n = len(durations) - int(durations[-1] != 100)
    if n < 1:
        raise ValueError(f"No full 100-ms interval: {directory}")
    slope = np.maximum(slope, np.r_[slope[1:], slope[-1]])[:n]
    unsafe = (flags.any(axis=1) | np.r_[flags[1:].any(axis=1), flags[-1].any()])[:n]
    swimming = (flags[:, 4].astype(bool) | np.r_[flags[1:, 4], flags[-1, 4]].astype(bool))[:n]

    frames = [json.loads(line) for line in (directory / "perception-safety-frames.jsonl").read_text().splitlines()]
    times = np.array([f["elapsed_ms"] for f in frames])
    if times[0] != 0 or times[-1] != trajectory[-1, 0] or not np.all(np.diff(times) > 0):
        raise ValueError(f"Incomplete physical support/water evidence: {directory}")
    starts = np.searchsorted(times, trajectory[:n, 0], side="left")
    ends = np.searchsorted(times, trajectory[1:n + 1, 0], side="right")
    if np.any(ends == starts):
        raise ValueError(f"Missing interval physical evidence: {directory}")

    def any_frame(predicate):
        return interval_any(np.array([predicate(f) for f in frames]), starts, ends)

    supported = ~any_frame(lambda f: not f["supported"])
    unsafe |= any_frame(lambda f: any(f[k] for k in
                                   ("in_air", "falling", "native_jump", "native_climb", "swimming")))
    wet = any_frame(lambda f: f["water_depth_cm"] > 10)
    deep = any_frame(lambda f: f["water_depth_cm"] + 1 >= f["deep_water_threshold_cm"])
    swimming |= any_frame(lambda f: f["swimming"])
    jump = any_frame(lambda f: f["native_jump"]) | (actions[:n, 5] != 0)
    climb = any_frame(lambda f: f["native_climb"]) | (actions[:n, 6] != 0)
    climb |= (flags[:n, 3] | np.r_[flags[1:, 3], flags[-1, 3]][:n]).astype(bool)
    # Weapon state is absent in this corpus. Never infer it from measured yaw.
    weapon = "unknown"
    if "weapon_state" in metadata:
        weapon = "unarmed" if metadata["weapon_state"] in (0, "NoWeapon") else "armed"
    relative = directory.relative_to(root)
    return Episode(str(relative), relative.parts[0], trajectory[:n + 1, 1:],
                   actions[:n, 1:5], key_ids(actions[:n, 1:5]), targets(trajectory[:n + 1, 1:]),
                   slope, supported, unsafe, wet, deep, swimming, around(jump), around(climb),
                   weapon, len(durations) - n)


def split_episodes(episodes, seed):
    rng = random.Random(seed)
    train, test = [], []
    for family in sorted({e.family for e in episodes}):
        members = sorted((e for e in episodes if e.family == family), key=lambda e: e.identifier)
        rng.shuffle(members)
        count = max(1, round(len(members) * .2))
        test.extend(members[:count])
        train.extend(members[count:])
    return train, test


def grouped(ids, values, quantile=.5):
    unique, inverse, counts = np.unique(ids, return_inverse=True, return_counts=True)
    order = np.argsort(inverse, kind="stable")
    bounds = np.r_[0, np.cumsum(counts)]
    estimates = np.array([np.quantile(values[order[a:b]], quantile, axis=0)
                          for a, b in zip(bounds[:-1], bounds[1:])])
    return {int(key): (int(count), value) for key, count, value in zip(unique, counts, estimates)}


def lookup(ids, tables, history):
    result = np.full((len(ids), 4), np.nan)
    used = np.full(len(ids), -2, dtype=int)
    # Exact key first, then shorter history with age, then current key without age.
    for level in range(history, -2, -1):
        for i in np.flatnonzero(used == -2):
            entry = tables[level + 1].get(int(ids[i, level + 1]))
            if entry is not None:
                result[i] = entry[1]
                used[i] = level
    return result, used


def terrain_candidate(episode):
    return (episode.supported & ~episode.unsafe & (episode.slope < 10) & ~episode.wet &
            ~episode.near_jump & ~episode.near_climb)


def fit(episodes):
    ids = np.concatenate([e.ids for e in episodes])
    truth = np.concatenate([e.target for e in episodes])
    candidate = np.concatenate([terrain_candidate(e) for e in episodes])
    speed = np.linalg.norm(truth[:, :2], axis=1)
    reference = [grouped(ids[candidate, column], speed[candidate, None], .75) for column in (0, 1)]
    free = np.concatenate([masks(e, reference)["free"] for e in episodes])
    if not free.any():
        raise ValueError("No free training steps")
    tables = [grouped(ids[free, column], truth[free]) for column in range(5)]
    return tables, reference


def masks(episode, reference):
    expected = np.full(len(episode.keys), np.nan)
    for column in (1, 0):
        for i in np.flatnonzero(np.isnan(expected)):
            entry = reference[column].get(int(episode.ids[i, column]))
            if entry is not None:
                expected[i] = entry[1][0]
    moving = np.any(episode.keys[:, :2] != 0, axis=1)
    blocked = moving & (expected >= 2) & (np.linalg.norm(episode.target[:, :2], axis=1) < .5 * expected)
    candidate = terrain_candidate(episode)
    return {"free": candidate & ~blocked & np.isfinite(expected), "probable_blocked": blocked,
            "flat_before_block_filter": candidate,
            "block_reference_unknown": ~np.isfinite(expected),
            "slope_0_10": episode.slope < 10,
            "slope_10_20": (episode.slope >= 10) & (episode.slope < 20),
            "slope_20_30": (episode.slope >= 20) & (episode.slope <= 30),
            "slope_gt30": episode.slope > 30,
            "in_water": episode.wet, "deep_water": episode.deep,
            "swimming": episode.swimming, "around_jump": episode.near_jump,
            "around_climb": episode.near_climb, "unsupported": ~episode.supported,
            "all": np.ones(len(episode.keys), dtype=bool)}


def quantiles(values):
    if len(values) == 0:
        return {"median": None, "p90": None, "p99": None}
    return dict(zip(("median", "p90", "p99"), map(float, np.quantile(values, [.5, .9, .99]))))


def step_errors(predicted, truth):
    delta = predicted - truth
    return np.column_stack((np.linalg.norm(delta[:, :3], axis=1),
                            np.linalg.norm(delta[:, :2], axis=1), np.abs(delta[:, 2]),
                            np.abs(wrap(delta[:, 3]))))


def error_summary(errors):
    valid = np.isfinite(errors).all(axis=1)
    return {"steps": len(errors), "predicted_steps": int(valid.sum()),
            "coverage_fraction": float(valid.mean()) if len(errors) else None,
            **{name: quantiles(errors[valid, i]) for i, name in enumerate(
                ("position_cm", "horizontal_cm", "vertical_cm", "yaw_deg"))}}


def rollout(episode, predicted, free, steps):
    """Overlapping windows; true initial pose, recorded key history, no pose resets."""
    valid = free & np.isfinite(predicted).all(axis=1)
    n = len(valid)
    if n < steps:
        return np.empty((0, 5))
    prefix = np.r_[0, np.cumsum(~valid)]
    start = np.flatnonzero(prefix[steps:] == prefix[:-steps])
    end = start + steps
    motion = np.nan_to_num(predicted)
    yaw = np.r_[0, np.cumsum(motion[:, 3])]
    angle = np.deg2rad(yaw[:-1])
    dx = motion[:, 0] * np.cos(angle) - motion[:, 1] * np.sin(angle)
    dz = motion[:, 0] * np.sin(angle) + motion[:, 1] * np.cos(angle)
    path = np.vstack((np.zeros(3), np.cumsum(np.column_stack((dx, motion[:, 2], dz)), axis=0)))
    delta = path[end] - path[start]
    angle = np.deg2rad(episode.pose[start, 3] - yaw[start])
    world = np.column_stack((delta[:, 0] * np.cos(angle) - delta[:, 2] * np.sin(angle),
                             delta[:, 1], delta[:, 0] * np.sin(angle) + delta[:, 2] * np.cos(angle)))
    error = world - (episode.pose[end, :3] - episode.pose[start, :3])
    travel = np.r_[0, np.cumsum(np.linalg.norm(episode.target[:, :2], axis=1))]
    return np.column_stack((np.linalg.norm(error, axis=1), np.linalg.norm(error[:, [0, 2]], axis=1),
                            np.abs(error[:, 1]), np.abs(wrap(yaw[end] - yaw[start] -
                            (episode.pose[end, 3] - episode.pose[start, 3]))), travel[end] - travel[start]))


def evaluate(train, test, tables, reference):
    all_episodes = train + test
    classified = {e.identifier: masks(e, reference) for e in all_episodes}
    counts = {}
    for family in sorted({e.family for e in all_episodes}):
        members = [e for e in all_episodes if e.family == family]
        total = sum(len(e.keys) for e in members)
        counts[family] = {"episodes": len(members), "steps": total,
                          "train_episodes": sum(e.family == family for e in train),
                          "test_episodes": sum(e.family == family for e in test),
                          "train_steps": sum(len(e.keys) for e in train if e.family == family),
                          "test_steps": sum(len(e.keys) for e in test if e.family == family),
                          "partial_steps_excluded": sum(e.partial_steps for e in members),
                          "free_steps": sum(int(classified[e.identifier]["free"].sum()) for e in members)}
        counts[family]["free_fraction"] = counts[family]["free_steps"] / total
    histories = {}
    breakdown = {}
    for history in range(4):
        errors, moving_errors, used, rollouts = [], [], [], {seconds: [] for seconds in (1, 3, 10)}
        per_family = {}
        bucket_errors = {name: [] for name in classified[test[0].identifier]}
        for e in test:
            predicted, level = lookup(e.ids, tables, history)
            free = classified[e.identifier]["free"]
            error = step_errors(predicted, e.target)
            errors.append(error[free])
            moving_errors.append(error[free & (np.linalg.norm(e.target[:, :2], axis=1) >= 1)])
            used.append(level[free])
            per_family.setdefault(e.family, []).append(error[free])
            for seconds in rollouts:
                rollouts[seconds].append(rollout(e, predicted, free, seconds * 10))
            if history == 3:
                for name, mask in classified[e.identifier].items():
                    bucket_errors[name].append(error[mask])
        levels = np.concatenate(used)
        replay_summary = {}
        for seconds, values in rollouts.items():
            windows = np.concatenate(values)
            moving = windows[:, 4] >= 1
            replay_summary[str(seconds)] = {
                **error_summary(windows[:, :4]), "windows": len(windows),
                "episodes_with_windows": sum(len(x) > 0 for x in values),
                "recorded_horizontal_travel_cm": quantiles(windows[:, 4]),
                "stationary_windows": int((~moving).sum()),
                "nonstationary": error_summary(windows[moving, :4])}
        histories[str(history)] = {
            "one_step_free": error_summary(np.concatenate(errors)),
            "one_step_free_nonstationary": error_summary(np.concatenate(moving_errors)),
            "by_family": {f: error_summary(np.concatenate(v)) for f, v in per_family.items()},
            "lookup_levels_free": {str(level): int((levels == level).sum()) for level in range(-2, history + 1)},
            "rollout_free": replay_summary}
        if history == 3:
            for name, values in bucket_errors.items():
                count = sum(int(classified[e.identifier][name].sum()) for e in all_episodes)
                breakdown[name] = {"corpus_steps": count,
                                   "corpus_fraction": count / sum(len(e.keys) for e in all_episodes),
                                   "test_error": error_summary(np.concatenate(values))}
    turns = []
    for weapon in ("unknown", "unarmed", "armed"):
        for walk in (0, 1):
            for turn in (-1, 1):
                values, free_values = [], []
                for e in all_episodes:
                    if e.weapon != weapon:
                        continue
                    mask = ((e.keys[:, 0] == 0) & (e.keys[:, 1] == 0) &
                            (e.keys[:, 2] == turn) & (e.keys[:, 3] == walk))
                    values.append(e.target[mask, 3])
                    free_values.append(e.target[mask & classified[e.identifier]["free"], 3])
                dyaw = np.concatenate(values) if values else np.empty(0)
                free_dyaw = np.concatenate(free_values) if free_values else np.empty(0)
                turns.append({"weapon": weapon, "pace": "walk" if walk else "run", "turn": turn,
                              "steps": len(dyaw), "signed_yaw_deg": quantiles(dyaw),
                              "absolute_yaw_deg": quantiles(np.abs(dyaw)),
                              "near_9deg_steps": int((np.abs(np.abs(dyaw) - 9) < .01).sum()),
                              "near_18deg_steps": int((np.abs(np.abs(dyaw) - 18) < .01).sum()),
                              "free_steps": len(free_dyaw), "free_signed_yaw_deg": quantiles(free_dyaw),
                              "abs_error_vs_unarmed_9deg": quantiles(np.abs(wrap(dyaw + turn * 9))),
                              "abs_error_vs_armed_18deg": quantiles(np.abs(wrap(dyaw + turn * 18)))})
    return {"families": counts, "history_comparison": histories, "failure_buckets_history3": breakdown,
            "turn_in_place": {"groups": turns,
                              "weapon_evidence": "No per-frame weapon state in NAVDEMO; absent metadata remains unknown. Rotation rate is not a weapon label."}}


def compact_tables(tables):
    return {str(level - 1): [[key, count, *np.round(value, 6).tolist()]
                            for key, (count, value) in sorted(table.items())]
            for level, table in enumerate(tables)}


def time_summary(values, warmup):
    return {"median": float(np.median(values)), "p90": float(np.quantile(values, .9)),
            "repeats": len(values), "warmup": warmup}


def command_output(command):
    return subprocess.check_output(command, text=True, stderr=subprocess.STDOUT).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--corpus", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    corpus, output = args.corpus.resolve(), args.output.resolve()
    if output.is_relative_to(corpus):
        parser.error("Output must be outside the read-only corpus")
    # gq limits its pool to worker CPUs, but does not pin an individual thread.
    cpu = min(os.sched_getaffinity(0))
    os.sched_setaffinity(0, {cpu})
    started = time.perf_counter()
    queue = command_output([str(Path.home() / "gothic-ai/bin/gq"), "status"])
    manifest_path = corpus / "manifest.json"
    manifest_bytes = manifest_path.read_bytes()
    manifest = json.loads(manifest_bytes)
    root = corpus / "recordings"
    directories = sorted(root.glob("*/*/*/episode.json"))
    if len(directories) < 200 or len({p.relative_to(root).parts[0] for p in directories}) < 4:
        raise ValueError("At least 200 episodes from four families are required")
    expected = {entry["id"] for entry in manifest["episodes"]}
    if {p.parent.name for p in directories} != expected:
        raise ValueError("Recording episodes differ from the versioned manifest")
    episodes = []
    for index, source in enumerate(directories):
        episodes.append(load_episode(source.parent, root))
        if (index + 1) % 25 == 0:
            print(f"Loaded {index + 1}/{len(directories)} episodes ({time.perf_counter() - started:.1f}s)", flush=True)
    load_seconds = time.perf_counter() - started
    train, test = split_episodes(episodes, SEED)
    # One full warmup, then three measured fits/evaluations on the extracted data.
    times = []
    for repeat in range(4):
        then = time.perf_counter()
        tables, reference = fit(train)
        result = evaluate(train, test, tables, reference)
        elapsed = time.perf_counter() - then
        if repeat:
            times.append(elapsed)
        print(f"Fit/evaluate {repeat}/3: {elapsed:.2f}s", flush=True)
    metrics_path = root / "collection-metrics.json"
    metrics_bytes = metrics_path.read_bytes()
    metrics = json.loads(metrics_bytes)
    result.update({
        "schema_version": 1, "dt_ms": 100,
        "corpus": {"name": manifest["dataset"], "manifest_sha256": hashlib.sha256(manifest_bytes).hexdigest(),
                   "collection_metrics": {"path": str(metrics_path),
                                          "sha256": hashlib.sha256(metrics_bytes).hexdigest(),
                                          **{k: metrics[k] for k in ("complete", "corpus_version", "demo_version",
                                              "navigation_memory_version", "by_family", "dataset_bytes")}},
                   "episodes": len(episodes), "steps": sum(len(e.keys) for e in episodes),
                   "partial_steps_excluded": sum(e.partial_steps for e in episodes),
                   "recordings": str(root), "selection": "all recorded teachers, including failures"},
        "split": {"seed": SEED, "test_fraction": .2, "algorithm": "random.Random shuffle within sorted families and episode identifiers; round(0.2*n) test per family",
                  "train_episode_ids": sorted(e.identifier for e in train),
                  "test_episode_ids": sorted(e.identifier for e in test),
                  "scope": "episode holdout; routes/NPCs may overlap; original source split is retained in identifiers"},
        "method": {
            "target_columns": ["forward_cm", "right_cm", "up_cm", "yaw_delta_deg"],
            "frame": "yaw=0 points world +X, right=+Z, up=+Y; rotate displacement by interval-start yaw; yaw errors wrap to [-180,180)",
            "fit": "componentwise median on free training intervals; no validation targets enter any table",
            "support": "supported at every available safety frame in the closed interval; body flags at available NAVDEMO endpoints and physical flags within interval; final full interval has only its start NAVDEMO sample",
            "free": "supported, no body/physical exceptional flags, max available endpoint slope <10deg, water depth <=10cm, outside +/-500ms jump/climb neighborhood, known block reference and no probable block",
            "block_reference": "training terrain-candidate horizontal displacement p75 per current-key+age; fallback current-key p75",
            "probable_block": "movement key held, reference >=2cm/100ms, actual horizontal displacement <50% reference; slow animation transitions can also trigger this proxy",
            "mask_caveat": "same frozen reference/mask for histories 0..3; uses observed displacement to condition evaluation, so free-only accuracy is optimistic and is not a deployable collision detector",
            "buckets": "overlap; max available endpoint slope; water depth>10cm, deep depth+1cm>=native threshold; jump/climb actions or native physical flags (also NAVDEMO climbing) dilated +/-500ms",
            "rollout": "all overlapping 1/3/10s windows with every step free and predicted; true initial position/yaw, actual pre-window key history; rotate next displacement by predicted yaw, no teacher pose correction",
            "nonstationary": "additional summaries require recorded horizontal path >=1cm per step/window; stationary windows remain in the primary summaries",
            "history_selection": "report all 0..3; history 3 fixed in advance for failure buckets, no test-based tuning",
            "limits": ["Tables reproduce observed root-motion-plus-physics displacement, not isolated animation root motion.",
                       "This measures neither Jolt CharacterVirtual nor the accuracy after replacing physics.",
                       "Body flags are sampled at 100ms; physical logs add substep support, jump/climb, air and water evidence."]},
        "table": {"age_bin_lower_ms": (AGE_LOWER_STEPS * 100).tolist(),
                  "key_columns": ["forward", "strafe", "turn", "walk"],
                  "action_code": "(((forward+1)*3+strafe+1)*3+turn+1)*2+walk; missing previous=54",
                  "packed_key": "age_bin + 8*(current_code + 55*prev1 + 55^2*prev2 + 55^3*prev3); truncate history to level; level -1=current_code only",
                  "initial_history": "missing previous decisions=54; first decision age=0",
                  "lookup": "requested history down to 0 (with age), then -1; unseen current keys remain unpredicted",
                  "columns": ["packed_key", "training_steps", "forward_cm", "right_cm", "up_cm", "yaw_delta_deg"],
                  "levels": compact_tables(tables),
                  "block_reference_columns": ["packed_key", "training_steps", "p75_horizontal_cm"],
                  "block_reference_levels": {str(i - 1): [[k, n, round(float(v[0]), 6)] for k, (n, v) in sorted(t.items())]
                                             for i, t in enumerate(reference)}},
        "execution": {"class": "batch", "gq_status_at_start": queue, "cpu_affinity": [cpu],
                      "cpu": next(line.split(":", 1)[1].strip() for line in Path("/proc/cpuinfo").read_text().splitlines() if line.startswith("model name")),
                      "gpu_used": False, "gpu": command_output(["nvidia-smi", "--query-gpu=name", "--format=csv,noheader"]),
                      "python": platform.python_version(), "numpy": np.__version__,
                      "compiler": platform.python_compiler(), "python_cflags": sysconfig.get_config_var("CFLAGS"),
                      "own_native_code": "none; no C++ build, -O2/-O3/-march not applicable to this script",
                      "source_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                      "load_seconds": time_summary([load_seconds], 0),
                      "fit_evaluate_seconds": time_summary(times, 1),
                      "timing_note": "Single pinned CPU, BLAS/OMP threads=1 supplied by launcher; loading is one uncontrolled-cache pass, in-memory fit/evaluation has one warmup and three repeats."}})
    result["execution"]["elapsed_before_write_seconds"] = time_summary([time.perf_counter() - started], 0)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(result, ensure_ascii=False, separators=(",", ":"), allow_nan=False) + "\n")
    print(f"Saved {output}: {len(episodes)} episodes, {result['corpus']['steps']} steps, {output.stat().st_size} bytes", flush=True)


if __name__ == "__main__":
    main()
