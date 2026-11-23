"""The traps of a smooth sampling, measured on the sparse clips of the base (reports/engine/2026-11-13-smooth-sampling):

  python3 traps.py <base> stops [model]      a hand that stops sharply at a key: how far it swings past the key
  python3 traps.py <base> feet [model]       feet of the clips that stand: how far a foot leaves the straight path
  python3 traps.py <base> slide <model> <clip> [keep]   a loop that carries the body: the slide of a planted foot;
                                             with keep=N a 25-key clip is thinned to every Nth key first and the
                                             source's own slide is printed too
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

import numpy as np

import sampling as sp

ga = sp.ga
FPS = 30.0
METHODS = ("linear", "cr", "crlim")


def sparse_clips(base, model):
    for path in sorted((base / "models").glob(f"{model}-*.MAN.json")):
        meta = json.loads(path.read_text())["data"]
        if meta["fps"] < 16.0 and meta["frame_count"] >= 3:
            yield ga.read_animation(path)


def is_loop(a):
    return a.next.upper() == a.name.upper()


def loop_count(a):
    if not is_loop(a) or a.node_indices[0] != 0:
        return a.frame_count
    travel = np.linalg.norm((a.positions[-1, 0] - a.positions[0, 0])[[0, 2]])
    return a.frame_count - 1 if travel >= 1.0 else a.frame_count


def paths(skel, a, sub=8):
    """World tracks per method at `sub` points per key interval, the root's way along the ground taken out."""
    loop, count = is_loop(a), loop_count(a)
    u = np.arange(((count if loop else count - 1)) * sub + 1) / sub
    out = {}
    for m in METHODS:
        p, q = sp.sample(a.positions[:count], a.rotations[:count], u, m, loop)
        w = sp.world(skel, a.node_indices, p, q)[:, :, :3, 3]
        out[m] = w - w[:, [0]] * np.array([1.0, 0.0, 1.0])
    return out, sub


def stops(base, model):
    skel = ga.read_skeleton(base / "models" / f"{model}.MDH.json")
    names = {n.name: i for i, n in enumerate(skel.nodes)}
    hands = [names[n] for n in ("BIP01 R HAND", "BIP01 L HAND") if n in names]
    over = {m: [] for m in METHODS}
    worst = {m: (0.0, "") for m in METHODS}
    for a in sparse_clips(base, model):
        w, sub = paths(skel, a)
        keys = w["linear"][::sub]
        for n in hands:
            k = keys[:, n]
            chord = np.linalg.norm(k[1:] - k[:-1], axis=-1)
            for i in range(1, len(chord)):
                # comes in fast, leaves slow: a stop at key i
                if chord[i - 1] < 3.0 or chord[i] > 0.15 * chord[i - 1]:
                    continue
                for m in METHODS:
                    seg = w[m][i * sub:(i + 1) * sub + 1, n]
                    past = max(np.linalg.norm(seg - k[i], axis=-1).max() - chord[i], 0.0)
                    over[m].append(past)
                    if past > worst[m][0]:
                        worst[m] = (past, f"{a.name} key {i}")
    print(f"{model}: hand stops at a key (in at 3 cm a key or faster, out at under 15% of that): {len(over['linear'])}")
    for m in METHODS:
        v = np.array(over[m]) if over[m] else np.zeros(1)
        print(f"  {m}: past the key, cm: median {np.median(v):.2f}, p95 {np.percentile(v, 95):.2f},"
              f" max {v.max():.2f} ({worst[m][1]})")


