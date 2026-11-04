"""Acceptance 1 of the nav-sim stand motion and its cost (docs/nav-sim.md, S2).

``run CORPUS TABLE STATIC RAW`` replays the 189 held-out teacher episodes of
``nav-demos-v5`` (the split and seed of ``motion_table.py``) through the stand:
the recorded pose and key history place an agent, the recorded keys drive it,
and its poses are compared with the recorded ones by decision.

- One step: every decision from its recorded pose (a fresh body each time).
- Open-loop windows of 1, 3 and 10 s from the recorded starting pose, over two
  sets: free steps only (the tables' set, ``rollout`` in motion_table.py) and
  all ground steps (free, blocked, any slope). "Moving" windows have a
  recorded horizontal path of at least 1 cm, as the tables' ``nonstationary``.
- The error is split into course and displacement: the window replay is run a
  second time with the recorded yaw forced at every decision (course oracle),
  and the end error is split along and across the recorded path. Steps are
  broken down by the tables' buckets and by family, next to the table's own
  error on the same steps (history 3, its up and yaw columns included).
- Jump, climb, water and unsupported steps are S5: they are reported, not gated.

``cost STATIC TABLE RAW`` times one decision (3 ticks) of 2 048 agents on
recorded poses and keys at several pool sizes.

``summary RAW OUT`` writes the compact motion.json (<= 100 KB).
"""
from __future__ import annotations

import argparse
from collections import namedtuple
import hashlib
import json
import os
from pathlib import Path
import pickle
import sys
import time

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE.parents[1]))
import motion_table  # noqa: E402
from stand import eye, motion  # noqa: E402

EPOCH = 1_000_000_000  # any 100 ms multiple: the 30 Hz grid gives 33/33/34 ms ticks
TABLE_SHA256 = "174b661c91291eeba5d91c7dfafae6413a3e8bb9aa0923408e936128b87f05c8"  # research §3.5
HISTORY = 3
THRESHOLDS = {"free_step_p90_cm": 5.0, "window_10s_moving_p90_cm": 100.0, "blocked_step_p90_cm": 17.0,
              "turn_deg_per_s": 90.0}
TABLE_REFERENCE = {"free_step_p90_cm": 4.8, "window_10s_moving_p90_cm": 210.3, "blocked_step_p90_cm": 33.6}
WINDOW_SECONDS = (1, 3, 10)
S5_BUCKETS = ("in_water", "deep_water", "swimming", "around_jump", "around_climb", "unsupported")
Light = namedtuple("Light", "identifier family")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def wrap(degrees):
    return (np.asarray(degrees) + 180) % 360 - 180


def quantiles(values):
    values = np.asarray(values, float)
    if len(values) == 0:
        return {"n": 0, "median": None, "p90": None, "p99": None}
    q = np.quantile(values, [.5, .9, .99])
    return {"n": int(len(values)), "median": round(float(q[0]), 4), "p90": round(float(q[1]), 4),
            "p99": round(float(q[2]), 4)}


# ---------------------------------------------------------------- corpus ----

def test_split(corpus: Path):
    root = corpus / "recordings"
    directories = sorted(root.glob("*/*/*/episode.json"))
    lights = [Light(str(p.parent.relative_to(root)), p.parent.relative_to(root).parts[0]) for p in directories]
    train, test = motion_table.split_episodes(lights, motion_table.SEED)
    return root, len(lights), sorted(e.identifier for e in test)


def load_test(corpus: Path, cache: Path | None):
    root, total, identifiers = test_split(corpus)
    if cache is not None and cache.is_file():
        stored = pickle.loads(cache.read_bytes())
        if stored["identifiers"] == identifiers:
            print(f"test episodes from {cache}", flush=True)
            return total, stored["episodes"]
    began = time.perf_counter()
    episodes = []
    for i, identifier in enumerate(identifiers):
        episodes.append(motion_table.load_episode(root / identifier, root))
        if (i + 1) % 20 == 0:
            print(f"loaded {i + 1}/{len(identifiers)} test episodes ({time.perf_counter() - began:.0f} s)", flush=True)
    if cache is not None:
        cache.parent.mkdir(parents=True, exist_ok=True)
        cache.write_bytes(pickle.dumps({"identifiers": identifiers, "episodes": episodes}))
    return total, episodes


