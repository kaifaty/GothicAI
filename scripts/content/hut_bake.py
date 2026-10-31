#!/usr/bin/env python3
"""Walls of the harbour huts (asset harbour-hut) built as Blender models and baked into textures, on the
shared piece_bake path (docs/remaster-pipeline.md, «Материал из модели»).

    python3 scripts/content/hut_bake.py layout wall|window|gable OUT.json [--seed N]
    python3 scripts/content/hut_bake.py render OUT.json OUTDIR [--wood PNG] [--street] [--sun E]
    python3 scripts/content/hut_bake.py compose OUT.json OUTDIR/passes.exr MAPS.png [--seed N]
    python3 scripts/content/hut_bake.py blockout OUT.json OUTDIR/passes.exr OUT.png

wall is NW_CITY_HAFEN_HOLZWAND_01: ten upright boards on a 3 × 3 m square, 25 to 42 cm wide, with
wavy split edges and dark gaps between them, a short board on top of the fifth, nails where the
rails behind them run, soil splashed over their feet. window is NW_CITY_HAFEN_HOLZWAND_02: the same
boards with the small dark window of the original, a plain frame of four boards round an opening
closed by an inner shutter. gable is NW_CITY_HAFEN_GIEBEL_01: eight rows of wide boards lapped over
each other, each with a wavy waney lower edge that throws a dark line on the board below, butt joints
where the original has them.

Positions come from the originals measured by Claude at their own size and on a 4x view
(original_view.py; u from the left, v from the top). No original pixel goes into the model or a
generator; the surface of the wood is an image_gen picture (--wood).
"""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

import numpy as np

import piece_bake as pb
import timber_bake as tb

PERIOD = (3., 3.)            # metres: the square of the original on the huts (harbour-house census)
STEP = (.0025, .0025)        # the grid of heights, metres
DROP = -.06                  # heights outside a board: below the void, so the gaps show the dark
VOID_Z = -.04
SURFACES = [dict(name='wood', colour=[.20, .18, .13], metres=1., bump=.0006),
            dict(name='frame', colour=[.30, .27, .20], metres=1., bump=.0006),
            dict(name='shutter', colour=[.05, .045, .03], metres=1., bump=.0004)]
WOOD, FRAME, SHUTTER = 0, 1, 2

# ---------------------------------------------------------------- wall and window: layout
# The ten gaps of the original, u from the left; board i runs from EDGES[i] to EDGES[i + 1] and the last
# one across the seam. The fifth board is two: a short one on top down to v .13.
EDGES = [.068, .164, .262, .366, .469, .61, .723, .806, .889, .996]
JOINT = dict(board=4, v=.13, gap=.006)
WAVE = (.006, (1.5, .6, .3))        # the split edges wander: amplitude in metres, wavelengths
BITES = (2, (.006, .016), (.08, .25))  # waney bites per board edge: count, depth, length, metres
GAP = (.008, .004)                   # half the gap: mean and spread, metres (1.6 cm wide, 0.8 to 2.4)
RAILS = (.14, .63)                   # v of the rails behind the boards: the nails
NAIL = (.007, .0022)                # nail head radius and dome height, metres
GROUND = .84                         # v where the soil splashed over the boards' feet begins
BOARD_STYLE = dict(adze=None, grain=(.0018, (.003, .009)), pits=((.006, .06), .006, .08, .12),
                   arris=(.030, .016), cup=.010)   # split boards: cupped a centimetre, edges well rounded
# The window of HOLZWAND_02 as (u0, u1, v0, v1): a head and a sill across, two jambs, the opening
# between them closed from inside by a shutter of upright boards.
HEAD = (.345, .627, .283, .310)
SILL = (.335, .632, .535, .560)
JAMBS = ((.345, .385, .310, .535), (.600, .627, .310, .535))
OPENING = (.385, .600, .310, .535)
FRAME_PROUD = (.030, .030, .018)     # how far the head, the sill and the jambs stand out of the boards
SHUTTER_Z = -.035                    # the inner shutter, behind the boards; above the void
SHUTTER_BOARDS = 6

