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
- The error is split into course and displacement: the 10 s windows run a
  second time with the recorded yaw forced at every decision (course oracle),
  and the end error is split along and across the recorded path. Steps are
  broken down by the tables' buckets and by family, next to the table's own
  error on the same steps (history 3, its up and yaw columns included).
- Jump, climb, water and unsupported steps are S5: they are reported, not gated.
  The blocked gate is on blocked ground steps for the same reason.

``fit CORPUS TABLE STATIC OUT`` fits the stand's own table on the 750 training
episodes: the research table learns only "free" steps, and its block proxy also
drops slow animation transitions, so the stand runs every training step from
its recorded pose with the research table and keeps all ground steps on flat
ground that the stand itself does not block (the physics handles the rest). The
NPC's pace (run, walk) against the table is the ratio of its recorded to the
table's steady forward travel; the table is fitted on paced-out steps.

``cost STATIC TABLE RAW`` times one decision (3 ticks) of 2 048 agents on
recorded poses and keys at several pool sizes.

``summary OUT RAW...`` writes the compact motion.json (<= 100 KB).
"""
from __future__ import annotations

import argparse
from collections import namedtuple
import hashlib
import json
import os
from pathlib import Path
import pickle
import resource
import sys
import time

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE.parents[1]))
import motion_table  # noqa: E402
from stand import eye, motion  # noqa: E402

# Recorded decisions sit at 100 ms multiples from the episode start, and its ticks
# end at +33, +67, +100 (perception-safety-frames): 33/34/33 ms. On the stand's
# 30 Hz grid that is the phase 33 ms after a 100 ms multiple.
EPOCH = 1_000_000_033
TABLE_SHA256 = "174b661c91291eeba5d91c7dfafae6413a3e8bb9aa0923408e936128b87f05c8"  # research §3.5
HISTORY = 3
THRESHOLDS = {"free_step_p90_cm": 5.0, "window_10s_moving_p90_cm": 100.0, "blocked_step_p90_cm": 17.0,
              "turn_deg_per_s": 90.0}
TABLE_REFERENCE = {"free_step_p90_cm": 4.8, "window_10s_moving_free_p90_cm": 210.3, "blocked_step_p90_cm": 33.6}
WINDOW_SECONDS = (1, 3, 10)
STEADY_AGE = 5          # decisions with the same keys before a step counts towards the pace
PACE_MIN_STEPS = 30     # steady steps an NPC needs for its pace in one gait
Light = namedtuple("Light", "identifier family")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def peak_rss_mb():
    return round(resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / 1024, 1)  # Linux: kilobytes


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

def split(corpus: Path, which: str):
    root = corpus / "recordings"
    directories = sorted(root.glob("*/*/*/episode.json"))
    lights = [Light(str(p.parent.relative_to(root)), p.parent.relative_to(root).parts[0]) for p in directories]
    train, test = motion_table.split_episodes(lights, motion_table.SEED)
    return root, len(lights), sorted(e.identifier for e in (test if which == "test" else train))


def load_split(corpus: Path, which: str, cache: Path | None):
    """Episodes of the test or train split and their NPCs, cached between runs."""
    root, total, identifiers = split(corpus, which)
    if cache is not None and cache.is_file():
        stored = pickle.loads(cache.read_bytes())
        if stored["identifiers"] == identifiers and "npcs" in stored:
            print(f"{which} episodes from {cache}", flush=True)
            return total, stored["episodes"], stored["npcs"]
    began = time.perf_counter()
    episodes = []
    for i, identifier in enumerate(identifiers):
        episodes.append(motion_table.load_episode(root / identifier, root))
        if (i + 1) % 50 == 0:
            print(f"loaded {i + 1}/{len(identifiers)} {which} episodes ({time.perf_counter() - began:.0f} s)", flush=True)
    npcs = [json.loads((root / i / "episode.json").read_text()).get("npc", "") for i in identifiers]
    if cache is not None:
        cache.parent.mkdir(parents=True, exist_ok=True)
        cache.write_bytes(pickle.dumps({"identifiers": identifiers, "episodes": episodes, "npcs": npcs}))
    return total, episodes, npcs


def reference_of(table: dict):
    """motion_table.fit's block reference from the exported table: [level -1, level 0] {key: (n, [p75])}."""
    levels = table["block_reference_levels"]
    return [{int(k): (int(n), np.array([v])) for k, n, v in levels[str(level)]} for level in (-1, 0)]


