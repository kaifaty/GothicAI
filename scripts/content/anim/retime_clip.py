"""Play parts of a finished clip faster or slower: frames of the clip are mapped onto new frames, the poses between
them are resampled, and feet that stand are pinned again.

    python3 scripts/content/anim/retime_clip.py <in.MAN.json> <models dir> <out.MAN.json> --keys 0:0,6:3,13:10

`--keys` is `source frame:new frame, ...`, from the first frame to the last; between the keys the time runs smoothly
(a monotone cubic). 0:0,6:3,13:10 plays the first six frames twice as fast and the rest as it was: a run start
cut from a Kimodo sample spends its first 0.2 s shifting the weight with the root nearly still, which reads as a
sluggish start in the game. The first and the last pose stay, so the clip still meets the stance and the loop; the
root's way is the same, made in less time. Name, next and the frame rate stay; events are dropped.
"""
from __future__ import annotations

import argparse
import dataclasses
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import finish
import gothic_anim as ga


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src", type=Path)
    ap.add_argument("models", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--keys", required=True, help="source frame:new frame, ...")
    ap.add_argument("--no-lock", action="store_true", help="do not pin the standing feet again")
    a = ap.parse_args()
    skel = ga.read_skeleton(a.models / "HUMANS.MDH.json")
    anim = ga.read_animation(a.src)
    keys = [tuple(float(x) / anim.fps for x in k.split(":")) for k in a.keys.split(",")]
    out = finish.retime(anim, keys)
    out = dataclasses.replace(out, name=anim.name, next=anim.next, layer=anim.layer, events=[])
    if not a.no_lock:
        finish.lock_feet(out, skel, turn=True)
    ga.write_animation(a.out, out, f"{a.src.name} retimed {a.keys}")
    print(f"{anim.frame_count} -> {out.frame_count} frames, {out.frame_count / out.fps:.3f} s")


if __name__ == "__main__":
    main()
