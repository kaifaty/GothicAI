#!/usr/bin/env python3
"""The stone ground storey of the lower town's timber-framed houses (asset market-house) built as a
Blender model and baked into textures, on the shared piece_bake path (docs/remaster-pipeline.md,
«Материал из модели»).

    python3 scripts/content/basement_bake.py layout empty|window|chain OUT.json [--seed N]
    python3 scripts/content/basement_bake.py render OUT.json OUTDIR [--stone PNG] [--block PNG] [--wood PNG] [--street] [--sun E]
    python3 scripts/content/basement_bake.py compose OUT.json OUTDIR/passes.exr MAPS.png [--seed N]
    python3 scripts/content/basement_bake.py blockout OUT.json OUTDIR/passes.exr OUT.png

empty is NW_CITY_FACHWERKBASEMENT_EMPTY_01, a 3 × 3 m square, from the top down:
- a row of flat stones;
- the dark sill beam of the frame above, 21 cm;
- rubble of flat bedded stones, 15 to 45 cm long and 4 to 10 cm high, in wavy courses, with deep
  joints;
- two courses of large dressed blocks at the foot, 25 to 35 × 16 cm.

window (_WINDOW_01) adds a small square opening in a frame of seven light dressed stones. chain
(_CHAIN_01) adds a narrow slit in a dressed frame, a ring in the beam and two chains hanging from rings
in the wall.

Positions come from the originals measured by Claude at their own size and on a 4x view
(original_view.py; u from the left, v from the top). No original pixel goes into the model or a
generator; the surfaces are image_gen pictures (--stone, --wood).

The square is one grid of heights (STEP apart) that wraps in both axes. Each kind of part is its own
grid piece, cut to its rows, so the passes tell them apart; the stones of one kind are told apart by
the label grid stored with the layout (key labels).
"""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

import numpy as np

import piece_bake as pb
import timber_bake as tb

PERIOD = (3., 3.)            # metres: the square of the original on the house (market-house measurement)
STEP = .003                  # the grid of heights, metres
DROP = -.14                  # heights outside a piece: under the pieces next to it and the opening

# v (from the top) of the bands, measured on the originals
TOP = (0., .032)             # a row of flat stones over the beam
BEAM = (.032, .105)          # the sill beam
RUBBLE = (.105, .849)        # the rubble
BLOCK_ROWS = (.849, .903, .962, 1.)   # the dressed blocks: two courses, then a smooth plinth ledge

COURSE = (.03, .14)         # rubble course heights, metres
STONE = (.22, .55, (.08, .50))        # rubble stone lengths: median, log spread, limits
JOINT = (.009, .006)         # joint width between stones: mean, spread; tight, as a waller packs them
COURSE_WAVE = (.022, (1.2, .5))       # the courses wander and break: amplitude, wavelengths
BLOCK = (.32, .12, (.18, .52))        # block lengths: mean, spread, limits
PLINTH = (1.2, .2, (.9, 1.5))         # the ledge at the foot: long slabs
BLOCK_JOINT = (.011, .005, .006)   # the blocks' joints: mean, spread, least; the course reads from across the square
MORTAR = (-.030, .004)       # the joints' bottom: depth behind the stone faces, roughness
BEAM_PROUD = .035            # the beam stands out of the rubble face

# the window of _WINDOW_01: a dressed frame (u0, u1, v0, v1) round the opening; the frame's stones
WINDOW = dict(frame=(.365, .577, .311, .523), opening=(.396, .543, .350, .482),
              stones=[(.365, .440, .311, .350), (.440, .510, .311, .350), (.510, .577, .311, .350),   # lintel
                      (.365, .396, .350, .416), (.365, .396, .416, .482),                            # left jamb
                      (.543, .577, .350, .416), (.543, .577, .416, .482),                            # right jamb
                      (.365, .500, .482, .523), (.500, .577, .482, .523)])                           # sill
# the slit of _CHAIN_01 and its frame; the ring in the beam, the rings and chains on the wall
SLIT = dict(frame=(.300, .390, .260, .480), opening=(.322, .365, .292, .452),
            stones=[(.300, .390, .260, .292),
                    (.300, .322, .292, .372), (.300, .322, .372, .452),
                    (.365, .390, .292, .372), (.365, .390, .372, .452),
                    (.300, .390, .452, .480)])
BEAM_RING = (.670, .055, .060)       # u, v, outer radius in metres
CHAINS = [(.660, .378, .700), (.850, .372, .800)]   # u, v of the wall ring, v where the chain ends
CHAIN = dict(ring=(.090, .011), link=(.030, .020, .007), proud=.012)  # ring radius and wire; link half
                                                                    # length, half width, wire radius
OPENING_Z = -.10             # the opening's inside, dark
SOCKET = (.040, .115, .105, .142)    # a joist socket under the beam (u0, u1, v0, v1)
FRAME_PROUD = .022

SURFACES = [dict(name='stone', colour=[.30, .28, .22], metres=.6, bump=.0008),
            dict(name='block', colour=[.20, .15, .10], metres=.6, bump=.0008),
            dict(name='frame', colour=[.42, .40, .33], metres=.6, bump=.0006),
            dict(name='wood', colour=[.05, .045, .045], metres=1., bump=.0006),
            dict(name='mortar', colour=[.32, .31, .27], metres=.6, bump=.0003),
            dict(name='iron', colour=[.04, .035, .025], metres=1., bump=0.),
            dict(name='opening', colour=[.01, .01, .01], metres=1., bump=0.)]