def tables_of(table: dict):
    return [{int(row[0]): (int(row[1]), np.array(row[2:6], float)) for row in table["levels"][str(level)]}
            for level in range(-1, 4)]


class Corpus:
    """Episodes flattened: decisions, poses and masks with per-episode offsets.

    ``reference`` is the research table (its block proxy defines the buckets);
    ``table`` and ``pace`` ({npc: [run, walk]}) are the translation under test."""

    def __init__(self, episodes, npcs, reference: dict, table: dict | None = None, pace: dict | None = None):
        table = reference if table is None else table
        self.episodes = episodes
        self.families = sorted({e.family for e in episodes})
        n = np.array([len(e.keys) for e in episodes])
        self.step_offset = np.r_[0, np.cumsum(n)][:-1]
        self.pose_offset = np.r_[0, np.cumsum(n + 1)][:-1]
        self.keys = np.concatenate([e.keys for e in episodes]).astype(np.int8)
        self.ids = np.concatenate([e.ids for e in episodes])
        self.ages = np.concatenate([motion.ages(e.keys) for e in episodes])
        self.pose = np.concatenate([e.pose for e in episodes]).astype(np.float64)
        self.target = np.concatenate([e.target for e in episodes])
        self.episode = np.repeat(np.arange(len(episodes)), n)
        self.step = np.concatenate([np.arange(k) for k in n])
        self.family = np.repeat(np.array([self.families.index(e.family) for e in episodes]), n)
        self.npcs = list(npcs)
        self.episode_pace = np.array([(pace or {}).get(npc, [1.0, 1.0]) for npc in self.npcs], np.float32).reshape(-1, 2)
        self.paced = pace is not None
        classified = [motion_table.masks(e, reference_of(reference)) for e in episodes]
        self.masks = {name: np.concatenate([c[name] for c in classified]) for name in classified[0]}
        self.masks["ground"] = np.concatenate([e.supported & ~e.unsafe & ~e.wet & ~e.near_jump & ~e.near_climb
                                               for e in episodes])
        self.masks["probable_blocked_ground"] = self.masks["probable_blocked"] & self.masks["ground"]
        self.masks["s5"] = ~self.masks["ground"]
        self.masks["turn_in_place"] = (self.keys[:, 0] == 0) & (self.keys[:, 1] == 0) & (self.keys[:, 2] != 0)
        # The tables alone, as motion_table evaluates them (history 3): the research one and the one under test.
        self.reference_errors = self._table_errors(tables_of(reference), None)[0]
        self.table_errors, self.table_predicted, self.table_level = self._table_errors(tables_of(table), pace)
        self.travel = np.linalg.norm(self.target[:, :2], axis=1)
        self.travel_prefix = np.r_[0, np.cumsum(self.travel)]

    def _table_errors(self, tables, pace):
        predicted, level = zip(*(motion_table.lookup(e.ids, tables, HISTORY) for e in self.episodes))
        predicted, level = np.concatenate(predicted), np.concatenate(level)
        if pace is not None:
            predicted[:, :2] *= self.step_pace()[:, None]
        return motion_table.step_errors(predicted, self.target), predicted, level

    def step_pace(self):
        return self.episode_pace[self.episode, self.keys[:, 3].astype(np.int64)]

    def pose_index(self, g):
        return self.pose_offset[self.episode[g]] + self.step[g]

    def windows(self, mask, length):
        """Global start steps of all overlapping windows of ``length`` decisions inside ``mask``."""
        starts = []
        for offset, n in zip(self.step_offset, [len(x.keys) for x in self.episodes]):
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
        m.set_speed(corpus.episode_pace[corpus.episode[g]])
        m.set_state(corpus.pose[p, :3], corpus.pose[p, 3], EPOCH + 100 * s.astype(np.uint64), history, held)
        keys = corpus.keys[g[:, None] + steps[None, :]]
        recorded_yaw = corpus.pose[p[:, None] + steps[None, :], 3]
        out = motion.Ticks.empty(b)
        for j in range(length):
            m.set_keys(keys[:, j])
            if oracle:
                m.set_yaw(recorded_yaw[:, j])
            m.step(0, b, out=out)
            flags_any[b0:b0 + b] |= np.bitwise_or.reduce(out.flags, axis=1)
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
    starts = np.asarray(starts)
    p0 = corpus.pose_index(starts)
    truth = corpus.pose[p0 + length]
    error = position - truth[:, :3]
    recorded = truth[:, [0, 2]] - corpus.pose[p0][:, [0, 2]]
    length_cm = np.linalg.norm(recorded, axis=1)
    direction = np.where(length_cm[:, None] > 1e-6, recorded / np.maximum(length_cm, 1e-6)[:, None], 0)
    along = error[:, 0] * direction[:, 0] + error[:, 2] * direction[:, 1]
    across = -error[:, 0] * direction[:, 1] + error[:, 2] * direction[:, 0]
    travel = corpus.travel_prefix[starts + length] - corpus.travel_prefix[starts]
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