# ---------------------------------------------------------------- gable: layout
# Lower edges of the eight rows (v), the u of the butt joints in the row above each edge.
ROWS = [(.156, [.59]), (.262, [.59]), (.385, []), (.535, [.59]), (.638, [.59]), (.765, [.435]), (.858, []),
        (.955, [.545])]
LAP = (.055, .008, .05)              # a board's height over the one below at its lower edge, at its top; overlap
EDGE_WAVE = (.035, (3., 1.5))    # the waney lower edge sweeps: amplitude in metres, wavelengths across
EDGE_BITES = (2, (.008, .025), (.5, 1.2))   # and dips where the log was narrower: count, depth, length
WANE = (.050, .016)                  # the rounded waney edge: width, drop
JOINT_GAP = .004
GABLE_STYLE = dict(adze=None, grain=(.0012, (.004, .012)), pits=((.006, .08), .004, .06, .15),
                   arris=(.004, .002), cup=.0)


def _wave(rng, t, period: float, amplitude: float, lengths) -> np.ndarray:
    """A smooth wandering offset along t, wrapping over PERIOD: sines of about the given wavelengths."""
    out = np.zeros_like(t)
    for k, length in enumerate(lengths):
        n = max(1, round(period / length))
        out += amplitude / (k + 1) ** .6 * rng.uniform(.5, 1) * np.sin(2 * math.pi * n * t / period + rng.uniform(0, 2 * math.pi))
    return out


def _bites(rng, t, period: float, bites=BITES) -> np.ndarray:
    """Waney bites into a board's edge along t: smooth dips of a few centimetres."""
    out = np.zeros_like(t)
    for _ in range(bites[0]):
        c, depth, length = rng.uniform(0, period), rng.uniform(*bites[1]), rng.uniform(*bites[2])
        d = (t - c + period / 2) % period - period / 2
        out += depth * np.clip(1 - (d / (length / 2)) ** 2, 0, 1) ** 1.5
    return out


def _board_edges(rng, ys):
    """x of each gap's middle and its half width along ys, for the ten gaps (wrapping in y)."""
    px, py = PERIOD
    middles, halves = [], []
    for u in EDGES:
        middles.append(u * px + _wave(rng, ys, py, WAVE[0], WAVE[1]))
        halves.append(np.clip(GAP[0] + GAP[1] * np.tanh(_wave(rng, ys, py, 1., (.6, .25))), .002, .012))
    return middles, halves


def _face_along_y(rng, local, gy, width, style, step):
    """Heights of a board face with its grain along y; LOCAL is the distance from its left edge."""
    return tb._hewn(rng, local, gy, width, PERIOD[1], step, **style)


