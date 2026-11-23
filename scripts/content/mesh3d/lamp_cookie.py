#!/usr/bin/env python3
"""Bake the light mask (cookie) of the street lamp: how much of the candle's light leaves the lantern each way.

    uv run --no-project --with numpy --with pillow python scripts/content/mesh3d/lamp_cookie.py LACE_ALBEDO_PNG OUT_PNG
        [--lamp scripts/content/mesh3d/blender/lamp.json] [--width 1024]

reports/content/remaster/city-hearth/lamp-design.md, variant 3 (the user's choice of 2026-11-02): the candle's light
leaves through the open filigree, so the lace falls as a patterned shadow on the cobbles and walls round the lamp.
The engine has no shadows from point lights by default; a cookie (a mask on the light, as games do for lanterns and
gobos) gives the same picture for nothing. The lantern is the frustum of lamp.json: four faces of lace between
frame bars, an opaque floor plate and roof. The flame is not a point: 1.2 cm wide and 3 cm tall, so a bar 15 cm away
throws a penumbra a few centimetres wide on the ground; the mask averages 128 points of the flame (24 left steps) (soft edges,
smeared up and down more than sideways, as a tall flame does).

The map is a latitude-longitude one round the light in the lamp's own axes (glTF: x right, y up, z back): u is the
azimuth atan2(z, x) / 2 pi + 0.5, v the angle from straight up / pi (row 0 up). White passes, black is iron. The
light stands at the flame's centre, 1.606 m over the ground (lamp.json: the candle's top 1.58 m, the wick 1.1 cm).
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image

LIGHT_Y = 1.606   # m over the lamp's origin: the flame's centre, the fire tree's light (vobtrees.d/FIRETREE_LAMP.json)
FLAME = (0.006, 0.015)  # half width and half height of the flame, m
CORNER = 0.006    # m of a corner bar inside the face's edge (lamp.json frame width / 2)
INSET = 0.004     # the lace's plane inside the frame (lamp.py)


def bilinear(img: np.ndarray, u: np.ndarray, v: np.ndarray) -> np.ndarray:
    h, w = img.shape
    x = np.clip(u * w - 0.5, 0, w - 1.001)
    y = np.clip((1 - v) * h - 0.5, 0, h - 1.001)  # v up the face, rows down
    x0, y0 = x.astype(int), y.astype(int)
    fx, fy = x - x0, y - y0
    return (img[y0, x0] * (1 - fx) * (1 - fy) + img[y0, x0 + 1] * fx * (1 - fy)
            + img[y0 + 1, x0] * (1 - fx) * fy + img[y0 + 1, x0 + 1] * fx * fy)


def transmission(d: np.ndarray, p: np.ndarray, body, ft: float, alpha: np.ndarray) -> np.ndarray:
    """Share of light leaving along unit directions d (N x 3) from the point p (3,) inside the lantern."""
    y0, y1, h0, h1 = body
    h0, h1 = h0 - INSET, h1 - INSET
    slope = (h1 - h0) / (y1 - y0)
    a = h0 - slope * y0
    dx, dy, dz = d[:, 0], d[:, 1], d[:, 2]
    best = np.full(len(d), np.inf)
    face = np.full(len(d), -1)
    # the four sides: +x, -x, +z, -z, each the plane |coordinate| = a + slope * y
    for k, (axis, sign) in enumerate(((0, 1), (0, -1), (2, 1), (2, -1))):
        denom = sign * d[:, axis] - slope * dy
        with np.errstate(divide='ignore', invalid='ignore'):
            t = (a + slope * p[1] - sign * p[axis]) / denom
        ok = (denom > 1e-9) & (t > 0) & (t < best)
        best = np.where(ok, t, best)
        face = np.where(ok, k, face)
    with np.errstate(divide='ignore', invalid='ignore'):
        for plane, k in ((y1, 4), (y0, 5)):  # roof and floor
            t = (plane - p[1]) / dy
            ok = (t > 0) & (t < best)
            best = np.where(ok, t, best)
            face = np.where(ok, k, face)
    hit = p[None, :] + best[:, None] * d
    side = face < 4
    across = np.where(face < 2, hit[:, 2], hit[:, 0])  # along the face's width
    half = a + slope * hit[:, 1]
    v = (hit[:, 1] - (y0 + ft)) / ((y1 - ft) - (y0 + ft))
    ub = 0.5 - h0 / (2 * h1)
    span = 0.5 - ub * (1 - v)
    u = 0.5 + across / np.maximum(half, 1e-6) * span
    lace = 1 - bilinear(alpha, u, np.clip(v, 0, 1))
    frame = (v < 0) | (v > 1) | (np.abs(across) > half - CORNER)
    return np.where(side & ~frame, lace, 0.0)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('lace', type=Path)
    parser.add_argument('out', type=Path)
    parser.add_argument('--lamp', type=Path, default=Path(__file__).resolve().parent / 'blender/lamp.json')
    parser.add_argument('--width', type=int, default=1024)
    args = parser.parse_args()
    cfg = json.loads(args.lamp.read_text())
    body, ft = cfg['body'], cfg['frame'][1]
    alpha = np.asarray(Image.open(args.lace).convert('RGBA'), dtype=np.float32)[..., 3] / 255
    w, h = args.width, args.width // 2
    u = (np.arange(w) + 0.5) / w
    v = (np.arange(h) + 0.5) / h
    phi = (u - 0.5) * 2 * np.pi
    theta = v * np.pi
    pp, tt = np.meshgrid(phi, theta)
    d = np.stack([np.cos(pp) * np.sin(tt), np.cos(tt), np.sin(pp) * np.sin(tt)], -1).reshape(-1, 3)
    rng = np.random.default_rng(5)
    points = []
    while len(points) < 128:  # stratified-ish points in the flame's ellipse (x-z disc of the width, y the height)
        q = rng.uniform(-1, 1, 3)
        if (q ** 2).sum() <= 1:
            points.append(np.array([q[0] * FLAME[0], LIGHT_Y + q[1] * FLAME[1], q[2] * FLAME[0]]))
    mask = np.zeros(len(d))
    for p in points:
        mask += transmission(d, p, body, ft, alpha)
    mask = (mask / len(points)).reshape(h, w)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(np.rint(np.clip(mask, 0, 1) * 255).astype(np.uint8)).save(args.out)
    print(f'{args.out}: {w}x{h}, light passes {float(mask.mean()):.0%} over the sphere, '
          f'{float(mask[h // 2:].mean()):.0%} below the horizon')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