def table_summary(errors, mask):
    t = errors[mask]
    valid = np.isfinite(t).all(axis=1)
    return {name: quantiles(t[valid, i]) for i, name in enumerate(("position_cm", "horizontal_cm", "vertical_cm",
                                                                    "yaw_deg"))} | {"coverage": round(float(valid.mean()), 6) if len(t) else None}


def open_world(static_bin, table, agents, threads):
    world = eye.EyeWorld(static_bin, agents=agents, threads=threads)
    began = time.perf_counter()
    m = motion.Motion(world, table=table)
    return world, m, time.perf_counter() - began


def levels_of(table: dict):
    """motion.set_table levels of an exported table dict."""
    out = {}
    for level, rows in table["levels"].items():
        rows = np.asarray(rows, np.float64).reshape(-1, 6)
        out[int(level)] = (rows[:, 0].astype(np.int64), np.ascontiguousarray(rows[:, 2:4], np.float32))
    return out


def load_reference(table_path: Path) -> dict:
    digest = sha256(table_path)
    if digest != TABLE_SHA256:
        raise ValueError(f"table-full.json sha256 {digest} != research {TABLE_SHA256}")
    return json.loads(table_path.read_text())


def run(corpus_path: Path, table_path: Path, static_bin: Path, raw: Path, threads: int, agents: int, cache: Path | None,
        seconds=WINDOW_SECONDS, stand_table: Path | None = None, use_stand_table: bool = False, pace: bool = False,
        tag: str = "r0"):
    """Acceptance of one translation: the research table or the fitted stand table, with or without the NPCs' pace
    (both from ``stand_table``, the output of fit)."""
    raw.mkdir(parents=True, exist_ok=True)
    began = time.perf_counter()
    reference = load_reference(table_path)
    fitted = json.loads(stand_table.read_text()) if stand_table is not None else None
    if (pace or use_stand_table) and fitted is None:
        raise ValueError("the stand table and the pace come from --stand-table (fit)")
    table = fitted if use_stand_table else reference
    paces = fitted["pace"] if pace else None
    total, episodes, npcs = load_split(corpus_path, "test", cache)
    corpus = Corpus(episodes, npcs, reference, table, paces)
    print(f"[{tag}] {len(episodes)} test episodes of {total}, {len(corpus.keys)} decisions "
          f"({time.perf_counter() - began:.0f} s)", flush=True)
    world, m, build_s = open_world(static_bin, levels_of(table), agents, threads)
    result = {"tag": tag,
              "translation": {"table": "research table-full.json" if table is reference else "stand table (fit)",
                              "pace": bool(pace),
                              "test_npcs_with_pace": None if paces is None else int(sum(n in paces for n in set(npcs))),
                              "test_npcs": len(set(npcs))},
              "corpus": {"path": str(corpus_path), "episodes_total": total, "test_episodes": len(episodes),
                         "test_decisions": int(len(corpus.keys)), "split_seed": motion_table.SEED,
                         "families": corpus.families},
              "table": {"path": str(table_path), "sha256": TABLE_SHA256, "history": HISTORY,
                        "stand_table": None if stand_table is None else {"path": str(stand_table),
                                                                        "sha256": sha256(stand_table)}},
              "static_bin": {"path": str(static_bin), "sha256": sha256(static_bin)},
              "world": {"threads": threads, "agents": agents, "shards": m.shards, "motion_build_s": round(build_s, 2),
                        "tick_phase": "33/34/33 ms after each decision, as the corpus",
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
    unpaced = corpus.table_predicted[finite, :2] / (corpus.step_pace()[finite, None] if pace else 1)
    table_agree = float(np.mean(np.abs(table0[finite] - unpaced) < 1e-3))
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
        buckets[name] = {"steps": int(mask.sum()), "fraction": round(float(mask.mean()), 6),
                         "stand": summarize(one, mask), "table": table_summary(corpus.table_errors, mask),
                         "research_table": table_summary(corpus.reference_errors, mask),
                         "stand_forward_cm": quantiles(one["forward_cm"][mask]),
                         "stand_right_cm": quantiles(one["right_cm"][mask]),
                         "stand_flags_fraction": {k: round(float(np.mean(counts[k][mask] > 0)), 6) if mask.any() else None
                                                  for k in counts}}
    # Blocked ground steps: those the stand blocks too (static geometry) and the rest.
    blocked = corpus.masks["probable_blocked_ground"]
    stand_blocked = counts["blocked"] > 0
    split_blocked = {name: {"steps": int(sel.sum()), "stand": summarize(one, sel),
                            "table": table_summary(corpus.table_errors, sel),
                            "age0_fraction": round(float(np.mean(corpus.ages[sel] == 0)), 4) if sel.any() else None}
                     for name, sel in (("stand_blocked", blocked & stand_blocked), ("stand_free", blocked & ~stand_blocked))}
    families = {}
    for f, family in enumerate(corpus.families):
        here = corpus.family == f
        families[family] = {name: {"steps": int((here & corpus.masks[name]).sum()),
                                   "stand": summarize(one, here & corpus.masks[name]),
                                   "table": table_summary(corpus.table_errors, here & corpus.masks[name])}
                            for name in ("free", "probable_blocked_ground", "ground")}
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
                          "buckets": buckets, "blocked_ground_split": split_blocked, "families": families,
                          "turn_in_place": turns}
    np.savez_compressed(raw / f"one-step-{tag}.npz", episode=corpus.episode, step=corpus.step, family=corpus.family,
                        position=position.astype(np.float32), yaw=yaw.astype(np.float32), flags=flags,
                        level=level0, table_cm=table0, **{f"count_{k}": v for k, v in counts.items()},
                        **{f"mask_{k}": v for k, v in corpus.masks.items()})
    print(json.dumps({"tag": tag, "one_step_free": buckets["free"]["stand"]["position_cm"],
                      "blocked_ground": buckets["probable_blocked_ground"]["stand"]["position_cm"],
                      "level_agree": level_agree, "translation_agree": table_agree, "seconds": one_s}), flush=True)

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
                np.savez_compressed(raw / f"windows-{set_name}-{s}s-{tag}.npz", starts=starts,
                                    position=position.astype(np.float32), yaw=yaw.astype(np.float32),
                                    oracle_position=oracle_position.astype(np.float32),
                                    **{f"count_{k}": v for k, v in counts.items()})
            windows[set_name][str(s)] = entry
            print(json.dumps({"tag": tag, "set": set_name, "seconds": s, "windows": len(starts),
                              "moving_p90_cm": entry["moving"]["position_cm"]["p90"]}), flush=True)
    result["windows"] = windows
    world.close()

    p90 = lambda b: buckets[b]["stand"]["position_cm"]["p90"]  # noqa: E731
    w90 = lambda name: windows[name]["10"]["moving"]["position_cm"]["p90"] if "10" in windows[name] else None  # noqa: E731
    gate = lambda value, threshold: {"value": value, "threshold": threshold,  # noqa: E731
                                     "passed": value is not None and value <= threshold}
    turn_rate = turns["stand_deg_per_s_median"]
    result["gates"] = {
        "free_step_p90_cm": gate(p90("free"), THRESHOLDS["free_step_p90_cm"]),
        "window_10s_moving_free_p90_cm": gate(w90("free"), THRESHOLDS["window_10s_moving_p90_cm"]),
        "window_10s_moving_ground_p90_cm": gate(w90("ground"), THRESHOLDS["window_10s_moving_p90_cm"]),
        "blocked_ground_step_p90_cm": gate(p90("probable_blocked_ground"), THRESHOLDS["blocked_step_p90_cm"]),
        "turn_in_place_deg_per_s": {"value": turn_rate, "threshold": THRESHOLDS["turn_deg_per_s"],
                                    "passed": turn_rate is not None and abs(turn_rate - THRESHOLDS["turn_deg_per_s"]) < 0.01},
    }
    result["info"] = {"blocked_all_step_p90_cm": p90("probable_blocked"),
                      "research_table_blocked_ground_p90_cm": buckets["probable_blocked_ground"]["research_table"]["position_cm"]["p90"]}
    result["table_reference"] = TABLE_REFERENCE
    result["elapsed_s"] = round(time.perf_counter() - began, 1)
    result["peak_rss_mb"] = peak_rss_mb()
    (raw / f"accept-{tag}.json").write_text(json.dumps(result, indent=1) + "\n")
    print(json.dumps({"tag": tag, "gates": result["gates"]}, indent=1), flush=True)
    return result


# ------------------------------------------------------------------- fit ----

def paces_of(corpus: Corpus, fitted_mask, predicted):
    """{npc: [run, walk]}: recorded over predicted forward travel on its steady forward steps."""
    steady = (fitted_mask & (corpus.ages >= STEADY_AGE) & (corpus.keys[:, 0] == 1) & (corpus.keys[:, 1] == 0)
              & (corpus.keys[:, 2] == 0) & np.isfinite(predicted[:, 0]))
    names = sorted(set(corpus.npcs))
    npc_of_step = np.array([names.index(n) for n in corpus.npcs])[corpus.episode]
    paces, steps = {}, {}
    for code, npc in enumerate(names):
        here = steady & (npc_of_step == code)
        pace, used = [1.0, 1.0], [0, 0]
        for walk in (0, 1):
            sel = here & (corpus.keys[:, 3] == walk)
            if sel.sum() >= PACE_MIN_STEPS and predicted[sel, 0].sum() > 0:
                pace[walk] = float(corpus.target[sel, 0].sum() / predicted[sel, 0].sum())
                used[walk] = int(sel.sum())
        paces[npc], steps[npc] = [round(pace[0], 5), round(pace[1], 5)], used
    return paces, steps


def fit(corpus_path: Path, table_path: Path, static_bin: Path, out: Path, threads: int, agents: int, cache: Path | None):
    began = time.perf_counter()
    reference = load_reference(table_path)
    total, episodes, npcs = load_split(corpus_path, "train", cache)
    corpus = Corpus(episodes, npcs, reference)
    print(f"{len(episodes)} train episodes, {len(corpus.keys)} decisions ({time.perf_counter() - began:.0f} s)", flush=True)
    world, m, _ = open_world(static_bin, levels_of(reference), agents, threads)
    every = np.arange(len(corpus.keys))
    position, _, _, table0, _, counts = replay(m, corpus, every, 1)
    world.close()
    stand_h = np.linalg.norm((position - corpus.pose[corpus.pose_index(every), :3])[:, [0, 2]], axis=1)
    table_h = np.linalg.norm(table0, axis=1)
    static = (counts["blocked"] > 0) | ((table_h >= 2) & (stand_h < .5 * table_h))
    flat_ground = corpus.masks["flat_before_block_filter"]
    keep = flat_ground & ~static
    # Pass 1: pooled table, the NPCs' pace against it; pass 2: the table of paced-out steps.
    pooled = [motion_table.grouped(corpus.ids[keep, c], corpus.target[keep]) for c in range(5)]
    predicted = np.concatenate([motion_table.lookup(e.ids, pooled, HISTORY)[0] for e in episodes])
    paces, _ = paces_of(corpus, keep, predicted)
    corpus.episode_pace = np.array([paces[n] for n in corpus.npcs], np.float32)
    normalized = corpus.target.copy()
    normalized[:, :2] /= np.maximum(corpus.step_pace(), 1e-3)[:, None]
    tables = [motion_table.grouped(corpus.ids[keep, c], normalized[keep]) for c in range(5)]
    predicted = np.concatenate([motion_table.lookup(e.ids, tables, HISTORY)[0] for e in episodes])
    paces, pace_steps = paces_of(corpus, keep, predicted)
    exported = motion_table.compact_tables(tables)
    result = {"schema": 1, "kind": "nav-sim stand motion table (S2)",
              "columns": ["packed_key", "training_steps", "forward_cm", "right_cm", "up_cm", "yaw_delta_deg"],
              "key": "motion_table.key_ids, same packing and lookup",
              "levels": exported,
              "block_reference_levels": reference["block_reference_levels"],
              "pace": paces, "pace_steady_steps": pace_steps,
              "fit": {"source": "nav-demos-v5 training split (motion_table.split_episodes, seed "
                                f"{motion_table.SEED})", "episodes": len(episodes), "decisions": int(len(corpus.keys)),
                      "rule": "ground steps on flat ground (motion_table terrain candidate: supported, no body flags, "
                              "slope <10 deg, no water, outside +/-500 ms of a jump or climb) that the stand does not "
                              "block: no blocked tick and at least half the research table's translation (>=2 cm) "
                              "when replayed from the recorded pose; forward/right divided by the NPC's pace",
                      "steps_flat_ground": int(flat_ground.sum()), "steps_static_blocked": int((flat_ground & static).sum()),
                      "steps_fitted": int(keep.sum()), "research_free_steps": int(corpus.masks["free"].sum()),
                      "pace": f"recorded / table forward travel over steady forward steps (same keys for >= {STEADY_AGE} "
                              f"decisions, no strafe or turn), per NPC and gait, >= {PACE_MIN_STEPS} steps, else 1",
                      "npcs": len(paces),
                      "pace_quantiles_run_walk": [quantiles([p[w] for p, s in zip(paces.values(), pace_steps.values()) if s[w]])
                                                  for w in (0, 1)],
                      "levels": {level: len(rows) for level, rows in exported.items()},
                      "elapsed_s": round(time.perf_counter() - began, 1), "peak_rss_mb": peak_rss_mb()}}
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(result, separators=(",", ":")) + "\n")
    print(json.dumps(result["fit"]), flush=True)
    return result


