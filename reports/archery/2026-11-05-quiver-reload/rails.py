"""The rails of the reload from the quiver by the frames of a bow-reload bench (60 frames a second of game time).

    python3 reports/archery/2026-11-05-quiver-reload/rails.py <bench out dir> [segment prefix ...]

Per segment (the hero's, or the watched npc's quiver columns) and per reload in it:
- the hand, the forearm, the arrow out of the head (hd_hand, hd_fore; the arrow from rq_an..rq_at against the head's
  capsule rq_h..rq_c, r 10 cm): deepest, cm (< 0 in it), on the hand's way (ANCHOR: at the string's anchor, the
  aim's own, apart as head_anchor);
- out of the trunk (the armour's points, qv_thand, qv_tfore, qv_tarrow) and the quiver's tube (rq_tube_*): deepest;
- the arrow on the quiver's axis while its tip is in it, till it first leaves (rq_offaxis): most;
- the frame of the grab (p crosses K3 = 0.34), of the arrow in the hand, of one arrow less in the quiver;
- at the end (the first frames nocked at draw 0): bd_nockgap, bd_overgrip, bd_atmiddle, bd_bowmiss;
- the jump |Δx(i) − Δx(i−1)| of the hand (rh_*) at the joints K0, K2–K3, K7 and into S_BOWAIM, and the most anywhere;
- the wrist's and the elbow's bend on the hand's way (rq_wrist, rq_elbow): most, degrees (G2's reload: 72 and 138);
- the quiver: its turn from the rest (qv_swing) while the arrow is drawn out, its most change in 1/60 s, the leg and
  the flaps in the tube meanwhile (qv_leg, qv_armor), how far it is back 0.25 s after the tip left the tube.
"""
from __future__ import annotations

import csv
import json
import math
import sys
from pathlib import Path

K3 = 0.34
ANCHOR = (0.04, 0.93)  # p: the hand at the string's anchor before and after
HEAD_R = 10.0


def num(r, k, default=float("nan")):
    try:
        return float(r[k])
    except (KeyError, ValueError, TypeError):
        return default


def vec(r, k):
    return [num(r, f"rq_{k}x"), num(r, f"rq_{k}y"), num(r, f"rq_{k}z")]


def seg_dist(p, a, b):
    ab = [b[i] - a[i] for i in range(3)]
    ap = [p[i] - a[i] for i in range(3)]
    ll = sum(x * x for x in ab)
    t = max(0.0, min(1.0, sum(ap[i] * ab[i] for i in range(3)) / ll)) if ll > 1e-9 else 0.0
    return math.dist(p, [a[i] + ab[i] * t for i in range(3)])


def resample(rows):
    """The rows at 60 frames a second of game time (t, ms): the row nearest each 1/60 s."""
    out, want = [], 0.0
    for r in rows:
        t = num(r, "t")
        if t + 1e-6 >= want:
            out.append(r)
            want = t + 1000.0 / 60.0 - 1.0
    return out


def reloads(rows):
    """Runs of rows where the reload plays (rq_p >= 0), with a few rows after it."""
    runs, cur = [], None
    for i, r in enumerate(rows):
        if num(r, "rq_p", -1) >= 0:
            if cur is None:
                cur = [i, i]
            cur[1] = i
        elif cur is not None:
            runs.append((cur[0], min(len(rows) - 1, cur[1] + 20)))
            cur = None
    if cur is not None:
        runs.append((cur[0], len(rows) - 1))
    return runs


def jump(rows, a, b):
    best = 0.0
    for i in range(max(a, 2), b + 1):
        p0, p1, p2 = ([num(rows[j], k) for k in ("rh_x", "rh_y", "rh_z")] for j in (i - 2, i - 1, i))
        d = math.sqrt(sum(((p2[k] - p1[k]) - (p1[k] - p0[k])) ** 2 for k in range(3)))
        if not math.isnan(d):
            best = max(best, d)
    return best