def reference_of(table: dict):
    """motion_table.fit's block reference from the exported table: [level -1, level 0] {key: (n, [p75])}."""
    levels = table["block_reference_levels"]
    return [{int(k): (int(n), np.array([v])) for k, n, v in levels[str(level)]} for level in (-1, 0)]


def tables_of(table: dict):
    return [{int(row[0]): (int(row[1]), np.array(row[2:6], float)) for row in table["levels"][str(level)]}
            for level in range(-1, 4)]


class Corpus:
    """Test episodes flattened: decisions, poses and masks with per-episode offsets."""

    def __init__(self, episodes, table: dict):
        reference, tables = reference_of(table), tables_of(table)
        self.episodes = episodes
        self.families = sorted({e.family for e in episodes})
        n = np.array([len(e.keys) for e in episodes])
        self.step_offset = np.r_[0, np.cumsum(n)][:-1]
        self.pose_offset = np.r_[0, np.cumsum(n + 1)][:-1]
        self.keys = np.concatenate([e.keys for e in episodes]).astype(np.int8)
        self.ages = np.concatenate([motion.ages(e.keys) for e in episodes])
        self.pose = np.concatenate([e.pose for e in episodes]).astype(np.float64)
        self.target = np.concatenate([e.target for e in episodes])
        self.episode = np.repeat(np.arange(len(episodes)), n)
        self.step = np.concatenate([np.arange(k) for k in n])
        self.family = np.repeat(np.array([self.families.index(e.family) for e in episodes]), n)
        classified = [motion_table.masks(e, reference) for e in episodes]
        self.masks = {name: np.concatenate([c[name] for c in classified]) for name in classified[0]}
        self.masks["ground"] = np.concatenate([e.supported & ~e.unsafe & ~e.wet & ~e.near_jump & ~e.near_climb
                                               for e in episodes])
        self.masks["s5"] = ~self.masks["ground"]
        self.masks["turn_in_place"] = (self.keys[:, 0] == 0) & (self.keys[:, 1] == 0) & (self.keys[:, 2] != 0)
        # The table alone, as motion_table evaluates it (history 3).
        predicted, level = zip(*(motion_table.lookup(e.ids, tables, HISTORY) for e in episodes))
        self.table_predicted = np.concatenate(predicted)
        self.table_level = np.concatenate(level)
        self.table_errors = motion_table.step_errors(self.table_predicted, self.target)
        self.travel = np.linalg.norm(self.target[:, :2], axis=1)
        self.travel_prefix = np.r_[0, np.cumsum(self.travel)]

    def pose_index(self, g):
        return self.pose_offset[self.episode[g]] + self.step[g]

    def windows(self, mask, length):
        """Global start steps of all overlapping windows of ``length`` decisions inside ``mask``."""
        starts = []
        for e, (offset, n) in enumerate(zip(self.step_offset, [len(x.keys) for x in self.episodes])):
            if n < length:
                continue
            bad = np.r_[0, np.cumsum(~mask[offset:offset + n])]
            s = np.flatnonzero(bad[length:] == bad[:-length])
            starts.append(offset + s)
        return np.concatenate(starts) if starts else np.zeros(0, np.int64)


# ---------------------------------------------------------------- replay ----