BANDS = [('block', BLOCK_ROWS[-1], BLOCK_ROWS[0]), ('rubble', RUBBLE[1], RUBBLE[0]), ('beam', BEAM[1], BEAM[0]),
         ('top', TOP[1], TOP[0])]       # the pieces of the wall by rows: (kind, v of the bottom, of the top)
SURFACE_OF = dict(rubble=0, top=0, block=1, beam=3, frame=2, opening=6, iron=5)


def _grid():
    """Coordinates of the square's grid: x, y (metres, y up) and u, v (from the top), one closing row and
    column more, equal to the first, so the copies meet."""
    px, py = PERIOD
    n, m = round(px / STEP), round(py / STEP)
    gx, gy = np.meshgrid(np.arange(n + 1) * STEP, np.arange(m + 1) * STEP)
    return gx, gy, gx / px, 1 - gy / py


def _wrap(field):
    """The closing row and column equal to the first."""
    field[-1, :] = field[0, :]
    field[:, -1] = field[:, 0]
    return field


def _wave(rng, t, period, amplitude, lengths):
    out = np.zeros_like(t)
    for k, length in enumerate(lengths):
        n = max(1, round(period / length))
        out += amplitude / (k + 1) ** .6 * rng.uniform(.5, 1) * np.sin(2 * math.pi * n * t / period + rng.uniform(0, 2 * math.pi))
    return out


def _cuts(rng, length, spec):
    """Positions of the joints along a course of LENGTH that wraps: lengths drawn from spec."""
    mean, spread, (lo, hi) = spec
    sizes = []
    while sum(sizes) < length - lo:
        if spec is STONE:
            sizes.append(float(np.clip(mean * math.exp(rng.normal(0, spread)), lo, hi)))
        else:
            sizes.append(float(np.clip(rng.normal(mean, spread), lo, hi)))
    sizes = np.array(sizes) * length / sum(sizes)
    start = rng.uniform(0, length)
    return (start + np.concatenate([[0], np.cumsum(sizes)[:-1]])) % length


def _stone(rng, gx, gy, x0, x1, ybot, ytop, joint, kind):
    """Heights of one stone over the cell [x0, x1] × [ybot(x), ytop(x)] shrunk by the joint, and its mask.
    A rubble stone is a flat slab with broken, roughly square ends: a rounded box with a ragged outline,
    a worn rim, a low uneven face, faint bedding and a broken corner or two. It rests on its bed and is
    often thinner than its course. A dressed block is flatter, with straight sides and chipped arrises."""
    a = (x1 - x0) / 2 - joint / 2
    full = np.maximum((ytop - ybot) - joint, .016)
    if kind == 'block':
        b, corner = full / 2 * rng.uniform(.9, 1.), rng.uniform(.012, .03)      # rock-faced, worn round
        dome, proud, rim = rng.uniform(.004, .010), rng.uniform(.002, .016), rng.uniform(.015, .03)
    else:
        b = full / 2 * rng.uniform(.8, 1.)              # thinner than its course: chinks above it
        corner = np.minimum(a, b) * rng.uniform(.2, .6)  # split, not water-worn: sharp-ish ends
        dome, proud, rim = rng.uniform(.002, .006), rng.uniform(-.010, .022), rng.uniform(.006, .014)
    cx, cy = (x0 + x1) / 2 + rng.normal(0, .004), ybot + joint / 2 + b + (rng.normal(0, .008) if kind != 'block' else 0)
    dx, dy = np.abs(gx - cx), np.abs(gy - cy)
    if kind != 'block':                                   # a split slab is a wedge or a lens, not a box: it
        xn = np.clip((gx - cx) / max(a, .01), -1.3, 1.3)  # tapers, thins to its ends and lies a little askew
        half = (1 + rng.uniform(-.35, .35) * xn) * (1 - rng.uniform(.1, .55) * xn ** 4)
        dy = np.abs(gy - cy - rng.normal(0, .3) * xn * np.mean(b)) / np.maximum(half, .25)
    else:                                                 # a block sits a little out of true
        dy = np.abs(gy - cy - rng.normal(0, .004) * (gx - cx) / max(a, .01))
    qx, qy = dx - (a - corner), dy - (b - corner)
    sdf = np.hypot(np.maximum(qx, 0), np.maximum(qy, 0)) + np.minimum(np.maximum(qx, qy), 0) - corner
    # a broken outline: smooth waves in metres, a few centimetres long
    amount = .008 if kind == 'block' else .009
    for length, share in ((.24, 1.), (.11, .6), (.05, .25)):
        t = rng.uniform(0, math.pi)
        sdf += amount * share * rng.uniform(.6, 1) * np.sin(2 * math.pi * (math.cos(t) * gx + math.sin(t) * gy) / length
                                                   + rng.uniform(0, 6.3))
    if kind != 'block':                                   # knocked off corners: straight cuts
        for _ in range(rng.integers(0, 3)):
            sx, sy = rng.choice([-1, 1]), rng.choice([-1, 1])
            t = rng.uniform(.2, 1.3)
            nx, ny = sx * math.cos(t), sy * math.sin(t)
            reach = rng.uniform(.78, .95) * (abs(nx) * a + abs(ny) * np.mean(b))
            sdf = np.maximum(sdf, nx * (gx - cx) + ny * (gy - cy) - reach)
    inside = sdf < 0
    depth = np.clip(-sdf, 0, None)                         # metres in from the rim
    face = proud + dome * (1 - np.exp(-depth / rim))
    if True:                                              # a cleft face: two or three flat facets, the blocks' fainter
        planes = [rng.normal(0, .10 if kind != 'block' else .05, 2) for _ in range(rng.integers(2, 4))]
        offs = [rng.uniform(0, .004) for _ in planes]
        facets = np.min([o + px_ * (gx - cx) + py_ * (gy - cy) for (px_, py_), o in zip(planes, offs)], axis=0)
        face += np.clip(facets - np.median(facets[inside]) if inside.any() else facets, -.006, .006)
    face += rng.normal(0, .012) * (gx - cx) + rng.normal(0, .03) * (gy - cy)
    for k in range(3):                                    # an uneven face: a few broad bumps
        fx, fy = rng.uniform(2, 6) / max(a, .05), rng.uniform(2, 5) / max(float(np.mean(b)), .02)
        face += (.0012 if kind != 'block' else .003) * np.sin(fx * (gx - cx) + rng.uniform(0, 6.3)) * np.sin(fy * (gy - cy) + rng.uniform(0, 6.3))
    if kind != 'block':                                   # faint bedding along the slab, broken
        wave = 2 * math.pi * (gy - cy + .006 * np.sin(gx * rng.uniform(5, 12))) / rng.uniform(.014, .03)
        face += .0004 * np.sin(wave) * (np.sin(gx * rng.uniform(8, 20) + rng.uniform(0, 6.3)) > -.2)
    for _ in range(rng.integers(0, 3)):                   # a broken corner: a flat scoop out of the rim
        ang = rng.uniform(0, 2 * math.pi)
        hx, hy = cx + a * math.cos(ang), cy + np.mean(b) * math.sin(ang)
        d = np.hypot((gx - hx) / (a * .4), (gy - hy) / (np.mean(b) * 1.1))
        face -= rng.uniform(.004, .010) * np.clip(1 - d, 0, 1)
    return face, inside