# ------------------------------------------------------------------ cost ----

def cost(static_bin: Path, table_path: Path, corpus_path: Path, cache: Path | None, raw: Path, threads_list, agents: int,
         warmup: int, steps: int, repeats: int):
    raw.mkdir(parents=True, exist_ok=True)
    reference = load_reference(table_path)
    _, episodes, npcs = load_split(corpus_path, "test", cache)
    corpus = Corpus(episodes, npcs, reference)
    length = warmup + steps
    starts = corpus.windows(corpus.masks["ground"], length)
    moving = corpus.travel_prefix[starts + length] - corpus.travel_prefix[starts] >= 100
    starts = starts[moving]
    pick = starts[np.linspace(0, len(starts) - 1, agents).astype(np.int64)]  # spread over the corpus
    cpus = sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else []
    world, m, build_s = open_world(static_bin, levels_of(reference), agents, max(threads_list))
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
              "rows": rows, "peak_rss_mb": peak_rss_mb()}
    (raw / f"cost-{'_'.join(map(str, threads_list))}.json").write_text(json.dumps(result, indent=1) + "\n")
    return result


# --------------------------------------------------------------- summary ----

def _compact_one(a: dict) -> dict:
    o = a["one_step"]
    pick = lambda s: {k: s[k] for k in ("position_cm", "horizontal_cm", "vertical_cm", "yaw_deg") if k in s}  # noqa: E731
    buckets = {name: {"steps": b["steps"], "fraction": b["fraction"], "stand": pick(b["stand"]),
                      "stand_forward_cm": b["stand_forward_cm"], "stand_right_cm": b["stand_right_cm"],
                      "table": {"position_cm": b["table"]["position_cm"]},
                      "research_table": {"position_cm": b["research_table"]["position_cm"]},
                      "stand_flags_fraction": b["stand_flags_fraction"]}
               for name, b in o["buckets"].items() if b["steps"]}
    families = {f: {name: {"steps": v["steps"], "stand_p90_cm": v["stand"]["position_cm"]["p90"],
                           "table_p90_cm": v["table"]["position_cm"]["p90"]} for name, v in d.items()}
                for f, d in o["families"].items()}
    windows = {}
    for set_name, entries in a["windows"].items():
        windows[set_name] = {}
        for s, w in entries.items():
            e = {k: w[k] for k in ("windows", "moving_windows", "seconds")}
            e["moving"] = w["moving"]
            e["moving_with_stand_block_p90_cm"] = w["moving_with_stand_block"]["position_cm"]["p90"]
            e["moving_without_stand_block_p90_cm"] = w["moving_without_stand_block"]["position_cm"]["p90"]
            e["moving_by_family_p90_cm"] = {f: v["position_cm"]["p90"] for f, v in w["moving_by_family"].items()}
            if "course_oracle_moving" in w:
                e["course_oracle_moving_position_cm"] = w["course_oracle_moving"]["position_cm"]
                e["course_share_of_p90"] = w["course_share_of_p90"]
            windows[set_name][s] = e
    return {"tag": a["tag"], "translation": a["translation"], "gates": a["gates"], "info": a["info"],
            "one_step": {"library_level_equals_motion_table": o["library_level_equals_motion_table"],
                         "library_translation_equals_motion_table": o["library_translation_equals_motion_table"],
                         "buckets": buckets, "blocked_ground_split": {k: {"steps": v["steps"], "age0_fraction": v["age0_fraction"],
                                                                          "stand": pick(v["stand"]),
                                                                          "table_position_cm": v["table"]["position_cm"]}
                                                                      for k, v in o["blocked_ground_split"].items()},
                         "families": families, "turn_in_place": o["turn_in_place"]},
            "windows": windows}