def wall(seed: int, window: bool):
    """Layout of NW_CITY_HAFEN_HOLZWAND_01 (or _02 with the window): (spec, points, grids)."""
    rng = np.random.default_rng([seed, 11])                  # the same boards for both textures
    px, py = PERIOD
    rows = round(py / STEP[1])
    ys = np.arange(rows + 1) * STEP[1]
    middles, halves = _board_edges(rng, ys)
    pieces, grids, nails, cracks, knots = [], {}, [], [], []
    for i in range(len(EDGES)):
        j = (i + 1) % len(EDGES)
        shift = px if j == 0 else 0.                         # the last board crosses the seam
        left = middles[i] + halves[i] + _bites(rng, ys, py)
        right = middles[j] + shift - halves[j] - _bites(rng, ys, py)
        x0 = math.floor((left.min() - .02) / STEP[0]) * STEP[0]
        cols = round((right.max() + .02 - x0) / STEP[0]) + 1
        gx, gy = np.meshgrid(x0 + np.arange(cols) * STEP[0], ys)
        width = (right - left)[:, None]
        local = gx - left[:, None]
        face = _face_along_y(rng, local, gy, np.broadcast_to(width, gx.shape), BOARD_STYLE, STEP)
        # every board sits a little differently: proud or sunk, leaning across and along
        face += rng.normal(0, .006) + rng.normal(0, .03) * (local - width / 2) + rng.normal(0, .002) * gy \
            + rng.normal(0, .02) * (local - width / 2) * (gy / py - .5)          # and twisted
        # knots and short checks along the grain, in the board's middle
        u_mid = ((EDGES[i] + EDGES[j] + (1 if j == 0 else 0)) / 2) % 1
        board_knots = [dict(v=float(rng.uniform(.05, .8)), u=float((u_mid + rng.normal(0, .015)) % 1),
                            r=float(rng.uniform(.010, .022))) for _ in range(rng.integers(2, 4))]
        board_cracks = []
        for _ in range(rng.integers(1, 3)):
            v0, u0 = rng.uniform(.02, .7), (u_mid + rng.normal(0, .012)) % 1
            length = rng.uniform(.06, .22)
            board_cracks.append(dict(path=[(v0, u0), (v0 + length / 2, u0 + rng.normal(0, .002)), (v0 + length, u0)],
                                     width=float(rng.uniform(.003, .006)), depth=.012))
        dh, _ = tb._knots(gx % px, gy, PERIOD, board_knots)
        crack, lips = tb._cracks(gx % px, gy, rng, PERIOD, board_cracks, tb.WIGGLE)
        face += dh - crack - .001 * lips
        knots += board_knots
        cracks += board_cracks
        # nails on the rails: one or two a board, not in a row
        for v in RAILS:
            for _ in range(rng.choice([1, 1, 2, 0], p=[.45, .2, .3, .05])):
                u = (u_mid + rng.uniform(-.3, .3) * (EDGES[j] + (1 if j == 0 else 0) - EDGES[i])) % 1
                vv = v + rng.normal(0, .012)
                nails.append([float(u), float(vv)])
                d = np.hypot((gx % px) - u * px, gy - (1 - vv) * py) / NAIL[0]
                face += NAIL[1] * np.clip(1 - d ** 2, 0, 1) ** .5 - .0015 * ((d > 1) & (d < 1.6))
        if i == JOINT['board']:                              # the short board on top: a gap across, a step
            yj = (1 - JOINT['v']) * py
            face += np.where(gy > yj, rng.normal(.003, .001), 0.)
            face = np.where(np.abs(gy - yj) < JOINT['gap'] / 2, DROP, face)
        face = np.where((local > 0) & (local < width), face, DROP)
        if window:                                           # the boards are cut away behind the window
            u0, u1, v0, v1 = OPENING
            inside = ((gx % px) > u0 * px) & ((gx % px) < u1 * px) & (gy > (1 - v1) * py) & (gy < (1 - v0) * py)
            face = np.where(inside, DROP, face)
        index = len(pieces)
        pieces.append(pb.grid_piece(index, (x0, 0), STEP, [0, 0, 0], WOOD, [rng.uniform(0, 1), rng.uniform(0, 1), 0.],
                                    copy_offsets=[[dx, dy] for dx in (-px, 0, px) for dy in (-py, 0, py)],
                                    kind='board', board=i))
        grids[index] = face
    spec = dict(tool='scripts/content/hut_bake.py', texture='NW_CITY_HAFEN_HOLZWAND_02.TGA' if window
                else 'NW_CITY_HAFEN_HOLZWAND_01.TGA', wall='window' if window else 'wall', seed=seed,
                period=list(PERIOD), camera=dict(tilt=0.), sky=dict(pitch=0., ground=.08),
                ao=dict(distance=.06, crevice=.012), surfaces=SURFACES, void=dict(z=VOID_Z),
                edges=EDGES, rails=list(RAILS), nails=nails, knots=knots, cracks=cracks, ground=GROUND, pieces=pieces)
    if window:
        _window(rng, pieces, grids)
        spec['window'] = dict(head=HEAD, sill=SILL, jambs=JAMBS, opening=OPENING)
    return spec, {}, grids


