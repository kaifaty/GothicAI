#!/usr/bin/env python3
"""Compare native and deferred NPC poses on the same post-animation world ticks."""

import argparse
import json
import math
from pathlib import Path


BONES = ("head", "pelvis", "left hand", "right hand", "left foot", "right foot")
STATE_FIELDS = ("symbol", "body", "routine", "state", "state_time_ms", "animations", "gesture")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def read_trace(path):
    if path.is_dir():
        path /= "events.jsonl"
    rows, marks = {}, {}
    with path.open() as stream:
        for line in stream:
            row = json.loads(line)
            if row["kind"] == "mark" and "t_game_ms" in row and row["text"] in ("return", "end"):
                marks[row["text"]] = row["t_game_ms"]
            if row["kind"] != "society_pose":
                continue
            key = row["actor"], row["tick_ms"]
            require(key not in rows, f"{path}: duplicate pose sample for {key}")
            rows[key] = row
    require(rows, f"{path}: no pose samples (enable OPENGOTHIC_SOCIETY_POSE_TRACE=1)")
    return rows, marks


def points(matrix):
    """Sample the translation and 10 cm along each transformed local axis."""
    require(len(matrix) == 16 and all(isinstance(v, (int, float)) and math.isfinite(v) for v in matrix),
            "invalid rendered bone matrix")
    origin = matrix[12:15]
    directions = [tuple(o + 10 * matrix[axis * 4 + i] for i, o in enumerate(origin)) for axis in range(3)]
    return origin, *directions


def compare(reference, candidate, step_ms, post_frames, min_wakes, root_limit, pose_limit,
            actor_symbol, require_gesture, require_live_debt):
    reference, reference_marks = reference
    candidate, candidate_marks = candidate
    require(reference_marks == candidate_marks, "return/end marks disagree between the takes")
    require(not any(row["debt"] or row["after_debt"] or row["recovered"] for row in reference.values()),
            "animation-on reference contains Society deferrals")
    start = candidate_marks.get("return", 0)
    end = candidate_marks.get("end", math.inf)
    exposure = [row for row in candidate.values() if row["first_exposure"] and row["after_debt"] and
                start <= row["tick_ms"] <= end and
                (actor_symbol is None or row["symbol"] == actor_symbol)]
    unique = {row["actor"] for row in exposure}
    require(len(unique) >= min_wakes,
            f"only {len(unique)} actors were exposed after actual deferral, need {min_wakes}")
    settled_debt = gesture_wakes = collider_wakes = 0
    worst_root = worst_pose = worst_step = 0.0
    first_debt = {}
    for (actor, tick), row in candidate.items():
        if row["debt"]:
            first_debt[actor] = min(tick, first_debt.get(actor, tick))
    for first in exposure:
        actor, tick = first["actor"], first["tick_ms"]
        label = f"{first['symbol']} at {tick} ms"
        require(first_debt.get(actor, tick) < tick, f"{label}: no earlier sample with actual Society debt")
        require(first["visibility_blocker"] in {"hero_near", "camera_visible"}, f"{label}: no exposure boundary")
        gesture_wakes += bool(first["gesture"]) and 0 < first["gesture_phase"] < 1
        settled_debt += first["recovered"]
        collider_wakes += first["collider_hits"] > 0
        previous = None
        for offset in range(post_frames + 1):
            key = actor, tick + offset * step_ms
            a, b = reference.get(key), candidate.get(key)
            require(a is not None and b is not None, f"{label}: missing same-tick pair at offset {offset}")
            if offset != 0 and b["visibility_blocker"] not in ("hero_near", "camera_visible"):
                break
            require(not a["debt"], f"{label}: animation-on reference carried Society debt")
            require(not b["debt"], f"{label}: Society debt remained after exposure at offset {offset}")
            for field in STATE_FIELDS:
                require(a[field] == b[field], f"{label}: {field} diverged at offset {offset}: {a[field]} / {b[field]}")
            require(math.isclose(a["gesture_phase"], b["gesture_phase"], rel_tol=0, abs_tol=1e-5),
                    f"{label}: active gesture advanced to a different phase at offset {offset}")
            require(a["collider_hits"] >= 0 and a["collider_hits"] == b["collider_hits"],
                    f"{label}: collider ray results differ at offset {offset}")
            root_delta = math.dist(a["root_cm"], b["root_cm"])
            worst_root = max(worst_root, root_delta)
            require(root_delta <= root_limit, f"{label}: {root_delta:.3f} cm root offset at frame {offset}")
            require(len(a["bones"]) == len(b["bones"]) == len(BONES), f"{label}: missing rendered bones")
            current = [(points(am), points(bm)) for am, bm in zip(a["bones"], b["bones"])]
            for index, (ap, bp) in enumerate(current):
                delta = max(math.dist(x, y) for x, y in zip(ap, bp))
                worst_pose = max(worst_pose, delta)
                require(delta <= pose_limit, f"{label}: {BONES[index]} offset {delta:.3f} cm at frame {offset}")
                if previous is not None:
                    prior_a, prior_b = previous[index]
                    step = max(math.dist([after-before for before, after in zip(a0, a1)],
                                         [after-before for before, after in zip(b0, b1)])
                               for a0, a1, b0, b1 in zip(prior_a, ap, prior_b, bp))
                    worst_step = max(worst_step, step)
                    require(step <= pose_limit, f"{label}: {BONES[index]} extra step {step:.3f} cm at frame {offset}")
            previous = current
    require(collider_wakes, "no first-exposure collider ray hit was observed")
    require(not require_gesture or gesture_wakes, "no sampled first exposure inside an active guard gesture")
    require(not require_live_debt or settled_debt, "no actor cleared live Society debt on its actual exposure frame")
    return dict(actors=len(unique), exposures=len(exposure), live_debt_cleared_on_exposure=settled_debt,
                in_flight_gestures=gesture_wakes, collider_hits_on_exposure=collider_wakes,
                max_root_error_cm=worst_root, max_pose_error_cm=worst_pose, max_pose_step_error_cm=worst_step)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", type=Path, help="animation-on trace or its bench output directory")
    parser.add_argument("candidate", type=Path, help="animation-off trace or its bench output directory")
    parser.add_argument("--step-ms", type=int, default=8)
    parser.add_argument("--post-frames", type=int, default=4)
    parser.add_argument("--min-wakes", type=int, default=1)
    parser.add_argument("--root-limit-cm", type=float, default=0.25)
    parser.add_argument("--pose-limit-cm", type=float, default=0.5)
    parser.add_argument("--actor", help="verify first exposures of one NPC symbol")
    parser.add_argument("--require-active-gesture", action="store_true")
    parser.add_argument("--require-live-debt", action="store_true")
    args = parser.parse_args()
    if args.step_ms <= 0 or args.post_frames < 1 or args.min_wakes < 1 or min(args.root_limit_cm, args.pose_limit_cm) < 0:
        parser.error("expected positive timestep, frame and wake counts and non-negative position limits")
    try:
        result = compare(read_trace(args.reference), read_trace(args.candidate), args.step_ms, args.post_frames,
                         args.min_wakes, args.root_limit_cm, args.pose_limit_cm, args.actor,
                         args.require_active_gesture, args.require_live_debt)
    except (KeyError, OSError, TypeError, ValueError) as error:
        parser.exit(1, f"pose verification failed: {error}\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
