#!/usr/bin/env python3
"""Dark frames and jumping blocks of a clip of scripts/bench/tree-shadow-flicker.bench (shadow-flicker.md).

For every frame: the mean luma (0..255, BT.601) of the slope under the camera and of the whole frame. A frame is
dark when the slope's mean falls more than a third below the reference: the median slope of --ref clips (a run
whose sun is known to be right), or else the 90th percentile of the clip itself.

  shadow_flicker.py <clip.mp4> [--ref <clip.mp4> ...] [--json out.json]

The slope is a box in fractions of the frame (--slope x0 y0 x1 y1), the hill under the camera of view near-30.

The mean of a large box does not see a shadow that comes and goes in a corner of the frame. --blocks measures that
(«Второй круг» of the note): the frame at 480x270 in blocks of 10x10 pixels; a block jumps when its luma changes
between neighbouring frames by more than 15 % of its median over the clip (plus 4, so that dark blocks do not jump by
noise). It counts the blocks that jump three times or more and the frames where two or more blocks jump, and with
--map draws the count of each block's jumps red over the clip's middle frame (960x540 PNG).

  shadow_flicker.py --blocks <clip.mp4> [--map map.png] [--json out.json]
"""
import argparse
import json
import subprocess
import sys

import numpy as np

W, H = 480, 270  # frames are scaled down for the measure


def frames(path):
    """Luma frames of a clip, W x H, as uint8 arrays."""
    cmd = ["ffmpeg", "-loglevel", "error", "-i", path, "-vf", f"scale={W}:{H}:flags=area",
           "-f", "rawvideo", "-pix_fmt", "gray", "-"]
    data = subprocess.run(cmd, check=True, capture_output=True).stdout
    n = len(data) // (W * H)
    return np.frombuffer(data[:n * W * H], dtype=np.uint8).reshape(n, H, W)


BLOCK = 10   # pixels of a block side at W x H
JUMP = 0.15  # a jump: the change between neighbouring frames past this share of the block's median luma
JUMPS = 3    # a block that jumps this many times or more flickers


def blocks(path, map_path=None):
    """The jumping blocks of a clip (--blocks): the counts, the box they are in, the map of their jumps."""
    # ffmpeg's default scaler and gray, as the first count of the user's (2026-11-18): the numbers stay comparable
    cmd = ["ffmpeg", "-loglevel", "error", "-i", path, "-vf", f"scale={W}:{H},format=gray", "-f", "rawvideo", "-"]
    data = subprocess.run(cmd, check=True, capture_output=True).stdout
    n = len(data) // (W * H)
    f = np.frombuffer(data[:n * W * H], dtype=np.uint8).reshape(n, H, W).astype(float)
    b = f.reshape(n, H // BLOCK, BLOCK, W // BLOCK, BLOCK).mean(axis=(2, 4))
    jump = np.abs(np.diff(b, axis=0)) > JUMP * (np.median(b, axis=0) + 4)
    count = jump.sum(axis=0)
    flicker = count >= JUMPS
    out = {
        "clip": path,
        "frames": n,
        "blocks": int(count.size),
        "blocks_flickering": int(flicker.sum()),
        "frames_with_jumps": int((jump.reshape(len(jump), -1).sum(axis=1) >= 2).sum()),
        "worst_block_jumps": int(count.max()) if count.size else 0,
    }
    ys, xs = np.nonzero(flicker)
    if len(xs):
        out["box"] = [round(xs.min() * BLOCK / W, 2), round(ys.min() * BLOCK / H, 2),
                      round((xs.max() + 1) * BLOCK / W, 2), round((ys.max() + 1) * BLOCK / H, 2)]
    if map_path:
        heat = np.kron(np.clip(count / max(count.max(), 1), 0, 1), np.ones((BLOCK, BLOCK)))
        base = f[n // 2] / 255 * 0.5
        rgb = np.stack([np.clip(base + heat, 0, 1), base, base], axis=2)
        rgb = (rgb * 255).astype(np.uint8).repeat(2, axis=0).repeat(2, axis=1)
        enc = ["ffmpeg", "-loglevel", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", f"{2 * W}x{2 * H}",
               "-i", "-", "-frames:v", "1", map_path]
        subprocess.run(enc, input=rgb.tobytes(), check=True)
    return out, count


def series(path, box):
    f = frames(path)
    x0, y0, x1, y1 = (int(round(v)) for v in (box[0] * W, box[1] * H, box[2] * W, box[3] * H))
    slope = f[:, y0:y1, x0:x1].reshape(len(f), -1).mean(axis=1)
    whole = f.reshape(len(f), -1).mean(axis=1)
    return slope, whole


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("clip")
    ap.add_argument("--blocks", action="store_true", help="count the jumping blocks instead of the dark frames")
    ap.add_argument("--map", help="--blocks: draw each block's jumps over the middle frame into this PNG")
    ap.add_argument("--ref", action="append", default=[], help="clips whose median slope is the reference")
    ap.add_argument("--slope", nargs=4, type=float, default=[0.08, 0.6, 0.4, 0.95],
                    metavar=("X0", "Y0", "X1", "Y1"), help="the slope's box in fractions of the frame")
    ap.add_argument("--json", help="write the numbers here")
    args = ap.parse_args()

    if args.blocks:
        out, count = blocks(args.clip, args.map)
        print(json.dumps(out, ensure_ascii=False))
        if args.json:
            with open(args.json, "w") as fp:
                json.dump(dict(out, jumps=count.tolist()), fp, ensure_ascii=False)
        return 0

    slope, whole = series(args.clip, args.slope)
    if args.ref:
        ref = float(np.median(np.concatenate([series(r, args.slope)[0] for r in args.ref])))
        how = "median of --ref"
    else:
        ref = float(np.percentile(slope, 90))
        how = "p90 of the clip"
    dark = slope < ref * (2.0 / 3.0)
    out = {
        "clip": args.clip,
        "frames": int(len(slope)),
        "reference": round(ref, 2),
        "reference_from": how,
        "dark_frames": int(dark.sum()),
        "dark_share": round(float(dark.mean()) if len(slope) else 0.0, 4),
        "slope_min": round(float(slope.min()), 2),
        "slope_median": round(float(np.median(slope)), 2),
        "slope_max": round(float(slope.max()), 2),
        "whole_min": round(float(whole.min()), 2),
        "whole_median": round(float(np.median(whole)), 2),
        "whole_max": round(float(whole.max()), 2),
        "dark_at": [int(i) for i in np.flatnonzero(dark)[:200]],
    }
    print(json.dumps({k: v for k, v in out.items() if k != "dark_at"}, ensure_ascii=False))
    if args.json:
        with open(args.json, "w") as fp:
            json.dump(dict(out, slope=[round(float(v), 2) for v in slope], whole=[round(float(v), 2) for v in whole]),
                      fp, ensure_ascii=False, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
