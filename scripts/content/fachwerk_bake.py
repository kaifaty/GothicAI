#!/usr/bin/env python3
"""The timber-framed upper storey of the lower town's houses (asset market-house) built as a Blender
model and baked into textures, on the shared piece_bake path (docs/remaster-pipeline.md, «Материал из
модели»).

    python3 scripts/content/fachwerk_bake.py layout wall|window|post|gable OUT.json [--seed N]
    python3 scripts/content/fachwerk_bake.py render OUT.json OUTDIR [--street] [--sun E]
    python3 scripts/content/fachwerk_bake.py compose OUT.json OUTDIR/passes.exr MAPS.png --plaster PNG --wood PNG
    python3 scripts/content/fachwerk_bake.py blockout OUT.json OUTDIR/passes.exr OUT.png

wall is NW_CITY_WALL_03: a plastered field with half a post on each edge, the head and sole rails and
two braces from the lower corners. window is NW_CITY_WINDOW_03: the same field with a head rail across,
two jambs and a sill rail round a closed two-leaf shutter of upright boards on strap hinges, braces
under the sill. post is NW_CITY_WALL_04: a field with one post a little left of the middle. gable is
NW_CITY_WALL_05: the top of the gable, a king post and two braces down to the tie beam.

The timber is hewn oak standing out of the plaster, with its grain along each member, a shrinkage gap
where the plaster pulled away and pegs at the joints; the plaster is a hand-smoothed lime render. Its
relief is designed for the normal map (two scales: the members and the plaster's undulation, then the
grain and trowel marks), as docs/remaster-pipeline.md asks.

Positions come from the originals measured by Claude at their own size and on a 4x view
(original_view.py; u from the left, v from the top). No original pixel goes into the model or a
generator; the surfaces are image_gen pictures sampled in compose (--plaster, --wood).
"""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

import numpy as np

import piece_bake as pb
import timber_bake as tb

STEP = .0025                 # the grid of heights, metres
# The texture squares on the houses, metres (market-house measurement, city-materials.csv)
PERIODS = dict(wall=(2.41, 2.15), window=(1.93, 2.14), post=(3.83, 2.64), gable=(3.03, 1.30))
PROUD = .028                 # the timber stands out of the plaster face
BRACE_DROP = .004            # a brace lies a little behind the post and rails it is housed into
# Members as (kind, (u0, v0), (u1, v1), width in metres); a post or rail on the square's edge is a
# half member, its other half on the next square.
RAIL_TOP, RAIL_BOTTOM = (0., .078), (.935, 1.)
MEMBERS = {
    'wall': [('brace', (.035, .60), (.33, .955), .14), ('brace', (.965, .62), (.67, .955), .14),
             ('post', (0., 0.), (0., 1.), .21), ('rail', (0., .039), (1., .039), .17),
             ('rail', (0., .967), (1., .967), .145)],
    'window': [('brace', (.035, .73), (.33, .955), .13), ('brace', (.965, .73), (.67, .955), .13),
               ('post', (0., 0.), (0., 1.), .21), ('rail', (0., .039), (1., .039), .17),
               ('rail', (0., .967), (1., .967), .145), ('head', (0., .15), (1., .15), .17),
               ('jamb', (.175, .19), (.175, .63), .135), ('jamb', (.805, .19), (.805, .63), .135),
               ('sill', (0., .675), (1., .675), .19)],
    'post': [('post', (.46, 0.), (.46, 1.), .22), ('rail', (0., .039), (1., .039), .2),
             ('rail', (0., .97), (1., .97), .16)],
    'gable': [('brace', (.17, .90), (.47, .12), .2), ('brace', (.83, .90), (.53, .12), .2),
              ('post', (.50, 0.), (.50, 1.), .2), ('rail', (0., .935), (1., .935), .17)],
}
SHUTTER = dict(rect=(.21, .79, .19, .63), boards=5, gap=.007, z=.006, straps=(.25, .57), strap=(.028, .004))
GAP = (.004, .010, .4)       # the plaster's shrinkage gap at the timber: width, depth, share of the edge
PEG = (.013, .003)           # oak pegs at the joints: radius, how far they stand out
PLASTER_RELIEF = ((.5, .004), (.12, .0022), (.03, .0008))   # median tilt about 5°   # undulation of the render: size, height
SPALLS = (0, (.02, .07), .003)    # flaked whitewash as dips: count per m², radius, depth; none now, the
                                  # plaster picture has its own flakes and gives them relief in compose
