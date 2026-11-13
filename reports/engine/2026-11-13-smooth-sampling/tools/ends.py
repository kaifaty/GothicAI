"""What a clip does at its ends, and which rule for the keys it does not have outside them brings the source back:
clips of the base keyed 25 times a second that are no loops, every 2nd or 3rd key kept, the first and the last key
interval sampled back by the curve with each rule and compared with the dropped frames.

  python3 ends.py <base> [model=HUMANS]

  mirror  the outer chord goes on at its speed
  rest    the clip starts from rest and comes to rest (what the engine has)
  half    half the chord's speed (plain Catmull-Rom's end)

Also prints how fast the source itself moves in its first and last frame against the mean of that key interval, and
for the sparse clips, shown as the game shows them (the first key held before, the last one after), the largest
change of a hand's step between two frames by rule against the line.
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

import numpy as np

import sampling as sp

ga = sp.ga
RULES = ("mirror", "rest", "half")
FPS = 30.0


def main():
    base = Path(sys.argv[1])
    model = sys.argv[2] if len(sys.argv) > 2 else "HUMANS"
    skel = ga.read_skeleton(base / "models" / f"{model}.MDH.json")
    names = {n.name: i for i, n in enumerate(skel.nodes)}
    hands = [names[n] for n in ("BIP01 R HAND", "BIP01 L HAND") if n in names]
    acc, first, last, clips = {}, [], [], 0
    shown = {r: [] for r in RULES}
    for path in sorted((base / "models").glob(f"{model}-*.MAN.json")):
        meta = json.loads(path.read_text())["data"]
        if meta["frame_count"] < 3:
            continue
        a = ga.read_animation(path)
        if a.next.upper() == a.name.upper():
            continue
        if meta["fps"] < 16.0:
            # a sparse clip as the game shows it
            u = np.clip(np.arange(-4, int((a.frame_count - 1) / a.fps * FPS) + 6) / FPS * a.fps, 0, a.frame_count - 1)
            inner = (u[2:] > 0.2) & (u[2:] < a.frame_count - 1.2)

            def pops(method, rule):
                p, q = sp.sample(a.positions, a.rotations, u, method, False, rule)
                w = sp.world(skel, a.node_indices, p, q)[:, :, :3, 3]
                pop = np.stack([sp.second_difference(w[:, n]) for n in hands])
                return pop.max(), pop[:, inner].max(), pop[:, ~inner].max()
            line = pops("linear", "mirror")
            if line[0] >= 0.1:
                for rule in RULES:
                    got = pops("crlim", rule)
                    shown[rule].append([got[i] / max(line[i], 1e-6) for i in range(3)])
            continue
        if meta["fps"] != 25.0 or meta["frame_count"] < 25 or \
                re.search(r"ATTACK|PARADE|COMBO|JUMP|FALL|STUMBLE|RUNL|WALKL|TURN|STRAFE", a.name):
            continue
        clips += 1
        truth = sp.world(skel, a.node_indices, a.positions, a.rotations)[:, :, :3, 3]
        for k in (2, 3):
            n = (a.frame_count - 1) // k * k + 1
            u = np.arange(n) / k
            for rule in RULES:
                p, q = sp.sample(a.positions[0:n:k], a.rotations[0:n:k], u, "crlim", False, rule)
                got = sp.world(skel, a.node_indices, p, q)[:, :, :3, 3]
                for node in hands:
                    e = np.linalg.norm(got[:, node] - truth[:n, node], axis=-1)
                    acc.setdefault((k, rule, "first"), []).append(e[1:k])
                    acc.setdefault((k, rule, "last"), []).append(e[n - k:n - 1])
            for node in hands:
                t = truth[:n, node]
                chord = np.linalg.norm(t[n - 1] - t[n - 1 - k]) / k
                if chord > 0.5:
                    last.append(np.linalg.norm(t[n - 1] - t[n - 2]) / chord)
                chord = np.linalg.norm(t[k] - t[0]) / k
                if chord > 0.5:
                    first.append(np.linalg.norm(t[1] - t[0]) / chord)
    print(f"{model}: {clips} clips of 25 keys a second that end")
    print("| keys kept | interval | " + " | ".join(f"{r}: rms / p95, cm" for r in RULES) + " |\n|---|---|---|---|---|")
    for k in (2, 3):
        for where in ("first", "last"):
            cells = []
            for rule in RULES:
                e = np.concatenate(acc[(k, rule, where)])
                cells.append(f"{np.sqrt((e * e).mean()):.2f} / {np.percentile(e, 95):.2f}")
            print(f"| every {k}th | {where} | " + " | ".join(cells) + " |")
    for name, r in (("first", np.array(first)), ("last", np.array(last))):
        print(f"the source's {name} frame against the mean step of that key interval: median {np.median(r):.2f},"
              f" quartiles {np.percentile(r, 25):.2f}..{np.percentile(r, 75):.2f}, under half of it: {(r < 0.5).mean() * 100:.0f}%")
    print(f"\n{len(shown['rest'])} sparse clips that end, as the game shows them: the largest change of a hand's step, curve / line,"
          " medians (the whole clip, inside it, at its ends)")
    for rule in RULES:
        v = np.array(shown[rule])
        print(f"  {rule}: {np.median(v[:, 0]):.2f}, {np.median(v[:, 1]):.2f}, {np.median(v[:, 2]):.2f}")


if __name__ == "__main__":
    main()