def summary(raws, out: Path, final: str | None = None):
    rounds, fits, rows, costs, meta = {}, [], {}, [], []
    for raw in map(Path, raws):
        nas = "/mnt/shared/gothic-ai/" + str(raw).split("gothic-ai/", 1)[-1]
        for path in sorted(raw.glob("accept-*.json")):
            a = json.loads(path.read_text())
            rounds[a["tag"]] = _compact_one(a) | {"raw": f"{nas}/{path.name}"}
        for path in sorted(raw.glob("table-stand.json")):
            f = json.loads(path.read_text())
            fits.append(f["fit"] | {"raw": f"{nas}/{path.name}", "sha256": sha256(path)})
        for path in sorted(raw.glob("cost-*.json")):
            c = json.loads(path.read_text())
            costs.append(c | {"raw": f"{nas}/{path.name}"})
            for threads in {r["threads"] for r in c["rows"]}:  # a later run replaces an earlier one
                rows[threads] = [r for r in c["rows"] if r["threads"] == threads]
        for path in sorted(raw.glob("meta-*.json")):
            meta.append(json.loads(path.read_text()) | {"raw": f"{nas}/{path.name}"})
    if not rounds:
        raise ValueError("no accept-*.json among the raw folders")
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
                               "load_average_at_repeats": [r["load_average"] for r in group]}
    final = final or sorted(rounds)[-1]
    for m in meta:
        m["gq_status"] = m.get("gq_status", "").splitlines()[:12]
    result = {"schema": 1, "task": "S2 ground motion: acceptance 1 (research §5) and cost",
              "final": final, "gates": rounds[final]["gates"], "table_reference": TABLE_REFERENCE,
              "rounds": rounds, "fit": fits,
              "cost": {"agents": costs[0]["agents"] if costs else None, "ticks_per_decision": motion.TICKS,
                       "warmup_decisions": costs[0]["warmup_decisions"] if costs else None,
                       "shards": costs[0]["shards"] if costs else None, "by_threads": table,
                       "raw": [c["raw"] for c in costs]},
              "measurements": meta}
    text = json.dumps(result, ensure_ascii=False, separators=(",", ":")) + "\n"
    for drop in ("families", "moving_by_family_p90_cm", "stand_flags_fraction"):
        if len(text.encode()) <= 100_000:
            break
        for r in result["rounds"].values():  # the non-final rounds lose details first
            if r["tag"] == final:
                continue
            if drop == "families":
                r["one_step"].pop("families", None)
            elif drop == "stand_flags_fraction":
                for b in r["one_step"]["buckets"].values():
                    b.pop(drop, None)
            else:
                for entries in r["windows"].values():
                    for w in entries.values():
                        w.pop(drop, None)
        text = json.dumps(result, ensure_ascii=False, separators=(",", ":")) + "\n"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(text)
    print(f"{out}: {len(text.encode())} bytes", flush=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("corpus", type=Path)
    common.add_argument("table", type=Path)
    common.add_argument("static_bin", type=Path)
    r = sub.add_parser("run", parents=[common])
    r.add_argument("raw", type=Path)
    r.add_argument("--threads", type=int, default=8)
    r.add_argument("--agents", type=int, default=4096)
    r.add_argument("--cache", type=Path)
    r.add_argument("--seconds", type=int, nargs="+", default=list(WINDOW_SECONDS))
    r.add_argument("--stand-table", type=Path, help="table-stand.json of fit")
    r.add_argument("--use-stand-table", action="store_true", help="translate with the fitted table, not the research one")
    r.add_argument("--pace", action="store_true", help="the NPCs' pace from --stand-table")
    r.add_argument("--tag", default="r0")
    f = sub.add_parser("fit", parents=[common])
    f.add_argument("out", type=Path)
    f.add_argument("--threads", type=int, default=8)
    f.add_argument("--agents", type=int, default=4096)
    f.add_argument("--cache", type=Path)
    c = sub.add_parser("cost", parents=[common])
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
    s.add_argument("--final")
    args = parser.parse_args()
    if args.command == "run":
        result = run(args.corpus, args.table, args.static_bin, args.raw, args.threads, args.agents, args.cache,
                     tuple(args.seconds), args.stand_table, args.use_stand_table, args.pace, args.tag)
        sys.exit(0 if all(g["passed"] for g in result["gates"].values()) else 1)
    if args.command == "fit":
        fit(args.corpus, args.table, args.static_bin, args.out, args.threads, args.agents, args.cache)
    if args.command == "cost":
        cost(args.static_bin, args.table, args.corpus, args.cache, args.raw,
             [int(t) for t in args.threads.split(",")], args.agents, args.warmup, args.steps, args.repeats)
    if args.command == "summary":
        summary(args.raw, args.out, args.final)


if __name__ == "__main__":
    main()