def one(rows, a, b):
    run = rows[a:b + 1]
    rep = {}
    hd = [r for r in run if num(r, "rq_p", -1) >= 0]
    # the head: on the hand's way (K1..K7); at the anchor (the aim's own, the string at the cheek) apart
    away = [r for r in hd if ANCHOR[0] < num(r, "rq_p") < ANCHOR[1]] or hd
    rep["head_hand"] = min(num(r, "hd_hand", 99) for r in away)
    rep["head_fore"] = min(num(r, "hd_fore", 99) for r in away)
    rep["head_anchor"] = min(num(r, "hd_hand", 99) for r in hd)
    arrow = 99.0
    for r in away:
        an, at, h0, h1 = vec(r, "an"), vec(r, "at"), vec(r, "h"), vec(r, "c")
        if num(r, "rq_tipin", -99) <= -99 and num(r, "rq_p") < K3:
            continue
        if any(math.isnan(x) for x in an + at):
            continue
        for k in range(17):
            q = [an[i] + (at[i] - an[i]) * k / 16 for i in range(3)]
            arrow = min(arrow, seg_dist(q, h0, h1) - HEAD_R)
    rep["head_arrow"] = arrow
    for k in ("tube_hand", "tube_fore"):
        rep[k] = min(num(r, f"rq_{k}", 99) for r in hd)
    # the arm's joints on the hand's way: the wrist (forearm against the fingers' line), the elbow
    for k in ("wrist", "elbow"):
        vals = [num(r, f"rq_{k}", -1) for r in away]
        rep[k] = max(vals or [-1.0])
    # the trunk by the armour's points (the quiver's lift: qv_thand, qv_tfore, qv_tarrow)
    for k, c in (("trunk_hand", "qv_thand"), ("trunk_fore", "qv_tfore"), ("trunk_arrow", "qv_tarrow")):
        rep[k] = min(num(r, c, 99) for r in away)
    # till the tip first leaves the tube (turned toward the bow it may dip under the mouth's plane again, beside it)
    drawing = []
    for r in hd:
        if num(r, "rq_tipin", -99) > 0:
            drawing.append(num(r, "rq_offaxis", -1))
        elif drawing:
            break
    rep["off_axis"] = max(drawing or [0.0])
    # the grab, the arrow in the hand, one less in the quiver
    fr = lambda pred: next((i for i, r in enumerate(run) if pred(r)), None)  # noqa: E731
    f_grab = fr(lambda r: num(r, "rq_p", -1) >= K3)
    f_arrow = fr(lambda r: num(r, "rq_p", -1) > 0.05 and num(r, "rq_tipin", -99) > -99)
    arrows0 = num(run[0], "qv_arrows")
    f_less = fr(lambda r: num(r, "qv_arrows") < arrows0)
    rep["grab_frame"], rep["arrow_frame"], rep["quiver_less_frame"] = f_grab, f_arrow, f_less
    # the end: nocked at draw 0
    end = [r for r in run[len(hd):] if num(r, "m_stage", -1) == 2 and num(r, "bd_w", 0) > 0.99]
    if end:
        e = end[0]
        for k in ("bd_nockgap", "bd_overgrip", "bd_atmiddle", "bd_bowmiss"):
            rep[k] = num(e, k)
    # the jumps at the joints
    idx = lambda p: next((i for i, r in enumerate(run) if num(r, "rq_p", -1) >= p), len(hd))  # noqa: E731
    n = len(run) - 1
    rep["jump_K0"] = jump(run, 0, min(n, idx(0.06)))
    rep["jump_K2K3"] = jump(run, max(0, idx(0.26) - 1), min(n, idx(0.37) + 1))
    rep["jump_K7"] = jump(run, max(0, idx(0.88) - 1), min(n, idx(0.96) + 1))
    rep["jump_aim"] = jump(run, max(0, len(hd) - 2), min(n, len(hd) + 8))
    rep["jump_most"] = jump(run, 0, n)
    # the quiver
    drawn = [i for i, r in enumerate(run) if num(r, "qv_drawout", -1) >= 0]
    if drawn:
        sw = [num(run[i], "qv_swing") for i in drawn]
        rep["quiver_turn_max"] = max(sw)
        rep["quiver_leg_in_draw"] = max(num(run[i], "qv_leg", 0) for i in drawn)
        rep["quiver_flaps_in_draw"] = max(num(run[i], "qv_armor", 0) for i in drawn)
        rep["quiver_line_in"] = max(num(run[i], "qv_line", 0) for i in drawn)
        last = drawn[-1]
        tl = num(run[last], "t")
        later = [r for r in run[last:] if num(r, "t") >= tl + 250]
        if later:
            rep["quiver_turn_at_tip_out"] = num(run[last], "qv_swing")
            rep["quiver_turn_250ms_later"] = num(later[0], "qv_swing")
    steps = [abs(num(run[i], "qv_swing") - num(run[i - 1], "qv_swing")) for i in range(1, len(run))]
    rep["quiver_step_max"] = max(steps) if steps else 0.0
    rep["quiver_leg_max"] = max(num(r, "qv_leg", 0) for r in run)
    rep["quiver_flaps_max"] = max(num(r, "qv_armor", 0) for r in run)
    rep["fletch_max"] = max(num(r, "qv_fletch", 0) for r in run)
    return {k: (round(v, 2) if isinstance(v, float) else v) for k, v in rep.items()}


def main():
    out = Path(sys.argv[1])
    prefixes = sys.argv[2:]
    rows = list(csv.DictReader(open(out / "metrics.csv")))
    segs = []
    for r in rows:
        if r["seg"] not in segs:
            segs.append(r["seg"])
    result = {}
    for s in segs:
        if prefixes and not any(s.startswith(p) for p in prefixes):
            continue
        sr = resample([r for r in rows if r["seg"] == s])
        result[s] = [one(sr, a, b) for a, b in reloads(sr)]
    print(json.dumps(result, indent=1, ensure_ascii=False))


if __name__ == "__main__":
    main()