WOOD_STYLE = dict(adze=tb.STROKE, grain=(.0018, (.003, .009)), pits=((.006, .05), .005, .09, .12),
                  arris=(.018, .012), cup=0.)   # median normal tilt about 8°: the frame beam timber-7 is 6.8°, the hut boards 9°
KIND_IDS = dict(plaster=0, post=1, rail=1, head=1, sill=1, jamb=1, brace=2, shutter=3, iron=4, peg=5)


def _grid(period):
    px, py = period
    n, m = round(px / STEP), round(py / STEP)
    gx, gy = np.meshgrid(np.arange(n + 1) * STEP, np.arange(m + 1) * STEP)
    return gx, gy


def _wrap(field):
    field[-1, :] = field[0, :]
    field[:, -1] = field[:, 0]
    return field


def _local(gx, gy, period, member):
    """Distance along and across a member (metres) for every grid point, the square wrapping in u."""
    px, py = period
    _, (u0, v0), (u1, v1), _ = member
    x0, y0, x1, y1 = u0 * px, (1 - v0) * py, u1 * px, (1 - v1) * py
    dx, dy = x1 - x0, y1 - y0
    length = math.hypot(dx, dy)
    tx, ty = dx / length, dy / length
    rx = (gx - x0 + px / 2) % px - px / 2 if dx == 0 else gx - x0   # an upright member wraps across u
    ry = gy - y0
    return rx * tx + ry * ty, -rx * ty + ry * tx, length


