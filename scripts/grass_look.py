#!/usr/bin/env python3
"""The look of the grass by measure (reports/engine/2026-11-14-grass-system, «Круг правки вида»).

Reads the frames of scripts/bench/landscape-meadow.bench and landscape-meadow-parts.bench; needs numpy and ffmpeg.

  mask    <mask-view.png> <out.png>      the sunlit ground of the meadow, from the frame painted by `grass parts 193`
  tone    <mask.png> <frame>...          the mean luminance (of the light) and luma (of the coded values) and the
                                         colour of that area in each frame, against the first
  cover   <paint.png> <bands.png>        the share of the ground the blades cover, by the band of the distance
  blades  <mask.png> <paint.png> <frame> <ground>
                                         the colour of the blades (red in the paint, inside the mask) in the frame
                                         against the ground under them, in the frame without blades
  columns <clip> [--half left|right] [--rows a:b] [--strips n] [--step s]
                                         the tone of the frame's vertical strips in time: a gust that crosses
  still   <clip> [--half left|right] [--crop x0:y0:x1:y1]
                                         the mean difference of a frame from the one before, of 255; the crop in
                                         shares of the frame keeps the hero and the sky out
"""
from __future__ import annotations

import argparse
import colorsys
import json
import subprocess
import sys

import numpy as np


def probe(path: str) -> tuple[int, int]:
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries", "stream=width,height", "-of", "csv=p=0", path],
        check=True, capture_output=True, text=True).stdout.strip().split(",")
    return int(out[0]), int(out[1])


def read_image(path: str) -> np.ndarray:
    w, h = probe(path)
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-frames:v", "1", "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
                         check=True, capture_output=True).stdout
    return np.frombuffer(raw, np.uint8).reshape(h, w, 3)


def write_gray(path: str, mask: np.ndarray) -> None:
    h, w = mask.shape
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "gray", "-s", f"{w}x{h}", "-i", "-",
                    "-frames:v", "1", path], input=(mask.astype(np.uint8)*255).tobytes(), check=True)


def linear(rgb: np.ndarray) -> np.ndarray:
    c = rgb.astype(np.float64)/255.0
    return np.where(c <= 0.04045, c/12.92, ((c + 0.055)/1.055)**2.4)


def luminance(rgb: np.ndarray) -> np.ndarray:
    return linear(rgb) @ np.array([0.2126, 0.7152, 0.0722])


def erode(mask: np.ndarray, n: int) -> np.ndarray:
    m = mask.copy()
    for _ in range(n):
        k = m.copy()
        k[1:, :] &= m[:-1, :]
        k[:-1, :] &= m[1:, :]
        k[:, 1:] &= m[:, :-1]
        k[:, :-1] &= m[:, 1:]
        m = k
    return m


def meadow_mask(painted: np.ndarray) -> np.ndarray:
    """Magenta is the ground of a material with `grass`; the sunlit part of it is the brighter half."""
    p = painted.astype(np.int32)
    r, g, b = p[..., 0], p[..., 1], p[..., 2]
    ground = (r > 24) & (b > 24) & (2*g < r) & (2*g < b)
    if not ground.any():
        return ground
    bright = (r + b)[ground]
    sunlit = ground & ((r + b) >= 0.5*np.percentile(bright, 90))
    return erode(sunlit, 4)


def cmd_mask(a: argparse.Namespace) -> int:
    painted = read_image(a.frame)
    m = meadow_mask(painted)
    write_gray(a.out, m)
    print(json.dumps({"mask": a.out, "share_of_frame": round(float(m.mean()), 4)}))
    return 0


def tone_of(rgb: np.ndarray, mask: np.ndarray) -> dict:
    px = rgb[mask]
    lum = float(luminance(px).mean())
    mean = px.mean(axis=0)
    # the colour of the mean light: what the area looks like from afar
    lin = linear(px).mean(axis=0)
    srgb = np.where(lin <= 0.0031308, lin*12.92, 1.055*lin**(1/2.4) - 0.055)*255.0
    h, s, v = colorsys.rgb_to_hsv(*(srgb/255.0))
    # the luma of the coded values: the tone as the eye reads it off the screen
    luma = float(mean @ np.array([0.2126, 0.7152, 0.0722]))
    return {"luminance": round(lum, 5), "luma": round(luma, 2), "mean_srgb": [round(float(x), 1) for x in mean],
            "colour": [round(float(x), 1) for x in srgb], "hue": round(h*360.0, 1), "saturation": round(s, 3)}


def cmd_tone(a: argparse.Namespace) -> int:
    mask = read_image(a.mask)[..., 0] > 127
    first = None
    for path in a.frames:
        t = tone_of(read_image(path), mask)
        if first is None:
            first = t
        t["luminance_ratio"] = round(t["luminance"]/first["luminance"], 3)
        t["luma_ratio"] = round(t["luma"]/first["luma"], 3)
        t["saturation_ratio"] = round(t["saturation"]/max(first["saturation"], 1e-6), 3)
        t["frame"] = path
        print(json.dumps(t, ensure_ascii=False))
    return 0


BANDS = {"2 m": (0, 0, 1), "3.5 m": (0, 1, 1), "5 m": (0, 1, 0), "10 m": (1, 1, 0)}


def band_masks(rgb: np.ndarray) -> dict[str, np.ndarray]:
    p = rgb.astype(np.float64)
    top = p.max(axis=2)
    lit = top > 20
    n = p/np.maximum(top, 1.0)[..., None]
    out = {}
    for name, key in BANDS.items():
        m = lit.copy()
        for ch in range(3):
            m &= (n[..., ch] > 0.6) if key[ch] else (n[..., ch] < 0.4)
        out[name] = m
    return out