def replay(m: "motion.Motion", corpus: Corpus, starts, length, oracle=False, batch=None):
    """Plays ``length`` recorded decisions from each global start step; returns end
    position [n, 3], yaw [n], flags OR over the ticks [n], the first decision's table
    translation and level, and tick counts of blocked, stepped, steep and falling."""
    batch = batch or m.agents
    n = len(starts)
    end_position, end_yaw = np.zeros((n, 3)), np.zeros(n)
    flags_any, level0 = np.zeros(n, np.uint8), np.zeros(n, np.int8)
    table0 = np.zeros((n, 2), np.float32)
    counts = {name: np.zeros(n, np.int32) for name in ("blocked", "stepped", "steep", "falling", "unknown_keys")}
    steps = np.arange(length)
    for b0 in range(0, n, batch):
        g = np.asarray(starts[b0:b0 + batch])
        b = len(g)
        p = corpus.pose_index(g)
        s = corpus.step[g]
        history = np.full((b, 3, 4), -1, np.int8)
        for h in range(3):
            valid = s - 1 - h >= 0
            history[valid, h] = corpus.keys[g[valid] - 1 - h]
        held = np.where(s >= 1, corpus.ages[np.maximum(g - 1, 0)], 0).astype(np.int32)
        m.set_state(corpus.pose[p, :3], corpus.pose[p, 3], EPOCH + 100 * s.astype(np.uint64), history, held)
        keys = corpus.keys[g[:, None] + steps[None, :]]
        recorded_yaw = corpus.pose[p[:, None] + steps[None, :], 3]
        out = motion.Ticks.empty(b)
        for j in range(length):
            m.set_keys(keys[:, j])
            if oracle:
                m.set_yaw(recorded_yaw[:, j])
            m.step(0, b, out=out)
            f = np.bitwise_or.reduce(out.flags, axis=1)
            flags_any[b0:b0 + b] |= f
            for name, bit in (("blocked", motion.BLOCKED), ("stepped", motion.STEPPED), ("steep", motion.STEEP),
                              ("falling", motion.FALLING), ("unknown_keys", motion.UNKNOWN_KEYS)):
                counts[name][b0:b0 + b] += np.count_nonzero(out.flags & bit, axis=1).astype(np.int32)
            if j == 0:
                state = m.read(0, b)
                table0[b0:b0 + b], level0[b0:b0 + b] = state.table_cm, state.level
        state = m.read(0, b)
        end_position[b0:b0 + b], end_yaw[b0:b0 + b] = state.position, state.yaw
    return end_position, end_yaw, flags_any, table0, level0, counts


def decompose(corpus: Corpus, starts, length, position, yaw):
    """End error of windows: 3D, horizontal, vertical, yaw; along/across the recorded path."""
    p0 = corpus.pose_index(starts)
    truth = corpus.pose[p0 + length]
    error = position - truth[:, :3]
    recorded = truth[:, [0, 2]] - corpus.pose[p0][:, [0, 2]]
    length_cm = np.linalg.norm(recorded, axis=1)
    direction = np.where(length_cm[:, None] > 1e-6, recorded / np.maximum(length_cm, 1e-6)[:, None], 0)
    along = error[:, 0] * direction[:, 0] + error[:, 2] * direction[:, 1]
    across = -error[:, 0] * direction[:, 1] + error[:, 2] * direction[:, 0]
    travel = corpus.travel_prefix[np.asarray(starts) + length] - corpus.travel_prefix[np.asarray(starts)]
    return {"position_cm": np.linalg.norm(error, axis=1), "horizontal_cm": np.linalg.norm(error[:, [0, 2]], axis=1),
            "vertical_cm": np.abs(error[:, 1]), "yaw_deg": np.abs(wrap(yaw - truth[:, 3])),
            "along_cm": along, "across_cm": across, "recorded_travel_cm": travel, "net_cm": length_cm}


def summarize(errors: dict, mask=None):
    keep = np.ones(len(errors["position_cm"]), bool) if mask is None else mask
    out = {name: quantiles(errors[name][keep]) for name in ("position_cm", "horizontal_cm", "vertical_cm", "yaw_deg")}
    if "along_cm" in errors:
        out["abs_along_cm"] = quantiles(np.abs(errors["along_cm"][keep]))
        out["abs_across_cm"] = quantiles(np.abs(errors["across_cm"][keep]))
        out["signed_along_cm_median"] = None if not keep.any() else round(float(np.median(errors["along_cm"][keep])), 3)
    return out


def table_summary(corpus: Corpus, mask):
    t = corpus.table_errors[mask]
    valid = np.isfinite(t).all(axis=1)
    return {name: quantiles(t[valid, i]) for i, name in enumerate(("position_cm", "horizontal_cm", "vertical_cm",
                                                                    "yaw_deg"))} | {"coverage": round(float(valid.mean()), 6) if len(t) else None}


def open_world(static_bin, table_path, agents, threads):
    world = eye.EyeWorld(static_bin, agents=agents, threads=threads)
    began = time.perf_counter()
    m = motion.Motion(world, table=table_path)
    return world, m, time.perf_counter() - began