def fachwerk(seed: int, variant: str):
    """Layout of one of the upper storey's textures: (spec, points, grids, labels, members)."""
    rng = np.random.default_rng([seed, 31, list(PERIODS).index(variant)])
    period = PERIODS[variant]
    px, py = period
    gx, gy = _grid(period)
    # the plaster: a hand-smoothed render, undulating at two scales and grained by the trowel
    height = sum(h * pb.noise_metres(rng, gx.shape, period, size) for size, h in PLASTER_RELIEF)
    labels = np.zeros(gx.shape, int)                 # 0: plaster, 1: flaked whitewash, then the parts
    kinds = ['plaster', 'spall']
    ragged = pb.noise_metres(rng, gx.shape, period, .025)
    for _ in range(round(SPALLS[0] * px * py)):      # flaked whitewash: shallow dips with torn edges
        cx, cy, r = rng.uniform(0, px), rng.uniform(0, py), rng.uniform(*SPALLS[1])
        d = np.hypot((gx - cx + px / 2) % px - px / 2, (gy - cy + py / 2) % py - py / 2) / r
        d *= np.clip(1 + .45 * ragged, .5, None)
        dip = np.clip(3 - d * 3, 0, 1)
        height -= SPALLS[2] * dip
        labels[dip > .5] = 1
    # the braces last: housed into the posts and rails, their ends cut to those members' faces
    members = sorted(MEMBERS[variant], key=lambda m: m[0] == 'brace')
    timber = np.zeros(gx.shape, bool)
    for index, member in enumerate(members):
        kind, _, _, width = member
        along, across, length = _local(gx, gy, period, member)
        # hand-built: rails and heads sag a little between their posts, posts and jambs lean
        t = np.clip(along / max(length, .01), 0, 1)
        if kind in ('rail', 'head', 'sill') and length < px * 1.5:
            across = across + rng.uniform(.004, .016) * np.sin(math.pi * t) * (1 if kind != 'rail' or member[1][1] > .5 else .6)
        elif kind in ('post', 'jamb'):
            across = across + rng.normal(0, .006) * (t - .5) * 2
        ends = width                                  # posts and rails run on past the square's edge
        inside = (np.abs(across) < width / 2) & (along > -ends) & (along < length + ends)
        if kind == 'brace':                           # run into the post and rail and cut flush to their
            from scipy.ndimage import label as _label  # faces: only the bay the brace stands in
            bays, _ = _label(~timber)
            mid = bays[round((1 - (member[1][1] + member[2][1]) / 2) * (gy.shape[0] - 1)),
                       round((member[1][0] + member[2][0]) / 2 * (gx.shape[1] - 1))]
            inside &= bays == mid
        w = width + .02 * rng.normal()
        face = tb._hewn(rng, np.clip(across + w / 2, 0, w), along % max(length, .01), w, max(length, .01),
                        (STEP, STEP), **WOOD_STYLE)
        z = PROUD + rng.normal(0, .003) + face + .006 * np.sin(math.pi * np.clip(along / max(length, .01), 0, 1)) * \
            rng.uniform(-1, 1) - (BRACE_DROP if kind == 'brace' else 0.)
        take = inside & (z > height) if kind != 'brace' else inside
        height[take] = z[take]
        labels[take] = len(kinds)
        kinds.append(kind)
        timber |= take
    # the shrinkage gap where the plaster pulled away from the timber, along part of every edge
    from scipy.ndimage import distance_transform_edt
    dist = distance_transform_edt(~timber) * STEP
    along_edge = pb.noise_metres(rng, gx.shape, period, .25)
    broken = along_edge > np.quantile(along_edge, 1 - GAP[2])
    gap = (dist > 0) & (dist < GAP[0] * (1 + .25 * pb.noise_metres(rng, gx.shape, period, .2))) & broken
    height[gap] = np.minimum(height[gap], -GAP[1])
    # pegs at the brace ends
    for member in members:
        if member[0] != 'brace':
            continue
        _, (u0, v0), (u1, v1), _ = member
        for (u, v), toward in (((u0, v0), (u1, v1)), ((u1, v1), (u0, v0))):
            t = .1
            cx = (u + (toward[0] - u) * t) * px
            cy = (1 - (v + (toward[1] - v) * t)) * py
            d = np.hypot((gx - cx + px / 2) % px - px / 2, gy - cy) / PEG[0]
            peg = (d < 1) & timber
            height[peg] = np.maximum(height[peg], PROUD + PEG[1] * np.sqrt(np.clip(1 - d[peg] ** 2, 0, 1)) + .002)
            labels[peg] = len(kinds)
            kinds.append('peg')
    if variant == 'window':
        _shutter(rng, gx, gy, period, height, labels, kinds)
    for field in (height, labels):
        _wrap(field)
    pieces = [pb.grid_piece(0, (0., 0.), (STEP, STEP), [0, 0, 0], 0, [0., 0., 0.],
                            copy_offsets=[[dx, dy] for dx in (-px, 0, px) for dy in (-py, 0, py)], kind='wall')]
    spec = dict(tool='scripts/content/fachwerk_bake.py', texture=TEXTURES[variant], wall=variant, seed=seed,
                period=list(period), camera=dict(tilt=0.), sky=dict(pitch=0., ground=.08),
                ao=dict(distance=.05, crevice=.010),
                surfaces=[dict(name='wall', colour=[.30, .29, .25], metres=1., bump=0.)],
                parts=kinds, members=[[m[0], list(m[1]), list(m[2]), m[3]] for m in members], pieces=pieces)
    return spec, {}, {0: height}, labels


TEXTURES = dict(wall='NW_CITY_WALL_03.TGA', window='NW_CITY_WINDOW_03.TGA', post='NW_CITY_WALL_04.TGA',
                gable='NW_CITY_WALL_05.TGA')