def _course_lines(rng, xs, y0, y1):
    """Wavy course lines from y0 up to y1 (metres, y up), wrapping in x."""
    lines = [np.full_like(xs, y0)]
    y = y0
    while True:
        y += rng.uniform(*COURSE)
        if y > y1 - COURSE[0] * .6:
            break
        lines.append(y + _wave(rng, xs, PERIOD[0], COURSE_WAVE[0], COURSE_WAVE[1]))
    lines.append(np.full_like(xs, y1))
    return lines


def _lay(rng, gx, gy, height, labels, kinds, kind, lines, size, joint_spread=True):
    """Lay courses of stones between LINES into the height and label grids."""
    px = PERIOD[0]
    xs = gx[0]
    for k in range(len(lines) - 1):
        cuts = np.sort(_cuts(rng, px, size))
        for i, x0 in enumerate(cuts):
            x1 = cuts[i + 1] if i + 1 < len(cuts) else cuts[0] + px
            joint = max(.005, rng.normal(*JOINT)) if joint_spread else max(BLOCK_JOINT[2], rng.normal(*BLOCK_JOINT[:2]))
            # the window of the grid that can hold this stone, wrapping in x
            lo, hi = lines[k].min(), lines[k + 1].max()
            r0, r1 = max(0, int(lo / STEP) - 1), min(height.shape[0], int(hi / STEP) + 2)
            c0, c1 = int(x0 / STEP) - 2, int(x1 / STEP) + 3
            cols = np.arange(c0, c1) % (height.shape[1] - 1)
            sx = (np.arange(c0, c1) * STEP)[None, :]
            sy = gy[r0:r1, :1]
            jitter = .008 if kind == 'rubble' else .004   # the rubble's beds step from stone to stone
            ybot = np.interp(sx % px, xs, lines[k]) + rng.normal(0, jitter)
            ytop = np.interp(sx % px, xs, lines[k + 1]) + rng.normal(0, jitter)
            # a third of the rubble cells hold two thin slabs one on the other
            split = kind == 'rubble' and rng.uniform() < .4 and (ytop - ybot).min() > .06
            beds = [(ybot, ybot + (ytop - ybot) * (f := rng.uniform(.4, .6))), (ybot + (ytop - ybot) * f, ytop)] if split \
                else [(ybot, ytop)]
            for bot, top in beds:
                xa, xb = (x0, x1) if not split else sorted(rng.uniform(-.03, .03, 2) + (x0, x1))
                face, inside = _stone(rng, sx, sy, xa, xb, bot, top, joint, kind)
                block = height[r0:r1, cols]
                take = inside & (face > block)
                block[take] = face[take]
                height[r0:r1, cols] = block
                lab = labels[r0:r1, cols]
                lab[take] = len(kinds)
                labels[r0:r1, cols] = lab
                kinds.append(kind)


