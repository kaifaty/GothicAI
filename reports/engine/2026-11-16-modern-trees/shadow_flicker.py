#!/usr/bin/env python3
"""Dark frames of a clip of scripts/bench/tree-shadow-flicker.bench (shadow-flicker.md).

For every frame: the mean luma (0..255, BT.601) of the slope under the camera and of the whole frame. A frame is
dark when the slope's mean falls more than a third below the reference: the median slope of --ref clips (a run
whose sun is known to be right), or else the 90th percentile of the clip itself.

  shadow_flicker.py <clip.mp4> [--ref <clip.mp4> ...] [--json out.json] [--sheet sheet.png]

The slope is a box in fractions of the frame (--slope x0 y0 x1 y1), the hill under the camera of view near-30.
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


def series(path, box):
    f = frames(path)
    x0, y0, x1, y1 = (int(round(v)) for v in (box[0] * W, box[1] * H, box[2] * W, box[3] * H))
    slope = f[:, y0:y1, x0:x1].reshape(len(f), -1).mean(axis=1)
    whole = f.reshape(len(f), -1).mean(axis=1)
    return slope, whole


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("clip")
    ap.add_argument("--ref", action="append", default=[], help="clips whose median slope is the reference")
    ap.add_argument("--slope", nargs=4, type=float, default=[0.08, 0.6, 0.4, 0.95],
                    metavar=("X0", "Y0", "X1", "Y1"), help="the slope's box in fractions of the frame")
    ap.add_argument("--json", help="write the numbers here")
    args = ap.parse_args()

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