def _rect_grid(rng, rect, proud: float, along_x: bool, slope=(0., 0.)):
    """A frame board over RECT (u0, u1, v0, v1): its origin, heights and size. A hewn sawn face standing
    PROUD out of the boards, a little crooked (slope across and along, metres per metre)."""
    px, py = PERIOD
    u0, u1, v0, v1 = rect
    x0, y0 = u0 * px, (1 - v1) * py
    cols, rows = round((u1 - u0) * px / STEP[0]) + 1, round((v1 - v0) * py / STEP[1]) + 1
    gx, gy = np.meshgrid(np.arange(cols) * STEP[0], np.arange(rows) * STEP[1])
    w, h = gx.max(), gy.max()
    style = dict(adze=None, grain=(.0007, (.003, .008)), pits=((.006, .05), .0025, .06, .12), arris=(.008, .006), cup=.0)
    if along_x:
        face = tb._hewn(rng, gy.T, gx.T, h, w, STEP[::-1], **style).T
    else:
        face = tb._hewn(rng, gx, gy, w, h, STEP, **style)
    face += proud + slope[0] * (gx - w / 2) + slope[1] * (gy - h / 2)
    return (x0, y0), face


def _window(rng, pieces, grids):
    """The frame and the shutter of HOLZWAND_02's window as pieces after the boards."""
    px, py = PERIOD
    parts = [('head', HEAD, FRAME_PROUD[0], True, (.012, 0.)), ('sill', SILL, FRAME_PROUD[1], True, (-.015, .08)),
             ('jamb', JAMBS[0], FRAME_PROUD[2], False, (0., .01)), ('jamb', JAMBS[1], FRAME_PROUD[2], False, (0., -.008))]
    for kind, rect, proud, along_x, slope in parts:
        origin, face = _rect_grid(rng, rect, proud, along_x, slope)
        index = len(pieces)
        pieces.append(pb.grid_piece(index, origin, STEP, [0, 0, 0], FRAME, [rng.uniform(0, 1), rng.uniform(0, 1),
                                    math.pi / 2 if along_x else 0.], copy_offsets=[[0, 0]], kind=kind))
        grids[index] = face
    # the inner shutter: upright boards with narrow dark joints, set back behind the wall
    u0, u1, v0, v1 = OPENING
    origin, face = _rect_grid(rng, (u0 - .01, u1 + .01, v0 - .01, v1 + .01), SHUTTER_Z, False)
    gx = np.arange(face.shape[1]) * STEP[0]
    width = (u1 - u0 + .02) * px / SHUTTER_BOARDS
    joints = np.abs((gx + rng.uniform(0, width)) % width - width / 2) > width / 2 - .003
    face = np.where(joints[None, :], SHUTTER_Z - .015, face)
    index = len(pieces)
    pieces.append(pb.grid_piece(index, origin, STEP, [0, 0, 0], SHUTTER, [rng.uniform(0, 1), rng.uniform(0, 1), 0.],
                                copy_offsets=[[0, 0]], kind='shutter'))
    grids[index] = face