def _chinks(rng, gx, gy, height, labels, kinds, y0, y1, gap=.017):
    """Small stones wedged into the wider joints between y0 and y1, as a waller packs them."""
    from scipy.ndimage import distance_transform_edt
    rows = (gy[:, 0] > y0 + .01) & (gy[:, 0] < y1 - .01)
    r0 = np.where(rows)[0][0]
    free = labels[rows] == 0
    dist = distance_transform_edt(free) * STEP
    ys, xs = np.where(dist > gap)
    for i in rng.permutation(len(ys))[:220]:
        r, c = ys[i], xs[i]
        if labels[r0 + r, c] != 0 or dist[r, c] <= gap:
            continue
        size = dist[r, c]
        a, b = min(size * rng.uniform(1.2, 2.2), .04), size * rng.uniform(.75, .95)
        cx, cy = gx[0, c], gy[r0 + r, 0]
        c0, c1 = c - int(a / STEP) - 3, c + int(a / STEP) + 4
        q0, q1 = max(0, r0 + r - int(b / STEP) - 3), r0 + r + int(b / STEP) + 4
        cols = np.arange(c0, c1) % (height.shape[1] - 1)
        sx, sy = (np.arange(c0, c1) * STEP)[None, :], gy[q0:q1, :1]
        face, inside = _stone(rng, sx, sy, cx - a, cx + a, np.full_like(sx, cy - b), np.full_like(sx, cy + b), .002,
                              'rubble')
        face -= .006                                   # set back between the big stones
        block, lab = height[q0:q1, cols], labels[q0:q1, cols]
        take = inside & (lab == 0) & (face > block)
        block[take], lab[take] = face[take], len(kinds)
        height[q0:q1, cols], labels[q0:q1, cols] = block, lab
        kinds.append('rubble')
        dist[max(0, r - int(b / STEP) - 2):r + int(b / STEP) + 3, np.arange(c0, c1) % dist.shape[1]] = 0


def _rect(gx, gy, rect):
    u0, u1, v0, v1 = rect
    px, py = PERIOD
    return (gx >= u0 * px) & (gx <= u1 * px) & (gy >= (1 - v1) * py) & (gy <= (1 - v0) * py)


def _frame(rng, gx, gy, height, labels, kinds, spec):
    """A dressed frame round an opening: the rubble cleared, the frame's stones, the dark inside."""
    px, py = PERIOD
    inside = _rect(gx, gy, spec['frame'])
    height[inside] = MORTAR[0]
    labels[inside] = 0
    for rect in spec['stones']:
        u0, u1, v0, v1 = rect
        x0, x1, ybot, ytop = u0 * px, u1 * px, (1 - v1) * py, (1 - v0) * py
        face, mask = _stone(rng, gx, gy, x0, x1, np.full_like(gx, ybot), np.full_like(gx, ytop), .008, 'block')
        face = face - face[mask].mean() + FRAME_PROUD if mask.any() else face
        height[mask] = face[mask]
        labels[mask] = len(kinds)
        kinds.append('frame')
    hole = _rect(gx, gy, spec['opening'])
    height[hole] = OPENING_Z
    labels[hole] = len(kinds)
    kinds.append('opening')


def _torus(gx, gy, cx, cy, radius, wire, sx=1., sy=1.):
    """Height of a ring of wire lying on the wall (an ellipse with half axes radius·sx, radius·sy)."""
    r = np.hypot((gx - cx) / sx, (gy - cy) / sy)
    d = np.abs(r - radius)
    return np.where(d < wire, np.sqrt(np.maximum(wire ** 2 - d ** 2, 0)), -1.), d < wire


def _iron(gx, gy, height, labels, kinds, base, cx, cy, radius, wire, sx=1., sy=1.):
    face, mask = _torus(gx, gy, cx, cy, radius, wire, sx, sy)
    face = base + face
    take = mask & (face > height)
    height[take] = face[take]
    labels[take] = len(kinds)
    kinds.append('iron')


