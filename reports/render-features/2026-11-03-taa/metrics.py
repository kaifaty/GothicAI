"""Sharpness and frame-to-frame change of the taa-clips.bench clips, CMAA2 against TAA.

    ~/.local/bin/uv run --with numpy python -I metrics.py <CMAA2 out dir> <TAA out dir>

sharp: mean |gradient| of the luminance (x plus y), frames 15.. (past the history warm-up); the 1a acceptance is
sharp(TAA)/sharp(CMAA2) >= 0.9 on every clip. frame-diff: mean |difference| of neighbouring frames; on a clip with
a camera that stands still it is the flicker. After the lead's script for the 800x600 clips (2026-11-04).
"""
import json
import pathlib
import subprocess
import sys

import numpy as np


def frames(path):
    probe = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries", "stream=width,height",
                            "-of", "json", str(path)], capture_output=True, check=True).stdout
    s = json.loads(probe)["streams"][0]
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", str(path), "-f", "rawvideo", "-pix_fmt", "gray", "-"],
                         capture_output=True, check=True).stdout
    return np.frombuffer(raw, np.uint8).reshape(-1, s["height"], s["width"]).astype(np.float32)


def sharp(f):
    return np.abs(np.diff(f, axis=2)).mean() + np.abs(np.diff(f, axis=1)).mean()


def flick(f):
    return np.abs(np.diff(f, axis=0)).mean()


def main():
    a, b = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    for clip in sorted(p.name for p in a.glob("taa-*.mp4")):
        if not (b / clip).exists():
            continue
        fa, fb = frames(a / clip)[15:], frames(b / clip)[15:]
        name = clip[4:-4]
        print(f"{name:14s} {fa.shape[2]}x{fa.shape[1]} sharp CMAA2 {sharp(fa):6.2f} TAA {sharp(fb):6.2f} "
              f"ratio {sharp(fb)/sharp(fa):.2f} | frame-diff CMAA2 {flick(fa):5.2f} TAA {flick(fb):5.2f} "
              f"ratio {flick(fb)/flick(fa):.2f}")


if __name__ == "__main__":
    main()
