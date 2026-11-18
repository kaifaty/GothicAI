#!/usr/bin/env python3
"""The look of the grass by measure (reports/engine/2026-11-14-grass-system, «Круг правки вида»).

Reads the frames of scripts/bench/landscape-meadow.bench and landscape-meadow-parts.bench; needs numpy and ffmpeg.

  mask    <mask-view.png> <out.png>      the sunlit ground of the meadow, from the frame painted by `grass parts 193`
  tone    <mask.png> <frame>...          the mean luminance (of the light) and luma (of the coded values) and the
                                         colour of that area in each frame, against the first
  cover   <paint.png> <bands.png>        the share of the ground the blades cover, by the band of the distance
  farbands <frame>...                    the pixels of the ground of the grass in the bands of grassPaintFarBand
                                         (scripts/bench/grass-bands.bench, `grass parts 512` and 640): what a layer
                                         over the far ground would shade (reports/engine/2026-11-18-grass-volume)
  blades  <mask.png> <paint.png> <frame> <ground>
                                         the colour of the blades (red in the paint, inside the mask) in the frame
                                         against the ground under them, in the frame without blades
  variety <mask.png> <cells.png> [--light <mask-view.png>] <frame>...
                                         how varied the meadow is (reports/engine/2026-11-14-meadow-ground, «Что
                                         отняло жизнь»): the spread of the luminance (of its logarithm) and of the hue
                                         (degrees) between squares of 5 m, between squares of 0.5 m within one of 5 m,
                                         and between pixels within a square of 0.5 m; linear light, the mask's area.
                                         <cells.png> is the frame painted by `grass parts 8385`. The first frame is
                                         what the others are measured against: `variety` is the geometric mean of
                                         the four ratios of the two larger scales. --light takes the light out:
                                         each pixel is divided by the one of the painted frame (`grass parts 193`:
                                         one colour under the same sun and shade), so what is left is the ground's
                                         and the blades' own
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


# grassPaintFarBand of shader/grass/grass_common.glsl (`grass parts 512`, with the blades hidden 640): the hue of
# each band's paint, its half width and the least saturation, degrees. By the hue, not by the channels: the paint in
# shade takes some sky in its other channels, and sunlit straw passes a test of channels for the yellow
FAR_BANDS = {"0-10 m": (300, 22, 0.7), "10-24 m": (0, 14, 0.75), "24-48 m": (241, 10, 0.75), "48-64 m": (120, 20, 0.75),
             "64-100 m": (180, 14, 0.75), "100+ m": (60, 8, 0.88)}


def far_band_masks(rgb: np.ndarray) -> dict[str, np.ndarray]:
    x = rgb.astype(np.float64)
    top, low = x.max(axis=2), x.min(axis=2)
    span = top - low
    sat = span/np.maximum(top, 1.0)
    r, g, b = x[..., 0], x[..., 1], x[..., 2]
    c = np.maximum(span, 1e-6)
    hue = np.where(top == r, ((g - b)/c) % 6, np.where(top == g, (b - r)/c + 2, (r - g)/c + 4))*60
    out = {}
    for name, (centre, half, least) in FAR_BANDS.items():
        off = np.abs((hue - centre + 180) % 360 - 180)
        out[name] = (off <= half) & (sat >= least) & (top >= 6) & (span >= 5)
    return out


def cmd_farbands(a: argparse.Namespace) -> int:
    rows = {}
    for path in a.frames:
        rgb = read_image(path)
        total = rgb.shape[0]*rgb.shape[1]
        rows[path] = {name: {"pixels": int(m.sum()), "share": round(float(m.sum())/total, 4)}
                      for name, m in far_band_masks(rgb).items()}
    print(json.dumps(rows, ensure_ascii=False))
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


def label_runs(m: np.ndarray) -> np.ndarray:
    """Connected areas of a mask, by four neighbours: runs of each row joined to those of the row above."""
    h, w = m.shape
    parent: list[int] = [0]

    def find(i: int) -> int:
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i

    rows, prev = [], []
    for y in range(h):
        d = np.flatnonzero(np.diff(np.concatenate(([0], m[y].view(np.int8), [0]))))
        cur, k = [], 0
        for a, b in zip(d[0::2].tolist(), d[1::2].tolist()):
            lab = len(parent)
            parent.append(lab)
            while k < len(prev) and prev[k][1] <= a:
                k += 1
            j = k
            while j < len(prev) and prev[j][0] < b:
                ra, rb = find(prev[j][2]), find(lab)
                if ra != rb:
                    parent[max(ra, rb)] = min(ra, rb)
                j += 1
            cur.append((a, b, lab))
        rows.append(cur)
        prev = cur
    out = np.zeros((h, w), np.int32)
    for y, cur in enumerate(rows):
        for a, b, lab in cur:
            out[y, a:b] = find(lab)
    return out


def squares(painted: np.ndarray, mask: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """The squares of 0.5 m and of 5 m of the ground, numbered: each a chessboard in the paint, red and green."""
    p = painted.astype(np.int32)
    r, g, b = p[..., 0], p[..., 1], p[..., 2]
    ground = mask & (b > 24)
    out = []
    for bit in (2*r > b, 2*g > b):
        on, off = label_runs(erode(ground & bit, 1)), label_runs(erode(ground & ~bit, 1))
        off[off > 0] += int(on.max())
        out.append(on + off)
    return out[0], out[1]


def hue_of(lin: np.ndarray) -> np.ndarray:
    """The hue, degrees, of colours in linear light: of their coded values, as tone_of."""
    c = np.where(lin <= 0.0031308, lin*12.92, 1.055*np.maximum(lin, 1e-9)**(1/2.4) - 0.055)
    r, g, b = c[..., 0], c[..., 1], c[..., 2]
    top, low = c.max(axis=-1), c.min(axis=-1)
    span = np.maximum(top - low, 1e-9)
    h = np.where(top == r, ((g - b)/span) % 6.0, np.where(top == g, (b - r)/span + 2.0, (r - g)/span + 4.0))
    return h*60.0


def turn(deg: np.ndarray) -> np.ndarray:
    return (deg + 180.0) % 360.0 - 180.0


def spread(values: np.ndarray, weight: np.ndarray) -> float:
    return float(np.sqrt((weight*values**2).sum()/max(float(weight.sum()), 1e-9)))


COEF = np.array([0.2126, 0.7152, 0.0722])


def variety_of(rgb: np.ndarray, small: np.ndarray, big: np.ndarray, light: np.ndarray | None = None) -> dict:
    lin = linear(rgb)
    if light is not None:
        lin = lin/light[..., None]

    def means(labels: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
        flat = labels.ravel()
        n = np.bincount(flat)
        m = np.stack([np.bincount(flat, weights=lin[..., c].ravel()) for c in range(3)], axis=1)/np.maximum(n, 1)[:, None]
        n[0] = 0  # what is no square
        return n, m

    nb, mb = means(big)
    ns, ms = means(small)
    # squares of 5 m, against the mean of them all
    ok = nb >= 400
    w = nb[ok].astype(np.float64)
    ly, hb = np.log(np.maximum(mb @ COEF, 1e-6)), hue_of(mb)
    mean_h = np.degrees(np.arctan2((w*np.sin(np.radians(hb[ok]))).sum(), (w*np.cos(np.radians(hb[ok]))).sum()))
    out = {"squares_5m": int(ok.sum()), "lum_5m": spread(ly[ok] - (w*ly[ok]).sum()/w.sum(), w),
           "hue_5m": spread(turn(hb[ok] - mean_h), w)}
    # squares of 0.5 m, each against its square of 5 m: the one most of its pixels lie in
    both = (small > 0) & (big > 0)
    pair = small[both].astype(np.int64)*int(len(nb)) + big[both]
    pairs, count = np.unique(pair, return_counts=True)
    order = np.lexsort((count, pairs//len(nb)))
    last = np.flatnonzero(np.diff(np.concatenate((pairs[order]//len(nb), [-1]))))
    home = np.zeros(len(ns), np.int64)
    home[pairs[order][last]//len(nb)] = pairs[order][last] % len(nb)
    ok = (ns >= 12) & (home > 0) & (nb[home] >= 400)
    w = ns[ok].astype(np.float64)
    lys, hs = np.log(np.maximum(ms @ COEF, 1e-6)), hue_of(ms)
    out.update({"squares_05m": int(ok.sum()), "lum_05m": spread(lys[ok] - ly[home[ok]], w),
                "hue_05m": spread(turn(hs[ok] - hb[home[ok]]), w)})
    # pixels, each against its square of 0.5 m: blade to blade
    px = (small > 0) & ok[small]
    idx = small[px]
    one = np.ones(int(px.sum()))
    out.update({"lum_px": spread(np.log(np.maximum(lin[px] @ COEF, 1e-4)) - lys[idx], one),
                "hue_px": spread(turn(hue_of(lin[px]) - hs[idx]), one)})
    return {k: (v if isinstance(v, int) else round(v, 4)) for k, v in out.items()}


def cmd_variety(a: argparse.Namespace) -> int:
    mask = read_image(a.mask)[..., 0] > 127
    small, big = squares(read_image(a.cells), mask)
    # the painted ground is magenta: its red is the light that fell on it
    light = np.maximum(linear(read_image(a.light))[..., 0], 1e-3) if a.light else None
    first = None
    for path in a.frames:
        v = variety_of(read_image(path), small, big, light)
        if first is None:
            first = v
        keys = ("lum_5m", "hue_5m", "lum_05m", "hue_05m")
        v["ratios"] = {k: round(v[k]/max(first[k], 1e-9), 3) for k in keys + ("lum_px", "hue_px")}
        v["variety"] = round(float(np.prod([v["ratios"][k] for k in keys]))**0.25, 3)
        v["frame"] = path
        print(json.dumps(v, ensure_ascii=False))
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
    p = sub.add_parser("farbands"); p.add_argument("frames", nargs="+"); p.set_defaults(fn=cmd_farbands)
    p = sub.add_parser("blades"); p.add_argument("mask"); p.add_argument("paint"); p.add_argument("frame")
    p.add_argument("ground"); p.set_defaults(fn=cmd_blades)
    p = sub.add_parser("variety"); p.add_argument("mask"); p.add_argument("cells"); p.add_argument("--light"); p.add_argument("frames", nargs="+")
    p.set_defaults(fn=cmd_variety)
    p = sub.add_parser("columns"); p.add_argument("clip"); p.add_argument("--half", choices=["left", "right"])
    p.add_argument("--rows", default="0.45:0.95"); p.add_argument("--strips", type=int, default=8)
    p.add_argument("--step", type=float, default=0.5); p.set_defaults(fn=cmd_columns)
    p = sub.add_parser("still"); p.add_argument("clip"); p.add_argument("--half", choices=["left", "right"])
    p.add_argument("--crop", default="0:0:1:1"); p.set_defaults(fn=cmd_still)
    a = ap.parse_args()
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
