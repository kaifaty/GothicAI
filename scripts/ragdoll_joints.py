#!/usr/bin/env python3
"""Anatomical joint angles of human corpses in a bench run (OPENGOTHIC_RAGDOLL_TRACE=1).

Every angle is measured from the standing pose the body had when it died (the
part reference transforms), in the body's own anatomical frame: x to its left,
y up, z forward. It does not use the constraint frames, so it judges any joint
implementation the same way. A value outside the human range is a violation.

    scripts/ragdoll_joints.py .bench/<run>                 # table per segment
    scripts/ragdoll_joints.py .bench/<run> --json out.json
    scripts/ragdoll_joints.py .bench/<before> .bench/<after>  # side by side
"""

import argparse
import json
import math
import sys
from pathlib import Path

import numpy as np

# Passive ranges of an adult, a little wider than the active ones (degrees).
RANGES = {
    "hip flex": (-20, 125), "hip abd": (-30, 45), "hip rot": (-50, 45),
    "knee flex": (-5, 155), "knee varus": (-10, 10),
    "ankle dorsi": (-55, 40), "ankle out": (-25, 25), "ankle inv": (-20, 35),
    "toe ext": (-40, 70), "toe side": (-20, 20),
    "trunk flex": (-30, 90), "trunk side": (-35, 35), "trunk twist": (-45, 45),
    "head flex": (-60, 65), "head side": (-45, 45), "head twist": (-80, 80),
    "shoulder back": (-90, 60), "shoulder across": (-90, 50), "shoulder rot": (-90, 90),
    "elbow flex": (-10, 150),
    "wrist swing": (0, 85), "wrist twist": (-90, 90),
    "clavicle": (0, 40),
}


def unit(v):
    n = np.linalg.norm(v)
    return v/n if n > 1e-9 else v


def perp(v, axis):
    return unit(v-axis*np.dot(v, axis))


def matrix(values):
    m = np.array(values, dtype=float).reshape(4, 4).T  # Tempest stores columns
    r = m[:3, :3].copy()
    for i in range(3):
        r[:, i] = unit(r[:, i])
    u, _, vt = np.linalg.svd(r)  # strip the visual affine stretch
    return u@vt, m[:3, 3]


def swing(a, b):
    """The shortest rotation taking unit a to unit b."""
    k = np.cross(a, b)
    s, c = np.linalg.norm(k), np.dot(a, b)
    if s < 1e-9:
        return np.eye(3) if c > 0 else -np.eye(3)+2*np.outer(perp(np.array([1., 0, 0]), a), perp(np.array([1., 0, 0]), a))
    k = k/s
    kx = np.array([[0, -k[2], k[1]], [k[2], 0, -k[0]], [-k[1], k[0], 0]])
    return np.eye(3)+s*kx+(1-c)*(kx@kx)


def twist(rot, d0, p0, q0):
    """Rotation of rot about d0 left after its swing, positive from p0 towards q0."""
    p = rot@p0
    s = swing(d0, rot@d0)
    return math.degrees(math.atan2(np.dot(p, s@q0), np.dot(p, s@p0)))


def deg(y, x):
    return math.degrees(math.atan2(y, x))


def asin(v):
    return math.degrees(math.asin(max(-1., min(1., v))))


class Body:
    """Names, reference frames and the anatomical basis of one corpse."""

    def __init__(self, names, parts):
        self.names = names
        self.ref = {}
        for part in parts:
            self.ref[names[part["bone"]]] = matrix(part["reference_transform"])
        self.find = lambda *keys: next((n for n in self.ref if all(k in n for k in keys)), None)
        lt, rt = self.find(" L THIGH"), self.find(" R THIGH")
        chest = self.find("SPINE2") or self.find("SPINE1") or self.find("NECK")
        if lt is None or rt is None or chest is None:
            raise LookupError("not a human rig")
        pl, pr = self.ref[lt][1], self.ref[rt][1]
        x = unit(pl-pr)
        y = perp(self.ref[chest][1]-(pl+pr)/2, x)
        z = np.cross(x, y)
        toes = [self.find(f" {s} TOE") for s in "LR"]
        feet = [self.find(f" {s} FOOT") for s in "LR"]
        ahead = sum(self.ref[t][1]-self.ref[f][1] for t, f in zip(toes, feet) if t and f)
        if np.dot(ahead, z) < 0:
            z = -z
        self.basis = np.stack([x, y, z])  # rows: world -> anatomical coordinates

    def pos(self, name):
        return self.basis@self.ref[name][1]

    def direction(self, a, b):
        return unit(self.pos(b)-self.pos(a))

    def delta(self, parent, child, now):
        """Child rotation since death, relative to its parent, in anatomical coordinates."""
        rp0, rc0 = self.ref[parent][0], self.ref[child][0]
        rp, rc = now[parent][0], now[child][0]
        local = (rp.T@rc)@(rp0.T@rc0).T
        a = rp0.T@self.basis.T  # anatomical axes in the parent's local frame
        return a.T@local@a


