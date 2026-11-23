"""Which sampling brings back what was dropped: clips of the base keyed 25 times a second, every 2nd or 3rd key
kept (12.5 and 8.3 keys a second, around the 10 of the sparse clips), sampled back at the dropped frames and
compared with them.

  python3 decimate.py <base> [model=HUMANS]

Per group of clips and per method: the error of the hands, the head and the feet in the model space (cm), the
change of the hand's step between two frames (the stand's an_pop, cm) against the source's own, and how far a
hand that the source holds still (under 0.3 cm over a key interval) is moved.
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

import numpy as np

import sampling as sp

ga = sp.ga


def group(name, layer):
    if re.search(r"ATTACK|PARADE|COMBO", name):
        return "blows"
    if re.search(r"^S_.*(RUNL|WALKL|WALKBL|SNEAKL|SNEAKBL|SWIMF|SPRINTL)$|STRAFE", name):
        return "walk"
    if re.search(r"JUMP|FALL|STUMBLE|DEAD|RUNL_2|WALKL_2|_2_.*RUNL|_2_.*WALKL|TURN", name):
        return "moves"
    return "calm"  # gestures, idle extras, work at objects, transitions between stances


def main():
    base = Path(sys.argv[1])
    model = sys.argv[2] if len(sys.argv) > 2 else "HUMANS"
    skel = ga.read_skeleton(base / "models" / f"{model}.MDH.json")
    names = {n.name: i for i, n in enumerate(skel.nodes)}
    track = {"hand": [names[n] for n in ("BIP01 R HAND", "BIP01 L HAND") if n in names],
             "head": [names[n] for n in ("BIP01 HEAD",) if n in names],
             "foot": [names[n] for n in ("BIP01 R FOOT", "BIP01 L FOOT") if n in names]}
    acc = {}
    clips = {}
    for path in sorted((base / "models").glob(f"{model}-*.MAN.json")):
        meta = json.loads(path.read_text())["data"]
        if meta["fps"] != 25.0 or meta["frame_count"] < 25:
            continue
        a = ga.read_animation(path)
        g = group(a.name, a.layer)
        clips[g] = clips.get(g, 0) + 1
        truth = sp.world(skel, a.node_indices, a.positions, a.rotations)[:, :, :3, 3]
        for k in (2, 3):
            n = (a.frame_count - 1) // k * k + 1
            keys = slice(0, n, k)
            u = np.arange(n) / k
            inner = (np.arange(n) % k) != 0
            for m in sp.METHODS:
                # the ends at the chord's speed: the source is cut in motion (ends.py), so the methods differ only
                # by what they do between keys
                p, q = sp.sample(a.positions[keys], a.rotations[keys], u, m, ends="mirror")
                got = sp.world(skel, a.node_indices, p, q)[:, :, :3, 3]
                rec = acc.setdefault((g, k, m), {"err": {t: [] for t in track}, "pop": [], "pop0": [], "hold": []})
                for t, nodes in track.items():
                    for node in nodes:
                        rec["err"][t].append(np.linalg.norm(got[inner, node] - truth[:n][inner, node], axis=-1))
                for node in track["hand"]:
                    rec["pop"].append(sp.second_difference(got[:, node]))
                    rec["pop0"].append(sp.second_difference(truth[:n, node]))
                    # a hand the source holds still over a whole key interval: how far the method moves it
                    for s in range(0, n - k, k):
                        seg = truth[s:s + k + 1, node]
                        if np.linalg.norm(seg - seg[0], axis=-1).max() < 0.3:
                            rec["hold"].append(np.linalg.norm(got[s:s + k + 1, node] - seg, axis=-1).max())
    print(f"model {model}: clips " + ", ".join(f"{g} {c}" for g, c in sorted(clips.items())))
    for k in (2, 3):
        print(f"\n## every {k}th key kept ({25 / k:.1f} keys a second)\n")
        print("| clips | method | hand: rms / p99 / max, cm | head: rms / max | foot: rms / max |"
              " hand step change: p99 / max, cm (source p99 / max) | held hand moved: p99 / max, cm (holds) |")
        print("|---|---|---|---|---|---|---|")
        for g in ("calm", "moves", "walk", "blows"):
            for m in sp.METHODS:
                rec = acc.get((g, k, m))
                if rec is None:
                    continue
                e = {t: np.concatenate(v) for t, v in rec["err"].items()}
                pop, pop0 = np.concatenate(rec["pop"]), np.concatenate(rec["pop0"])
                hold = np.array(rec["hold"]) if rec["hold"] else np.zeros(1)
                rms = lambda x: float(np.sqrt((x * x).mean()))
                print(f"| {g} | {m} | {rms(e['hand']):.2f} / {np.percentile(e['hand'], 99):.2f} / {e['hand'].max():.1f}"
                      f" | {rms(e['head']):.2f} / {e['head'].max():.1f} | {rms(e['foot']):.2f} / {e['foot'].max():.1f}"
                      f" | {np.percentile(pop, 99):.2f} / {pop.max():.1f} ({np.percentile(pop0, 99):.2f} / {pop0.max():.1f})"
                      f" | {np.percentile(hold, 99):.2f} / {hold.max():.2f} ({len(rec['hold'])}) |")


if __name__ == "__main__":
    main()