def gable(seed: int):
    """Layout of NW_CITY_HAFEN_GIEBEL_01: (spec, points, grids)."""
    rng = np.random.default_rng([seed, 13])
    px, py = PERIOD
    cols = round(px / STEP[0])
    xs = np.arange(cols + 1) * STEP[0]
    # lower edges from the bottom of the square up, with their waves (wrapping in x)
    rows = sorted(((1 - v) * py, joints) for v, joints in ROWS)
    edges = [y + _wave(rng, xs, px, EDGE_WAVE[0], EDGE_WAVE[1]) + _bites(rng, xs, px, EDGE_BITES) for y, _ in rows]
    pieces, grids, knots, cracks = [], {}, [], []
    for k, (y, joints) in enumerate(rows):
        upper = edges[(k + 1) % len(rows)] + (py if k + 1 == len(rows) else 0)
        y0 = math.floor((edges[k].min() - .06) / STEP[1]) * STEP[1]
        n = round((upper.max() + LAP[2] - y0) / STEP[1]) + 1
        gx, gy = np.meshgrid(xs, y0 + np.arange(n) * STEP[1])
        u_knots = [dict(v=float(1 - (y + rng.uniform(.06, .2)) / py) % 1, u=float(rng.uniform(0, 1)),
                        r=float(rng.uniform(.012, .025))) for _ in range(rng.integers(1, 4))]
        dh, _ = tb._knots(gx % px, gy % py, PERIOD, u_knots)
        board_cracks = []
        for _ in range(rng.integers(0, 2)):
            u0, v0 = rng.uniform(0, .8), 1 - (y + rng.uniform(.05, .15)) / py
            board_cracks.append(dict(path=[(v0, u0), (v0 + rng.normal(0, .004), u0 + .06), (v0, u0 + .14)],
                                     width=float(rng.uniform(.003, .005)), depth=.010))
        crack, lips = tb._cracks(gx % px, gy % py, rng, PERIOD, board_cracks, tb.WIGGLE)
        knots += u_knots
        cracks += board_cracks
        # butt joints split the row into boards. Every board is its own: its lower edge leans its own way
        # and wanders on its own, so at a joint the edges of the two boards meet with a step
        cuts = sorted(joints)
        spans = [(cuts[i], cuts[(i + 1) % len(cuts)] + (1 if i + 1 == len(cuts) else 0)) for i in range(len(cuts))] \
            if cuts else [(0., 1.)]
        for a, b in spans:
            uu = xs / px
            along = ((uu - a) % 1) / (b - a)                 # 0..1 along this board from the joint
            lower = edges[k].copy()
            if cuts:
                lower += rng.normal(0, .02) * (along - .5) + rng.normal(0, .008) \
                    + _wave(rng, xs, px, .008, (.9, .4)) * np.sin(math.pi * np.clip(along, 0, 1))
            above = gy - lower[None, :]                      # height over this board's own lower edge
            visible = (upper - lower)[None, :]
            # lapped: thick at the lower edge where it lies on the board below, thin under the next one
            thick = LAP[0] * (1 + .3 * np.tanh(_wave(rng, xs, px, 1.5, (3., 1.5))))[None, :]   # an uneven edge
            lap = LAP[1] + (thick - LAP[1]) * np.clip(1 - above / (visible + LAP[2]), 0, 1)
            # face with the grain along x: build it turned
            grain = tb._hewn(rng, above.T, gx.T, (visible + LAP[2]).T * np.ones_like(above.T), px, STEP[::-1],
                             **GABLE_STYLE).T
            face = lap + grain + rng.normal(0, .002) + rng.normal(0, .002) * np.sin(2 * math.pi * gx / px + rng.uniform(0, 6.3))
            # the waney lower edge: rounded off and a little ragged, bark long gone
            wane = WANE[0] * (1 + .5 * np.tanh(_wave(rng, xs, px, 1., (.4, .15))))[None, :]
            face -= WANE[1] * np.clip(1 - above / wane, 0, 1) ** 2
            face += dh - crack - .001 * lips
            inside = above >= 0
            if cuts:
                gap = JOINT_GAP / 2 / px
                u2 = (gx / px - a) % 1
                inside &= (u2 > gap) & (u2 < (b - a) - gap)
            index = len(pieces)
            pieces.append(pb.grid_piece(index, (0, y0), STEP, [0, 0, 0], WOOD,
                                        [rng.uniform(0, 1), rng.uniform(0, 1), math.pi / 2],
                                        copy_offsets=[[dx, dy] for dx in (-px, 0, px) for dy in (-py, 0, py)],
                                        kind='board', row=k))
            grids[index] = np.where(inside, face, DROP)
    spec = dict(tool='scripts/content/hut_bake.py', texture='NW_CITY_HAFEN_GIEBEL_01.TGA', wall='gable', seed=seed,
                period=list(PERIOD), camera=dict(tilt=0.), sky=dict(pitch=0., ground=.08),
                ao=dict(distance=.06, crevice=.012), surfaces=SURFACES, void=dict(z=VOID_Z),
                rows=[[1 - y / py, j] for y, j in rows], knots=knots, cracks=cracks, pieces=pieces)
    return spec, {}, grids


LAYOUTS = {'wall': lambda seed: wall(seed, False), 'window': lambda seed: wall(seed, True), 'gable': gable}


# ---------------------------------------------------------------- weathering (compose)
# Measured on the originals at their own size (sRGB). The wall: boards from 53/47/26 (dark, olive) to
# 92/85/66 (weathered grey), mean 68/62/43, 57% of the pixels greenish; the window's opening 23/22/12,
# its jambs light grey (105/96/78), head and sill as the boards; soil over the boards' feet from v .84;
# the top darker under the eaves. The gable: mean 71/67/47, 66% greenish, a dark line under every row.
def _linear(srgb) -> np.ndarray:
    return (np.asarray(srgb, dtype=np.float64) / 255) ** 2.2