def angles(body, now):
    out = {}
    x, y, z = np.eye(3)
    f = body.find

    for side, sign in (("L", 1.), ("R", -1.)):
        pelvis, thigh, calf, foot, toe = f("PELVIS"), f(f" {side} THIGH"), f(f" {side} CALF"), f(f" {side} FOOT"), f(f" {side} TOE")
        lat = sign*x
        if pelvis and thigh and calf:
            d0 = body.direction(thigh, calf)
            rot = body.delta(pelvis, thigh, now)
            d = rot@d0
            out[f"hip flex {side}"] = deg(np.dot(d, z), -np.dot(d, y))
            out[f"hip abd {side}"] = asin(np.dot(d, lat))
            out[f"hip rot {side}"] = twist(rot, d0, perp(z, d0), perp(-lat, d0))
        if thigh and calf and foot:
            e = body.direction(thigh, calf)
            d = body.delta(thigh, calf, now)@body.direction(calf, foot)
            out[f"knee flex {side}"] = deg(-np.dot(d, perp(z, e)), np.dot(d, e))
            out[f"knee varus {side}"] = asin(np.dot(d, perp(lat, e)))
        if calf and foot and toe:
            d0 = body.direction(foot, toe)
            up = perp(-body.direction(calf, foot), d0)
            out_ = perp(lat-up*np.dot(lat, up), d0)
            rot = body.delta(calf, foot, now)
            d = rot@d0
            out[f"ankle dorsi {side}"] = deg(np.dot(d, up), np.dot(d, d0))
            out[f"ankle out {side}"] = asin(np.dot(d, out_))
            out[f"ankle inv {side}"] = twist(rot, d0, out_, up)
            rot = body.delta(foot, toe, now)
            d = rot@d0
            out[f"toe ext {side}"] = deg(np.dot(d, up), np.dot(d, d0))
            out[f"toe side {side}"] = asin(np.dot(d, out_))

        chest, clav, upper, fore, hand = f("SPINE2") or f("SPINE1"), f(f" {side} CLAVICLE"), f(f" {side} UPPERARM"), f(f" {side} FOREARM"), f(f" {side} HAND")
        if chest and upper and fore:
            d0 = body.direction(upper, fore)
            rot = body.delta(chest, upper, now)
            d = rot@d0
            # The arm may point anywhere in front; behind the body and across it is bounded.
            out[f"shoulder back {side}"] = asin(-np.dot(d, z))
            out[f"shoulder across {side}"] = asin(-np.dot(d, lat))
            out[f"shoulder rot {side}"] = twist(rot, d0, perp(z, d0), perp(-lat, d0))
        if chest and clav:
            rot = body.delta(chest, clav, now)
            out[f"clavicle {side}"] = math.degrees(math.acos(max(-1., min(1., (np.trace(rot)-1)/2))))
        if upper and fore and hand:
            e = body.direction(upper, fore)
            d = body.delta(upper, fore, now)@body.direction(fore, hand)
            out[f"elbow flex {side}"] = deg(np.dot(d, perp(z, e)), np.dot(d, e))
            d0 = body.direction(fore, hand)
            rot = body.delta(fore, hand, now)
            out[f"wrist swing {side}"] = math.degrees(math.acos(max(-1., min(1., np.dot(rot@d0, d0)))))
            out[f"wrist twist {side}"] = twist(rot, d0, perp(z, d0), perp(x, d0))

    pelvis, chest, neck, head = f("PELVIS"), f("SPINE2") or f("SPINE1"), f("NECK"), f("HEAD")
    for label, parent, child, a, b in (("trunk", pelvis, chest, chest, neck), ("head", chest, head, neck, head)):
        if not (parent and child and a and b):
            continue
        d0 = body.direction(a, b)
        rot = body.delta(parent, child, now)
        d = rot@d0
        fwd, left = perp(z, d0), perp(x-z*np.dot(x, z), d0)
        out[f"{label} flex"] = deg(np.dot(d, fwd), np.dot(d, d0))
        out[f"{label} side"] = deg(np.dot(d, left), np.dot(d, d0))
        out[f"{label} twist"] = twist(rot, d0, fwd, left)
    return out


