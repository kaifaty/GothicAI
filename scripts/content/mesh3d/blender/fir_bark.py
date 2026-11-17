"""The bark of a spruce built as geometry and photographed from above: scales, lichen, resin, moss at the foot.

    blender -b --factory-startup -P fir_bark.py -- --out DIR [--config fir.json] [--seed N] [--size 2048]

The sheet is the trunk unrolled, `bark_uv` of the config: its width goes once round the trunk, its height is
laid out as fir_tree.py reads it. The lower share (`root_share`) is the foot, used once: from the trunk's sunk end
through the ground line up the root flares. The upper share repeats up the trunk and along the branches, so it
tiles in height by itself and the whole sheet tiles in width.

- **Scales**: thin round plates in a jittered brick lattice, each a low dome with its lower edge lifted like a
  shingle; the lattice is periodic in the upper share and goes on into the foot, where the plates grow thicker
  and rougher. Dark cracks are the bottom between them.
- **Lichen**: grey-green crust in blotches — the plates under a blotch take its tone and carry small flakes.
- **Resin**: thin pale broken streaks that run down from a wound.
- **Moss**: cushions on one side of the foot, thickest at the ground line; nothing of the foot crosses into the
  upper share.

DIR/bark.npz holds the passes of fir_boughs.bake as float arrays, row 0 is the top of the sheet: colour (linear
albedo), normal (x right, y up the sheet, z to the viewer), ao, height in metres. No picture is used.
"""
from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path

import bpy
import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import fir_boughs  # noqa: E402


def linear(*srgb) -> np.ndarray:
    v = np.array(srgb, np.float64) / 255
    return np.where(v <= .04045, v / 12.92, ((v + .055) / 1.055) ** 2.4)


# Albedo. The intent sheet: cold grey-brown, darker than the rock (#5a4c3b), cracks #1f1a16.
CRACK = linear(31, 26, 22)
TONES = np.array([linear(86, 75, 66), linear(94, 74, 61), linear(104, 97, 89), linear(66, 57, 50)])
TONE_SHARE = (.56, .1, .18, .16)       # grey-brown, the red-brown of a fresh flake, weathered grey, dark
LICHEN = linear(112, 112, 97)
RESIN = linear(176, 162, 128)
MOSS = linear(62, 74, 36)              # the moss of the pilot's rocks
SOIL = linear(52, 44, 36)


def smooth(x):
    x = np.clip(x, 0., 1.)
    return x * x * (3 - 2 * x)


