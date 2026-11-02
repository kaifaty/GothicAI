#!/usr/bin/env python3
"""Tiles of the street lamp (FIREPLACE_NW_LAMP_01, asset city-hearth wave 3): its forged filigree and the tallow candle.

    uv run --no-project --with numpy --with pillow --with scipy python scripts/content/mesh3d/lamp_tiles.py TILES_DIR [--sheet PNG]

reports/content/remaster/city-hearth/lamp-design.md. The lace is the lamp's main detail: every one of its four
faces, a trapezoid 0.23 m wide at the bottom, 0.35 m at the top and 0.45 m tall, is filled edge to edge with
forged vine scrolls, an open lace about 60 % open. image_gen gives no clean alpha and no exact shape, so the lace
is drawn here as forged work is made: flat bar 9 mm wide bent into a stem, mirrored C and S scrolls ending in
curls, a few leaves, all riveted to a band along the face's edges. One tile is one face: u runs across the face's
bounding width (0.35 m), v up its height (0.45 m); lamp.py maps each face onto the same trapezoid, so the bottom
corners sit at u 0.171 and 0.829. Drawn at 10 px/mm and resampled to 1024 x 1024.

own_hearth_lace/   albedo.png (RGBA, alpha = iron), normal.png, roughness.png, metallic.png, ao.png, height.png
own_hearth_tallow/ the candle: off-white tallow, runs down its side, sooted near the wick; no alpha

The relief is designed, not stretched (prop-texture-relief): a bar is a hammered flat bar, 1.2 mm proud of the
face at its middle and rounded to its edges. Colour as the lamp's iron: near black, dull, a little blue in the scale.
"""
from __future__ import annotations

import argparse
import math
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw
from scipy.ndimage import gaussian_filter

W_MM, H_MM = 350.0, 450.0  # the face's bounding box
BOTTOM_MM = 230.0          # the face's bottom width
PX_MM = 10                 # drawing resolution
SIZE = 1024
BAR = 9.0                  # mm, flat bar of the scrolls: 6 mm fell under a pixel at the usual 4.5 m (r1)
STEM = 12.0                # mm, the middle stem
BAND = 12.0                # mm, the band along the edges the scrolls are riveted to


def half_width(y: float) -> float:
    return BOTTOM_MM / 2 + (W_MM - BOTTOM_MM) / 2 * y / H_MM


def px(p) -> tuple[float, float]:
    """mm in the face (x from the middle, y up from the bottom) to drawing pixels (row 0 at the top)."""
    return ((p[0] + W_MM / 2) * PX_MM, (H_MM - p[1]) * PX_MM)


def bezier(p0, p1, p2, n=24):
    t = np.linspace(0, 1, n)[:, None]
    return (1 - t) ** 2 * np.array(p0) + 2 * (1 - t) * t * np.array(p1) + t ** 2 * np.array(p2)


def spiral(c, r0, a0, turns, sense, n=60, shrink=0.82):
    """An inward curl round c from radius r0 at angle a0, turning `turns` times (sense +1 counter-clockwise)."""
    t = np.linspace(0, 1, n)
    r = r0 * (1 - shrink * t)
    a = a0 + sense * 2 * math.pi * turns * t
    return np.c_[c[0] + r * np.cos(a), c[1] + r * np.sin(a)]


def leaf(draw, at, direction, length, width):
    """A forged leaf: a pointed oval along direction (radians) from its stalk at `at`."""
    d = np.array([math.cos(direction), math.sin(direction)])
    n = np.array([-d[1], d[0]])
    s = np.linspace(0, 1, 16)
    bulge = np.sin(np.pi * s) ** 0.8 * width / 2
    left = np.array(at) + np.outer(s * length, d) + np.outer(bulge, n)
    right = np.array(at) + np.outer(s * length, d) - np.outer(bulge, n)
    draw.polygon([px(p) for p in np.vstack([left, right[::-1]])], fill=255)