def _shutter(rng, gx, gy, period, height, labels, kinds):
    """The closed two-leaf shutter of upright boards between the jambs, with strap hinges."""
    px, py = period
    u0, u1, v0, v1 = SHUTTER['rect']
    x0, x1, y0, y1 = u0 * px, u1 * px, (1 - v1) * py, (1 - v0) * py
    inside = (gx > x0) & (gx < x1) & (gy > y0) & (gy < y1)
    height[inside] = -.015                           # the dark behind the boards' gaps
    labels[inside] = len(kinds)
    kinds.append('dark')
    middle = (x0 + x1) / 2
    split = np.abs(gx - middle) < .008                  # the two leaves meet with a clear dark line
    for leaf, (a, b) in enumerate(((x0, middle), (middle, x1))):
        widths = rng.uniform(.82, 1.18, SHUTTER['boards'])     # boards of the widths a plank gave
        edges = a + np.concatenate([[0], np.cumsum(widths)]) / widths.sum() * (b - a)
        for k in range(SHUTTER['boards']):
            bx0 = edges[k] + SHUTTER['gap'] / 2
            bx1 = edges[k + 1] - SHUTTER['gap'] / 2
            top, foot = y1 - rng.uniform(0, .012), y0 + rng.uniform(0, .012)   # uneven ends
            board = inside & (gx > bx0) & (gx < bx1) & (gy < top) & (gy > foot) & ~split
            local = np.clip(gx - bx0, 0, bx1 - bx0)
            style = dict(adze=None, grain=(.0010, (.002, .007)), pits=((.005, .05), .002, .08, .12),
                         arris=(.006, .004), cup=.003)
            face = tb._hewn(rng, local, gy - y0, bx1 - bx0, y1 - y0, (STEP, STEP), **style)
            z = SHUTTER['z'] + face + rng.normal(0, .0015) + rng.normal(0, .01) * (gx - (bx0 + bx1) / 2)
            height[board] = z[board]
            labels[board] = len(kinds)
            kinds.append('shutter')
        # strap hinges from the outer edge two thirds across the leaf, nailed
        width_s, thick = SHUTTER['strap']
        outer = a if leaf == 0 else b
        for v in SHUTTER['straps']:
            sy = (1 - v) * py
            reach = (b - a) * .78
            span = (gx > min(outer, outer + (reach if leaf == 0 else -reach))) & \
                (gx < max(outer, outer + (reach if leaf == 0 else -reach)))
            taper = 1 - .4 * np.clip(np.abs(gx - outer) / reach, 0, 1)
            strap = inside & span & (np.abs(gy - sy) < width_s / 2 * taper)
            height[strap] = np.maximum(height[strap], SHUTTER['z'] + thick + .002)
            labels[strap] = len(kinds)
            kinds.append('iron')


# ---------------------------------------------------------------- weathering (compose)
# Targets: the original's plaster as measured, dirty like it (the user, 2026-10-23: clean white walls
# belong to the upper quarter), the oak as accepted on house-2, the shutter as the original's boards.
PLASTER = (159, 158, 134)
PLASTER_HUE = ((52, 50, 24), (107, 106, 69), (138, 137, 107), (163, 163, 139), (186, 186, 168), (212, 213, 200))
DETAIL = (.02, .5, 1., .004)          # the scan's grain: split at this size (metres), the picture's own fine
                                       # light kept to this power, the scan's to this, its relief metres per unit
SPREAD_FIT = .75                       # how far the picture's light goes to the original's quantiles
OWN_HUE = .5                           # how much of the plaster picture's own hue stays over the original's by light
PLASTER_SPREAD = ((0, 55), (5, 83), (10, 108), (25, 137), (50, 160), (75, 178), (90, 193), (95, 202),
                  (99, 220), (100, 232))       # the original's plaster luminance (sRGB) by percentile

OAK = (41, 34, 20)
OAK_SPREAD = (31, 56)                  # the original timber's sRGB luminance: median, p90
GRAIN = 2.8                            # power on the oak picture's light: stripier grain
EDGE = (.03, .004, 2.8, .3)          # worn arris: reach, the dark rim's width (metres), lightening; the rim's factor
SHUTTER_TONE = (63, 58, 47)
IRON = (35, 31, 23)
PLASTER_RELIEF_PX = (.0008, .012)      # the picture's flakes and grain as relief: metres per unit of its light, blur in metres
MOULD = (.06, .35)                    # black-green mould hanging from each rail: reach in metres, strength
MOULD_TONE = (48, 52, 28)
RIM = (.025, .3)                       # grime at the timber: reach in metres, darkening
GREEN = ((92, 100, 70), .12, .06)      # a faint green film above the sole rail: colour, reach, share
OCCLUSION = (1.6, 1.5)
ROUGH = dict(plaster=.93, wood=.88, iron=.72)


