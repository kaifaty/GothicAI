"""The sparse clips themselves, sampled at the game's 30 frames a second on the line and on the curve, as the game
shows them (a clip that ends: its first key held before it, its last key held after it): how the step of a hand,
the head and the chest changes between two frames (the stand's an_pop, cm), what share of that change falls on
the frames next to a key, and how far the hand is from where the line has it at the same moment.

  python3 sparse.py <base> [model=HUMANS] [name-regex=T_DIALOGGESTURE]
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

import numpy as np

import sampling as sp

ga = sp.ga
FPS = 30.0
RATE = 16.0   # keys a second: slower clips are the sparse ones (AnimSample::SparseRate)
METHODS = ("linear", "cr", "crlim")


def tracks(skel):
    names = {n.name: i for i, n in enumerate(skel.nodes)}
    want = {"hand": ("BIP01 R HAND", "BIP01 L HAND"), "head": ("BIP01 HEAD",), "chest": ("BIP01 SPINE2",)}
    return {k: [names[n] for n in v if n in names] for k, v in want.items()}


def measure(skel, a, loop):
    """The clip as the game shows it: a loop over one cycle; a clip that ends with its first key held before it
    starts and its last key held after its keys are over."""
    count = a.frame_count
    if loop:
        travel = np.linalg.norm((a.positions[-1, 0] - a.positions[0, 0])[[0, 2]]) if a.node_indices[0] == 0 else 0.0
        if travel >= 1.0:
            count -= 1  # Animation::AnimData::loopFrames
        u = np.arange(int(count / a.fps * FPS) + 1) / FPS * a.fps
    else:
        u = np.clip(np.arange(-4, int((count - 1) / a.fps * FPS) + 6) / FPS * a.fps, 0, count - 1)
    cross = (np.floor(u[2:]) != np.floor(u[1:-1])) | (np.floor(u[1:-1]) != np.floor(u[:-2]))
    out = {}
    path = {}
    for m in METHODS:
        p, q = sp.sample(a.positions[:count], a.rotations[:count], u, m, loop)
        w = sp.world(skel, a.node_indices, p, q)[:, :, :3, 3]
        w = w - w[:, [0]] * np.array([1.0, 0.0, 1.0])  # the root's way along the ground is the body's, not the pose's
        path[m] = w
        for name, nodes in tracks(skel).items():
            pops = [sp.second_difference(w[:, n]) for n in nodes]
            if not pops:
                continue
            pop = np.concatenate(pops)
            on = np.concatenate([x[cross] for x in pops])
            speed = max(np.linalg.norm(w[1:, n] - w[:-1, n], axis=-1).max() for n in nodes) * FPS
            out[(m, name)] = (pop.max(), np.percentile(pop, 95), on.sum() / max(pop.sum(), 1e-9), speed)
    hands = tracks(skel)["hand"]
    for m in METHODS[1:]:
        out[(m, "off")] = max(np.linalg.norm(path[m][:, n] - path["linear"][:, n], axis=-1).max() for n in hands)
    return out, path, u


def main():
    base = Path(sys.argv[1])
    model = sys.argv[2] if len(sys.argv) > 2 else "HUMANS"
    pattern = re.compile(sys.argv[3] if len(sys.argv) > 3 else "T_DIALOGGESTURE")
    skel = ga.read_skeleton(base / "models" / f"{model}.MDH.json")
    print("| clip | keys/s | method | hand: step change max / p95, cm | on key frames | hand speed, cm/s |"
          " head: max / p95 | chest: max / p95 | hand off the line at the same moment, cm |")
    print("|---|---|---|---|---|---|---|---|---|")
    total = {}
    for path in sorted((base / "models").glob(f"{model}-*.MAN.json")):
        meta = json.loads(path.read_text())["data"]
        if meta["fps"] >= RATE or meta["frame_count"] < 3 or not pattern.search(meta["name"]):
            continue
        a = ga.read_animation(path)
        res, _, _ = measure(skel, a, a.next.upper() == a.name.upper())
        for m in METHODS:
            h, hd, ch = res.get((m, "hand")), res.get((m, "head")), res.get((m, "chest"))
            if h is None:
                continue
            if m == "cr":
                continue  # the table: the line and the curve the engine has
            off = f"{res[(m, 'off')]:.1f}" if m != "linear" else ""
            print(f"| {a.name} | {a.fps:g} | {m} | {h[0]:.2f} / {h[1]:.2f} | {h[2] * 100:.0f}% | {h[3]:.0f} |"
                  f" {hd[0]:.2f} / {hd[1]:.2f} | {ch[0]:.2f} / {ch[1]:.2f} | {off} |")
            total.setdefault(m, []).append((h[0], h[1], h[2], hd[0], ch[0]))
    for m, rows in total.items():
        r = np.array(rows)
        print(f"all {len(rows)} clips, {m}: hand max {r[:, 0].max():.2f}, median of max {np.median(r[:, 0]):.2f},"
              f" median p95 {np.median(r[:, 1]):.2f}, median share on key frames {np.median(r[:, 2]) * 100:.0f}%,"
              f" head max {r[:, 3].max():.2f}, chest max {r[:, 4].max():.2f}")


if __name__ == "__main__":
    main()