def mirror(points, side):
    pts = np.array(points, dtype=float)
    pts[:, 0] *= side
    return pts


def leaf_m(draw, at, direction, length, width, side):
    """leaf() on the right half, mirrored onto the left for side -1."""
    if side < 0:
        at, direction = (-at[0], at[1]), math.pi - direction
    leaf(draw, at, direction, length, width)


def polyline(draw, points, width):
    pts = [px(p) for p in points]
    draw.line(pts, fill=255, width=int(round(width * PX_MM)), joint='curve')
    r = width * PX_MM / 2
    for x, y in (pts[0], pts[-1]):  # round ends, as a bar's cut end is hammered round
        draw.ellipse((x - r, y - r, x + r, y + r), fill=255)


def lace_mask(rng: np.random.Generator) -> np.ndarray:
    """The face's iron as a hi-res mask (1 = iron), mirrored about the stem with a little hand-made scatter."""
    w, h = int(W_MM * PX_MM), int(H_MM * PX_MM)
    image = Image.new('L', (w, h), 0)
    draw = ImageDraw.Draw(image)
    # the band round the trapezoid, a bar wide, just inside the frame the model gives it
    inset = BAND / 2 + 1.0
    edge = [(-half_width(0) + inset, inset), (half_width(0) - inset, inset), (half_width(H_MM) - inset, H_MM - inset),
            (-half_width(H_MM) + inset, H_MM - inset), (-half_width(0) + inset, inset)]
    polyline(draw, edge, BAND)
    polyline(draw, [(0, 0), (0, H_MM)], STEM)
    # each half (mirrored about the stem, scattered a little apart, as both are bent by hand): a big C-scroll
    # from the stem's foot round the outer side curling in, a big scroll from the middle of the stem curling
    # over toward the top corner, an S between them against the band, small curls off the stem and in the
    # corners, and leaves on the arms; mm, x out from the stem
    for side in (-1, 1):
        j = lambda: rng.uniform(-2.5, 2.5)
        def scroll(stem, ctrl, centre, r0, a0, turns, sense, bar=BAR):
            sp = spiral((centre[0] + j(), centre[1] + j()), r0, math.radians(a0), turns, sense)
            arm = bezier(stem, (ctrl[0] + j(), ctrl[1] + j()), tuple(sp[0])) if stem is not None else sp[:1]
            polyline(draw, mirror(np.vstack([arm, sp[1:]]), side), bar)
            return arm
        low = scroll((STEM / 2, 15), (105, 38), (62, 150), 38, 0, 1.6, 1)
        high = scroll((STEM / 2, 195), (55, 240), (104, 335), 42, 180, 1.6, -1)
        # the S: its lower curl from inside out, a spine, its upper curl in; the lower curl rests on the big C,
        # the upper one on the band (every piece of forged lace touches another, where it is riveted)
        a = spiral((90 + j() * 0.3, 196 + j() * 0.3), 18, math.radians(90), 1.4, 1)[::-1]
        b = spiral((121 + j() * 0.3, 268 + j() * 0.3), 20, math.radians(270), 1.4, 1)
        polyline(draw, mirror(np.vstack([a, bezier(tuple(a[-1]), (108, 246), tuple(b[0]))[1:-1], b]), side), BAR)
        scroll((STEM / 2, 70), (20, 62), (28, 95), 16, 270, 1.1, 1, BAR * 0.9)
        scroll((STEM / 2, 282), (15, 276), (28, 300), 15, 270, 1.1, 1, BAR * 0.9)
        scroll((STEM / 2, 430), (25, 440), (40, 410), 14, 90, 1.1, -1, BAR * 0.9)
        scroll(None, None, (101, 28), 11, 0, 1.0, 1, BAR * 0.9)
        scroll((95, 441), (150, 440), (132, 404), 17, 90, 1.3, 1, BAR * 0.9)  # hanging from the top band in the corner
        scroll(None, None, (114, 150), 12, 0, 1.2, 1, BAR * 0.85)  # against the band beside the big C
        scroll((70, 441), (60, 428), (76, 404), 12, 90, 1.2, -1, BAR * 0.85)  # under the top band by the stem
        spine = bezier(tuple(a[-1]), (108, 246), tuple(b[0]))
        for t, turn in ((0.35, 1.0), (0.7, -1.0)):
            at, nxt = spine[int(t * 23)], spine[int(t * 23) + 1]
            leaf_m(draw, at, math.atan2(nxt[1] - at[1], nxt[0] - at[0]) + turn, 16, 8, side)
        for arm, t, turn in ((low, 0.28, -0.9), (high, 0.45, 0.9), (low, 0.62, 0.9), (high, 0.78, -0.9)):
            at = arm[int(t * (len(arm) - 1))]
            nxt = arm[int(t * (len(arm) - 1)) + 1]
            ang = math.atan2(nxt[1] - at[1], nxt[0] - at[0]) + turn
            leaf_m(draw, at, ang, 20, 9, side)
        for y, ang in ((160, 0.75), (372, 0.7), (110, -0.6)):
            leaf_m(draw, (STEM / 2, y + j()), ang, 18, 8, side)
    # the stem ends in a turned knob under the top band and a trefoil at the bottom
    for y, size in ((H_MM - 30, 9), (30, 7)):
        x, yy = px((0, y))
        r = size * PX_MM / 2
        draw.ellipse((x - r, yy - r, x + r, yy + r), fill=255)
    mask = np.asarray(image, dtype=np.float32) / 255
    # outside the trapezoid there is no face: clear it, and keep the band's outer side straight
    yy, xx = np.mgrid[0:h, 0:w]
    xm = (xx + 0.5) / PX_MM - W_MM / 2
    ym = H_MM - (yy + 0.5) / PX_MM
    inside = np.abs(xm) <= half_width(ym)
    return mask * inside