def _linear(srgb) -> np.ndarray:
    return (np.asarray(srgb, dtype=np.float64) / 255) ** 2.2


def _plaster_frame(spec, labels):
    """The rectangle (x0, x1, y0, y1, metres) the plaster picture spans: from the faces of the timbers
    round the plaster, the whole width where the plaster runs on over the square's side edges."""
    px, py = spec['period']
    plaster = np.isin(np.array(spec['parts'])[labels], ['plaster', 'spall'])
    rows = np.where(plaster.any(axis=1))[0]
    cols = np.where(plaster.any(axis=0))[0]
    n, m = labels.shape[1] - 1, labels.shape[0] - 1
    y0, y1 = rows[0] / m * py, rows[-1] / m * py
    if plaster[:, 0].any() and plaster[:, -1].any():
        return 0., px, y0, y1
    return cols[0] / n * px, cols[-1] / n * px, y0, y1


def _sample(image: np.ndarray, a, b, metres) -> np.ndarray:
    """An image tiled every METRES (one length, or across and down), read at a (across) and b (along), bilinear."""
    h, w = image.shape[:2]
    mx, my = (metres, metres) if np.isscalar(metres) else metres
    fx = (a / mx % 1) * w
    fy = (b / my % 1) * h
    x0, y0 = np.floor(fx).astype(int) % w, np.floor(fy).astype(int) % h
    x1, y1 = (x0 + 1) % w, (y0 + 1) % h
    tx, ty = (fx - np.floor(fx))[..., None], (fy - np.floor(fy))[..., None]
    return (image[y0, x0] * (1 - tx) * (1 - ty) + image[y0, x1] * tx * (1 - ty) +
            image[y1, x0] * (1 - tx) * ty + image[y1, x1] * tx * ty)


