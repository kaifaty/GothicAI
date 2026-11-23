#!/usr/bin/env python3
"""Offline DAgger4 label and live-input analysis; never loads a policy or engine.

uv run --with numpy --with pillow python scripts/nav_dagger_circling.py \
    --shared-root /Volumes/shared/gothic-ai
Requires numpy and Pillow (the existing live binary reader imports Pillow).
Output is JSON; source recordings are read-only.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path
import sys

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import nav_demos


HEADS = ("forward", "strafe", "turn", "walk", "jump", "climb")


def read_json(path):
    return json.loads(path.read_text())


def rows(path):
    with path.open() as stream:
        return [json.loads(line) for line in stream if line.strip()]


def action_counts(keys, mask=None):
    if mask is None:
        mask = np.ones(len(keys), bool)
    forward, strafe, turn, walk, jump, climb = keys.T
    adjacent = mask[:-1] & mask[1:]
    turning = adjacent & (turn[:-1] != 0) & (turn[1:] != 0)
    return Counter(n=int(mask.sum()), forward=int(((forward > 0) & mask).sum()),
        turn_only=int(((forward == 0) & (strafe == 0) & (turn != 0) & mask).sum()),
        walk=int(((walk != 0) & mask).sum()),
        idle=int(((forward == 0) & (strafe == 0) & (turn == 0)
                  & (jump == 0) & (climb == 0) & mask).sum()),
        adjacent=int(adjacent.sum()), adjacent_turning=int(turning.sum()),
        flips=int(((turn[:-1] * turn[1:] < 0) & adjacent).sum()))


def fractions(counts):
    return {**dict(counts), **{name + "_fraction": counts[name] / counts["n"] if counts["n"] else None
        for name in ("forward", "turn_only", "walk", "idle")},
        "flip_fraction": counts["flips"] / counts["adjacent"] if counts["adjacent"] else None,
        "flip_given_turning": counts["flips"] / counts["adjacent_turning"] if counts["adjacent_turning"] else None}


def close_opposite_pairs(direction, distance, maps, turn):
    """All unordered within-episode pairs, not just adjacent samples.

    Goal unit-vector separation <=5 degrees, distance difference <=50 cm.
    Near-map central 8x8 m, seven probability channels, pooled to 4x4 cells:
    mean absolute difference <=.02 and every pooled value differs <=.10.
    Maps are in [0,1]. No map, temporal or episode subsampling.
    """
    matched = np.zeros(len(turn), bool)
    pair_count = 0
    examples = []
    # Work one left endpoint at a time to bound temporary memory in long stalls.
    for i in np.flatnonzero(turn):
        candidates = np.flatnonzero((np.arange(len(turn)) > i) & (turn * turn[i] < 0)
            & (np.abs(distance - distance[i]) <= 50)
            & (np.sum(direction * direction[i], axis=1) >= np.cos(np.deg2rad(5))))
        if not len(candidates):
            continue
        delta = np.abs(maps[candidates] - maps[i])
        good = candidates[(delta.mean(axis=1) <= .02) & (delta.max(axis=1) <= .10)]
        matched[good] = True
        if len(good):
            matched[i] = True
            pair_count += len(good)
            if not examples:
                j = int(good[0])
                examples.append(dict(i=int(i), j=j, map_mae=float(np.abs(maps[i] - maps[j]).mean()),
                    map_max=float(np.abs(maps[i] - maps[j]).max()),
                    goal_angle_difference_deg=float(np.rad2deg(np.arccos(np.clip(direction[i] @ direction[j], -1, 1)))),
                    goal_distance_difference_cm=float(abs(distance[i] - distance[j]))))
    return matched, pair_count, examples


def path_distance(point, path):
    """Horizontal distance to a frozen polyline, including degenerate segments."""
    path = np.asarray(path, np.float64)
    if len(path) == 1:
        return float(np.linalg.norm(path[0] - point))
    delta = np.diff(path, axis=0)
    square = (delta * delta).sum(axis=1)
    ratio = np.divide(((point - path[:-1]) * delta).sum(axis=1), square,
                      out=np.zeros_like(square), where=square != 0)
    error = point - (path[:-1] + np.clip(ratio, 0, 1)[:, None] * delta)
    return float(np.sqrt((error * error).sum(axis=1).min()))


def frozen_path_distances(directory, stamps, position, teacher_version):
    result = np.full(len(stamps), np.nan)
    if teacher_version != 3:
        # v2's full retained corridor is not in teacher-paths.jsonl.
        return result
    plans = rows(directory / "teacher-paths.jsonl")
    if [p["elapsed_ms"] for p in plans] != stamps.tolist():
        raise ValueError(f"Teacher path timestamps differ: {directory}")
    for i, now in enumerate(stamps):
        if now < 2000:
            continue
        old = np.searchsorted(stamps, now - 2000, side="right") - 1
        plan = plans[old]
        local = np.asarray(plan["local_plan_cm"], np.float64).reshape(-1, 2)
        yaw = np.deg2rad(plan["yaw_deg"])
        origin = np.asarray(plan["position_cm"])[[0, 2]]
        world = np.column_stack((local[:, 1] * np.cos(yaw) - local[:, 0] * np.sin(yaw),
                                 local[:, 1] * np.sin(yaw) + local[:, 0] * np.cos(yaw))) + origin
        result[i] = path_distance(position[i, [0, 2]], np.vstack((origin, world)))
    return result


def teacher_corpus(root, version):
    source = root / ("archive/nav-demos-v4-20261029" if version == 4 else "nav-demos-v5")
    manifest = read_json(source / "manifest.json")
    selected = {identity for group in manifest["splits"]["train"].values() for identity in group}
    counts, families = Counter(), defaultdict(Counter)
    episodes = 0
    for entry in manifest["episodes"]:
        if entry["id"] not in selected:
            continue
        if not entry["training_eligible"]:
            raise ValueError("Training selection includes an ineligible episode")
        directory = source / "recordings" / entry["family"] / "train" / entry["id"]
        actions = np.loadtxt(directory / "actions.txt", ndmin=2)
        n = entry["decisions"]
        if len(actions) != n + 1 or not (np.diff(actions[:n, 0]) == 100).all():
            raise ValueError(f"Unexpected teacher action count/clock: {directory}")
        # These are native teacher recordings, not DAgger drives. Final stop is excluded.
        keys = actions[:n, 1:7]
        if version == 5:
            compact = read_json(root / "nav-lean-data/corpus-v5" / entry["family"] / "train" / entry["id"] / "episode.json")
            stats = compact["nav_bc_stats"]
            if (compact["navdemo_storage"]["source_samples_sha256"] != entry["samples_sha256"]
                    or stats["decisions"] != n or not compact["training_eligible"]):
                raise ValueError(f"Compact v5 source differs: {directory}")
            for column, head in enumerate(HEADS[:4]):
                classes = (keys[:, column].astype(int) + (column < 3))
                if np.bincount(classes, minlength=3 if column < 3 else 2).tolist() != stats["class_counts"][head]:
                    raise ValueError(f"Compact v5 labels differ: {directory}")
        c = action_counts(keys)
        counts.update(c)
        families[entry["family"]].update(c)
        episodes += 1
    return dict(episodes=episodes, all=fractions(counts),
                by_family={k: fractions(v) for k, v in families.items()})


def analyze_dagger(directory):
    metadata = read_json(directory / "episode.json")
    annotations = rows(directory / "samples.jsonl")
    fields = defaultdict(list)
    for chunk in nav_demos.iter_chunks(directory, 64):
        fields["keys"].append(np.column_stack([chunk["keys"][h] for h in HEADS]))
        for name in ("time_ms", "position_cm"):
            fields[name].append(chunk[name].copy())
        fields["direction"].append(chunk["roads"]["goal_direction"].copy())
        fields["distance"].append(chunk["roads"]["goal_distance_cm"].copy())
        # Native probability maps, excluding familiar and absolute height.
        crop = chunk["memory_near"]["channels"][:, 16:48, 16:48, :7]
        fields["maps"].append(crop.reshape(-1, 4, 8, 4, 8, 7).mean(axis=(2, 4)).reshape(-1, 112).astype(np.float32) / 255)
    a = {k: np.concatenate(v) for k, v in fields.items()}
    n = len(a["keys"])
    if n != metadata["decisions"] or n != len(annotations) or not np.array_equal(
            a["time_ms"], [row["time_ms"] for row in annotations]):
        raise ValueError(f"DAgger annotation count/clock differs: {directory}")
    stamps = np.asarray([row["elapsed_ms"] for row in annotations])
    if not (np.diff(stamps) == 100).all():
        raise ValueError(f"Unexpected DAgger decision clock: {directory}")
    controlled = np.asarray([row["teacher_control"] for row in annotations])
    reason = np.asarray([row["takeover"] for row in annotations])
    groups = dict(all=np.ones(n, bool), teacher=controlled, learner=~controlled,
                  **{f"reason:{r}": reason == r for r in ("beta", "stall", "memory_danger", "heading")})
    counts = {k: action_counts(a["keys"], mask) for k, mask in groups.items()}
    matched, pairs, examples = close_opposite_pairs(a["direction"], a["distance"], a["maps"], a["keys"][:, 2])
    path = frozen_path_distances(directory, stamps, a["position_cm"], metadata["teacher_version"])
    angle = np.abs(np.rad2deg(np.arctan2(a["direction"][:, 0], a["direction"][:, 2])))
    # Preserve missing instrumentation; a held takeover reason is not a current stall flag.
    stall = np.asarray([float(row["stall"]) if "stall" in row else np.nan for row in annotations])
    old = np.searchsorted(stamps, stamps - 2000, side="right") - 1
    displacement = np.linalg.norm(a["position_cm"][:, [0, 2]] - a["position_cm"][np.maximum(old, 0)][:, [0, 2]], axis=1)
    immobile = np.where(stamps >= 2000, (displacement < 30).astype(float), np.nan)
    summaries = {}
    for key, mask in {"matched": matched, "other": ~matched, **groups}.items():
        summaries[key] = dict(n=int(mask.sum()), matched=int((matched & mask).sum()),
            angle_sum=float(angle[mask].sum()),
            path_n=int(np.isfinite(path[mask]).sum()), path_sum=float(np.nansum(path[mask])),
            stall_n=int(np.isfinite(stall[mask]).sum()), stall_sum=float(np.nansum(stall[mask])),
            immobile_n=int(np.isfinite(immobile[mask]).sum()), immobile_sum=float(np.nansum(immobile[mask])))
    for example in examples:
        example["directory"] = directory.as_posix()
        for side in ("i", "j"):
            i = example[side]
            example[side] = dict(elapsed_ms=int(stamps[i]), direction=a["direction"][i].tolist(),
                distance_cm=float(a["distance"][i]), label=a["keys"][i].tolist(),
                teacher_control=bool(controlled[i]), takeover=str(reason[i]),
                stall=None if np.isnan(stall[i]) else bool(stall[i]),
                frozen_path_distance_cm=None if np.isnan(path[i]) else float(path[i]))
    return dict(counts=counts, summaries=summaries, pairs=int(pairs), examples=examples,
                dart=sum(row["dart"] for row in annotations))


def analyze_live(directory):
    """Summarize a saved trace; reconstruct road input without running a policy."""
    from nav_live_render import observation_frames

    directory = Path(directory)
    rows = [json.loads(line) for line in (directory / 'live-decisions.jsonl').read_text().splitlines() if line]
    episode = json.loads((directory / 'episode.json').read_text())
    metadata = json.loads((directory / 'live-format.json').read_text())
    time = np.array([r['elapsed_ms'] for r in rows])
    if not len(rows) or not np.array_equal([r['decision_index'] for r in rows], np.arange(len(rows))) or np.any(np.diff(time) <= 0):
        raise ValueError('Live decisions must be nonempty and ordered')
    position = np.array([r['position_cm'] for r in rows], dtype=float)
    delta = np.array([r['goal_cm'] for r in rows], dtype=float) - position
    distance = np.linalg.norm(delta, axis=1)
    yaw = np.deg2rad([r['yaw_deg'] for r in rows])
    direction = np.column_stack((-delta[:, 0] * np.sin(yaw) + delta[:, 2] * np.cos(yaw),
                                 delta[:, 1], delta[:, 0] * np.cos(yaw) + delta[:, 2] * np.sin(yaw)))
    direction /= np.where(distance > 0, distance, 1)[:, None]
    angle = np.rad2deg(np.arctan2(direction[:, 0], direction[:, 2]))
    keys = np.array([[r['keys'][k] for k in ('forward', 'strafe', 'turn', 'walk', 'jump', 'climb')] for r in rows])
    teacher = np.array([[r['teacher_keys'][k] for k in ('forward', 'strafe', 'turn', 'walk', 'jump', 'climb')] for r in rows])
    speed = np.array([r['speed_cm_s'] for r in rows])
    toward = (keys[:, 2] * angle < 0) & (np.abs(angle) > 10)
    away = (keys[:, 2] * angle > 0) & (np.abs(angle) > 10)
    aligned = np.abs(angle) <= 30
    steps = np.diff(position[:, [0, 2]], axis=0)
    horizontal = np.linalg.norm(delta[:, [0, 2]], axis=1)
    goal_unit = delta[:, [0, 2]] / np.where(horizontal > 0, horizontal, 1)[:, None]
    progress = np.sum(steps * goal_unit[:-1], axis=1)
    dt = np.diff(np.r_[time, episode['elapsed_ms']]) / 1000
    if np.any(dt < 0):
        raise ValueError('Episode ends before its recorded decision')

    def describe(mask):
        n = int(mask.sum())
        if not n:
            return {'decisions': 0}
        fraction = lambda values: float(np.mean(values[mask]))
        selected = np.flatnonzero(mask)
        result = dict(decisions=n, seconds=float(dt[mask].sum()),
            forward=fraction(keys[:, 0] > 0), effective_forward=fraction((keys[:, 0] > 0) & (keys[:, 1] == 0)), turn_only=fraction((keys[:, 0] == 0) & (keys[:, 1] == 0) & (keys[:, 2] != 0)),
            idle=fraction(np.all(keys[:, [0, 1, 2, 4, 5]] == 0, axis=1)), walk=fraction(keys[:, 3] != 0),
            turn_toward_goal=fraction(toward), turn_away_goal=fraction(away),
            angle_abs_median_deg=float(np.median(np.abs(angle[mask]))),
            aligned_decisions=int((mask & aligned).sum()),
            aligned_forward=float(np.mean(keys[mask & aligned, 0] > 0)) if np.any(mask & aligned) else None,
            aligned_teacher_forward=float(np.mean(teacher[mask & aligned, 0] > 0)) if np.any(mask & aligned) else None,
            aligned_turn_away=float(np.mean(away[mask & aligned])) if np.any(mask & aligned) else None,
            standing=fraction(speed < 20), circling=fraction((speed < 20) & (keys[:, 2] != 0)),
            stuck=fraction(np.array([r.get('stuck', False) for r in rows])),
            contact=fraction(np.array([r.get('contact', False) for r in rows])),
            teacher_disagreement={k: fraction(keys[:, i] != teacher[:, i]) for i, k in enumerate(('forward', 'strafe', 'turn', 'walk', 'jump', 'climb'))},
            goal_distance_cm=[float(distance[selected[0]]), float(distance[selected[-1]])],
            goal_distance_range_cm=[float(distance[mask].min()), float(distance[mask].max())],
            goal_input_distance_range=[float(np.clip(distance[mask] / 1000, 0, 50).min()), float(np.clip(distance[mask] / 1000, 0, 50).max())],
            goal_direction_min=direction[mask].min(axis=0).tolist(), goal_direction_max=direction[mask].max(axis=0).tolist(),
            yaw_range_deg=[float(np.rad2deg(yaw[mask]).min()), float(np.rad2deg(yaw[mask]).max())],
            max_displacement_from_start_cm=float(np.max(np.linalg.norm(position[mask][:, [0, 2]] - position[0, [0, 2]], axis=1))),
            net_displacement_cm=float(np.linalg.norm(position[selected[-1], [0, 2]] - position[selected[0], [0, 2]])))
        pair_mask = mask[:-1] & mask[1:]
        result['travel_cm'] = float(np.linalg.norm(steps[pair_mask], axis=1).sum())
        result['movement_projection_toward_goal_cm'] = float(progress[pair_mask].sum())
        if rows[0].get('probabilities'):
            result['probability_means'] = {head: np.mean([rows[i]['probabilities'][head] for i in selected], axis=0).tolist() for head in rows[0]['probabilities']}
            result['modal_action_fractions'] = {}
            result['selected_nonmodal_fractions'] = {}
            for head in rows[0]['probabilities']:
                modal = np.array([np.argmax(r['probabilities'][head]) for r in rows])
                choices = np.array([r['selected'][head] for r in rows])
                result['modal_action_fractions'][head] = [fraction(modal == i) for i in range(len(rows[0]['probabilities'][head]))]
                result['selected_nonmodal_fractions'][head] = fraction(choices != modal)
        return result

    actions = np.loadtxt(directory / 'actions.txt', ndmin=2)
    action_by_time = {int(r[0]): r[1:7] for r in actions}
    matched = sum(int(t) in action_by_time and np.array_equal(action_by_time[int(t)], k) for t, k in zip(time, keys))
    if matched != len(rows):
        raise ValueError('Recorded live decisions differ from actions.txt')
    result = dict(directory=str(directory), elapsed_ms=episode['elapsed_ms'], success=episode['success'],
                  failure=episode.get('failure'), action_rows_matched=matched,
                  goal_input_reconstructed_from='goal_cm-position_cm, yaw_deg; distance clip(cm/1000,0,50)',
                  overall=describe(np.ones(len(rows), dtype=bool)),
                  early=describe(time < (16600 if '10-local-stranger' in str(directory) else 28400)),
                  late=describe(time >= 30000), angle_definition='atan2(body right, body forward); turn*angle<0 is toward goal outside 10 degree deadband')
    examples = [int(np.argmin(np.abs(time - 60000)))]
    unusual = np.flatnonzero((time >= 30000) & aligned & away & (keys[:, 0] <= 0))
    examples.append(int(unusual[len(unusual) // 2]) if len(unusual) else len(rows) // 2)
    result['examples'] = [dict(elapsed_ms=int(time[i]), goal_direction=direction[i].tolist(),
        goal_distance_cm=float(distance[i]), goal_distance_input=float(np.clip(distance[i] / 1000, 0, 50)),
        goal_angle_deg=float(angle[i]), yaw_deg=float(np.rad2deg(yaw[i])), keys=rows[i]['keys'],
        teacher_keys=rows[i]['teacher_keys'], probabilities=rows[i].get('probabilities'), speed_cm_s=float(speed[i])) for i in dict.fromkeys(examples)]
    frame_count, goal_cells = 0, []
    for frame in observation_frames(directory, metadata):
        i = int(frame['decision_index'])
        if i >= len(rows) or frame['elapsed_ms'] != time[i]:
            raise ValueError('Live observation frame is not aligned to its decision')
        frame_count += 1
        layer = metadata['maps']['near']
        channel_names = layer['channels']
        if horizontal[i] <= 0:
            continue
        # Recorded map cells on the goal ray, 50..300 cm ahead. This is map evidence, not a collision or visibility test.
        unit = direction[i, [0, 2]] / np.linalg.norm(direction[i, [0, 2]])
        for extent in (50, 100, 150, 200, 250, 300):
            if extent > horizontal[i]:
                continue
            col = int(np.floor((unit[0] * extent - layer['min_right_cm']) / layer['cell_cm']))
            row = int(np.floor((unit[1] * extent - layer['min_forward_cm']) / layer['cell_cm']))
            if 0 <= row < frame['near'].shape[1] and 0 <= col < frame['near'].shape[2]:
                values = {name: int(frame['near'][j, row, col]) for j, name in enumerate(channel_names)}
                goal_cells.append((values['confidence'] >= 128, values['free'] >= 128,
                                   any(values[k] >= 128 for k in ('wall', 'ledge', 'drop', 'water'))))
    result['observation_frames_aligned'] = frame_count
    result['recorded_goal_ray_cells'] = dict(count=len(goal_cells),
        definition='Nearest encoded near-map cell at 50,100,150,200,250,300 cm along horizontal goal direction, bounded by goal distance; channel threshold 128/255',
        confident_fraction=float(np.mean([r[0] for r in goal_cells])) if goal_cells else None,
        free_fraction=float(np.mean([r[1] for r in goal_cells])) if goal_cells else None,
        hazard_fraction=float(np.mean([r[2] for r in goal_cells])) if goal_cells else None)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--shared-root", required=True, type=Path)
    args = parser.parse_args()
    root = args.shared_root
    result = {f"v{v}": teacher_corpus(root, v) for v in (4, 5)}
    print("Teacher corpora read", file=sys.stderr, flush=True)
    counts, phase_counts, summaries = defaultdict(Counter), defaultdict(Counter), defaultdict(Counter)
    total_pairs = dart = episodes = 0
    examples = []
    for phase in range(1, 7):
        for directory in sorted((root / f"nav-dagger4/round1/phase-{phase}/dagger").glob("*/*/seed-*")):
            episode = analyze_dagger(directory)
            for name, value in episode["counts"].items():
                counts[name].update(value)
                phase_counts[f"{phase}:{name}"].update(value)
            for name, value in episode["summaries"].items():
                summaries[name].update(value)
            total_pairs += episode["pairs"]
            dart += episode["dart"]
            episodes += 1
            if episode["examples"] and not examples:
                examples.append(episode["examples"][0])
            if episodes % 20 == 0:
                print(f"DAgger {episodes}/600", file=sys.stderr, flush=True)
    result["dagger"] = dict(episodes=episodes, dart=dart, counts={k: fractions(v) for k, v in counts.items()},
        by_phase={k: fractions(v) for k, v in phase_counts.items()},
        close_pairs=total_pairs, similarity={k: dict(v) for k, v in summaries.items()}, examples=examples)
    round_root = root / "nav-dagger4/round1"
    control = read_json(round_root / "control-metrics.json")["all"]
    if (episodes != control["episodes"] or counts["all"]["n"] != control["decisions"]
            or counts["teacher"]["n"] != control["teacher_control_decisions"]):
        raise ValueError("DAgger counts differ from control-metrics.json")
    history = read_json(round_root / "run/history.json")
    result["training"] = dict(config={k: v for k, v in read_json(round_root / "run/training_config.json").items()
        if k in ("max_dagger_fraction", "dagger_teacher_control_weight", "family_balance")},
        epochs=[{k: row[k] for k in ("epoch", "source_decisions", "family_weights")} for row in history])
    result["live"] = [analyze_live(root / "nav-live/dagger4-r1" / model / place / f"seed-{seed}")
        for model in ("dagger4-r1", "HD")
        for place, seed in (("10-local-stranger", 18102029), ("05-wall-pocket", 18102027))]
    print(json.dumps(result, indent=2, allow_nan=False))


if __name__ == "__main__":
    main()
