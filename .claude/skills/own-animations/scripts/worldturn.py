"""Model-space turns a frame of chosen bones: the snap the eye sees. A local turn overstates a whip that the child undoes
for its parent (the novice's neck read 98° a frame locally, the head turned 65° in the world).

    python3 worldturn.py <base models> <first frame> <last frame> <record.MAN.json>... [--bones "BIP01 HEAD,BIP01 L HAND"]
"""
import argparse
from pathlib import Path

import numpy as np

from common import ga, skeleton, turn

ap = argparse.ArgumentParser()
ap.add_argument("base", type=Path)
ap.add_argument("first", type=int)
ap.add_argument("last", type=int)
ap.add_argument("records", nargs="+")
ap.add_argument("--bones", default="BIP01 SPINE2,BIP01 HEAD,BIP01 L FOREARM,BIP01 L HAND,BIP01 R UPPERARM,BIP01 R HAND")
args = ap.parse_args()
skel = skeleton(args.base)
ix = skel.index
for f in args.records:
    a = ga.read_animation(f)
    W = [ga.world_pose(skel, a, k) for k in range(a.frame_count)]
    print(f)
    for n in args.bones.split(","):
        t = [turn(W[k][ix(n)][:3, :3], W[k + 1][ix(n)][:3, :3]) for k in range(args.first, min(args.last, a.frame_count - 1))]
        print("  ", n[6:].ljust(12), " ".join(f"{x:3.0f}" for x in t), f" max {max(t):.0f} at {args.first + int(np.argmax(t))}")