def run(corpus_path: Path, table_path: Path, static_bin: Path, raw: Path, threads: int, agents: int, cache: Path | None,
        seconds=WINDOW_SECONDS):
    raw.mkdir(parents=True, exist_ok=True)
    began = time.perf_counter()
    table_digest = sha256(table_path)
    if table_digest != TABLE_SHA256:
        raise ValueError(f"table-full.json sha256 {table_digest} != research {TABLE_SHA256}")
    table = json.loads(table_path.read_text())
    total, episodes = load_test(corpus_path, cache)
    corpus = Corpus(episodes, table)
    load_s = time.perf_counter() - began
    print(f"{len(episodes)} test episodes of {total}, {len(corpus.keys)} decisions ({load_s:.0f} s)", flush=True)
    world, m, build_s = open_world(static_bin, table_path, agents, threads)
    result = {"corpus": {"path": str(corpus_path), "episodes_total": total, "test_episodes": len(episodes),
                         "test_decisions": int(len(corpus.keys)), "split_seed": motion_table.SEED,
                         "families": corpus.families},
              "table": {"path": str(table_path), "sha256": table_digest, "history": HISTORY},
              "static_bin": {"path": str(static_bin), "sha256": sha256(static_bin)},
              "world": {"threads": threads, "agents": agents, "shards": m.shards, "motion_build_s": round(build_s, 2),
                        "params": {name: round(float(getattr(motion.default_params(), name)), 6)
                                   for name, _ in motion.Params._fields_}}}

    # One step from every recorded pose.
    t0 = time.perf_counter()
    every = np.arange(len(corpus.keys))
    position, yaw, flags, table0, level0, counts = replay(m, corpus, every, 1)
    one = decompose(corpus, every, 1, position, yaw)
    one_s = time.perf_counter() - t0
    # The library's table key must be motion_table's (history 3, then shorter, then current only).
    level_agree = float(np.mean(level0 == corpus.table_level))
    finite = np.isfinite(corpus.table_predicted[:, 0])
    table_agree = float(np.mean(np.abs(table0[finite] - corpus.table_predicted[finite, :2]) < 1e-3))
    # The decision frame: course (yaw) and displacement (forward/right) separately.
    p0 = corpus.pose_index(every)
    rad = np.radians(corpus.pose[p0, 3])
    d = position - corpus.pose[p0, :3]
    stand_fr = np.column_stack((d[:, 0] * np.cos(rad) + d[:, 2] * np.sin(rad), -d[:, 0] * np.sin(rad) + d[:, 2] * np.cos(rad)))
    one["forward_cm"] = np.abs(stand_fr[:, 0] - corpus.target[:, 0])
    one["right_cm"] = np.abs(stand_fr[:, 1] - corpus.target[:, 1])
    stand_dyaw = wrap(yaw - corpus.pose[p0, 3])

    buckets = {}
    for name, mask in corpus.masks.items():
        b = {"steps": int(mask.sum()), "fraction": round(float(mask.mean()), 6),
             "stand": summarize(one, mask), "table": table_summary(corpus, mask),
             "stand_forward_cm": quantiles(one["forward_cm"][mask]), "stand_right_cm": quantiles(one["right_cm"][mask]),
             "stand_flags_fraction": {k: round(float(np.mean(counts[k][mask] > 0)), 6) if mask.any() else None
                                      for k in counts}}
        buckets[name] = b
    families = {}
    for f, family in enumerate(corpus.families):
        here = corpus.family == f
        families[family] = {name: {"steps": int((here & corpus.masks[name]).sum()),
                                   "stand": summarize(one, here & corpus.masks[name]),
                                   "table": table_summary(corpus, here & corpus.masks[name])}
                            for name in ("free", "probable_blocked", "ground")}
    turn = corpus.masks["turn_in_place"]
    recorded_dyaw = corpus.target[:, 3]
    turns = {"steps": int(turn.sum()), "free_steps": int((turn & corpus.masks["free"]).sum()),
             "stand_abs_deg_per_decision": quantiles(np.abs(stand_dyaw[turn])),
             "stand_deg_per_s_median": round(float(np.median(np.abs(stand_dyaw[turn]))) * 10, 4) if turn.any() else None,
             "recorded_abs_deg_per_decision": quantiles(np.abs(recorded_dyaw[turn])),
             "recorded_9deg_fraction": round(float(np.mean(np.abs(np.abs(recorded_dyaw[turn]) - 9) < .01)), 6),
             "recorded_18deg_fraction": round(float(np.mean(np.abs(np.abs(recorded_dyaw[turn]) - 18) < .01)), 6),
             "abs_error_deg": quantiles(np.abs(wrap(stand_dyaw[turn] - recorded_dyaw[turn]))),
             "sign_agrees": round(float(np.mean(np.sign(stand_dyaw[turn]) == np.sign(recorded_dyaw[turn]))), 6)}
    result["one_step"] = {"seconds": round(one_s, 1), "library_level_equals_motion_table": round(level_agree, 6),
                          "library_translation_equals_motion_table": round(table_agree, 6),
                          "buckets": buckets, "families": families, "turn_in_place": turns}
    np.savez_compressed(raw / "one-step.npz", episode=corpus.episode, step=corpus.step, family=corpus.family,
                        position=position.astype(np.float32), yaw=yaw.astype(np.float32), flags=flags,
                        level=level0, table_cm=table0, **{f"count_{k}": v for k, v in counts.items()},
                        **{f"mask_{k}": v for k, v in corpus.masks.items()})
    print(json.dumps({"one_step_free": buckets["free"]["stand"]["position_cm"],
                      "blocked": buckets["probable_blocked"]["stand"]["position_cm"],
                      "level_agree": level_agree, "seconds": one_s}), flush=True)

    # Open-loop windows.
    windows = {}
    for set_name in ("free", "ground"):
        windows[set_name] = {}
        for s in seconds:
            length = 10 * s
            starts = corpus.windows(corpus.masks[set_name], length)
            t0 = time.perf_counter()
            position, yaw, flags, _, _, counts = replay(m, corpus, starts, length)
            errors = decompose(corpus, starts, length, position, yaw)
            moving = errors["recorded_travel_cm"] >= 1
            entry = {"windows": int(len(starts)), "moving_windows": int(moving.sum()),
                     "all": summarize(errors), "moving": summarize(errors, moving),
                     "moving_with_stand_block": summarize(errors, moving & (counts["blocked"] > 0)),
                     "moving_without_stand_block": summarize(errors, moving & (counts["blocked"] == 0)),
                     "moving_by_family": {family: summarize(errors, moving & (corpus.family[starts] == f))
                                          for f, family in enumerate(corpus.families)},
                     "seconds": round(time.perf_counter() - t0, 1)}
            if s == 10:
                oracle_position, oracle_yaw, *_ = replay(m, corpus, starts, length, oracle=True)
                oracle = decompose(corpus, starts, length, oracle_position, oracle_yaw)
                entry["course_oracle_moving"] = summarize(oracle, moving)
                entry["course_share_of_p90"] = None
                if moving.any():
                    full, kept = np.quantile(errors["position_cm"][moving], .9), np.quantile(oracle["position_cm"][moving], .9)
                    entry["course_share_of_p90"] = round(float(1 - kept / full), 4) if full > 0 else None
                np.savez_compressed(raw / f"windows-{set_name}-{s}s.npz", starts=starts, position=position.astype(np.float32),
                                    yaw=yaw.astype(np.float32), oracle_position=oracle_position.astype(np.float32),
                                    **{f"count_{k}": v for k, v in counts.items()})
            windows[set_name][str(s)] = entry
            print(json.dumps({"set": set_name, "seconds": s, "windows": len(starts),
                              "moving_p90_cm": entry["moving"]["position_cm"]["p90"]}), flush=True)
    result["windows"] = windows
    world.close()

    free_p90 = buckets["free"]["stand"]["position_cm"]["p90"]
    blocked_p90 = buckets["probable_blocked"]["stand"]["position_cm"]["p90"]
    w_free = windows["free"]["10"]["moving"]["position_cm"]["p90"]
    w_ground = windows["ground"]["10"]["moving"]["position_cm"]["p90"]
    turn_rate = turns["stand_deg_per_s_median"]
    result["gates"] = {
        "free_step_p90_cm": {"value": free_p90, "threshold": THRESHOLDS["free_step_p90_cm"],
                             "passed": free_p90 is not None and free_p90 <= THRESHOLDS["free_step_p90_cm"]},
        "window_10s_moving_free_p90_cm": {"value": w_free, "threshold": THRESHOLDS["window_10s_moving_p90_cm"],
                                          "passed": w_free is not None and w_free <= THRESHOLDS["window_10s_moving_p90_cm"]},
        "window_10s_moving_ground_p90_cm": {"value": w_ground, "threshold": THRESHOLDS["window_10s_moving_p90_cm"],
                                            "passed": w_ground is not None and w_ground <= THRESHOLDS["window_10s_moving_p90_cm"]},
        "blocked_step_p90_cm": {"value": blocked_p90, "threshold": THRESHOLDS["blocked_step_p90_cm"],
                                "passed": blocked_p90 is not None and blocked_p90 <= THRESHOLDS["blocked_step_p90_cm"]},
        "turn_in_place_deg_per_s": {"value": turn_rate, "threshold": THRESHOLDS["turn_deg_per_s"],
                                    "passed": turn_rate is not None and abs(turn_rate - THRESHOLDS["turn_deg_per_s"]) < 0.01},
    }
    result["table_reference"] = TABLE_REFERENCE
    result["elapsed_s"] = round(time.perf_counter() - began, 1)
    (raw / "accept.json").write_text(json.dumps(result, indent=1) + "\n")
    print(json.dumps(result["gates"], indent=1), flush=True)
    return result