def resample(a: np.ndarray, size=SIZE) -> np.ndarray:
    return np.asarray(Image.fromarray(a.astype(np.float32), 'F').resize((size, size), Image.Resampling.BOX))


def normal_from_height(h_mm: np.ndarray) -> np.ndarray:
    """Tangent-space normal, +Y up in the PNG (rows down), the tile's anisotropic texel in mm."""
    mx, my = W_MM / SIZE, H_MM / SIZE
    gx = (np.roll(h_mm, -1, 1) - np.roll(h_mm, 1, 1)) / (2 * mx)
    gr = (np.roll(h_mm, -1, 0) - np.roll(h_mm, 1, 0)) / (2 * my)
    n = np.stack((-gx, gr, np.ones_like(gx)), -1)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    return n * 0.5 + 0.5


def srgb(lin: np.ndarray) -> np.ndarray:
    c = np.clip(lin, 0, 1)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * np.power(c, 1 / 2.4) - 0.055)


def save(path: Path, data: np.ndarray, mode: str | None = None) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(np.rint(np.clip(data, 0, 1) * 255).astype(np.uint8), mode).save(path)


def lace_tile(rng: np.random.Generator) -> dict[str, np.ndarray]:
    hi = lace_mask(rng)
    # a hammered flat bar: proud in its middle, rounded to its edges (blur of the mask), 1.2 mm at most
    dome = gaussian_filter(hi, 1.6 * PX_MM) * hi
    dome = dome / max(float(dome.max()), 1e-6)
    alpha = resample(hi)
    h_mm = resample(np.sqrt(dome) * 1.2)
    hammer = gaussian_filter(rng.standard_normal((SIZE, SIZE)), 3.0)
    hammer /= hammer.std()
    h_mm = h_mm + 0.06 * hammer * (alpha > 0.1)
    # near black iron with blue-grey mill scale, a little lighter on the bars' crowns where hands and cloths rub
    crown = np.clip(h_mm / 1.2, 0, 1)
    base = np.array([0.017, 0.0185, 0.021])  # with the crowns, a mean near the lamp atlas's iron (0.02, 0.022, 0.025)
    albedo = base * (1 + 0.18 * hammer[..., None] * 0.5) * (1 + 0.55 * crown[..., None] ** 2)
    # bleed the colour into the transparent texels so mips and filtering do not pull the edges to black
    rgba = np.dstack([srgb(albedo), alpha])
    rough = 0.68 - 0.12 * crown
    print(f'own_hearth_lace: open {1 - float(alpha.mean()):.0%} of the bounding box, '
          f'{1 - float(alpha[trapezoid()].mean()):.0%} of the face')
    return dict(albedo=rgba, normal=normal_from_height(h_mm), roughness=rough, metallic=np.zeros_like(rough),
                ao=1 - 0.25 * (1 - crown) * (alpha > 0.5), height=h_mm / 1.3)


