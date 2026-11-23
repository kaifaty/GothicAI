"""The guards of the TAA reactive mask (step-1v.md) from the clips of taa-reactive-clips.bench.

    uv run --no-project --with numpy python -I metrics.py <OPENGOTHIC_TAA=0 out> <mask 0 out> <mask 1 out> <view out>

The four runs record the same moments (a bench step is 1/30 s); the particles' random draws differ between runs, so
the measures are sums over zones and frames, not pixel pairs. Frames come from the clips (yuv420 H.264) as RGB.

Spark light (clip rv-1-torch, the camera stands): the zone is the dark box above the flame (SPARK_ZONE). The background of a
pixel is its median red over the clip (the wall, the sky: what stands still); its excess in a frame is its red minus
that, so only what passes lights up: sparks and the tips of the flame. light = the sum of excess over 10 levels, per frame, mean
over frames; peak = the largest excess, mean over frames 30, 60, 120, 150 (the measure of the wall torch report).
Both against the clip without TAA.

Flicker behind smoke (clip rv-3-smoke-day, the camera stands): the smoke is where the mask view is between 0.03 and
0.5 in a fifth of the frames or more (the flame, near 1, is left out). flicker = mean |F(k) - F(k-1)| of luma there;
the same over the water on the left (the control: the mask writes 0 there) and over the still rest of the frame.
"""
import json
import subprocess
import sys

import numpy as np

WARMUP = 10


def frames(path):
    probe = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries", "stream=width,height",
                            "-of", "json", str(path)], capture_output=True, check=True).stdout
    s = json.loads(probe)["streams"][0]
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", str(path), "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
                         capture_output=True, check=True).stdout
    return np.frombuffer(raw, np.uint8).reshape(-1, s["height"], s["width"], 3).astype(np.float32)


def view_mask(rgb):
    """reactive_view.frag back to the mask: 0 is black, red 0.25..1 is 0..0.5, then green joins up to 1."""
    r, g = rgb[..., 0] / 255.0, rgb[..., 1] / 255.0
    m = np.where(g > 0.02, (g + 1.0) / 2.0, np.clip((r - 0.25) / 0.75, 0.0, 1.0) / 2.0)
    return np.where(rgb.sum(-1) > 12, m, 0.0)  # coding noise near black is 0


# The zone above the flame of rv-1-torch at 1920x1080 (x0, y0, x1, y1): the dark corner of the wall and the night sky
# the sparks rise through, below it the flame and the lens ring of its flare start
SPARK_ZONE = (880, 100, 1040, 400)


def spark_light(runs):
    x0, y0, x1, y1 = SPARK_ZONE
    out = {"zone": list(SPARK_ZONE)}
    for name in ("taa0", "off", "on"):
        f = frames(f"{runs[name]}/rv-1-torch.mp4")[:, y0:y1, x0:x1, 0]
        bg = np.median(f[WARMUP:], axis=0)[None]  # the still picture of each pixel: the wall, the sky
        ex = f - bg
        light = np.where(ex > 10, ex, 0).reshape(len(f), -1).sum(1)[WARMUP:]
        peaks = [float(ex[k].max()) for k in (30, 60, 120, 150) if k < len(ex)]
        out[name] = {"light": float(light.mean()), "peak": float(np.mean(peaks)), "peaks": peaks,
                     "bright_px": float((ex > 40).reshape(len(f), -1).sum(1)[WARMUP:].mean())}
    for name in ("off", "on"):
        out[name]["light_vs_taa0"] = out[name]["light"] / out["taa0"]["light"]
        out[name]["peak_vs_taa0"] = out[name]["peak"] / out["taa0"]["peak"]
    return out


def luma(rgb):
    return rgb @ np.array([0.299, 0.587, 0.114], np.float32)


def flicker(runs):
    view = view_mask(frames(f"{runs['view']}/rv-3-smoke-day.mp4"))
    smoke = (((view > 0.03) & (view < 0.5)).mean(0) > 0.2) & ~((view > 0.5).mean(0) > 0.05)
    hgt, wid = smoke.shape
    water = np.zeros_like(smoke)
    water[int(hgt * 0.55):int(hgt * 0.8), :int(wid * 0.15)] = True  # the water on the left of the harbour shot
    water &= ~(view > 0).any(0)
    rest = ~(view > 0).any(0) & ~water
    out = {"smoke_px": int(smoke.sum()), "water_px": int(water.sum())}
    for name in ("taa0", "off", "on"):
        y = luma(frames(f"{runs[name]}/rv-3-smoke-day.mp4"))[WARMUP:]
        d = np.abs(np.diff(y, axis=0))
        out[name] = {"smoke": float(d[:, smoke].mean()), "water": float(d[:, water].mean()),
                     "rest": float(d[:, rest].mean())}
    return out


def main():
    runs = dict(zip(("taa0", "off", "on", "view"), sys.argv[1:5]))
    print(json.dumps({"spark": spark_light(runs), "flicker": flicker(runs)}, indent=1))


if __name__ == "__main__":
    main()