# ------------------------------------------------------------------ cost ----

def cost(static_bin: Path, table_path: Path, corpus_path: Path, cache: Path | None, raw: Path, threads_list, agents: int,
         warmup: int, steps: int, repeats: int):
    raw.mkdir(parents=True, exist_ok=True)
    table = json.loads(table_path.read_text())
    _, episodes = load_test(corpus_path, cache)
    corpus = Corpus(episodes, table)
    length = warmup + steps
    starts = corpus.windows(corpus.masks["ground"], length)
    moving = corpus.travel_prefix[starts + length] - corpus.travel_prefix[starts] >= 100
    starts = starts[moving]
    pick = starts[np.linspace(0, len(starts) - 1, agents).astype(np.int64)]  # spread over the corpus
    cpus = sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else []
    world, m, build_s = open_world(static_bin, table_path, agents, max(threads_list))
    p = corpus.pose_index(pick)
    s = corpus.step[pick]
    history = np.full((agents, 3, 4), -1, np.int8)
    for h in range(3):
        valid = s - 1 - h >= 0
        history[valid, h] = corpus.keys[pick[valid] - 1 - h]
    held = np.where(s >= 1, corpus.ages[np.maximum(pick - 1, 0)], 0).astype(np.int32)
    keys = corpus.keys[pick[:, None] + np.arange(length)[None, :]]
    out = motion.Ticks.empty(agents)
    rows = []
    for threads in threads_list:
        pinned = cpus[:threads] if len(cpus) >= threads else None
        world.set_threads(threads, pinned)
        for repeat in range(-1, repeats):
            m.set_state(corpus.pose[p, :3], corpus.pose[p, 3], EPOCH + 100 * s.astype(np.uint64), history, held)
            walls, load = [], os.getloadavg()
            for j in range(length):
                m.set_keys(keys[:, j])
                t0 = time.perf_counter()
                m.step(out=out)
                wall = time.perf_counter() - t0
                if j >= warmup:
                    walls.append(wall)
            if repeat < 0:
                continue
            total = float(np.sum(walls))
            rows.append({"threads": threads, "repeat": repeat, "pinned": pinned is not None, "agents": agents,
                         "decisions": steps, "wall_s": total,
                         "step_ms_median": float(np.median(walls)) * 1e3, "step_ms_p90": float(np.percentile(walls, 90)) * 1e3,
                         "wall_us_per_agent_decision": total * 1e6 / (agents * steps),
                         "thread_us_per_agent_decision": total * 1e6 * threads / (agents * steps),
                         "decisions_per_second": agents * steps / total, "load_average": list(load)})
            print(json.dumps(rows[-1]), flush=True)
    world.close()
    result = {"agents": agents, "warmup_decisions": warmup, "decisions": steps, "repeats": repeats,
              "ticks_per_decision": motion.TICKS, "shards": m.shards, "motion_build_s": build_s, "cpus": cpus,
              "rows": rows}
    (raw / f"cost-{'_'.join(map(str, threads_list))}.json").write_text(json.dumps(result, indent=1) + "\n")
    return result