def cmd_cover(a: argparse.Namespace) -> int:
    paint, bands = band_masks(read_image(a.paint)), band_masks(read_image(a.bands))
    row = {}
    for name in BANDS:
        total = int(bands[name].sum())
        if total < 500:
            row[name] = None  # the band is not in the frame
            continue
        seen = int((bands[name] & paint[name]).sum())
        row[name] = {"covered": round(1.0 - seen/total, 3), "pixels": total}
    print(json.dumps(row, ensure_ascii=False))
    return 0


def cmd_blades(a: argparse.Namespace) -> int:
    paint = read_image(a.paint).astype(np.int32)
    blades = (paint[..., 0] > 40) & (3*paint[..., 1] < paint[..., 0]) & (3*paint[..., 2] < paint[..., 0])
    area = blades & (read_image(a.mask)[..., 0] > 127)
    grass, ground = tone_of(read_image(a.frame), area), tone_of(read_image(a.ground), area)
    print(json.dumps({"blades_share_of_mask": round(float(area.sum())/max(int((read_image(a.mask)[..., 0] > 127).sum()), 1), 3),
                      "blades": grass, "ground": ground, "hue_difference": round(grass["hue"] - ground["hue"], 1),
                      "saturation_ratio": round(grass["saturation"]/max(ground["saturation"], 1e-6), 3),
                      "luminance_ratio": round(grass["luminance"]/max(ground["luminance"], 1e-9), 3)}, ensure_ascii=False))
    return 0


def clip_frames(path: str, half: str | None, scale: int):
    w, h = probe(path)
    crop = {"left": f"crop=iw/2:ih:0:0,", "right": f"crop=iw/2:ih:iw/2:0,", None: ""}[half]
    cw = w//2 if half else w
    ow, oh = cw//scale, h//scale
    proc = subprocess.Popen(["ffmpeg", "-v", "error", "-i", path, "-vf", f"{crop}scale={ow}:{oh}:flags=area",
                             "-f", "rawvideo", "-pix_fmt", "gray", "-"], stdout=subprocess.PIPE)
    size = ow*oh
    assert proc.stdout is not None
    while True:
        raw = proc.stdout.read(size)
        if len(raw) < size:
            break
        yield np.frombuffer(raw, np.uint8).reshape(oh, ow).astype(np.float64)
    proc.wait()


def fps_of(path: str) -> float:
    out = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries", "stream=r_frame_rate",
                          "-of", "csv=p=0", path], check=True, capture_output=True, text=True).stdout.strip()
    num, den = out.split("/")
    return float(num)/float(den)


def cmd_columns(a: argparse.Namespace) -> int:
    fps = fps_of(a.clip)
    r0, r1 = (float(x) for x in a.rows.split(":"))
    series = []
    for f in clip_frames(a.clip, a.half, 4):
        h, w = f.shape
        band = f[int(r0*h):int(r1*h), :]
        series.append([band[:, int(w*i/a.strips):int(w*(i + 1)/a.strips)].mean() for i in range(a.strips)])
    s = np.array(series)
    per = max(int(round(fps*a.step)), 1)
    steps = s[:len(s)//per*per].reshape(-1, per, a.strips).mean(axis=1)
    base = steps[:max(int(1.0/a.step), 1)].mean(axis=0)  # the first second: before the gust
    rise = steps - base
    out = {"clip": a.clip, "half": a.half, "strips_left_to_right": a.strips, "step_s": a.step,
           "peak_rise_of_255": [round(float(x), 2) for x in rise.max(axis=0)],
           "peak_at_s": [round(float(x)*a.step, 1) for x in rise.argmax(axis=0)],
           "half_rise_at_s": [round(float(np.argmax(rise[:, i] >= 0.5*rise[:, i].max()))*a.step, 1) for i in range(a.strips)]}
    print(json.dumps(out, ensure_ascii=False))
    return 0


def cmd_still(a: argparse.Namespace) -> int:
    prev, diffs = None, []
    x0, y0, x1, y1 = (float(v) for v in a.crop.split(":"))
    for f in clip_frames(a.clip, a.half, 1):
        h, w = f.shape
        f = f[int(y0*h):int(y1*h), int(x0*w):int(x1*w)]
        if prev is not None:
            diffs.append(float(np.abs(f - prev).mean()))
        prev = f
    d = np.array(diffs)
    print(json.dumps({"clip": a.clip, "half": a.half, "crop": a.crop, "frames": len(d) + 1, "mean_diff_of_255": round(float(d.mean()), 3),
                      "worst_diff_of_255": round(float(d.max()), 3)}))
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("mask"); p.add_argument("frame"); p.add_argument("out"); p.set_defaults(fn=cmd_mask)
    p = sub.add_parser("tone"); p.add_argument("mask"); p.add_argument("frames", nargs="+"); p.set_defaults(fn=cmd_tone)
    p = sub.add_parser("cover"); p.add_argument("paint"); p.add_argument("bands"); p.set_defaults(fn=cmd_cover)
    p = sub.add_parser("blades"); p.add_argument("mask"); p.add_argument("paint"); p.add_argument("frame")
    p.add_argument("ground"); p.set_defaults(fn=cmd_blades)
    p = sub.add_parser("columns"); p.add_argument("clip"); p.add_argument("--half", choices=["left", "right"])
    p.add_argument("--rows", default="0.45:0.95"); p.add_argument("--strips", type=int, default=8)
    p.add_argument("--step", type=float, default=0.5); p.set_defaults(fn=cmd_columns)
    p = sub.add_parser("still"); p.add_argument("clip"); p.add_argument("--half", choices=["left", "right"])
    p.add_argument("--crop", default="0:0:1:1"); p.set_defaults(fn=cmd_still)
    a = ap.parse_args()
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