def feet(base, model):
    skel = ga.read_skeleton(base / "models" / f"{model}.MDH.json")
    names = {n.name: i for i, n in enumerate(skel.nodes)}
    nodes = [names[n] for n in ("BIP01 R FOOT", "BIP01 L FOOT", "BIP01 R TOE0", "BIP01 L TOE0") if n in names]
    rows = []
    for a in sparse_clips(base, model):
        if not set(nodes) & set(a.node_indices):
            continue
        w, _ = paths(skel, a)
        off = {m: max(np.linalg.norm(w[m][:, n] - w["linear"][:, n], axis=-1).max() for n in nodes) for m in METHODS[1:]}
        rows.append((off["crlim"], off["cr"], a.name))
    rows.sort(reverse=True)
    v = np.array([r[0] for r in rows])
    print(f"{model}: {len(rows)} sparse clips key the feet; a foot off the straight path, crlim, cm:"
          f" median {np.median(v):.2f}, p95 {np.percentile(v, 95):.2f}, max {v.max():.2f}")
    for r in rows[:8]:
        print(f"  {r[2]}: crlim {r[0]:.2f}, cr {r[1]:.2f}")


def slide(base, model, clip, keep):
    skel = ga.read_skeleton(base / "models" / f"{model}.MDH.json")
    a = sp.load(base, model, clip)
    names = {n.name: i for i, n in enumerate(skel.nodes)}
    feet_nodes = [i for n, i in names.items() if n.endswith(" FOOT")]
    count = loop_count(a)
    rate = a.fps
    truth = None
    if keep > 1:
        truth = sp.world(skel, a.node_indices, a.positions, a.rotations)[:, :, :3, 3]
        n = (count // keep) * keep
        a.positions, a.rotations, count, rate = a.positions[0:n + 1:keep], a.rotations[0:n + 1:keep], n // keep, a.fps / keep
        truth = truth[:n + 1]
    cycle = count / rate
    t = np.arange(int(cycle * FPS)) / FPS
    u = t * rate
    # the body goes on by the root's keys along a straight line between them (Animation::Sequence::translateXZ)
    body, _ = sp.sample(a.positions[:count + 1, :1], a.rotations[:count + 1, :1], u, "linear")
    body = body[:, 0] * np.array([1.0, 0.0, 1.0])
    print(f"{model} {clip}: {rate:g} keys a second, cycle {cycle:.2f} s, {np.linalg.norm(body[-1] - body[0]) / t[-1]:.0f} cm/s")
    track = {}
    for m in METHODS:
        p, q = sp.sample(a.positions[:count], a.rotations[:count], u, m, True)
        w = sp.world(skel, a.node_indices, p, q)[:, :, :3, 3]
        track[m] = w - w[:, [0]] * np.array([1.0, 0.0, 1.0]) + body[:, None]
    if truth is not None:
        tb = truth[:, 0] * np.array([1.0, 0.0, 1.0])
        track["source"] = (truth - tb[:, None] + tb[:, None])[::1]
    for n in feet_nodes:
        lin = track["linear"][:, n]
        low = lin[:, 1] < lin[:, 1].min() + 2.0  # the foot is down: within 2 cm of its lowest
        line = f"  {skel.nodes[n].name}: down {low.mean() * 100:.0f}% of the cycle; slide while down, cm/s (mean / max):"
        for m in METHODS:
            v = np.linalg.norm((track[m][1:, n] - track[m][:-1, n])[:, [0, 2]], axis=-1) * FPS
            line += f" {m} {v[low[1:]].mean():.1f} / {v[low[1:]].max():.1f};"
        if truth is not None:
            s = track["source"][:, n]
            lows = s[:, 1] < s[:, 1].min() + 2.0
            v = np.linalg.norm((s[1:] - s[:-1])[:, [0, 2]], axis=-1) * a.fps * keep if False else \
                np.linalg.norm((s[1:] - s[:-1])[:, [0, 2]], axis=-1) * (rate * keep)
            line += f" source {v[lows[1:]].mean():.1f} / {v[lows[1:]].max():.1f}"
        print(line)


if __name__ == "__main__":
    base = Path(sys.argv[1])
    what = sys.argv[2]
    if what == "stops":
        stops(base, sys.argv[3] if len(sys.argv) > 3 else "HUMANS")
    elif what == "feet":
        feet(base, sys.argv[3] if len(sys.argv) > 3 else "HUMANS")
    else:
        slide(base, sys.argv[3], sys.argv[4], int(sys.argv[5]) if len(sys.argv) > 5 else 1)