def compose(passes: dict, spec: dict, labels: np.ndarray, plaster_img: np.ndarray, wood_img: np.ndarray,
            seed: int = 0, detail: dict | None = None) -> dict:
    """Albedo (linear RGB) and masks of one of the upper storey's textures."""
    rng = np.random.default_rng([seed, 37])
    rows, cols = passes['piece'].shape
    period = spec['period']
    px, py = period
    x, y = passes['x'] % px, passes['y'] % py
    v = 1 - y / py
    n = labels.shape[1] - 1
    part = labels[np.rint(y / STEP).astype(int) % (labels.shape[0] - 1), np.rint(x / STEP).astype(int) % n]
    kinds = np.array(spec['parts'])[part]
    spall = (kinds == 'spall').astype(float)
    plaster = np.isin(kinds, ['plaster', 'spall'])
    shutter = kinds == 'shutter'
    iron = kinds == 'iron'
    wood = np.isin(kinds, ['post', 'rail', 'head', 'sill', 'jamb', 'brace', 'peg'])
    out = np.zeros((rows, cols, 3))
    # the plaster: one picture of old dirty whitewash over the whole square, drawn for this square's
    # timbers (mould under the rails, grime along the posts), so nothing repeats inside it
    x0, x1, y0, y1 = _plaster_frame(spec, labels)
    pl = _sample(plaster_img, x - x0, y1 - y, (x1 - x0, y1 - y0)) ** 2.2   # its edges at the timbers' faces; linear
    out[plaster] = pl[plaster]
    if detail is not None:
        # frequency split: the picture keeps the broad stains, a photographed plaster scan gives the
        # grain at its true scale (its millimetres per pixel), multiplied in linear light
        from scipy.ndimage import gaussian_filter
        sigma = DETAIL[0] / (py / rows)
        weight = gaussian_filter(plaster.astype(float), sigma, mode='wrap') + 1e-6
        low = np.stack([gaussian_filter(out[..., k] * plaster, sigma, mode='wrap') for k in range(3)], -1) / weight[..., None]
        own = (out @ tb.LUM) / np.maximum(low @ tb.LUM, 1e-6)
        scan = _sample(detail['albedo'], x, py - y, detail['metres']) ** 2.2 @ tb.LUM
        grain = scan / np.maximum(gaussian_filter(scan, sigma, mode='wrap'), 1e-6)
        mix = low * (own ** DETAIL[1] * grain ** DETAIL[2])[..., None]
        out[plaster] = mix[plaster]
    # the oak: its picture read along each member's grain; every member a little its own tone
    members = spec['members']
    member_ids = [i for i, k in enumerate(spec['parts']) if k in ('post', 'rail', 'head', 'sill', 'jamb', 'brace')]
    for index, member in enumerate(members):
        mask = part == member_ids[index]
        if not mask.any():
            continue
        along, across, _ = _local(x, y, period, (member[0], tuple(member[1]), tuple(member[2]), member[3]))
        out[mask] = _sample(wood_img, across[mask] + index * .37, along[mask], 1.) * math.exp(rng.normal(0, .2))
    pegs = kinds == 'peg'
    out[pegs] = _sample(wood_img, x[pegs] * 3, y[pegs] * 3, 1.) * .8          # end grain, darker
    if shutter.any():
        ids = np.unique(part[shutter])
        for i in ids:
            mask = part == i
            out[mask] = _sample(wood_img, x[mask] + i * .21, y[mask], 1.) * math.exp(rng.normal(0, .15))
    out[iron] = 1.
    dark = kinds == 'dark'
    lum = out @ tb.LUM
    for mask in (plaster, wood, shutter):            # each part's own light around 1
        if mask.any():
            out[mask] /= np.median(lum[mask])
    out[wood | shutter] = (out[wood | shutter] @ tb.LUM)[:, None] ** GRAIN * np.array([1., .86, .62])   # grain, olive brown
    # the oak's tone spreads like the original's (luminance cv .66 against .28 without this): worn light
    # arrises along every member and a dark line where it goes into the plaster, on both sides alike
    from scipy.ndimage import distance_transform_edt
    inner = distance_transform_edt(wood) * (py / rows)
    worn = np.clip(1 - inner / EDGE[0], 0, 1) * (inner > EDGE[1]) * wood
    worn *= np.clip(.6 + .6 * pb.noise_metres(rng, (rows, cols), period, .08), 0, 1)
    out *= (1 + EDGE[2] * worn)[..., None]
    out[wood & (inner <= EDGE[1])] *= EDGE[3]

    # plaster weathering by form, over the picture's own: black-green mould hanging from each rail,
    # grime gathered at the timber, a faint green film above the sole rail
    timber_px = ~plaster & ~(kinds == 'shutter')
    r_idx = np.arange(rows)[:, None] * np.ones((1, cols))
    last = np.maximum.accumulate(np.where(timber_px, r_idx, -1e6), axis=0)
    rails_above = (r_idx - last) * (py / rows)       # how far the nearest timber straight above is
    m_reach, m_depth = MOULD
    m_ragged = m_reach * (1 + .6 * pb.noise_metres(rng, (rows, cols), period, .05))
    mould = np.clip(1 - rails_above / np.maximum(m_ragged, .01), 0, 1) ** 1.2 * plaster
    mould *= np.clip(.8 + .6 * pb.noise_metres(rng, (rows, cols), period, .03), 0, 1)
    out = out * (1 - m_depth * mould[..., None]) + _linear(MOULD_TONE) / _linear(PLASTER) * m_depth * mould[..., None]
    nearest = distance_transform_edt(plaster) * (py / rows)
    rim = np.clip(1 - nearest / RIM[0], 0, 1) ** 2 * plaster
    out *= (1 - RIM[1] * rim)[..., None]
    green_srgb, g_reach, g_share = GREEN
    nxt = np.minimum.accumulate(np.where(timber_px, r_idx, 1e6)[::-1], axis=0)[::-1]
    rails_below = (nxt - r_idx) * (py / rows)
    film = np.clip(1 - rails_below / g_reach, 0, 1) * plaster
    film *= np.clip(pb.noise_metres(rng, (rows, cols), period, .05) + 1 - g_share * 8, 0, 1)
    tint = _linear(green_srgb) / _linear(PLASTER)
    out = out * (1 - .5 * film[..., None]) + out * tint * .5 * film[..., None]

    occlusion = np.clip(passes['ao'], 0, 1) ** OCCLUSION[0] * np.clip(passes['crevice'], 0, 1) ** OCCLUSION[1]
    out *= occlusion[..., None]
    shade = pb.overhang_shade(passes['height'], py / rows, (30., 40., 55.), .04, softness=.004)
    out *= (1 - .45 * shade)[..., None]

    out[dark] = _linear((12, 10, 8))
    for mask, srgb in ((plaster, PLASTER), (wood, OAK), (shutter, SHUTTER_TONE), (iron, IRON)):
        if mask.any():
            out[mask] *= _linear(srgb) / np.maximum(out[mask].mean(axis=0), 1e-6)

    # the plaster's light spread as the original's: its luminance mapped rank by rank onto the
    # original's quantiles
    pix = out[plaster]
    lum = pix @ tb.LUM
    srgb = np.clip(lum, 1e-6, 1) ** (1 / 2.2) * 255
    q, values = zip(*PLASTER_SPREAD)
    ranks = np.argsort(np.argsort(srgb)) / max(len(srgb) - 1, 1) * 100
    target = np.interp(ranks, q, values)
    target = SPREAD_FIT * target + (1 - SPREAD_FIT) * srgb * np.median(target) / max(np.median(srgb), 1e-6)   # the picture keeps half its own light
    # colour by light as the original's: olive-black grime, near-neutral thick wash; the picture's own
    # hue over it in part
    hue = _linear(np.array(PLASTER_HUE, dtype=float))
    hue /= (hue @ tb.LUM)[:, None]
    keys = np.array(PLASTER_HUE, dtype=float) @ tb.LUM
    by_light = np.stack([np.interp(target, keys, hue[:, k]) for k in range(3)], axis=-1)
    own = pix / np.maximum(lum, 1e-9)[:, None]
    own /= own.mean(axis=0)
    out[plaster] = ((target / 255) ** 2.2)[:, None] * by_light * own ** OWN_HUE

    # the oak's light spread as the original's, not only its mean: sRGB luminance median and p90 of its
    # timber (31 and 56); a power curve through both keeps the dark rims dark
    lum = out[wood] @ tb.LUM
    srgb = np.clip(lum, 1e-6, 1) ** (1 / 2.2) * 255
    x50, x90 = np.percentile(srgb, [50, 90])
    b_ = math.log(OAK_SPREAD[1] / OAK_SPREAD[0]) / math.log(max(x90, 1e-3) / max(x50, 1e-3))
    a_ = math.log(OAK_SPREAD[0]) - b_ * math.log(max(x50, 1e-3))
    target = np.exp(a_ + b_ * np.log(np.maximum(srgb, 1e-3)))
    out[wood] *= (((target / 255) ** 2.2) / np.maximum(lum, 1e-6))[:, None]

    normal = passes['normal'] / (np.linalg.norm(passes['normal'], axis=-1, keepdims=True) + 1e-9)
    # the picture's flakes and grain as relief: its light, less its broad stains, as a height
    from scipy.ndimage import gaussian_filter
    metres = py / rows
    light = pl @ tb.LUM
    relief = (light - gaussian_filter(light, PLASTER_RELIEF_PX[1] / metres, mode='wrap')) * PLASTER_RELIEF_PX[0]
    if detail is not None and detail.get('height') is not None:   # the scan's own relief, high-passed
        h = _sample(detail['height'], x, py - y, detail['metres']) @ tb.LUM
        relief = relief * (1 - DETAIL[2]) + (h - gaussian_filter(h, DETAIL[0] / metres, mode='wrap')) * DETAIL[3] * DETAIL[2]
    relief = gaussian_filter(relief, 1., mode='wrap')
    dy_, dx_ = np.gradient(relief, metres)
    tilt = np.stack([-dx_, dy_, np.zeros_like(dx_)], axis=-1) * plaster[..., None]
    normal = normal + tilt
    normal /= np.linalg.norm(normal, axis=-1, keepdims=True) + 1e-9
    rough = np.where(plaster, ROUGH['plaster'], np.where(iron, ROUGH['iron'], ROUGH['wood']))
    rough = np.where(mould > .3, rough - .04, rough)
    return dict(albedo=np.clip(out, 0, 1), roughness=rough, normal=normal)


