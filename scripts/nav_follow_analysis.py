#!/usr/bin/env python3
"""Explain saved follow trials from actions and physical trajectories, without a game."""
from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np


def analyze(directory):
    directory = Path(directory)
    metadata = json.loads((directory / "episode.json").read_text())
    actions = np.loadtxt(directory / "actions.txt", ndmin=2)
    actor = np.loadtxt(directory / "trajectory.txt", ndmin=2)
    target = np.loadtxt(directory / "target-trajectory.txt", ndmin=2)
    if (len(actions) != len(actor) or len(actor) != len(target)
            or not np.array_equal(actions[:, 0], target[:, 0])
            or not np.array_equal(actor[:, 0], target[:, 0])):
        raise ValueError(f"Unaligned follow traces: {directory}")
    times = target[:, 0]
    dt = np.diff(times)
    if not len(dt) or np.any(dt <= 0):
        raise ValueError(f"Invalid follow clock: {directory}")
    distances = target[:-1, 4]
    contacts = np.flatnonzero(target[:, 4] <= 500)
    post = np.arange(len(dt)) >= (int(contacts[0]) if len(contacts) else len(dt))
    actor_speed = np.linalg.norm(np.diff(actor[:, [1, 3]], axis=0), axis=1) * 1000 / dt
    target_speed = np.linalg.norm(np.diff(target[:, [1, 3]], axis=0), axis=1) * 1000 / dt
    walking = actions[:-1, 4] != 0
    turning = actions[:-1, 3] != 0
    moving = (actions[:-1, 1] != 0) | (actions[:-1, 2] != 0)
    masks = dict(near=post & (distances < 200), band=post & (distances >= 200) & (distances <= 500),
                 far=post & (distances > 500))
    duration = float(dt[post].sum())

    def speed(values, mask):
        return float(np.average(values[mask], weights=dt[mask])) if mask.any() else None

    bands = {name: dict(ms=float(dt[mask].sum()), fraction=float(dt[mask].sum()) / duration if duration else None,
                       actor_cm_s=speed(actor_speed, mask), target_cm_s=speed(target_speed, mask))
             for name, mask in masks.items()}
    far = masks["far"]
    conditions = dict(far_walking=far & walking, far_turning=far & turning,
                      far_neutral=far & ~moving, far_commanded_but_slow=far & moving & (actor_speed < 50),
                      far_target_running=far & (target_speed > 250))
    excursions = []
    for name in ("near", "far"):
        mask = masks[name]
        begins = np.flatnonzero(mask & ~np.r_[False, mask[:-1]])
        ends = np.flatnonzero(mask & ~np.r_[mask[1:], False]) + 1
        for begin, end in zip(begins, ends):
            window = np.arange(len(dt)) >= begin
            window &= np.arange(len(dt)) < end
            excursions.append(dict(band=name, start_ms=float(times[begin]), duration_ms=float(times[end] - times[begin]),
                min_distance_cm=float(target[begin:end + 1, 4].min()), max_distance_cm=float(target[begin:end + 1, 4].max()),
                actor_cm_s=speed(actor_speed, window), target_cm_s=speed(target_speed, window)))
    return dict(id=metadata.get("id", directory.name), teacher_version=metadata["teacher_version"],
                first_contact_ms=float(times[contacts[0]]) if len(contacts) else None,
                observed_ms=duration, bands=bands, conditions_ms={name: float(dt[mask].sum()) for name, mask in conditions.items()},
                blocked_spans=metadata.get("blocked_spans"), start_not_found_decisions=metadata.get("start_not_found_decisions"),
                longest_excursions=sorted(excursions, key=lambda row: -row["duration_ms"])[:8])


def summarize(episodes):
    groups = {}
    for version in sorted({row["teacher_version"] for row in episodes}):
        selected = [row for row in episodes if row["teacher_version"] == version]
        observed = sum(row["observed_ms"] for row in selected)
        groups[f"v{version}"] = dict(episodes=len(selected), observed_ms=observed,
            band_fractions={name: sum(row["bands"][name]["ms"] for row in selected) / observed if observed else None
                            for name in ("near", "band", "far")},
            conditions_ms={name: sum(row["conditions_ms"][name] for row in selected)
                           for name in selected[0]["conditions_ms"]})
    return dict(by_teacher=groups, episodes=episodes,
        notes=["Time-weighted intervals after first <=5m contact; terminal partial interval included.",
               "Speeds are horizontal physical displacement / game time, not requested animation speed.",
               "Conditions overlap; speed >250cm/s estimates target running, not a recorded gait label.",
               "No new game, teacher rollout, NAVDEMO recording or student training."])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("validation", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    episodes = [analyze(path.parent) for version in ("v3", "v2")
                for path in sorted((args.validation / version / "follow").glob("*/episode.json"))]
    if not episodes:
        raise ValueError("No follow trials")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(summarize(episodes), ensure_ascii=False, indent=2) + "\n")
    print(json.dumps(summarize(episodes)["by_teacher"], ensure_ascii=False), flush=True)


if __name__ == "__main__":
    main()