def basement(seed: int, variant: str):
    """Layout of NW_CITY_FACHWERKBASEMENT_<variant>_01: (spec, points, grids, labels)."""
    rng = np.random.default_rng([seed, 17])          # the same wall for the three textures
    px, py = PERIOD
    gx, gy, _, _ = _grid()
    xs = gx[0]
    height = MORTAR[0] + MORTAR[1] * pb.noise_metres(rng, gx.shape, PERIOD, .02)
    labels = np.zeros(gx.shape, int)
    kinds = ['mortar']
    y = lambda v: (1 - v) * py
    # two courses of blocks over a long smooth plinth ledge at the very foot
    _lay(rng, gx, gy, height, labels, kinds, 'block', [np.full_like(xs, y(v)) for v in BLOCK_ROWS[-2::-1]], BLOCK, False)
    _lay(rng, gx, gy, height, labels, kinds, 'block', [np.full_like(xs, 0.), np.full_like(xs, y(BLOCK_ROWS[-2]))], PLINTH, False)
    _lay(rng, gx, gy, height, labels, kinds, 'rubble', _course_lines(rng, xs, y(RUBBLE[1]), y(RUBBLE[0])), STONE)
    _chinks(rng, gx, gy, height, labels, kinds, y(RUBBLE[1]), y(RUBBLE[0]))
    _lay(rng, gx, gy, height, labels, kinds, 'top', [np.full_like(xs, y(TOP[1])), np.full_like(xs, y(TOP[1]) + .045),
                                                    np.full_like(xs, py)], STONE)
    # the beam: hewn oak with its grain along x, proud of the wall
    band = (gy >= y(BEAM[1])) & (gy <= y(BEAM[0]))
    r0, r1 = np.where(band[:, 0])[0][[0, -1]]
    by = gy[r0:r1 + 1] - gy[r0, 0]
    bx = gx[r0:r1 + 1]
    width = float(by.max())
    style = dict(adze=tb.STROKE, grain=(.0007, (.003, .008)), pits=((.006, .05), .003, .06, .12), arris=(.012, .008), cup=0.)
    face = tb._hewn(rng, by.T, bx.T, width, px, (STEP, STEP), **style).T
    height[r0:r1 + 1] = BEAM_PROUD + face + .004 * np.sin(2 * math.pi * bx / px + rng.uniform(0, 6.3))
    labels[r0:r1 + 1] = len(kinds)
    kinds.append('beam')
    # the socket of a joist under the beam, as in all three originals: a dark hole cut by the beam
    hole = _rect(gx, gy, SOCKET)
    height[hole] = OPENING_Z
    labels[hole] = len(kinds)
    kinds.append('opening')
    if variant == 'window':
        _frame(rng, gx, gy, height, labels, kinds, WINDOW)
    if variant == 'chain':
        _frame(rng, gx, gy, height, labels, kinds, SLIT)
    wall = height.copy()                             # the wall goes on under the iron
    if variant == 'chain':
        u, v, radius = BEAM_RING
        ring_base = float(height[int(y(v) / STEP), int(u * px / STEP)])
        _iron(gx, gy, height, labels, kinds, ring_base + .004, u * px, y(v), radius * .75, CHAIN['ring'][1])
        radius, wire = CHAIN['ring']
        half, half_w, link_wire = CHAIN['link']
        for u, v0, v1 in CHAINS:
            cx = u * px
            _iron(gx, gy, height, labels, kinds, .012, cx, y(v0), radius * .6, wire)
            # links hang from the ring's bottom: face-on ovals and edge-on bars in turn, a little swing
            top = y(v0) - radius * .6
            n = int((y(v0) - y(v1)) / (2 * half - 2 * link_wire))
            for i in range(n):
                cy = top - half - i * (2 * half - 2 * link_wire)
                sway = .006 * math.sin(i * .9 + u * 10)
                if i % 2 == 0:
                    _iron(gx, gy, height, labels, kinds, CHAIN['proud'], cx + sway, cy, half_w, link_wire,
                          1., half / half_w)
                else:                        # edge on: a bar with rounded ends
                    d = np.hypot(np.clip(np.abs(gy - cy) - half + link_wire, 0, None), gx - cx - sway)
                    face = CHAIN['proud'] + .004 + np.sqrt(np.maximum(link_wire ** 2 - d ** 2, 0))
                    mask = (d < link_wire) & (face > height)
                    height[mask] = face[mask]
                    labels[mask] = len(kinds)
                    kinds.append('iron')
    for field in (height, wall, labels):
        _wrap(field)
    kind_grid = np.array(kinds)[labels]
    # the wall in pieces by its bands of rows, with their mortar and the wall under the iron; the frame,
    # the opening and the iron each a piece of their own
    pieces, grids = [], {}
    parts = []
    for kind, v0, v1 in BANDS:
        rows = (gy[:, 0] >= y(v0) - STEP / 2) & (gy[:, 0] <= y(v1) + STEP / 2)
        own = np.isin(kind_grid, ['frame', 'opening'])
        parts.append((kind, rows, np.where(own, DROP, np.where(kind_grid == 'iron', wall, height))))
    for kind in ('frame', 'opening', 'iron'):
        mask = kind_grid == kind
        if mask.any():
            parts.append((kind, mask.any(axis=1), np.where(mask, height, DROP)))
    for kind, rows, field in parts:
        rows = np.where(rows)[0]
        r0, r1 = max(0, rows[0] - 1), min(field.shape[0] - 1, rows[-1] + 1)
        heights = field[r0:r1 + 1]
        index = len(pieces)
        lean = math.pi / 2 if kind == 'beam' else 0.      # the stone pictures' bedding already runs across
        pieces.append(pb.grid_piece(index, (0., float(gy[r0, 0])), (STEP, STEP), [0, 0, 0], SURFACE_OF[kind],
                                    [rng.uniform(0, 1), rng.uniform(0, 1), lean],
                                    copy_offsets=[[dx, dy] for dx in (-px, 0, px) for dy in (-py, 0, py)], kind=kind))
        grids[index] = heights
    texture = f'NW_CITY_FACHWERKBASEMENT_{variant.upper()}_01.TGA'
    spec = dict(tool='scripts/content/basement_bake.py', texture=texture, wall=variant, seed=seed,
                period=list(PERIOD), camera=dict(tilt=0.), sky=dict(pitch=0., ground=.08),
                ao=dict(distance=.05, crevice=.010), surfaces=SURFACES, stones=kinds, pieces=pieces)
    return spec, {}, grids, labels


LAYOUTS = ('empty', 'window', 'chain')