def trapezoid() -> np.ndarray:
    yy, xx = np.mgrid[0:SIZE, 0:SIZE]
    xm = (xx + 0.5) / SIZE * W_MM - W_MM / 2
    ym = H_MM - (yy + 0.5) / SIZE * H_MM
    return np.abs(xm) <= half_width(ym)


def tallow_tile(rng: np.random.Generator, size=512) -> dict[str, np.ndarray]:
    """Off-white tallow, the tile spans 0.1 m (lamp.py maps the candle in metres x 10): runs of melted tallow
    down the side (v up), a little soot near the top, waxy (roughness 0.45)."""
    runs = gaussian_filter(rng.standard_normal((size, size)), (40, 3), mode='wrap')
    runs /= runs.std()
    fine = gaussian_filter(rng.standard_normal((size, size)), 2, mode='wrap')
    h = np.clip(runs, 0, None) * 0.6 + 0.1 * fine
    base = np.array([0.62, 0.52, 0.33])  # linear: aged tallow, yellower than wax
    v = (np.arange(size)[::-1] / size)[:, None, None]  # 1 at the top row
    soot = np.clip((v - 0.85) / 0.15, 0, 1) * 0.5
    albedo = base * (0.92 + 0.08 * fine[..., None]) * (1 + 0.06 * runs[..., None]) * (1 - soot)
    return dict(albedo=srgb(albedo), normal=normal_tallow(h), roughness=0.45 - 0.08 * np.clip(runs, 0, 1),
                metallic=np.zeros((size, size)), ao=np.ones((size, size)), height=(h - h.min()) / (np.ptp(h) + 1e-9))


def normal_tallow(h_mm: np.ndarray, span_mm=100.0) -> np.ndarray:
    mm = span_mm / h_mm.shape[0]
    gx = (np.roll(h_mm, -1, 1) - np.roll(h_mm, 1, 1)) / (2 * mm)
    gr = (np.roll(h_mm, -1, 0) - np.roll(h_mm, 1, 0)) / (2 * mm)
    n = np.stack((-gx, gr, np.ones_like(gx)), -1)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    return n * 0.5 + 0.5


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('tiles', type=Path)
    parser.add_argument('--sheet', type=Path, help='the lace on grey, for a look')
    args = parser.parse_args()
    tiles = args.tiles.expanduser()
    for key, maps in (('own_hearth_lace', lace_tile(np.random.default_rng(23))),
                      ('own_hearth_tallow', tallow_tile(np.random.default_rng(24)))):
        for name, data in maps.items():
            save(tiles / key / f'{name}.png', data, 'RGBA' if data.ndim == 3 and data.shape[2] == 4 else None)
    if args.sheet:
        rgba = np.asarray(Image.open(tiles / 'own_hearth_lace' / 'albedo.png'), dtype=np.float32) / 255
        grey = np.full_like(rgba[..., :3], 0.55)
        a = rgba[..., 3:4]
        lit = np.clip(rgba[..., :3] * 6, 0, 1)  # brightened to see the bars
        save(args.sheet, lit * a + grey * (1 - a))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