def measure(name):
    base = name.rsplit(" ", 1)[0] if name[-2:] in (" L", " R") else name
    return base, RANGES.get(base)


def outside(name, value):
    _, span = measure(name)
    if span is None:
        return 0.
    return max(span[0]-value, value-span[1], 0.)


def read(run):
    segments = {}
    for line in (Path(run)/"events.jsonl").read_text().splitlines():
        event = json.loads(line)
        if event.get("kind") != "ragdoll_frame" or not event.get("seg") or not event.get("snapshot"):
            continue
        names = [b["bone"] for b in event["world_bones"]]
        parts = event["snapshot"]["parts"]
        try:
            body = Body(names, parts)
        except LookupError:
            continue
        now = {names[p["bone"]]: matrix(p["transform"]) for p in parts}
        seg = segments.setdefault(event["seg"], {"frames": 0, "final": {}, "low": {}, "high": {}})
        values = angles(body, now)
        seg["frames"] += 1
        seg["final"] = values
        for key, value in values.items():
            seg["low"][key] = min(seg["low"].get(key, value), value)
            seg["high"][key] = max(seg["high"].get(key, value), value)
    return segments


def summary(segments):
    result = {}
    for name, seg in segments.items():
        rest = {k: round(v, 1) for k, v in seg["final"].items()}
        bad_rest = {k: round(v, 1) for k, v in seg["final"].items() if outside(k, v) > 0}
        bad_fall = {}
        for k in seg["low"]:
            worst = max(outside(k, seg["low"][k]), outside(k, seg["high"][k]))
            if worst > 0:
                bad_fall[k] = round(seg["low"][k] if outside(k, seg["low"][k]) >= outside(k, seg["high"][k]) else seg["high"][k], 1)
        result[name] = {"frames": seg["frames"], "rest": rest, "rest_violations": bad_rest, "fall_violations": bad_fall}
    return result


def print_run(label, result):
    print(f"## {label}")
    total_rest = total_fall = 0
    for name, seg in result.items():
        total_rest += len(seg["rest_violations"])
        total_fall += len(seg["fall_violations"])
        rest = ", ".join(f"{k} {v:+.0f}" for k, v in sorted(seg["rest_violations"].items())) or "-"
        fall = ", ".join(f"{k} {v:+.0f}" for k, v in sorted(seg["fall_violations"].items())) or "-"
        print(f"{name:10s} {seg['frames']:4d} frames | at rest: {rest}")
        print(f"{'':10s}             | in the fall: {fall}")
    print(f"violations: at rest {total_rest}, in the fall {total_fall}")
    groups = {}
    for seg in result.values():
        for k, v in seg["rest"].items():
            groups.setdefault(measure(k)[0], []).append(v)
    print("\nat rest, all segments and sides: measure  min / median / max  (human range)")
    for k in sorted(groups):
        v = sorted(groups[k])
        span = RANGES.get(k)
        print(f"  {k:14s} {v[0]:+6.0f} / {v[len(v)//2]:+6.0f} / {v[-1]:+6.0f}   ({span[0]:+d}..{span[1]:+d})")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("runs", nargs="+")
    parser.add_argument("--json")
    args = parser.parse_args()
    results = {}
    for run in args.runs:
        results[run] = summary(read(run))
        if not results[run]:
            print(f"{run}: no human ragdoll frames (OPENGOTHIC_RAGDOLL_TRACE=1?)", file=sys.stderr)
            return 1
        print_run(run, results[run])
        print()
    if args.json:
        Path(args.json).write_text(json.dumps(results, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