# ---------------------------------------------------------------- weathering (compose)
# Measured on the originals at their own size: mean colour (sRGB of the linear mean) of each part with its
# joints. The rubble darkens and greens towards the foot: its colour at v (from the top), the light lime
# bed under the beam first.
PART_COLOUR = dict(top=(115, 98, 79), beam=(46, 40, 42), block=(78, 70, 47))
FOOT = dict(blocks=(57, 46, 30), joints=(131, 118, 88), plinth=(71, 66, 40))   # sRGB means of the original's foot
BLOCK_DARKER = .78    # the blocks read in game 12% darker than the rubble, the original's 22-29%: look over number
RUBBLE_COLOUR = [(.105, (134, 130, 120)), (.215, (128, 118, 101)), (.40, (121, 111, 90)), (.59, (112, 104, 78)),
                 (.765, (86, 79, 56)), (.849, (82, 76, 53))]   # the foot darker: the original's median falls 92 -> 71
FRAME_COLOUR = dict(window=(159, 148, 117), chain=(143, 131, 106))
OPENING_COLOUR = dict(window=(24, 21, 16), chain=(15, 12, 11), empty=(18, 16, 13))
IRON = (35, 31, 23)
STONE_TINTS = [((1., .92, .78), .50), ((1., .86, .76), .12), ((1., .93, .70), .12), ((.97, .95, .82), .10),
               ((1., .80, .62), .08), ((.92, .97, .78), .08)]   # and rusty, and green-stained
                                       # olive-grey, pinkish, ochre, grey: linear ratios to R, and their share
STONE_SPREAD = .34                     # log spread of the stones' own tone
MOSS = ((40, 50, 24), (.55, .85), .45)  # moss colour, the v where it starts and where it is full, its share there
POINTING_LOST = -.3                    # where the blocks' pointing is gone: below this of a unit noise (a third)
BED_JOINT = .016                       # metres either side of a course line that is its bed joint
LOST_JOINT = (52, 46, 32)              # there the joint is earth and grime, dark but not black
LIME = ((150, 146, 132), .035)         # the light lime bed under the beam and how far below it reaches (v)
OCCLUSION = (1.8, 1.4)
ROUGH = dict(stone=(.88, .95), damp=.80, moss=.95, wood=(.86, .95), iron=.72)


def _linear(srgb) -> np.ndarray:
    return (np.asarray(srgb, dtype=np.float64) / 255) ** 2.2


def _rubble_target(v):
    """The original's mean linear colour of the rubble at v (from the top)."""
    vs = [b[0] for b in RUBBLE_COLOUR]
    cols = np.array([_linear(b[1]) for b in RUBBLE_COLOUR])
    return np.stack([np.interp(v, vs, cols[:, c]) for c in range(3)], axis=-1)


def _match(out, mask, v, target, bands=14):
    """Scale the pixels of MASK so that their mean colour follows TARGET(v): one gain per band of v, the gains
    blended linearly between the bands' middles, so no band shows as a stripe."""
    vs = v[mask]
    lo, hi = vs.min(), vs.max()
    edges = np.linspace(lo, hi + 1e-6, bands + 1)
    mids, gains = [], []
    for b in range(bands):
        m = (vs >= edges[b]) & (vs < edges[b + 1])
        if m.sum() > 50:
            mids.append((edges[b] + edges[b + 1]) / 2)
            gains.append(target((edges[b] + edges[b + 1]) / 2) / np.maximum(out[mask][m].mean(axis=0), 1e-6))
    gains = np.array(gains)
    gain = np.stack([np.interp(vs, mids, gains[:, c]) for c in range(3)], axis=-1)
    out[mask] *= gain