def write_layout(path: Path, spec, points, grids, labels) -> None:
    pb.write_layout(path, spec, points, grids)
    np.savez_compressed(path.with_name(path.stem + '-labels.npz'), labels=labels.astype(np.int32))


def _image(path) -> np.ndarray:
    from PIL import Image
    return np.asarray(Image.open(path).convert('RGB'), dtype=np.float64) / 255


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    p = sub.add_parser('layout')
    p.add_argument('variant', choices=sorted(PERIODS))
    p.add_argument('out', type=Path)
    p.add_argument('--seed', type=int, default=0)
    p = sub.add_parser('render')
    p.add_argument('layout', type=Path)
    p.add_argument('out_dir', type=Path)
    p.add_argument('--resolution', type=int, default=1536)
    p.add_argument('--samples', type=int, default=32)
    p.add_argument('--street', action='store_true')
    p.add_argument('--sun', type=float, default=0.)
    p = sub.add_parser('compose')
    p.add_argument('layout', type=Path)
    p.add_argument('passes', type=Path)
    p.add_argument('out', type=Path)
    p.add_argument('--plaster', type=Path, required=True)
    p.add_argument('--wood', type=Path, required=True)
    p.add_argument('--detail', type=Path, help='a photographed plaster scan (CC0) for the fine grain')
    p.add_argument('--detail-height', type=Path, help="the scan's height map")
    p.add_argument('--detail-metres', type=float, default=2., help='how much wall the scan covers')
    p.add_argument('--seed', type=int, default=0)
    p = sub.add_parser('blockout')
    p.add_argument('layout', type=Path)
    p.add_argument('passes', type=Path)
    p.add_argument('out', type=Path)
    args = parser.parse_args(argv)
    if args.command == 'layout':
        write_layout(args.out, *fachwerk(args.seed, args.variant))
        print(args.out)
    elif args.command == 'render':
        spec = json.loads(args.layout.read_text())
        px, py = spec['period']
        options = (dict(view='street', pitch='90', eaves='.0', stretch='1', close=f'{px * .5},{py * .55}')
                   if args.street else {})
        if args.sun:
            options['sun'] = str(args.sun)
        print(pb.render(args.layout, args.out_dir, {}, args.resolution, args.samples, options))
    elif args.command == 'compose':
        spec = json.loads(args.layout.read_text())
        passes = pb.read_passes(args.passes, spec['period'])
        labels = np.load(args.layout.with_name(args.layout.stem + '-labels.npz'))['labels']
        detail = None if not args.detail else dict(
            albedo=_image(args.detail), metres=args.detail_metres,
            height=_image(args.detail_height) if args.detail_height else None)
        result = compose(passes, spec, labels, _image(args.plaster), _image(args.wood), args.seed, detail)
        px, py = spec['period']
        size = (1024, 1024) if args.out else None
        pb.write_maps(args.out, size, result['albedo'], result['normal'], {'roughness': result['roughness']})
        print(args.out)
    elif args.command == 'blockout':
        spec = json.loads(args.layout.read_text())
        passes = pb.read_passes(args.passes, spec['period'])
        width = passes['piece'].shape[1]
        pb.write_preview(passes, args.out, (width, passes['piece'].shape[0]))
        print(args.out)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