LOOKS = {
    'wall': dict(dark=(42, 38, 18), light=(106, 99, 78), mean=(68, 62, 43), median=61., desaturate=.6, grain=1.3,
                 streaks=((.015, .5), .5), moss=(.26, (.035, .45), (42, 56, 20)), eaves=(.07, .5), contrast=1.35,
                 soil=((70, 64, 50), (.05, .012)), shadow=((60, 50, 14), 1.), foot=((54, 64, 26), (.16, .22)), opening=(23, 22, 12), jamb=(105, 96, 78), shade=None),
    'gable': dict(dark=(48, 45, 26), light=(112, 108, 94), mean=(71, 67, 47), median=71., desaturate=.65, grain=1.0,
                  shadow=((60, 52, 14), 1.), weather=(.45, .75), streaks=((.4, .015), .12), moss=(.17, (.04, .45), (46, 62, 22)), eaves=None, soil=None, contrast=1.2,
                  shade=((22., 28.), .12, .95)),
}
LOOKS['window'] = LOOKS['wall']
OCCLUSION = (1.4, 2.4)
SHUTTER_LIFT = 2.4                   # the shutter's boards show in the original's opening: not a black hole
ROUGH = ((.86, .95), .78, .95)       # dry wood, damp wood, moss and soil


def compose(passes: dict, spec: dict, seed: int = 0) -> dict:
    """Albedo (linear RGB) and masks of one of the hut's textures."""
    look = LOOKS[spec['wall']]
    rng = np.random.default_rng([seed, 21])
    piece = passes['piece']
    rows, cols = piece.shape
    period = spec['period']
    px, py = period
    kinds = {p['id']: p['kind'] for p in spec['pieces']}
    kind = np.vectorize(lambda i: kinds.get(i, 'void'))(piece)
    wood = np.isin(kind, ['board', 'head', 'sill', 'jamb'])
    shutter = kind == 'shutter'
    x, y = passes['x'] % px, passes['y'] % py
    u, v = x / px, 1 - y / py

    # the generated wood's light, in the original's colour: every board between its dark olive and its
    # weathered grey, by a weathering of its own
    colour = passes['colour']
    lum = colour @ tb.LUM
    rel = lum / np.median(lum[wood])
    weather = np.zeros((rows, cols))
    for p in spec['pieces']:
        lo, hi = look.get('weather', (0., 1.))   # how far the boards' own weathering spreads
        weather[piece == p['id']] = lo + (hi - lo) * rng.uniform(0, 1) ** .9
    weather += .12 * pb.noise_metres(rng, (rows, cols), period, .4)
    weather = np.clip(weather, 0, 1)
    dark, light = _linear(look['dark']), _linear(look['light'])
    base = dark[None, None] * (1 - weather[..., None]) + light[None, None] * weather[..., None]
    grain = np.maximum(rel, 1e-6) ** look['grain']
    grey = (colour - lum[..., None]) * (1 - look['desaturate'])
    out = base * grain[..., None] + grey * .15 * np.median(base[wood] @ tb.LUM) / max(np.median(lum[wood]), 1e-6)

    # rain streaks and damp: long along the boards (across for the gable's laps)
    (across, along), strength = look['streaks']
    fx, fy = px / across, py / along
    streak = pb.periodic_noise(rng, (rows, cols), fx, fy)
    out *= np.exp(strength * np.tanh(streak))[..., None]
    damp = pb.noise_metres(rng, (rows, cols), period, .15) + .5 * pb.noise_metres(rng, (rows, cols), period, .05)
    out *= np.exp(.12 * damp)[..., None]
    if spec.get('knots'):
        _, knots = tb._knots(x, y, period, spec['knots'])
        out *= (1 - .45 * knots)[..., None]

    # moss: in the shade of the gaps and the laps, in streaks, more of it low down (the sea air, the
    # splash); a green tint over all of it, dense patches on part
    share, sizes, moss_srgb = look['moss']
    shade = 1 - np.clip(passes['crevice'], 0, 1)
    across, along = sizes                    # runs along the grain: down the boards, along the laps
    fx, fy = (px / across, py / along) if spec['wall'] != 'gable' else (px / along, py / across)
    field = pb.periodic_noise(rng, (rows, cols), fx, fy) + .5 * pb.noise_metres(rng, (rows, cols), period, .08)
    field += 2.5 * shade
    if spec['wall'] != 'gable':              # the wall's foot is wetter; the gable's rows are all alike
        field += 1.2 * np.clip(v - .4, 0, 1)
    moss = np.clip((field - np.quantile(field[wood], 1 - share)) * .7 + .4, 0, 1) * wood
    green = _linear(moss_srgb) * np.exp(.25 * pb.noise_metres(rng, (rows, cols), period, .03))[..., None]
    out = out * (1 - .75 * moss[..., None]) + green * .75 * moss[..., None]

    if spec.get('nails'):                    # iron heads, a rust run below each
        for nu, nv in spec['nails']:
            dx = (u - nu + .5) % 1 - .5
            dy = v - nv
            head = np.hypot(dx * px, dy * py) < .006
            run = (np.abs(dx * px) < .004 * (1 + dy * py * 3)) & (dy > 0) & (dy * py < .08)
            out[run] *= _linear((60, 42, 25)) / np.maximum(out[run], 1e-6).mean(axis=0) * .4 + .6
            out[head] = _linear((30, 27, 22))
    if look.get('eaves'):                    # the top of the wall under the eaves stays dark and damp
        reach, factor = look['eaves']
        out *= (1 - (1 - factor) * np.clip(1 - v / reach, 0, 1) ** 1.5)[..., None]
    soil = np.zeros((rows, cols))
    if look.get('soil'):                     # soil splashed over the boards' feet, ragged at its top
        soil_srgb, (size, fine) = look['soil']
        edge = spec['ground'] + .03 * pb.noise_metres(rng, (rows, cols), period, size) \
            + .015 * pb.noise_metres(rng, (rows, cols), period, fine)
        soil = np.clip((v - edge) / .015, 0, 1)
        soil *= np.clip(1.1 + .5 * pb.noise_metres(rng, (rows, cols), period, .03), 0, 1)
        speck = pb.noise_metres(rng, (rows, cols), period, .007) + .6 * pb.noise_metres(rng, (rows, cols), period, .02)
        grit = np.clip(pb.noise_metres(rng, (rows, cols), period, .006) - .8, 0, 1)       # light pebbles
        dirt = _linear(soil_srgb) * np.exp(.9 * np.tanh(speck))[..., None] * (1 + 2. * grit)[..., None]
        out = out * (1 - soil[..., None]) + dirt * soil[..., None]
        # the wet foot of the wall goes green: moss up the boards from the soil and tufts in it
        foot_srgb, (reach, share) = look['foot']
        rise = np.clip((v - (spec['ground'] - reach)) / reach, 0, 1)
        field = pb.periodic_noise(rng, (rows, cols), px / .03, py / .25) + .6 * pb.noise_metres(rng, (rows, cols), period, .01)
        foot = np.clip((field + 2.2 * rise - np.quantile(field, 1 - share) - 1.1) * .8, 0, 1) * (wood | (soil > 0))
        foot *= 1 - .95 * soil * (speck < 1.2)                # on the soil only in tufts: the gravel shows
        green = _linear(foot_srgb) * np.exp(.3 * np.tanh(speck))[..., None]
        out = out * (1 - foot[..., None]) + green * foot[..., None]
        moss = np.maximum(moss, foot)
    if 'window' in spec:                     # the jambs weathered light, the shutter almost black
        jamb = np.isin(kind, ['jamb', 'head'])
        out[jamb] = out[jamb] / np.median(out[jamb] @ tb.LUM) * (_linear(look['jamb']) @ tb.LUM) * .5 \
            + _linear(look['jamb']) * .5
        out[shutter] = _linear(look['opening']) * SHUTTER_LIFT * (lum[shutter] / np.median(lum[shutter]))[..., None] ** .5

    lum_out = out @ tb.LUM
    median = np.median(lum_out[wood])
    gain = (np.maximum(lum_out, 1e-6) / median) ** (look['contrast'] - 1)
    out = np.where(wood[..., None], out * gain[..., None], out)
    occlusion = np.clip(passes['ao'], 0, 1) ** OCCLUSION[0] * np.clip(passes['crevice'], 0, 1) ** OCCLUSION[1]
    # the shutter is lit only through the opening, its own occlusion is in its measured tone; the soil
    # splashed over the boards' feet fills the gaps too
    occlusion = np.maximum(occlusion, .75 * soil)
    out *= np.where(shutter, np.sqrt(occlusion), occlusion)[..., None]
    if look.get('shade'):                    # the sky from above is cut off under every lapped edge
        elevations, reach, depth = look['shade']
        shade = pb.overhang_shade(passes['height'], py / rows, elevations, reach, softness=.002)
        out *= (1 - depth * shade)[..., None]
    out[(piece < 0) & (soil < .5)] = .002    # the dark behind the gaps, above the soil
    # the dark of the original's wood is olive brown (sRGB B/R .39 against .78 of its light boards): the
    # shade, the damp and the dirt take that hue, the light stays grey
    lum_out = out @ tb.LUM
    median = np.median(lum_out[wood])
    olive = _linear(look['shadow'][0])
    olive = olive / (olive @ tb.LUM)
    w = look["shadow"][1] * np.clip(1.6 * (1 - lum_out / median), 0, 1)[..., None] * (wood | (soil > 0))[..., None]
    out = out * (1 - w) + lum_out[..., None] * olive * w
    # the original's median light over the whole square (sRGB luminance); the gaps and the shutter keep
    # their dark
    target = (look['median'] / 255) ** 2.2
    out[wood | (soil > 0)] *= target / np.median(out @ tb.LUM)

    normal = passes['normal'] / (np.linalg.norm(passes['normal'], axis=-1, keepdims=True) + 1e-9)
    wet = np.clip(-damp * .6, 0, 1) * wood
    dry = ROUGH[0][0] + (ROUGH[0][1] - ROUGH[0][0]) * np.clip(.5 + .3 * pb.noise_metres(rng, (rows, cols), period, .1), 0, 1)
    rough = np.where(wood, dry - (dry - ROUGH[1]) * wet, 1.)
    rough = np.maximum(rough, ROUGH[2] * np.maximum(moss, soil))
    rough = np.where(piece < 0, 1., rough)
    crack = wood & (passes['crevice'] < .35)
    return dict(albedo=np.clip(out, 0, 1), roughness=rough, normal=normal, crack=crack.astype(float), moss=moss,
                soil=soil)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    p = sub.add_parser('layout')
    p.add_argument('texture', choices=sorted(LAYOUTS))
    p.add_argument('out', type=Path)
    p.add_argument('--seed', type=int, default=0)
    p = sub.add_parser('render')
    p.add_argument('layout', type=Path)
    p.add_argument('out_dir', type=Path)
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
        spec, points, grids = LAYOUTS[args.texture](args.seed)
        pb.write_layout(args.out, spec, points, grids)
        print(args.out)
    elif args.command == 'render':
        spec = json.loads(args.layout.read_text())
        surfaces = {}
        if args.wood:
            args.out_dir.mkdir(parents=True, exist_ok=True)
            image = pb.seamless_along(args.wood, args.out_dir / 'surface-wood.png')
            surfaces = {name: image for name in ('wood', 'frame', 'shutter')}
        px, py = spec['period']
        options = (dict(view='street', pitch='90', eaves='.0', stretch='1', close=f'{px * .5},{py * .55}')
                   if args.street else {})
        if args.sun:
            options['sun'] = str(args.sun)
        print(pb.render(args.layout, args.out_dir, surfaces, args.resolution, args.samples, options))
    elif args.command == 'compose':
        spec = json.loads(args.layout.read_text())
        passes = pb.read_passes(args.passes, spec['period'])
        result = compose(passes, spec, args.seed)
        pb.write_maps(args.out, (1024, 1024), result['albedo'], result['normal'],
                      {k: result[k] for k in ('roughness', 'crack', 'moss', 'soil')})
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