def compose(passes: dict, spec: dict, labels: np.ndarray, seed: int = 0) -> dict:
    """Albedo (linear RGB) and masks of one of the basement's textures."""
    rng = np.random.default_rng([seed, 23])
    piece = passes['piece']
    rows, cols = piece.shape
    period = spec['period']
    px, py = period
    kinds = {p['id']: p['kind'] for p in spec['pieces']}
    kind = np.vectorize(lambda i: kinds.get(i, 'void'))(piece)
    x, y = passes['x'] % px, passes['y'] % py
    u, v = x / px, 1 - y / py
    # which stone every pixel shows: the layout's label grid at the pixel's point on the model
    n = labels.shape[1] - 1
    stone = labels[np.rint(y / STEP).astype(int) % (labels.shape[0] - 1), np.rint(x / STEP).astype(int) % n]
    stone = np.where(np.isin(kind, ['frame', 'opening', 'iron']), -1, stone)
    stone_kind = np.array(spec['stones'])
    what = np.where(stone >= 0, stone_kind[np.maximum(stone, 0)], kind)
    mortar = what == 'mortar'
    rock = np.isin(what, ['rubble', 'top', 'block'])
    beam, frame, iron, opening = what == 'beam', kind == 'frame', kind == 'iron', kind == 'opening'

    colour = passes['colour']
    lum = colour @ tb.LUM
    out = np.zeros((rows, cols, 3))
    # every stone its own tone and tint, the surface picture's light over it
    count = len(spec['stones'])
    tone = np.clip(np.exp(rng.normal(0, STONE_SPREAD, count)), .55, 1.4)   # no single pale stone flares in the shade
    tints = np.array([t for t, _ in STONE_TINTS])
    tint = tints[rng.choice(len(STONE_TINTS), count, p=[w for _, w in STONE_TINTS])]
    for mask in (rock, frame, beam, iron, mortar, opening):
        if mask.any():
            out[mask] = (lum[mask] / np.median(lum[mask]))[:, None]
    idx = np.maximum(stone, 0)
    blocks = np.array(spec['stones']) == 'block'          # the blocks keep their red-brown, some redder
    tint[blocks] = np.where(rng.uniform(0, 1, (count, 1)) < .6, np.array([1., .62, .58]), np.where(rng.uniform(0, 1, (count, 1)) < .5, np.array([1., .74, .66]), np.array([.9, 1., .62])))[blocks]   # red-purple brown, some mossy
    tone[blocks] = np.clip(np.exp(rng.normal(0, .4, blocks.sum())), .5, 1.7)
    out[rock] *= (tone[idx[rock]][:, None] * tint[idx[rock]])
    out[frame] *= np.exp(.08 * rng.normal(0, 1, count))[idx[frame]][:, None] * np.array([1., .93, .74])
    out[mortar] *= np.array([1., .97, .86])
    # the joints keep their lime in patches: light mortar flush with the faces, elsewhere washed out deep
    lime_patch = np.clip(2.5 * pb.noise_metres(rng, (rows, cols), period, .12) - 1.2, 0, 1) * mortar * (v < RUBBLE[1])
    # the blocks' joints are pointed with light lime, the rubble's lie deep
    out[beam] *= np.array([1., .87, .88])
    out[iron] *= np.array([1., .88, .62])
    # grime: broad damp and dirt over everything, the stones less clean than they were cut
    out *= np.exp(.20 * (pb.noise_metres(rng, (rows, cols), period, .25) + .6 * pb.noise_metres(rng, (rows, cols), period, .06)))[..., None]

    # moss towards the foot: in the joints first, then over the stones' lower faces
    moss_srgb, (start, full), share = MOSS
    rise = np.clip((v - start) / (full - start), 0, 1)
    field = pb.noise_metres(rng, (rows, cols), period, .05) + .6 * pb.noise_metres(rng, (rows, cols), period, .015)
    shade = 1 - np.clip(passes['crevice'], 0, 1)
    field += 3.2 * shade + 1.6 * rise - 2.
    moss = np.clip((field - np.quantile(field[rock | mortar], 1 - share * rise.mean() * 2)) * .8, 0, 1) * (rock | mortar)
    moss *= rise ** .7
    moss = np.where(what == 'block', moss * .5, moss)   # on the blocks thinner, in patches
    moss = np.where(mortar & (v > RUBBLE[1]), np.clip(.5 + .5 * field, 0, 1), moss)
    green = _linear(moss_srgb) / (_linear(moss_srgb) @ tb.LUM)
    out = out * (1 - .8 * moss[..., None]) + (out @ tb.LUM)[..., None] * green * .8 * moss[..., None]

    occlusion = np.clip(passes['ao'], 0, 1) ** OCCLUSION[0] * np.clip(passes['crevice'], 0, 1) ** OCCLUSION[1]
    # the lime bed right under the beam is light and flush: less shadow, a crumbling edge
    lime_srgb, reach = LIME
    under = np.clip(1 - (v - BEAM[1]) / reach, 0, 1) * (v > BEAM[1]) * (mortar | rock)
    under *= np.clip(.8 + .6 * pb.noise_metres(rng, (rows, cols), period, .05), 0, 1)
    out = out * (1 - under[..., None]) + (_linear(lime_srgb) / (_linear(lime_srgb) @ tb.LUM)) * under[..., None]
    occlusion = np.maximum(occlusion, under * .8)
    lime_rgb = _linear(lime_srgb) / (_linear(lime_srgb) @ tb.LUM)
    out = out * (1 - .7 * lime_patch[..., None]) + lime_rgb * .7 * lime_patch[..., None]
    occlusion = np.maximum(occlusion, .55 * lime_patch)
    out *= occlusion[..., None]
    # the sky from above is cut off under every stone's lower edge: a dark line under each, as on the
    # original
    shade = pb.overhang_shade(passes['height'], py / rows, (25., 35., 50.), .05, softness=.004)
    out *= (1 - .65 * shade * (rock | mortar))[..., None]
    # the blocks' joints are pointed with lime, flush and light, mossy in places
    # in patches: elsewhere the lime is gone and the joint lies deep and dark, as on the original
    held = pb.noise_metres(rng, (rows, cols), period, .10) + .5 * pb.noise_metres(rng, (rows, cols), period, .03)
    joints = mortar & (v > RUBBLE[1] + .004)
    keep = np.clip((held - POINTING_LOST) * 2.5, 0, 1) * joints       # soft: the lime crumbles out at its edge
    # the bed joints lie tight under the blocks' weight and show as dark lines; the lime is in the
    # upright joints
    bed = np.min([np.abs(v - b_) for b_ in BLOCK_ROWS[:3]], axis=0) * py < BED_JOINT
    keep *= np.where(bed, .15, 1.)
    pointing = keep > .5
    lime = _linear(LIME[0]) * 1.1 * np.exp(.25 * pb.noise_metres(rng, (rows, cols), period, .03))[..., None]
    pointed = lime * (1 - .12 * moss[..., None]) + (green * .06)[None, None] * .12 * moss[..., None]   # grey lime, not green
    out = np.where(joints[..., None], pointed * keep[..., None] + _linear(LOST_JOINT) * (1 - keep[..., None]), out)

    # the original's colour part by part, the joints and their shadow included; the rubble darkens and
    # greens towards the foot. Frames, openings and iron keep their own colour.
    band = np.where(v < TOP[1], 'top', np.where(v < BEAM[1], 'beam', np.where(v < RUBBLE[1], 'rubble', 'block')))
    for part in ('top', 'beam', 'rubble', 'block'):
        mask = (band == part) & (rock | mortar | beam)
        if mask.sum() > 50:
            target = _rubble_target if part == 'rubble' else (lambda _v, c=_linear(PART_COLOUR[part]): c)
            _match(out, mask, v, target, 14 if part == 'rubble' else 3)
            if part == 'block':
                # the foot measured part by part on the original: red-brown blocks, light grey-ochre
                # pointing, a plain olive ledge under them
                plinth = v > BLOCK_ROWS[2]
                for m, srgb in ((mask & ~mortar & ~plinth, FOOT['blocks']), (mask & pointing, FOOT['joints']),
                                (mask & plinth & ~mortar, FOOT['plinth'])):
                    if m.sum() > 50:
                        out[m] *= _linear(srgb) / np.maximum(out[m].mean(axis=0), 1e-6)
    for mask, srgb in ((frame, FRAME_COLOUR.get(spec['wall'])), (opening, OPENING_COLOUR.get(spec['wall'])),
                       (iron, IRON)):
        if mask.any() and srgb:
            out[mask] *= _linear(srgb) / np.maximum(out[mask].mean(axis=0), 1e-6)

    normal = passes['normal'] / (np.linalg.norm(passes['normal'], axis=-1, keepdims=True) + 1e-9)
    dry = ROUGH['stone'][0] + (ROUGH['stone'][1] - ROUGH['stone'][0]) * np.clip(.5 + .3 * pb.noise_metres(rng, (rows, cols), period, .1), 0, 1)
    rough = np.where(rock | mortar | frame, dry - (dry - ROUGH['damp']) * rise * .5, 1.)
    rough = np.where(beam, ROUGH['wood'][0], rough)
    rough = np.where(iron, ROUGH['iron'], rough)
    rough = np.maximum(rough, ROUGH['moss'] * moss)
    return dict(albedo=np.clip(out, 0, 1), roughness=rough, normal=normal, moss=moss)


