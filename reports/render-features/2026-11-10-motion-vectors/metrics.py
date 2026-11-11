"""Trail and sharpness of a moving figure in motion-vectors-clips.bench, TAA without and with object vectors.

    uv run --no-project --with numpy python -I metrics.py <OPENGOTHIC_TAA=0 out> <-mv 0 out> <-mv 1 out>

The three runs record the same moments (a bench step is 1/30 s), so frame k of each clip shows the same scene.
Frames come from the clips (yuv420 H.264), as gray.

Clip a (the hero walks across a still camera), from the frames without TAA, with d = 6 frames and T = 20 levels:
  figure: |F(k) - F(k-d)| > T and |F(k) - F(k+d)| > T -- here now, not d frames before or after;
  trail band: |F(k) - F(k-d)| > T and |F(k) - F(k+d)| <= T -- where the figure was d frames ago and the background
  is now and stays: a trail of the history lies there.
  trail = mean |TAA(k) - F(k)| over the band; base = the same over the pixels that do not change (the cost of TAA
  itself: jitter, sharpening, coding).
  sharp = mean |gradient| (x plus y) over the figure, against the same pixels without TAA.
Clip b (the hero runs, the camera follows): the box where the hero stays on the picture; sharp only.
"""
import json
import pathlib
import subprocess
import sys

import numpy as np

D = 6
T = 20.0
WARMUP = 15
BOX_B = (850, 380, 1050, 820)  # x0, y0, x1, y1 of the hero in clip b at 1920x1080


def frames(path):
    probe = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries", "stream=width,height",
                            "-of", "json", str(path)], capture_output=True, check=True).stdout
    s = json.loads(probe)["streams"][0]
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", str(path), "-f", "rawvideo", "-pix_fmt", "gray", "-"],
                         capture_output=True, check=True).stdout
    return np.frombuffer(raw, np.uint8).reshape(-1, s["height"], s["width"]).astype(np.float32)


def grad(f):
    g = np.zeros_like(f)
    g[:, :-1] += np.abs(np.diff(f, axis=1))
    g[:-1, :] += np.abs(np.diff(f, axis=0))
    return g


def clip_a(ref, a, b):
    n = min(len(ref), len(a), len(b))
    trail = {"mv0": [], "mv1": []}
    base = {"mv0": [], "mv1": []}
    sharp = {"taa0": [], "mv0": [], "mv1": []}
    used = 0
    for k in range(WARMUP + D, n - D):
        past = np.abs(ref[k] - ref[k - D]) > T
        fut = np.abs(ref[k] - ref[k + D]) > T
        fig = past & fut
        band = past & ~fut
        still = ~(past | fut)
        if fig.sum() < 2000 or band.sum() < 2000:
            continue  # the figure has left the picture
        used += 1
        for name, f in (("mv0", a), ("mv1", b)):
            trail[name].append(np.abs(f[k] - ref[k])[band].mean())
            base[name].append(np.abs(f[k] - ref[k])[still].mean())
        for name, f in (("taa0", ref), ("mv0", a), ("mv1", b)):
            sharp[name].append(grad(f[k])[fig].mean())
    m = lambda v: float(np.mean(v)) if v else float("nan")
    return used, {k: m(v) for k, v in trail.items()}, {k: m(v) for k, v in base.items()}, {k: m(v) for k, v in sharp.items()}


def clip_b(ref, a, b):
    x0, y0, x1, y1 = BOX_B
    n = min(len(ref), len(a), len(b))
    out = {}
    for name, f in (("taa0", ref), ("mv0", a), ("mv1", b)):
        out[name] = float(np.mean([grad(f[k])[y0:y1, x0:x1].mean() for k in range(WARMUP, n)]))
    return n - WARMUP, out


def main():
    ref, off, on = (pathlib.Path(p) for p in sys.argv[1:4])
    fa = [frames(d / "mv-a-walk.mp4") for d in (ref, off, on)]
    used, trail, base, sharp = clip_a(*fa)
    print(f"a-walk: {used} frames | trail band |TAA - noTAA|: -mv 0 {trail['mv0']:.2f}, -mv 1 {trail['mv1']:.2f} "
          f"(still pixels: {base['mv0']:.2f}, {base['mv1']:.2f}) | figure sharp: noTAA {sharp['taa0']:.2f}, "
          f"-mv 0 {sharp['mv0']:.2f} ({sharp['mv0']/sharp['taa0']:.2f}), -mv 1 {sharp['mv1']:.2f} "
          f"({sharp['mv1']/sharp['taa0']:.2f})")
    fb = [frames(d / "mv-b-run.mp4") for d in (ref, off, on)]
    used, sharp = clip_b(*fb)
    print(f"b-run: {used} frames | hero box sharp: noTAA {sharp['taa0']:.2f}, -mv 0 {sharp['mv0']:.2f} "
          f"({sharp['mv0']/sharp['taa0']:.2f}), -mv 1 {sharp['mv1']:.2f} ({sharp['mv1']/sharp['taa0']:.2f})")


if __name__ == "__main__":
    main()