# --------------------------------------------------------------- summary ----

def summary(raws, out: Path, meta_paths):
    accept = None
    rows = {}
    costs = []
    for raw in raws:
        raw = Path(raw)
        if (raw / "accept.json").is_file():
            accept = json.loads((raw / "accept.json").read_text())
            accept["raw"] = str(raw)
        for path in sorted(raw.glob("cost-*.json")):
            c = json.loads(path.read_text())
            c["raw"] = str(path)
            costs.append(c)
            for threads in {r["threads"] for r in c["rows"]}:  # a later run replaces an earlier one
                rows[threads] = [r for r in c["rows"] if r["threads"] == threads]
    if accept is None:
        raise ValueError("no accept.json among the raw folders")
    table = {}
    for threads in sorted(rows):
        group = rows[threads]
        pick = lambda k: float(np.median([r[k] for r in group]))  # noqa: E731
        table[str(threads)] = {"thread_us_per_agent_decision": round(pick("thread_us_per_agent_decision"), 3),
                               "wall_us_per_agent_decision": round(pick("wall_us_per_agent_decision"), 3),
                               "decisions_per_second": round(pick("decisions_per_second"), 1),
                               "step_ms_median": round(pick("step_ms_median"), 3),
                               "step_ms_p90": round(pick("step_ms_p90"), 3),
                               "repeats": len(group), "decisions_per_repeat": group[0]["decisions"],
                               "load_average": [r["load_average"] for r in group][:3]}
    meta = [json.loads(Path(p).read_text()) for p in meta_paths if Path(p).is_file()]
    result = {"schema": 1, "task": "S2 ground motion, acceptance 1 (research §5) and cost",
              "gates": accept["gates"], "table_reference": accept["table_reference"],
              "corpus": accept["corpus"], "table": accept["table"], "static_bin": accept["static_bin"],
              "world": accept["world"], "one_step": accept["one_step"], "windows": accept["windows"],
              "cost": {"agents": costs[0]["agents"] if costs else None, "ticks_per_decision": motion.TICKS,
                       "warmup_decisions": costs[0]["warmup_decisions"] if costs else None,
                       "by_threads": table}, "measurements": meta, "raw": [str(r) for r in raws]}
    text = json.dumps(result, ensure_ascii=False, separators=(",", ":")) + "\n"
    if len(text.encode()) > 100_000:
        # Drop the per-family window splits first: they repeat the per-family steps.
        for entry in result["windows"].values():
            for w in entry.values():
                w.pop("moving_by_family", None)
        text = json.dumps(result, ensure_ascii=False, separators=(",", ":")) + "\n"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(text)
    print(f"{out}: {len(text.encode())} bytes", flush=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    r = sub.add_parser("run")
    r.add_argument("corpus", type=Path)
    r.add_argument("table", type=Path)
    r.add_argument("static_bin", type=Path)
    r.add_argument("raw", type=Path)
    r.add_argument("--threads", type=int, default=8)
    r.add_argument("--agents", type=int, default=4096)
    r.add_argument("--cache", type=Path)
    r.add_argument("--seconds", type=int, nargs="+", default=list(WINDOW_SECONDS))
    c = sub.add_parser("cost")
    c.add_argument("corpus", type=Path)
    c.add_argument("table", type=Path)
    c.add_argument("static_bin", type=Path)
    c.add_argument("raw", type=Path)
    c.add_argument("--threads", default="1,8,28")
    c.add_argument("--agents", type=int, default=2048)
    c.add_argument("--warmup", type=int, default=10)
    c.add_argument("--steps", type=int, default=30)
    c.add_argument("--repeats", type=int, default=3)
    c.add_argument("--cache", type=Path)
    s = sub.add_parser("summary")
    s.add_argument("out", type=Path)
    s.add_argument("raw", type=Path, nargs="+")
    s.add_argument("--meta", type=Path, nargs="*", default=[])
    args = parser.parse_args()
    if args.command == "run":
        result = run(args.corpus, args.table, args.static_bin, args.raw, args.threads, args.agents, args.cache,
                     tuple(args.seconds))
        sys.exit(0 if all(g["passed"] for g in result["gates"].values()) else 1)
    if args.command == "cost":
        cost(args.static_bin, args.table, args.corpus, args.cache, args.raw,
             [int(t) for t in args.threads.split(",")], args.agents, args.warmup, args.steps, args.repeats)
    if args.command == "summary":
        summary(args.raw, args.out, args.meta)


if __name__ == "__main__":
    main()