def domes(rng, centre, a, b, turn, thick, z0, tone, rough=.3, lift=.3, segments=9,
          shade=(1.06, 1., .82, .5), profile=(1., .95, .62, -.35)) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Low domes with a ragged rim: half-sizes a across and b along, turned, `thick` high above z0. The edge down
    the sheet is lifted by `lift` of the thickness, the upper one tucked in. Returns position, colour, triangles."""
    n, k = len(centre), segments
    theta = np.linspace(0, 2 * math.pi, k, endpoint=False)
    rim = rng.uniform(-1, 1, (n, k))
    rim = 1 + rough * (rim + .5 * np.roll(rim, 1, 1) + .5 * np.roll(rim, -1, 1)) / 1.4
    rings = np.array([.5, .82, 1.])
    spread = rings[None, :, None] * (1 + (rim[:, None, :] - 1) * rings[None, :, None])           # (n, 3, k)
    lx = a[:, None, None] * spread * np.cos(theta)
    ly = b[:, None, None] * spread * np.sin(theta)
    cos, sin = np.cos(turn)[:, None, None], np.sin(turn)[:, None, None]
    x = centre[:, 0, None, None] + lx * cos - ly * sin
    y = centre[:, 1, None, None] + lx * sin + ly * cos
    t = thick[:, None, None]
    z = z0[:, None, None] + t * (np.array(profile[1:])[None, :, None] + rng.uniform(-.12, .12, (n, 3, k))
                                 - lift * ly / b[:, None, None])
    ring = np.stack([x, y, z], axis=-1).reshape(n, 3 * k, 3)
    top = np.concatenate([centre, (z0 + thick * (profile[0] + rng.uniform(-.1, .1, n)))[:, None]], axis=1)
    position = np.concatenate([top[:, None], ring], axis=1)                                      # (n, 1 + 3k, 3)
    tint = np.concatenate([np.full((n, 1), shade[0]), np.repeat(np.array(shade[1:])[None], n, 0).repeat(k, axis=1)],
                          axis=1) * rng.uniform(.86, 1.14, (n, 1 + 3 * k))
    colour = tone[:, None, :] * tint[..., None]
    tris = [(0, 1 + s, 1 + (s + 1) % k) for s in range(k)]
    for r in range(2):
        for s in range(k):
            p, q = 1 + r * k + s, 1 + r * k + (s + 1) % k
            tris += [(p, p + k, q + k), (p, q + k, q)]
    index = np.array(tris)[None] + (np.arange(n) * (1 + 3 * k))[:, None, None]
    return position.reshape(-1, 3), colour.reshape(-1, 3), index.reshape(-1, 3)


def blotches(rng, count: int, width: float, low: float, high: float, period: float | None, radius=(.03, .09)):
    """A field of round blotches, periodic in width and, with `period`, in height."""
    centre = np.stack([rng.uniform(0, width, count), rng.uniform(low, high, count)], axis=1)
    size = rng.uniform(*radius, count)

    def field(point: np.ndarray) -> np.ndarray:
        dx = np.abs(point[:, None, 0] - centre[None, :, 0])
        dx = np.minimum(dx, width - dx)
        dy = np.abs(point[:, None, 1] - centre[None, :, 1])
        if period:
            dy = np.mod(dy, period)
            dy = np.minimum(dy, period - dy)
        return np.exp(-(dx * dx + dy * dy) / (size * size)[None]).max(axis=1)

    return field, centre, size


def build(config: dict, seed: int) -> dict:
    rng = np.random.default_rng(seed)
    uv = config['bark_uv']
    width, height = uv['width_m'], uv['height_m']
    foot = uv['root_share'] * height                # metres of the sheet's lower share
    period = height - foot
    ground = -height + config['trunk']['sunk']       # the ground line in the sheet's y
    position, colour, index, count = [], [], [], 0

    def push(part) -> None:
        nonlocal count
        position.append(part[0])
        colour.append(part[1])
        index.append(part[2] + count)
        count += len(part[0])

    def images(centre: np.ndarray, down: int = 1):
        """Every copy of a periodic site the camera can see: (index of the site, its place)."""
        out = []
        for kx in (-1, 0, 1):
            for ky in range(-down, 2):
                place = centre + np.array([kx * width, ky * period])
                seen = (place[:, 0] > -.12) & (place[:, 0] < width + .12) & (place[:, 1] > -height - .12) \
                    & (place[:, 1] < .12)
                out.append((np.flatnonzero(seen), place[seen]))
        return np.concatenate([o[0] for o in out]), np.concatenate([o[1] for o in out])

    def deep(y):
        """0 in the upper share, 1 from 0.4 m above the ground down: how much of the foot a place is."""
        return smooth((-period - .1 - y) / .7)

    # the bottom of the cracks
    plane = np.array([(-.3, .3, 0.), (width + .3, .3, 0.), (width + .3, -height - .3, 0.), (-.3, -height - .3, 0.)])
    push((plane, np.repeat(CRACK[None], 4, 0), np.array([(0, 3, 2), (0, 2, 1)])))

    # scales
    columns, rows = round(width / .034), round(period / .046)
    cell = np.array([width / columns, period / rows])
    grid = np.stack(np.meshgrid(np.arange(columns), np.arange(rows), indexing='ij'), axis=-1).reshape(-1, 2).astype(float)
    grid[:, 0] += .5 * (grid[:, 1] % 2)
    site = (grid + rng.uniform(-.42, .42, grid.shape)) * cell - np.array([0., period])
    sites = len(site)
    big = np.exp(rng.normal(0, .22, sites))
    a = cell[0] * rng.uniform(.62, .85, sites) * big
    b = cell[1] * rng.uniform(.7, 1.05, sites) * big
    turn = rng.uniform(-.45, .45, sites)
    thick = rng.uniform(.003, .007, sites)
    stack = rng.uniform(0, .5, sites)
    kind = rng.choice(len(TONES), sites, p=TONE_SHARE)
    tone = TONES[kind] * rng.uniform(.84, 1.16, (sites, 1)) * (1 + rng.uniform(-.03, .03, (sites, 3)))
    crust, crust_at, crust_size = blotches(rng, 34, width, -period, 0., period)
    crust_foot, foot_at, foot_size = blotches(rng, 22, width, -height + .5, -period - .15, None)

    which, place = images(site)
    f = deep(place[:, 1])
    lichen = np.clip(np.maximum(crust(place), crust_foot(place) * (place[:, 1] < -period)) * 1.5 - .45, 0, 1)
    lichen = (lichen * rng.uniform(.3, 1., len(place)))[:, None]
    damp = (.62 + .38 * smooth((place[:, 1] - ground) / .55))[:, None]
    tones = (tone[which] * (1 - lichen * .8) + LICHEN * lichen * .8) * damp
    soil = .5 * smooth((ground + .3 - place[:, 1]) / .4)[:, None]
    tones = tones * (1 - soil) + SOIL * soil
    grow = 1 + .3 * f
    plate = thick[which] * (1 + 1.6 * f)
    under = stack[which] * plate
    flat = dict(rough=.5, segments=11, profile=(1., .98, .84, -.4))
    push(domes(rng, place, a[which] * grow, b[which] * grow, turn[which], plate, under, tones, **flat))
    # chips: the thin upper layers of a plate that have not flaked off yet
    for share in (.62, .4):
        keep = rng.uniform(size=len(place)) < .7
        n = int(keep.sum())
        shift = rng.uniform(-.3, .3, (n, 2)) * np.stack([a[which], b[which]], axis=1)[keep]
        push(domes(rng, place[keep] + shift, a[which][keep] * grow[keep] * share * rng.uniform(.8, 1.2, n),
                   b[which][keep] * grow[keep] * share * rng.uniform(.8, 1.2, n), rng.uniform(-.6, .6, n),
                   plate[keep] * .3, under[keep] + plate[keep] * (.95 if share > .5 else 1.2),
                   tones[keep] * rng.uniform(.88, 1.14, (n, 1)), **flat))

    # flakes of lichen on the blotches
    def scatter(at, size):
        return np.concatenate([c + rng.normal(0, s * .55, (int(rng.integers(6, 16)), 2)) for c, s in zip(at, size)])

    flake = np.concatenate([images(scatter(crust_at, crust_size), down=0)[1], scatter(foot_at, foot_size)])
    flake = flake[(flake[:, 1] > -height + .45)]
    m = len(flake)
    r = rng.uniform(.006, .016, m)
    push(domes(rng, flake, r, r * rng.uniform(.7, 1.3, m), rng.uniform(0, 6.28, m), rng.uniform(.0015, .003, m),
               .0125 + .02 * deep(flake[:, 1]), LICHEN * rng.uniform(.8, 1.15, (m, 1)) * np.ones(3), rough=.45,
               lift=0., segments=7, shade=(1.05, 1., .9, .7)))

    # resin: broken streaks of drops running down the sheet
    drops, drop_size = [], []
    for start, long in [((rng.uniform(.1, width - .1), rng.uniform(-period - .5, -period - .15)), rng.uniform(.3, .6))
                        for _ in range(3)] + [((rng.uniform(.1, width - .1), rng.uniform(-period + .5, -.2)),
                                               rng.uniform(.18, .32)) for _ in range(2)]:
        x, y, run = start[0], start[1], 0.
        while run < long:
            half = rng.uniform(.012, .04) * (1 - .5 * run / long)
            drops.append((x + rng.normal(0, .002), y - run - half))
            drop_size.append((rng.uniform(.0035, .0065) * (1 - .4 * run / long), half))
            run += 2 * half + max(0., rng.normal(.008, .02))
            x += rng.normal(0, .004)
    drops, drop_size = np.array(drops), np.array(drop_size)
    m = len(drops)
    push(domes(rng, drops, drop_size[:, 0], drop_size[:, 1], rng.normal(0, .06, m), np.full(m, .003),
               .016 + .022 * deep(drops[:, 1]), RESIN * rng.uniform(.75, 1.1, (m, 1)) * np.ones(3), rough=.25,
               lift=-.2, segments=8, shade=(1.08, 1.04, .95, .8), profile=(1., .9, .6, 0.)))

    # moss: cushions on one side of the foot, thickest at the ground line, and specks on the other flares
    side = rng.uniform(.3, .7) * width
    cushions = []
    while len(cushions) < 1700:
        x, y = rng.normal(side, .26), rng.uniform(-height, -period - .32)
        up = (y - ground)
        chance = math.exp(-max(up, 0.) / .22) if up > 0 else 1.
        if rng.uniform() < chance and 0.02 < x < width - .02:
            cushions.append((x, y))
    for _ in range(160):
        cushions.append((rng.uniform(.02, width - .02), ground + rng.exponential(.07) - .1))
    cushions = np.array(cushions)
    m = len(cushions)
    r = rng.uniform(.007, .026, m) * (.6 + .4 * np.exp(-np.maximum(cushions[:, 1] - ground, 0) / .3))
    dry = rng.uniform(0, 1, (m, 1)) ** 3 * .5
    moss = (MOSS * (1 - dry) + linear(84, 74, 46) * dry) * rng.uniform(.6, 1.1, (m, 1))
    push(domes(rng, cushions, r, r * rng.uniform(.75, 1.25, m), rng.uniform(0, 6.28, m), r * rng.uniform(.3, .6, m),
               np.full(m, .03), moss, rough=.6, lift=0., segments=9, shade=(1.15, 1., .72, .4)))

    return dict(position=np.concatenate(position), colour=np.concatenate(colour), index=np.concatenate(index),
                report=dict(seed=seed, scales=int(len(place)), lattice=[columns, rows], lichen_flakes=int(len(flake)),
                            resin_drops=int(len(drops)), moss_cushions=int(m), ground_row_share=round(-ground / height, 3),
                            foot_share=uv['root_share'], size_m=[width, height]))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--config', type=Path, default=HERE / 'fir.json')
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--seed', type=int, default=5)
    parser.add_argument('--size', type=int, default=2048)
    parser.add_argument('--samples', type=int, default=32)
    parser.add_argument('--blend', action='store_true', help='keep the scene as DIR/bark.blend')
    args = parser.parse_args(sys.argv[sys.argv.index('--') + 1:])
    config = json.loads(args.config.read_text())
    args.out.mkdir(parents=True, exist_ok=True)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    built = build(config, args.seed)
    obj = fir_boughs.mesh('bark', built['position'], built['colour'], built['index'])
    obj.data.polygons.foreach_set('use_smooth', np.ones(len(built['index']), bool))
    width, height = built['report']['size_m']
    baked = fir_boughs.bake(obj, args.size, width, height, args.out, args.samples, (0., .08), ao_distance=.04,
                            transparent=False)
    if args.blend:
        bpy.ops.wm.save_as_mainfile(filepath=str(args.out / 'bark.blend'))
    np.savez_compressed(args.out / 'bark.npz', colour=baked['colour'][..., :3].astype(np.float16),
                        normal=(baked['normal'][..., :3] * 2 - 1).astype(np.float16),
                        ao=baked['ao'][..., 0].astype(np.float16),
                        height=(baked['height'][..., 0] * .08).astype(np.float16))
    report = built['report'] | dict(size=args.size, triangles=int(len(built['index'])))
    (args.out / 'bark.json').write_text(json.dumps(report, indent=1) + '\n')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