def write_layout(path: Path, spec, points, grids, labels) -> None:
    pb.write_layout(path, spec, points, grids)
    np.savez_compressed(path.with_name(path.stem + '-labels.npz'), labels=labels.astype(np.int32))


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    p = sub.add_parser('layout')
    p.add_argument('variant', choices=LAYOUTS)
    p.add_argument('out', type=Path)
    p.add_argument('--seed', type=int, default=0)
    p = sub.add_parser('render')
    p.add_argument('layout', type=Path)
    p.add_argument('out_dir', type=Path)
    p.add_argument('--stone', type=Path, help='image_gen surface of the rubble, frame and mortar')
    p.add_argument('--block', type=Path, help='image_gen surface of the dressed blocks')
    p.add_argument('--wood', type=Path, help='image_gen surface of the wood')
    p.add_argument('--resolution', type=int, default=1536)
    p.add_argument('--samples', type=int, default=48)
    p.add_argument('--street', action='store_true', help='perspective views of a wall of the texture')
    p.add_argument('--sun', type=float, default=0., help='a raking sun, for the block-out only')
    p = sub.add_parser('compose')
    p.add_argument('layout', type=Path)
    p.add_argument('passes', type=Path)
    p.add_argument('out', type=Path)
    p.add_argument('--seed', type=int, default=0)
    p = sub.add_parser('blockout')
    p.add_argument('layout', type=Path)
    p.add_argument('passes', type=Path)
    p.add_argument('out', type=Path)
    args = parser.parse_args(argv)
    if args.command == 'layout':
        spec, points, grids, labels = basement(args.seed, args.variant)
        write_layout(args.out, spec, points, grids, labels)
        print(args.out)
    elif args.command == 'render':
        spec = json.loads(args.layout.read_text())
        surfaces = {}
        if args.stone:
            surfaces.update({name: args.stone for name in ('stone', 'block', 'frame', 'mortar', 'opening')})
        if args.block:
            surfaces['block'] = args.block
        if args.wood:
            args.out_dir.mkdir(parents=True, exist_ok=True)
            surfaces['wood'] = pb.seamless_along(args.wood, args.out_dir / 'surface-wood.png')
        px, py = spec['period']
        options = (dict(view='street', pitch='90', eaves='.0', stretch='1', close=f'{px * .5},{py * .55}')
                   if args.street else {})
        if args.sun:
            options['sun'] = str(args.sun)
        print(pb.render(args.layout, args.out_dir, surfaces, args.resolution, args.samples, options))
    elif args.command == 'compose':
        spec = json.loads(args.layout.read_text())
        passes = pb.read_passes(args.passes, spec['period'])
        labels = np.load(args.layout.with_name(args.layout.stem + '-labels.npz'))['labels']
        result = compose(passes, spec, labels, args.seed)
        pb.write_maps(args.out, (1024, 1024), result['albedo'], result['normal'],
                      {k: result[k] for k in ('roughness', 'moss')})
        print(args.out)
    elif args.command == 'blockout':
        spec = json.loads(args.layout.read_text())
        passes = pb.read_passes(args.passes, spec['period'])
        width = passes['piece'].shape[1]
        pb.write_preview(passes, args.out, (width, width))
        print(args.out)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
