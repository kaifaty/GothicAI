#!/usr/bin/env python3
"""City timber (asset city-timber) built as Blender models and baked into textures, on the shared
piece_bake path (docs/remaster-pipeline.md, «Материал из модели»).

    python3 scripts/content/timber_bake.py layout beam02|beam03|plain|carved|floor OUT.json [--seed N]
    python3 scripts/content/timber_bake.py render OUT.json OUTDIR [--wood PNG] [--street]
    python3 scripts/content/timber_bake.py compose OUT.json OUTDIR/passes.exr MAPS.png [--seed N]
    python3 scripts/content/timber_bake.py blockout OUT.json OUTDIR/passes.exr OUT.png

beam02 is NW_CITY_WOODBEAM_02, the dark frame beam of the harbour huts and the market houses: one face
of a hewn oak beam. Its face is a grid of heights (piece_bake.grid_piece): adze scallops along the
beam, grain, pits, worn arrises and the original's three cracks, deep enough to be black at the
bottom. An iron strap with two nails crosses it near the top.

The other four are WOODS on the same parts: beam03 the ship's beam (NW_CITY_WOODBEAM_03), plain the
plain board (NW_CITY_WOOD_ORNAMENT_02), carved the carved board of Lobart's stable (NW_CITY_WOOD_ORNAMENT_01)
and floor the three boards with gaps (NW_CITY_WOOD_01), each with its look in LOOKS.

The model is laid out in the texture's own proportions, 0.5 × 1.0 "texture metres": the game stretches
this texture along the beam about 3.4 times (intent.md), the same way it stretches the original, so
the nails stay round in the texture as they are in the original. Positions come from the original
measured by Claude on its preview, the carving at the texture's own size and on a 4x view
(original_view.py; texture u from the left, v from the top); no original pixel goes into the model or
a generator.
"""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

import numpy as np

import piece_bake as pb

# ---------------------------------------------------------------- beam02: layout

PERIOD = (.5, 1.)            # texture metres: one face across, the texture's length along
STEP = (.001, .001)          # the face grid: across and along the beam, metres
STRETCH = 3.4                # the game stretches the texture this much along the beam (0.5 × 3.4 m face)
STROKE = ((.09, .07), ((.06, .10), (.045, .075)), (.0015, .004))   # adze strokes: spacing across, along;
                             # half width and half length ranges; depth range, metres
GRAIN = (.0005, (.004, .012))   # fine grain ridges: depth, wavelengths across the beam
PITS = ((.008, .06), .003, .06, .12)   # weathered pockets along the grain: size across and along, depth, share,
                             # and the size of the patches they keep to
ARRIS = (.02, .010)          # the worn corner of the beam at both long edges: width, drop
# The original's cracks, measured on its preview: (v, u) points from the top, a width at the widest in
# metres and a depth. The main crack runs the whole length past the texture's seam with a step near its
# lower end; the second one wanders through the upper half; the third is short, top left.
CRACKS = [
    dict(path=[(.84, .61), (.98, .59), (1.02, .59), (1.05, .53), (1.14, .53), (1.21, .50), (1.28, .49), (1.38, .46),
               (1.47, .46), (1.56, .48), (1.66, .50), (1.75, .48), (1.87, .45)], width=.036, depth=.055),
    dict(path=[(.80, .34), (.87, .33), (.94, .30), (1.0, .35), (1.07, .39), (1.12, .36), (1.21, .33), (1.30, .31),
               (1.40, .30)], width=.021, depth=.045),
    dict(path=[(.93, .12), (1.0, .17), (1.04, .20), (1.10, .20)], width=.010, depth=.025),
    dict(path=[(.99, .66), (1.04, .65), (1.09, .67)], width=.005, depth=.012),
]
WIGGLE = (.0015, .03)        # cracks wander by this much over this length, metres
STRAP = (.219, .266, .006)   # the iron strap: v of its top and bottom edge, thickness in metres
NAILS = ((.16, .81), .0075, .004)  # nails: u of each on the strap's middle line, head radius, dome height
SURFACES = [  # name, plain colour of the block-out (linear), metres per surface image, bump in metres
    dict(name='oak', colour=[.24, .21, .17], metres=1., bump=.0006),
    dict(name='iron', colour=[.08, .08, .08], metres=.5, bump=.0003),
]
OAK, IRON = 0, 1


def _noise(rng, shape, size_x: float, size_y: float, step) -> np.ndarray:
    """Smooth noise wrapping in both axes, unit spread, with features about size_x by size_y metres."""
    rows, cols = shape
    return pb.periodic_noise(rng, shape, cols * step[0] / size_x, rows * step[1] / size_y)


def _cracks(gx, gy, rng, period=PERIOD, cracks=CRACKS, wiggle=WIGGLE) -> tuple[np.ndarray, np.ndarray]:
    """Depth of every crack at the grid (positive down) and a 0..1 map of the crack lips. A crack is a
    path of (v, u) texture points from the top, its widest width and its depth in metres."""
    from scipy.spatial import cKDTree
    px, py = period
    depth = np.zeros_like(gx)
    lips = np.zeros_like(gx)
    for crack in cracks:
        path = np.asarray(crack['path'])
        v = np.interp(np.linspace(0, 1, 2000), np.linspace(0, 1, len(path)), path[:, 0])
        u = np.interp(np.linspace(0, 1, 2000), np.linspace(0, 1, len(path)), path[:, 1])
        x, y = u * px, (1 - v) * py
        s = np.concatenate([[0], np.cumsum(np.hypot(np.diff(x), np.diff(y)))])
        length = s[-1]
        # the line wanders: a smooth random sideways offset, a few wavelengths of WIGGLE[1]
        wander = sum(rng.uniform(.4, 1) * np.sin(2 * math.pi * s / (wiggle[1] * k) + rng.uniform(0, 2 * math.pi))
                     for k in (.6, 1., 1.7))
        x = x + wiggle[0] * wander / 2
        t = s / length
        taper = np.clip(np.minimum(t, 1 - t) / .12, 0, 1) ** .7          # closes at both ends
        swell = 1 + .35 * sum(np.sin(2 * math.pi * t * k + rng.uniform(0, 2 * math.pi)) for k in (2.3, 5.1)) / 2
        if crack.get('through'):                     # runs the whole period and meets itself at the seam
            x = u * px + wiggle[0] * sum(np.sin(2 * math.pi * t * k + rng.uniform(0, 2 * math.pi)) for k in (3, 7, 13)) / 3
            taper = np.ones_like(t)
            swell = 1 + .35 * np.sin(2 * math.pi * t * 2 + rng.uniform(0, 2 * math.pi))
        half = crack['width'] / 2 * taper * swell
        floor = crack['depth'] * np.sqrt(taper)
        points = np.stack([x, y], axis=1)
        tree = cKDTree(np.concatenate([points + [dx, dy] for dx in (-px, 0, px) for dy in (-py, 0, py)]))
        dist, index = tree.query(np.stack([gx.ravel(), gy.ravel()], axis=1), distance_upper_bound=.03)
        near = np.isfinite(dist)
        i = index[near] % len(points)
        d = dist[near]
        h = np.maximum(half[i], 1e-6)
        inside = np.clip(1 - d / h, 0, 1)
        flat_depth = depth.ravel()
        flat_depth[near] = np.maximum(flat_depth[near], floor[i] * inside ** .45)    # steep walls, sharp at the bottom
        depth = flat_depth.reshape(gx.shape)
        flat_lips = lips.ravel()
        flat_lips[near] = np.maximum(flat_lips[near], np.clip(1 - (d - h) / .004, 0, 1) * (d >= h) * taper[i])
        lips = flat_lips.reshape(gx.shape)
    return depth, lips


def _hewn(rng, gx, gy, width: float, length: float, step, adze=STROKE, grain=GRAIN, pits=PITS, arris=ARRIS,
          cup: float = 0.) -> np.ndarray:
    """Heights of a wooden face with its grain along y (rows), x across (columns), wrapping along y:
    adze scallops (adze=None: a sawn or planed face, cupped by CUP metres), grain, weathered pockets
    along the grain, worn arrises at both long edges."""
    if adze:
        # adze: every stroke leaves a shallow dish; the face is what all strokes left (their minimum)
        (sx, sy), ((w0, w1), (l0, l1)), (d0, d1) = adze
        face = np.full(gx.shape, np.inf)
        for cy in np.arange(0, length, sy):
            offset = rng.uniform(0, sx)
            for cx in np.arange(-sx + offset, width + sx, sx):
                x0, y0 = cx + rng.normal(0, sx * .2), cy + rng.normal(0, sy * .25)
                half_w, half_l, deep = rng.uniform(w0, w1), rng.uniform(l0, l1), rng.uniform(d0, d1)
                dy = (gy - y0 + length / 2) % length - length / 2          # the strokes wrap along the beam
                face = np.minimum(face, -deep + deep * (((gx - x0) / half_w) ** 2 + (dy / half_l) ** 2))
        face = np.minimum(face, 0) - face.max()
    else:
        face = -cup * (1 - (2 * gx / width - 1) ** 2)                     # a board cups away from its heart

    # fine grain along the beam, a little wavy
    ridges = _noise(rng, gx.shape, grain[1][0], .25, step) * .6 + _noise(rng, gx.shape, grain[1][1], .5, step) * .4
    face += grain[0] * np.tanh(ridges)

    # weathered pockets: the soft wood between the late-wood ridges washed out, long along the grain
    holes = _noise(rng, gx.shape, *pits[0], step) + .5 * _noise(rng, gx.shape, pits[0][0] * .5, pits[0][1] * .4, step)
    pit = np.clip((holes - np.quantile(holes, 1 - pits[2])) / .9, 0, 1)
    patches = np.clip(_noise(rng, gx.shape, pits[3], pits[3] * 1.5, step) * 1.5 + .3, 0, 1)
    face -= pits[1] * pit * patches

    # worn arrises at both long edges, unevenly
    wear = 1 + .4 * _noise(rng, gx.shape, .5, .12, step)
    edge = np.minimum(gx, width - gx)
    face -= arris[1] * wear * np.clip(1 - edge / (arris[0] * wear), 0, 1) ** 2
    return face


def _strap(rng, face, bridged, gx, gy, step, length: float, strap):
    """A thin iron sheet across the face over its high points, from v STRAP[0] to STRAP[1] (from the
    top): (first row, heights). BRIDGED is the face without its cracks, which the strap spans."""
    from scipy.ndimage import maximum_filter, uniform_filter
    top, bottom = (1 - strap[0]) * length, (1 - strap[1]) * length
    r0, r1 = int(bottom / step[1]) - 2, int(top / step[1]) + 3
    bed = uniform_filter(maximum_filter(bridged[r0:r1], size=(1, 41)), size=(1, 21))
    ys = gy[r0:r1]
    ragged = .0006 * _noise(rng, gx.shape, .03, .5, step)[r0:r1]
    inside = np.minimum(ys - bottom - ragged, top + ragged - ys)
    dents = .0003 * _noise(rng, gx.shape, .015, .015, step)[r0:r1]
    sheet = bed + strap[2] + dents - .002 * np.clip(1 - inside / .004, 0, 1) ** 2
    return r0, np.where(inside > 0, sheet, bed - .01)                  # off the strap: under the face


def _nails(rng, pieces, points, sheet, r0, step, period, strap, nails, surface):
    """Dome-headed nails on the strap's middle line at the u of NAILS[0]."""
    from shapely.geometry import Point
    px, py = period
    middle = (1 - (strap[0] + strap[1]) / 2) * py
    for u in nails[0]:
        x = u * px
        row = int(middle / step[1]) - r0
        z = sheet[row, int(x / step[0])]
        head = Point(x, middle).buffer(nails[1], 16)
        index = len(pieces)
        pieces.append(pb.piece(index, head, [z - .003, 0, 0], [z + nails[2] - .001, 0, 0], surface,
                               [rng.uniform(0, 1), rng.uniform(0, 1), 0],
                               [dict(kind='round', width=nails[1] * .9, drop=nails[2] * .8)], [[0, 0]],
                               kind='nail', closed=True))
        points[index] = pb.mesh_points(head, None, .0012, .0012, rings=(.001, .003, .006))


def beam02(seed: int):
    """Layout of NW_CITY_WOODBEAM_02: (spec, points, grids)."""
    rng = np.random.default_rng([seed, 2])
    px, py = PERIOD
    cols, rows = round(px / STEP[0]) + 1, round(py / STEP[1])
    gx, gy = np.meshgrid(np.arange(cols) * STEP[0], np.arange(rows) * STEP[1])
    face = _hewn(rng, gx, gy, px, py, STEP)
    crack, lips = _cracks(gx, gy, rng)
    face -= crack + .0012 * lips                                       # the lips are a little eroded
    r0, strap = _strap(rng, face, face + crack + .0012 * lips, gx, gy, STEP, py, STRAP)

    pieces, points, grids = [], {}, {}
    pieces.append(pb.grid_piece(0, (0, 0), STEP, [0, 0, 0], OAK, [rng.uniform(0, 1), rng.uniform(0, 1), 0.],
                                copy_offsets=[[0, -py], [0, 0], [0, py]], kind='face'))
    grids[0] = np.concatenate([face, face[:1]])                        # the last row closes the period
    pieces.append(pb.grid_piece(1, (0, r0 * STEP[1]), STEP, [0, 0, 0], IRON, [0, 0, 0], copy_offsets=[[0, 0]],
                                kind='strap'))
    grids[1] = strap
    _nails(rng, pieces, points, strap, r0, STEP, PERIOD, STRAP, NAILS, IRON)

    spec = dict(tool='scripts/content/timber_bake.py', texture=TEXTURES['beam02'], wood='beam02', seed=seed,
                period=list(PERIOD), camera=dict(tilt=0.), sky=dict(pitch=0., ground=.08),
                ao=dict(distance=.05, crevice=.012), surfaces=SURFACES,
                strap=dict(v=[STRAP[0], STRAP[1]]), nails=list(NAILS[0]),
                cracks=[dict(path=c['path'], width=c['width'], depth=c['depth']) for c in CRACKS], pieces=pieces)
    return spec, points, grids


# ---------------------------------------------------------------- beam02: weathering (compose)
# The original, measured on its preview: very dark olive-brown (sRGB 41/35/21, G/R .85, B/R .49), no
# green at all; blotchy damp (block spread of 16 px blocks 9.6 against a pixel spread of 15.4); darker
# above the strap than below it; 7% of the face almost black, the cracks; the strap in the wood's own
# dark olive, told apart by its edges, with light worn nail heads. Colours are linear.
LUM = np.array([.2126, .7152, .0722])
OLIVE = np.array([1., .70, .21])       # channel ratios of the original's olive brown, linear
GREY_OLIVE = np.array([1., .77, .30])  # the light streaks, weathered grey-olive (sRGB 1 : .89 : .58)
PALE_OLIVE = np.array([1., .80, .33])  # the pale weathered patches (sRGB 1 : .90 : .60); the colour
                                       # correction to the original's target adds blue, bluer read as smoke
WOOD_DESATURATE = .55                  # share of the generated oak's own colour removed
GRAIN_CONTRAST = .55                   # power on the oak's own light: its grain is stripier than the original
GREY_LIGHT = .55                       # the lighter wood is weathered grey-olive, not yellow: share of grey at the top
MOTTLE = ((.08, .03), .14, (1.5, .55))   # damp and weathered patches with sharp edges: sizes in metres,
                                       # share of each, light and dark factor
DAMP = (.12, .05, .38)                 # damp blotches: sizes in metres, strength (log spread)
ABOVE = (.62, .03)                     # the wood above the strap is darker: factor, soft edge in metres
OCCLUSION = (.6, 2.2)                  # powers of the AO and the crevice AO: the cracks go black
CRACK_SIDE = (.015, .8)                # near a crack the wood is damper: reach in metres, factor
STRAP_TONE = (.7, 2.6)                 # the strap against the wood's median; its worn edges lighter
IRON_COLOUR = np.array([1., .79, .25]) # the strap's dark olive iron (the original's strap: sRGB 33/30/15)
CRACK_BLACK = (.15, .6, .06)           # the crack's inside goes black: crevice AO from, to, darkest factor
NAIL = np.array([.16, .15, .12])       # the worn nail heads
RUST = (.18, .03, .5, np.array([.045, .026, .012]))   # streaks under the strap: reach, column size, share, colour
CONTRAST = 1.3
ROUGH = ((.86, .95), .80, .72, .6)     # dry wood range, damp wood (no sky highlight: it fades out at 0.8), the strap, the nail heads


def _kinds(spec, piece):
    return np.array([p['kind'] for p in spec['pieces']] + ['none'])[piece]


def compose(passes: dict, spec: dict, seed: int = 0) -> dict:
    """Albedo (linear RGB) and the masks it was made with, at the render size."""
    rng = np.random.default_rng([seed, 7])
    piece = passes['piece']
    rows, cols = piece.shape
    period = spec['period']
    px, py = period
    kind = _kinds(spec, piece)
    face, strap, nail = kind == 'face', kind == 'strap', kind == 'nail'

    # the generated oak's light and grain, in the original's olive brown
    colour = passes['colour']
    lum = colour @ LUM
    grey = lum[..., None]
    wood = grey + (colour - grey) * (1 - WOOD_DESATURATE)
    ratio = np.median(wood[face], axis=0)
    wood = wood / (ratio / ratio[0]) * OLIVE
    rel = lum / np.median(lum[face])
    wood *= (np.maximum(rel, 1e-6) ** (GRAIN_CONTRAST - 1))[..., None]     # calmer grain
    light = np.clip((rel - 1) / 1.5, 0, 1) * GREY_LIGHT                    # light streaks go grey-olive
    wood = wood * (1 - light[..., None]) + (wood @ LUM)[..., None] * GREY_OLIVE / (GREY_OLIVE @ LUM) * light[..., None]
    mottle = pb.noise_metres(rng, (rows, cols), period, MOTTLE[0][0]) + .5 * pb.noise_metres(rng, (rows, cols), period,
                                                                                         MOTTLE[0][1])
    mottle += .25 * pb.noise_metres(rng, (rows, cols), period, .008)            # ragged edges
    pale = np.clip((mottle - np.quantile(mottle, 1 - MOTTLE[1])) * 10, 0, 1)
    stain = np.clip((np.quantile(mottle, MOTTLE[1]) - mottle) * 10, 0, 1)
    pale_colour = (wood @ LUM)[..., None] * PALE_OLIVE / (PALE_OLIVE @ LUM)
    wood = wood * (1 - pale[..., None]) + pale_colour * MOTTLE[2][0] * pale[..., None]
    wood *= (1 - (1 - MOTTLE[2][1]) * stain)[..., None]

    damp = sum(w * pb.noise_metres(rng, (rows, cols), period, size) for w, size in zip((.75, .45), DAMP[:2]))
    wood *= np.exp(DAMP[2] * damp)[..., None]
    y = passes['y'] % py
    strap_top, strap_bottom = (1 - spec['strap']['v'][0]) * py, (1 - spec['strap']['v'][1]) * py
    above = np.clip((y - strap_top) / ABOVE[1] + .5, 0, 1)
    wood *= (1 - (1 - ABOVE[0]) * above)[..., None]

    # near a crack the wood stays damp and dark
    from scipy.ndimage import distance_transform_edt
    crack = face & (passes['crevice'] < .35)
    near = distance_transform_edt(~np.tile(crack, (3, 1)))[rows:2 * rows] * (px / cols)
    side = np.clip(1 - near / CRACK_SIDE[0], 0, 1) * np.clip(.6 + .5 * pb.noise_metres(rng, (rows, cols), period, .03), 0, 1)
    wood *= (1 - (1 - CRACK_SIDE[1]) * side)[..., None]

    # rust and dirt washed down from the strap, in columns
    below = strap_bottom - y
    columns = pb.periodic_noise(rng, (1, cols), px / RUST[1], 1)[0]
    columns = np.clip((columns - np.quantile(columns, 1 - RUST[2])) * 1.5, 0, 1)
    streak = face * (below > 0) * np.exp(-np.maximum(below, 0) / (RUST[0] / 3)) * columns[None, :]
    wood = wood * (1 - .6 * streak[..., None]) + RUST[3] * .6 * streak[..., None]

    out = np.where(face[..., None], wood, 0.)
    median = np.median((wood @ LUM)[face])
    # the strap: the wood's dark olive, its rounded edges worn lighter
    normal = passes['normal'] / (np.linalg.norm(passes['normal'], axis=-1, keepdims=True) + 1e-9)
    edge = np.clip((1 - normal[..., 2]) * 6, 0, 1)
    iron = IRON_COLOUR / (IRON_COLOUR @ LUM) * median * STRAP_TONE[0] * (1 + (STRAP_TONE[1] - 1) * edge)[..., None]
    pitting = pb.noise_metres(rng, (rows, cols), period, .003) + .6 * pb.noise_metres(rng, (rows, cols), period, .012)
    iron *= np.exp(.28 * np.tanh(pitting))[..., None]                      # hammered and pitted
    scale = np.clip((pb.noise_metres(rng, (rows, cols), period, .01) - 1.) * 2, 0, 1)
    iron = iron * (1 - .5 * scale[..., None]) + RUST[3] * .7 * scale[..., None]   # rust scale
    out = np.where(strap[..., None], iron, out)
    out = np.where(nail[..., None], NAIL * (1 + .5 * edge)[..., None], out)

    occlusion = np.clip(passes['ao'], 0, 1) ** OCCLUSION[0] * np.clip(passes['crevice'], 0, 1) ** OCCLUSION[1]
    inside = np.clip((CRACK_BLACK[1] - passes['crevice']) / (CRACK_BLACK[1] - CRACK_BLACK[0]), 0, 1) * face
    out *= (occlusion * (1 - (1 - CRACK_BLACK[2]) * inside))[..., None]
    luminance = out @ LUM
    mean = np.median(luminance[face])
    out *= (((np.maximum(luminance, 1e-6) / mean) ** CONTRAST * mean) / np.maximum(luminance, 1e-6))[..., None]

    wet = np.clip(np.maximum(-damp * .6, side), 0, 1) * face
    dry = ROUGH[0][0] + (ROUGH[0][1] - ROUGH[0][0]) * np.clip(.5 + .3 * pb.noise_metres(rng, (rows, cols), period, .1), 0, 1)
    rough = np.where(face, dry - (dry - ROUGH[1]) * wet, 1.)
    rough = np.where(strap, ROUGH[2], rough)
    rough = np.where(nail, ROUGH[3], rough)
    rough = np.where(crack, 1., rough)
    return dict(albedo=np.clip(out, 0, 1), roughness=rough, normal=normal, crack=crack.astype(float),
                rust=streak, damp=np.clip(-damp, 0, 1) * face)


# ---------------------------------------------------------------- the other timber: layouts
# Measured by Claude on the originals' previews (u from the left, v from the top). Every texture is laid
# out in its own proportions; "stretch" is how much the game stretches it along its length (intent.md).
WOODS = {
    # the ship's beam: smoothly hewn resinous pine worn by hands and ropes, one strap low down, no long
    # cracks (review R2: the image_gen one invented two)
    'beam03': dict(texture='NW_CITY_WOODBEAM_03.TGA', period=(.6, 1.2), step=(.001, .001), stretch=3.1,
                   adze=((.12, .10), ((.08, .13), (.06, .10)), (.0006, .0016)), cup=0., grain=(.0005, (.004, .012)),
                   pits=((.006, .05), .002, .04, .15), arris=(.025, .012),
                   cracks=[dict(path=[(.30, .40), (.36, .41), (.43, .40)], width=.005, depth=.012),
                           dict(path=[(.52, .70), (.57, .71), (.63, .69)], width=.004, depth=.010),
                           dict(path=[(.05, .18), (.12, .19)], width=.004, depth=.010)],
                   strap=(.72, .76, .005), nails=((.31, .66), .0065, .0035), out=(1024, 2048)),
    # the truss timber of the harbour huts (asset harbour-hut): rough, dark grey-brown, checked all over
    # with short splits along the grain; the same strap and nails as beam02, measured on its own original
    'beam01': dict(texture='NW_CITY_WOODBEAM_01.TGA', period=(.5, 1.), step=(.001, .001), stretch=3.4,
                   adze=((.06, .05), ((.03, .05), (.03, .06)), (.0015, .0045)), cup=0., grain=(.0009, (.003, .008)),
                   pits=((.006, .05), .004, .10, .10), arris=(.02, .010),
                   cracks=[dict(path=[(.05, .62), (.12, .63), (.19, .61)], width=.005, depth=.014),
                           dict(path=[(.30, .25), (.40, .26), (.52, .24)], width=.006, depth=.016),
                           dict(path=[(.34, .78), (.45, .80), (.55, .79)], width=.005, depth=.014),
                           dict(path=[(.58, .45), (.68, .46), (.80, .44)], width=.006, depth=.016),
                           dict(path=[(.70, .15), (.78, .16), (.86, .15)], width=.004, depth=.012),
                           dict(path=[(.82, .70), (.90, .71), (.97, .70)], width=.005, depth=.014),
                           dict(path=[(.42, .55), (.47, .56)], width=.003, depth=.008),
                           dict(path=[(.88, .35), (.94, .36)], width=.003, depth=.008)],
                   strap=(.219, .266, .006), nails=((.16, .81), .0075, .004), out=(1024, 2048)),
    # the plain board of light buildings: sawn spruce, one dark line along its whole length, a knot low left
    'plain': dict(texture='NW_CITY_WOOD_ORNAMENT_02.TGA', period=(.45, 1.8), step=(.001, .0015), stretch=2.06,
                  adze=None, cup=.002, grain=(.0008, (.003, .008)), pits=((.006, .05), .0025, .06, .12),
                  arris=(.012, .006),
                  cracks=[dict(path=[(0., .55), (.5, .55), (1., .55)], width=.004, depth=.012, through=True),
                          dict(path=[(.40, .30), (.46, .31), (.52, .30)], width=.003, depth=.008),
                          dict(path=[(.10, .82), (.18, .83)], width=.003, depth=.008)],
                  knots=[dict(v=.87, u=.14, r=.018)], out=(512, 2048)),
    # the carved board of Lobart's stable, measured on the original at its own size (128 × 512) and on the
    # 4x view: from the top a lily, a figure-eight, a square in a frame, a figure-eight, a lily (_carving)
    'carved': dict(texture='NW_CITY_WOOD_ORNAMENT_01.TGA', period=(.39, 1.56), step=(.0008, .0012), stretch=2.1,
                   adze=None, cup=.0015, grain=(.0012, (.003, .007)), pits=((.005, .05), .002, .05, .12),
                   arris=(.010, .005),
                   cracks=[dict(path=[(.02, .10), (.10, .11), (.18, .10)], width=.003, depth=.008),
                           dict(path=[(.62, .90), (.70, .91), (.80, .90)], width=.003, depth=.008)],
                   carving=dict(
                       depth=.006, band=.012, groove=.003, wobble=.0015, seed=5,
                       # the waist's u and v; each loop's length from the waist along the band's middle line,
                       # the loop next to the square the longer one; which one is outer. The strands cross at
                       # 27.5° to the horizontal before the eight is narrowed to .9 of its width (~30° after)
                       eights=[dict(u=.415, v=.343, outer=.147, inner=.161, side='up'),
                               dict(u=.415, v=.655, outer=.147, inner=.161, side='down')],
                       cross=27.5, narrow=.9,
                       # the sunk rectangle (half width, half height) in a standing frame, a slit along its right
                       square=dict(u=.415, v=.50, half=(.033, .059), frame=.0125, slit=(.455, .545)),
                       # a lily: a lens across the board touching the outer loop, a neck, a bud along the board
                       # pointing away from the eights and two leaves curling the same way, the right one longer
                       lilies=[dict(u=.435, lens=.230, bud=(.45, .130), side='up'),
                               dict(u=.435, lens=.770, bud=(.45, .8625), side='down')],
                       lens=(.078, .030), bud_half=(.0155, .045), leaves=(.09, .12)),
                   out=(512, 2048)),
    # three wide spruce boards of the farms, the town gates and the lighthouse, along U, with gaps
    'floor': dict(texture='NW_CITY_WOOD_01.TGA', period=(2.16, .54), step=(.0015, .001), stretch=1.,
                  adze=None, cup=.002, grain=(.0009, (.004, .012)), pits=((.006, .08), .003, .08, .15),
                  arris=(.012, .006), boards=((0., .32), (.32, .67), (.67, 1.)), gap=.016,
                  cracks=[dict(path=[(.20, .08), (.21, .16), (.20, .24)], width=.004, depth=.010),
                          dict(path=[(.52, .62), (.53, .72)], width=.004, depth=.010),
                          dict(path=[(.84, .30), (.85, .41), (.84, .50)], width=.004, depth=.010)],
                  knots=[dict(v=.15, u=.47, r=.025), dict(v=.48, u=.37, r=.02), dict(v=.84, u=.80, r=.018)],
                  out=(2048, 512)),
}
WOOD_SURFACES = [
    dict(name='wood', colour=[.24, .21, .17], metres=1., bump=.0006),
    dict(name='iron', colour=[.08, .08, .08], metres=.5, bump=.0003),
]
VOID_Z = -.04                    # the dark behind the gaps between boards
CROSSING = .02                   # the carving's band dips under itself this far from the figure-eight's waist
GROOVE = .55                     # the groove outside a band, in parts of the carving's depth: soft, not a black line


def _knots(gx, gy, period, knots):
    """Heights of the knots (raised a little, with a ring of bent grain and dry checks) and a 0..1 map of them."""
    px, py = period
    dh, mask = np.zeros_like(gx), np.zeros_like(gx)
    for knot in knots:
        dx = (gx - knot['u'] * px + px / 2) % px - px / 2
        dy = (gy - (1 - knot['v']) * py + py / 2) % py - py / 2
        d = np.hypot(dx / 1.4, dy) / knot['r']                         # a knot is a little long along the grain
        angle = np.arctan2(dy, dx)
        dh += .0012 * np.clip(1 - d ** 2, 0, 1)
        dh -= .003 * (d < .9) * np.clip(np.cos(5 * angle) - .9, 0, 1) * 10 * np.clip(.9 - d, 0, 1)   # dry checks
        dh += .0004 * np.sin(2 * math.pi * d * 2.5) * np.clip(2.2 - d, 0, 1) * (d > 1)               # bent grain
        mask = np.maximum(mask, np.clip(1.3 - d, 0, 1))
    return dh, mask


def _arc(centre, r, a0, a1, step=.0015):
    """Points of a circular arc from angle a0 to a1 (radians, y up), about one per step metres."""
    n = max(2, int(abs(a1 - a0) * r / step) + 1)
    a = np.linspace(a0, a1, n)
    return np.stack([centre[0] + r * np.cos(a), centre[1] + r * np.sin(a)], axis=1)


def _vesica(cx, cy, a, b, along=False):
    """A pointed lens, closed: tips a from the middle, its widest b; across the board, or along it."""
    radius = (a * a + b * b) / (2 * b)
    half = math.asin(a / radius)
    top = _arc((0, b - radius), radius, math.pi / 2 - half, math.pi / 2 + half)      # right tip to left tip
    lens = np.concatenate([top, -top[1:]])                                          # and back underneath
    if along:
        lens = lens[:, ::-1]
    return lens + (cx, cy), radius, half


def _chaikin(points, passes=3):
    for _ in range(passes):
        p, q = points[:-1], points[1:]
        points = np.concatenate([points[:1], np.stack([.75 * p + .25 * q, .25 * p + .75 * q], 1).reshape(-1, 2), points[-1:]])
    return points


def _hand(points, amount, rng, closed):
    """The line as cut by hand: moved sideways by a smooth wander of about amount metres."""
    seg = np.linalg.norm(np.diff(points, axis=0), axis=1)
    s = np.concatenate([[0], np.cumsum(seg)])
    length = s[-1]
    tangent = np.gradient(points, axis=0)
    normal = np.stack([-tangent[:, 1], tangent[:, 0]], 1) / (np.linalg.norm(tangent, axis=1, keepdims=True) + 1e-12)
    wander = np.zeros(len(points))
    for wavelength, weight in ((.04, .5), (.09, .8), (.18, 1.)):
        cycles = max(1, round(length / wavelength)) if closed else length / wavelength
        wander += weight * np.sin(2 * math.pi * cycles * s / length + rng.uniform(0, 2 * math.pi))
    if not closed:                                          # a stroke starts and ends where it is drawn to
        wander *= np.clip(np.minimum(s, length - s) / .01, 0, 1)
    return points + normal * (amount * wander / 1.3)[:, None]


def _longest(geometry):
    parts = getattr(geometry, 'geoms', [geometry])
    return np.asarray(max(parts, key=lambda g: g.length).coords)


def _ornament(carving, period):
    """The carving's geometry in texture metres (x from the left, y up from the period's bottom):
    bands — a middle line and its half width at the start and end; pits — closed outlines sunk inside
    their bands; crescents — dark chips along a band, between two angles round a centre; slits — narrow
    dark chips along a line; crossings — where the figure-eight's band passes over itself."""
    from shapely.geometry import LineString
    px, py = period
    rng = np.random.default_rng(carving['seed'])
    hw, wobble = carving['band'], carving['wobble']

    def at(u, v):
        return np.array([u * px, (1 - v) * py])
    out = dict(bands=[], pits=[], crescents=[], slits=[], crossings=[])

    def band(points, w0, w1=None, closed=False, smooth=False):
        if smooth:
            points = _chaikin(points)
        points = _hand(points, wobble, rng, closed)
        out['bands'].append(dict(line=points, w=(w0, w0 if w1 is None else w1)))
        return points

    # a figure-eight is one band with no start or end. From the waist it runs up to the right, tangent to
    # the upper loop, round it anticlockwise, back into the waist from the upper left, across the first
    # strand down to the right, round the lower loop clockwise and back into the waist from the lower left.
    # The loops are round, the strands touch both, so they cross at the waist as in ∞; the whole eight is
    # then narrowed to the original's width.
    phi = math.radians(carving['cross'])
    for eight in carving['eights']:
        waist = at(eight['u'], eight['v'])
        up = eight['side'] == 'up'
        ha, hb = (eight['outer'], eight['inner']) if up else (eight['inner'], eight['outer'])
        ra, rb = ha / (1 + 1 / math.cos(phi)), hb / (1 + 1 / math.cos(phi))     # loop length = d + r
        da, db = ra / math.cos(phi), rb / math.cos(phi)
        alpha = math.pi / 2 - phi
        upper = _arc((0, da), ra, -alpha, math.pi + alpha)
        lower = _arc((0, -db), rb, alpha, -math.pi - alpha)
        zero = np.zeros(2)
        line = np.concatenate([np.linspace(zero, upper[0], 12)[:-1], upper, np.linspace(upper[-1], zero, 12)[1:],
                               np.linspace(zero, lower[0], 12)[1:-1], lower, np.linspace(lower[-1], zero, 12)[1:]])
        mid = 11 + len(upper) + 10
        line = band(waist + line * (carving['narrow'], 1), hw, closed=True)
        out['pits'] += [line[:mid + 1], line[mid:]]
        # the strand into the outer loop's right side lies on top
        over = np.concatenate([line[-12:], line[:12]]) if up else line[mid - 11:mid + 13]
        out['crossings'].append(dict(over=over, at=waist))
        for lobe, centre, is_upper in ((line[:mid + 1], waist + (0, da), True), (line[mid:], waist - (0, db), False)):
            # a narrow black chip outside each loop on the right and a wide dark one inside on the left
            right = (-55, 75) if is_upper else (-55, 60)
            left = (110, 235) if is_upper else (125, 250)
            out['crescents'].append(dict(centre=centre, line=lobe, hw=hw, span=right, width=.012, depth=2.2, outside=True))
            out['crescents'].append(dict(centre=centre, line=lobe, hw=hw, span=left, width=.016, depth=2.4, outside=False))

    square = carving['square']
    centre = at(square['u'], square['v'])
    (hx, hy), frame = square['half'], square['frame']
    fx, fy = hx + frame, hy + frame
    rect = centre + np.array([(fx, fy), (-fx, fy), (-fx, -fy), (fx, -fy), (fx, fy)])
    rect = np.concatenate([np.linspace(p, q, 40)[:-1] for p, q in zip(rect, rect[1:])] + [rect[-1:]])
    out['pits'].append(band(rect, frame, closed=True))
    (v0, v1) = square['slit']
    x = centre[0] + fx + frame + .0025
    out['slits'].append(dict(line=np.array([(x, (1 - v0) * py), (x, (1 - v1) * py)]), width=.005, depth=1.6))

    a, b = carving['lens']
    ba, bb = carving['bud_half']
    for lily in carving['lilies']:
        s = 1 if lily['side'] == 'up' else -1
        lens_centre = at(lily['u'], lily['lens'])
        lens, radius, half = _vesica(*lens_centre, a, b)
        lens = band(lens, .009, closed=True)
        out['pits'].append(lens)
        # the dark chip inside along the lens's top, more of it left of the middle
        top_centre = lens_centre + (0, b - radius)
        out['crescents'].append(dict(centre=top_centre, span=(90 - math.degrees(.3 * half), 90 + math.degrees(.85 * half)),
                                     width=.008, depth=1.8, outside=False, line=lens[:len(lens) // 2], hw=.009))
        bud_centre = at(*lily['bud'])
        out['pits'].append(band(_vesica(*bud_centre, bb, ba, along=True)[0], .008, closed=True))
        base = bud_centre - (0, s * bb)
        band(np.linspace(lens_centre + (0, s * b), base, 20), .012, .008)
        for side, length in zip((-1, 1), carving['leaves']):
            local = np.array([(.008, -.004), (.03, .003), (.055, .012), (.075, .026), (.088, .045), (.090, .060),
                              (.084, .068)]) * length / .09
            leaf = band(base + local * (side, s), .009, .005, smooth=True)
            offsets = [_longest(LineString(leaf).offset_curve(d)) for d in (.013, -.013)]
            slit = max(offsets, key=lambda o: s * o[:, 1].mean())
            out['slits'].append(dict(line=slit, width=.005, depth=1.6))
    return out


def _carving(gx, gy, period, carving):
    """Depth of the carving (positive down), a 0..1 map of the bands left standing and a 0..1 map of
    the dark cut-away parts.

    The original at its own size and on the 4x view (docs/remaster-defaults.md, row 5c), from the top:
    - a lily: a bud along the board pointing up, two leaves curling up and out from its base, a neck
      down to a lens lying across the board, with a dark chip inside along its top;
    - a figure-eight: one band whose two strands cross at the waist, as in ∞; each loop sunk inside,
      a dark crescent chip inside it on the left, a narrow black one outside it on the right; the loop
      next to the square is the bigger one;
    - a sunk rectangle in a standing frame whose top and bottom run into the loops' bands, a black slit
      along the frame's right;
    - the figure-eight and the lily again, mirrored.
    Every band stands at the board's face between a narrow groove outside and the sunk part inside: the
    light falls on its flat top (docs/remaster-pipeline.md, 2026-10-22)."""
    from shapely import contains_xy, distance, line_locate_point, points as shapely_points
    from shapely.geometry import LineString, Polygon
    shape = gx.shape
    deep, groove = carving['depth'], carving['groove']
    geometry = _ornament(carving, period)
    xs, ys = gx.ravel(), gy.ravel()

    def near(line, margin):
        lo, hi = line.min(axis=0) - margin, line.max(axis=0) + margin
        return np.flatnonzero((xs > lo[0]) & (xs < hi[0]) & (ys > lo[1]) & (ys < hi[1]))
    # half width wanders by ±12% along the cut, as a knife does
    wander = (np.sin(xs * 61 + ys * 23 + 1.3) + np.sin(xs * -37 + ys * 89 + 4.1) + np.sin(ys * 151 + 2.2)) / 3

    edge = np.full(xs.shape, np.inf)               # signed distance to the nearest band's edge, < 0 on a band
    for b in geometry['bands']:
        idx = near(b['line'], .05)
        line = LineString(b['line'])
        pts = shapely_points(xs[idx], ys[idx])
        d = distance(line, pts)
        w0, w1 = b['w']
        w = w0 if w0 == w1 else w0 + (w1 - w0) * line_locate_point(line, pts, normalized=True)
        edge[idx] = np.minimum(edge[idx], d - w * (1 + .12 * wander[idx]))
    inside = np.zeros(xs.shape, bool)
    for pit in geometry['pits']:
        idx = near(pit, .001)
        inside[idx] |= contains_xy(Polygon(pit).buffer(0), xs[idx], ys[idx])

    depth = np.where(inside, deep * np.clip(edge / .002, 0, 1),                               # the sunk parts
                     GROOVE * deep * np.clip(1 - abs(edge - groove) / groove, 0, 1))         # the grooves outside
    dark = np.where(inside, .5 * np.clip(edge / .003, 0, 1), 0.)
    for c in geometry['crescents']:
        r_idx = near(c['line'], .03)
        dx, dy = xs[r_idx] - c['centre'][0], ys[r_idx] - c['centre'][1]
        theta = np.degrees(np.arctan2(dy, dx))
        a0, a1 = c['span']
        t = ((theta - a0) % 360) / (a1 - a0)
        s = edge[r_idx]
        side = ~inside[r_idx] if c['outside'] else inside[r_idx]
        own = distance(LineString(c['line']), shapely_points(xs[r_idx], ys[r_idx])) - c['hw'] * (1 + .12 * wander[r_idx])
        side &= own < s + .001                      # next to its own band, not another one
        width = c['width'] * np.clip(np.sin(math.pi * np.clip(t, 0, 1)), 0, 1) ** .6
        hit = side & (t > 0) & (t < 1) & (s > 0) & (s < width)
        floor = 0 if c['outside'] else deep
        cut = floor + (c['depth'] * deep - floor) * (1 - s / np.maximum(width, 1e-9))
        r_idx, cut = r_idx[hit], cut[hit]
        depth[r_idx] = np.maximum(depth[r_idx], cut)
        dark[r_idx] = 1.
    for slit in geometry['slits']:
        idx = near(slit['line'], .01)
        line = LineString(slit['line'])
        pts = shapely_points(xs[idx], ys[idx])
        d = distance(line, pts)
        t = line_locate_point(line, pts, normalized=True)
        half = slit['width'] / 2 * np.sin(math.pi * t) ** .6
        hit = (d < half) & (edge[idx] > 0)
        idx, cut = idx[hit], slit['depth'] * deep * (1 - d[hit] / half[hit])
        depth[idx] = np.maximum(depth[idx], cut)
        dark[idx] = 1.
    # where the band passes under itself it dips a little and the upper strand's grooves cross it
    for crossing in geometry['crossings']:
        idx = near(np.array([crossing['at']]), CROSSING + .01)
        reach = np.clip((CROSSING - np.hypot(xs[idx] - crossing['at'][0], ys[idx] - crossing['at'][1])) / .005, 0, 1)
        over = distance(LineString(crossing['over']), shapely_points(xs[idx], ys[idx]))
        w = carving['band'] * (1 + .12 * wander[idx])
        under = (edge[idx] < 0) & (over > w)
        cross_groove = GROOVE * deep * np.clip(1 - abs(over - w - groove) / groove, 0, 1) * reach
        depth[idx] = np.maximum(depth[idx], np.where(under, .0015 * reach, 0) + cross_groove * (over > w))
        edge[idx] = np.where(under & (cross_groove > .2 * deep), groove, edge[idx])
    standing = np.clip(-edge / .0015, 0, 1)
    return depth.reshape(shape), standing.reshape(shape), dark.reshape(shape)


def wood(name: str, seed: int):
    """Layout of one of WOODS: (spec, points, grids)."""
    cfg = WOODS[name]
    rng = np.random.default_rng([seed, 3, len(name)])
    px, py = cfg['period']
    step = cfg['step']
    style = dict(adze=cfg['adze'], grain=cfg['grain'], pits=cfg['pits'], arris=cfg['arris'], cup=cfg['cup'])
    pieces, points, grids = [], {}, {}
    if 'boards' in cfg:
        # boards along x: each is built in its grain frame (rows along the grain) and turned
        cols = round(px / step[0])
        for v0, v1 in cfg['boards']:
            y0, y1 = (1 - v1) * py + cfg['gap'] / 2, (1 - v0) * py - cfg['gap'] / 2
            width = y1 - y0
            n = round(width / step[1]) + 1
            ga, gb = np.meshgrid(np.arange(n) * step[1], np.arange(cols) * step[0])
            face = _hewn(rng, ga, gb, width, px, (step[1], step[0]), **style).T      # rows along y now
            gx, gy = np.meshgrid(np.arange(cols) * step[0], y0 + np.arange(n) * step[1])
            knots, _ = _knots(gx, gy, (px, py), cfg.get('knots', []))
            crack, lips = _cracks(gx, gy, rng, (px, py), cfg['cracks'], WIGGLE)
            face += knots - crack - .0012 * lips
            index = len(pieces)
            pieces.append(pb.grid_piece(index, (0, y0), step, [0, 0, 0], 0, [rng.uniform(0, 1), rng.uniform(0, 1), 0.],
                                        copy_offsets=[[-px, 0], [0, 0], [px, 0]], kind='board'))
            grids[index] = np.concatenate([face, face[:, :1]], axis=1)          # the last column closes the period
    else:
        cols, rows = round(px / step[0]) + 1, round(py / step[1])
        gx, gy = np.meshgrid(np.arange(cols) * step[0], np.arange(rows) * step[1])
        face = _hewn(rng, gx, gy, px, py, step, **style)
        knots, _ = _knots(gx, gy, (px, py), cfg.get('knots', []))
        crack, lips = _cracks(gx, gy, rng, (px, py), cfg['cracks'], WIGGLE)
        face += knots - crack - .0012 * lips
        if 'carving' in cfg:
            carved, _, _ = _carving(gx, gy, (px, py), cfg['carving'])
            face -= carved
        pieces.append(pb.grid_piece(0, (0, 0), step, [0, 0, 0], 0, [rng.uniform(0, 1), rng.uniform(0, 1), 0.],
                                    copy_offsets=[[0, -py], [0, 0], [0, py]], kind='face'))
        grids[0] = np.concatenate([face, face[:1]])
        if 'strap' in cfg:
            r0, strap = _strap(rng, face, face + crack + .0012 * lips, gx, gy, step, py, cfg['strap'])
            pieces.append(pb.grid_piece(1, (0, r0 * step[1]), step, [0, 0, 0], 1, [0, 0, 0], copy_offsets=[[0, 0]],
                                        kind='strap'))
            grids[1] = strap
            _nails(rng, pieces, points, strap, r0, step, (px, py), cfg['strap'], cfg['nails'], 1)
    spec = dict(tool='scripts/content/timber_bake.py', texture=cfg['texture'], wood=name, seed=seed,
                period=[px, py], camera=dict(tilt=0.), sky=dict(pitch=0., ground=.08),
                ao=dict(distance=.05, crevice=.012), surfaces=WOOD_SURFACES,
                cracks=[dict(c) for c in cfg['cracks']], knots=cfg.get('knots', []), pieces=pieces)
    if 'strap' in cfg:
        spec.update(strap=dict(v=list(cfg['strap'][:2])), nails=list(cfg['nails'][0]))
    if 'carving' in cfg:
        spec['carving'] = cfg['carving']
    if 'boards' in cfg:
        spec.update(boards=[list(b) for b in cfg['boards']], gap=cfg['gap'], void=dict(z=VOID_Z))
    return spec, points, grids


# ---------------------------------------------------------------- the other timber: weathering (compose)
# Each look is measured on its original's preview (sRGB means, channel ratios turned linear). The steps
# are those of beam02: the generated wood's light in the original's colour, patches, damp, then the
# texture's own marks.
LOOKS = {
    # reddish brown worn smooth, light and dark smudges along the grain (pixel spread 15.9); a rusty strap
    'beam03': dict(ratio=(1., .64, .45), desaturate=.45, grain=.6, light=(.45, (1., .76, .56)),
                   mottle=((.10, .04), .07, (1.3, .7), (1., .74, .54)), damp=((.14, .05), .22),
                   smudge=((.06, .30), .35), strap=((1., .43, .28), .85), contrast=1.0),
    # dark grey-brown, hardly olive (sRGB 59/53/49), blotchy and rough (pixel spread 20.4); a rusty strap
    'beam01': dict(ratio=(1., .79, .68), desaturate=.6, grain=1.0, light=(.4, (1., .85, .76)),
                   mottle=((.08, .03), .10, (1.4, .6), (1., .85, .76)), damp=((.12, .05), .35),
                   strap=((1., .62, .45), .7), contrast=1.3),
    # grey-brown sawn spruce, a bleached pale streak down the left (sRGB 76/68/58 against 63/54/43)
    'plain': dict(ratio=(1., .75, .48), desaturate=.6, grain=.75, light=(.5, (1., .82, .62)),
                  mottle=((.08, .03), .08, (1.3, .65), (1., .82, .62)), damp=((.12, .05), .28),
                  bleach=((.06, .38), 1.5, (1., .86, .68)), contrast=1.25),
    # an almost black board with whitened grain and a light carving (13% of it lighter than sRGB 110,
    # pixel spread 39.7). Measured at its own size by region of the carving (sRGB medians): the bands
    # 1.8 times the board and grey-brown (99/85/66), the cut-away insides 0.77 of it and warmer (49/36/21),
    # the chips almost black
    'carved': dict(ratio=(1., .68, .33), desaturate=.6, grain=2.2, light=(.9, (1., .83, .62)),
                   mottle=((.06, .02), .12, (1.8, .45), (1., .83, .62)), damp=((.10, .04), .40),
                   carved=(2.0, (1., .72, .41)), recess=((.55, (1., .50, .16)), .35), contrast=1.6),
    # olive-brown boards (sRGB 62/57/30) under green moss on 37% of them, more to the right; the soft
    # edge of the mask thins it, so 55% of the boards get some moss to make 36% of the pixels green
    'floor': dict(ratio=(1., .80, .20), desaturate=.5, grain=.7, light=(.35, (1., .62, .17)),
                  mottle=((.10, .03), .10, (1.4, .55), (1., .85, .5)), damp=((.15, .05), .50),
                  moss=(.55, (.16, .05), (.5, 1., .25), .7, (.26, .15, .45, .63)), contrast=1.15),
}


def _compose_wood(passes: dict, spec: dict, seed: int = 0) -> dict:
    """Albedo (linear RGB) and masks of one of WOODS."""
    look = LOOKS[spec['wood']]
    rng = np.random.default_rng([seed, 8])
    piece = passes['piece']
    rows, cols = piece.shape
    period = spec['period']
    px, py = period
    kind = _kinds(spec, piece)
    strap, nail = kind == 'strap', kind == 'nail'
    face = np.isin(kind, ['face', 'board'])
    x, y = passes['x'] % px, passes['y'] % py

    colour = passes['colour']
    lum = colour @ LUM
    grey = lum[..., None]
    wood = grey + (colour - grey) * (1 - look['desaturate'])
    ratio = np.median(wood[face], axis=0)
    wood = wood / (ratio / ratio[0]) * np.array(look['ratio'])
    rel = lum / np.median(lum[face])
    wood *= (np.maximum(rel, 1e-6) ** (look['grain'] - 1))[..., None]
    share, light_ratio = look['light']
    light = np.clip((rel - 1) / 1.5, 0, 1) * share
    light_ratio = np.array(light_ratio)
    wood = wood * (1 - light[..., None]) + (wood @ LUM)[..., None] * light_ratio / (light_ratio @ LUM) * light[..., None]

    sizes, share, (up, down), pale_ratio = look['mottle']
    mottle = pb.noise_metres(rng, (rows, cols), period, sizes[0]) + .5 * pb.noise_metres(rng, (rows, cols), period, sizes[1])
    mottle += .25 * pb.noise_metres(rng, (rows, cols), period, .008)
    pale = np.clip((mottle - np.quantile(mottle, 1 - share)) * 10, 0, 1)
    stain = np.clip((np.quantile(mottle, share) - mottle) * 10, 0, 1)
    pale_ratio = np.array(pale_ratio)
    wood = wood * (1 - pale[..., None]) + (wood @ LUM)[..., None] * pale_ratio / (pale_ratio @ LUM) * up * pale[..., None]
    wood *= (1 - (1 - down) * stain)[..., None]
    damp = sum(w * pb.noise_metres(rng, (rows, cols), period, size) for w, size in zip((.75, .45), look['damp'][0]))
    wood *= np.exp(look['damp'][1] * damp)[..., None]

    grain_along_x = bool(spec.get('boards'))
    if 'smudge' in look:            # long light and dark smudges worn along the grain
        (across, along), strength = look['smudge']
        fx, fy = (px / along, py / across) if grain_along_x else (px / across, py / along)
        wood *= np.exp(strength * np.tanh(pb.periodic_noise(rng, (rows, cols), fx, fy)))[..., None]
    if 'bleach' in look:            # a sun-bleached streak along the board
        (u0, u1), gain, bleach_ratio = look['bleach']
        u = x / px + .03 * pb.noise_metres(rng, (rows, cols), period, .05)
        band = np.clip(np.minimum(u - u0, u1 - u) / .05, 0, 1) * np.clip(.7 + .4 * pb.noise_metres(rng, (rows, cols), period, .1), 0, 1)
        bleach_ratio = np.array(bleach_ratio)
        bleached = (wood @ LUM)[..., None] * bleach_ratio / (bleach_ratio @ LUM) * gain
        wood = wood * (1 - band[..., None]) + bleached * band[..., None]
    if spec.get('knots'):
        _, knots = _knots(x, y, period, spec['knots'])
        wood *= (1 - .5 * knots)[..., None]
    carved = np.zeros((rows, cols))
    if 'carving' in spec:           # the carving's bands are worn light, its cut-away parts hold dirt
        _, carved, recess = _carving(x, y, period, spec['carving'])
        gain, carved_ratio = look['carved']
        carved_ratio = np.array(carved_ratio)
        median = np.median((wood @ LUM)[face])
        light_wood = median * gain * carved_ratio / (carved_ratio @ LUM)
        light_wood = light_wood * (np.maximum(wood @ LUM, 1e-6) / median)[..., None] ** .2     # the grain runs on
        # the cut faces are smoother than the weathered skin: half of its whitened grain, darker; dirt
        # sits deepest in the chips
        (pit_gain, pit_ratio), chip_gain = look['recess']
        pit, chip = np.clip(recess / .5, 0, 1)[..., None], np.clip((recess - .5) / .5, 0, 1)[..., None]
        pit_ratio = np.array(pit_ratio)
        dirt = median * pit_gain * pit_ratio / (pit_ratio @ LUM)
        wood = wood * (1 - pit) + (.5 * wood * pit_gain + .5 * dirt) * pit
        wood *= 1 - (1 - chip_gain) * chip
        wood = wood * (1 - carved[..., None]) + light_wood * carved[..., None]
    moss = np.zeros((rows, cols))
    if 'moss' in look:              # green moss in patches, more of it towards the right of the boards
        share, sizes, moss_ratio, tone, by_quarter = look['moss']
        field = pb.noise_metres(rng, (rows, cols), period, sizes[0]) + .5 * pb.noise_metres(rng, (rows, cols), period, sizes[1])
        bias = np.interp(x / px, [.125, .375, .625, .875], by_quarter, period=1.)
        field += 2.2 * (bias - np.mean(by_quarter))
        moss = np.clip((field - np.quantile(field[face], 1 - share)) * .5 + .5, 0, 1) * face
        moss_ratio = np.array(moss_ratio)
        green = np.median((wood @ LUM)[face]) * tone * moss_ratio / (moss_ratio @ LUM)
        green = green * np.exp(.2 * pb.noise_metres(rng, (rows, cols), period, .05))[..., None]
        wood = wood * (1 - moss[..., None]) + green * moss[..., None]

    out = np.where(face[..., None], wood, 0.)
    normal = passes['normal'] / (np.linalg.norm(passes['normal'], axis=-1, keepdims=True) + 1e-9)
    if strap.any():
        median = np.median((wood @ LUM)[face])
        (strap_ratio, tone) = look['strap']
        strap_ratio = np.array(strap_ratio)
        edge = np.clip((1 - normal[..., 2]) * 6, 0, 1)
        iron = strap_ratio / (strap_ratio @ LUM) * median * tone * (1 + 1.2 * edge)[..., None]
        pitting = pb.noise_metres(rng, (rows, cols), period, .003) + .6 * pb.noise_metres(rng, (rows, cols), period, .012)
        iron *= np.exp(.28 * np.tanh(pitting))[..., None]
        out = np.where(strap[..., None], iron, out)
        out = np.where(nail[..., None], NAIL * (1 + .5 * edge)[..., None], out)

    occlusion = np.clip(passes['ao'], 0, 1) ** OCCLUSION[0] * np.clip(passes['crevice'], 0, 1) ** OCCLUSION[1]
    inside = np.clip((CRACK_BLACK[1] - passes['crevice']) / (CRACK_BLACK[1] - CRACK_BLACK[0]), 0, 1) * face * (carved < .5)
    out *= (occlusion * (1 - (1 - CRACK_BLACK[2]) * inside))[..., None]
    luminance = out @ LUM
    mean = np.median(luminance[face])
    out *= (((np.maximum(luminance, 1e-6) / mean) ** look['contrast'] * mean) / np.maximum(luminance, 1e-6))[..., None]
    out[piece < 0] = .002                                                # the dark behind the gaps

    crack = face & (passes['crevice'] < .35)
    wet = np.clip(-damp * .6, 0, 1) * face
    dry = ROUGH[0][0] + (ROUGH[0][1] - ROUGH[0][0]) * np.clip(.5 + .3 * pb.noise_metres(rng, (rows, cols), period, .1), 0, 1)
    rough = np.where(face, dry - (dry - ROUGH[1]) * wet, 1.)
    rough = np.maximum(rough, .95 * moss)
    rough = np.where(strap, ROUGH[2], rough)
    rough = np.where(nail, ROUGH[3], rough)
    rough = np.where(crack | (piece < 0), 1., rough)
    return dict(albedo=np.clip(out, 0, 1), roughness=rough, normal=normal, crack=crack.astype(float),
                damp=np.clip(-damp, 0, 1) * face, moss=moss, carved=carved)


LAYOUTS = {'beam02': beam02, **{name: (lambda seed, name=name: wood(name, seed)) for name in WOODS}}
OUT_SIZE = {'beam02': (1024, 2048), **{name: cfg['out'] for name, cfg in WOODS.items()}}
TEXTURES = {'beam02': 'NW_CITY_WOODBEAM_02.TGA', **{name: cfg['texture'] for name, cfg in WOODS.items()}}


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
    p.add_argument('--resolution', type=int, default=1024)
    p.add_argument('--samples', type=int, default=64)
    p.add_argument('--street', action='store_true', help='perspective views of a wall of beams instead of the passes')
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
        surfaces = {}
        spec = json.loads(args.layout.read_text())
        if args.wood:
            args.out_dir.mkdir(parents=True, exist_ok=True)
            image = pb.seamless_along(args.wood, args.out_dir / 'surface-wood.png')
            if spec.get('boards'):            # the boards' grain runs along x: turn the surface image
                from PIL import Image
                Image.open(image).transpose(Image.Transpose.ROTATE_90).save(image)
            surfaces = {s['name']: image for s in spec['surfaces'] if s['name'] in ('oak', 'wood')}
        px, py = spec['period']
        stretch = STRETCH if spec.get('wood', 'beam02') == 'beam02' else WOODS[spec['wood']]['stretch']
        # a wall of beams, stretched along as the game stretches the texture
        options = (dict(view='street', pitch='90', eaves='.6', stretch=str(stretch), close=f'{px / 2},{py * .75}')
                   if args.street else {})
        if args.sun:
            options['sun'] = str(args.sun)
        print(pb.render(args.layout, args.out_dir, surfaces, args.resolution, args.samples, options))
    elif args.command == 'compose':
        spec = json.loads(args.layout.read_text())
        name = spec.get('wood', 'beam02')
        passes = pb.read_passes(args.passes, spec['period'])
        result = compose(passes, spec, args.seed) if name == 'beam02' else _compose_wood(passes, spec, args.seed)
        masks = ('roughness', 'crack', 'rust', 'damp') if name == 'beam02' else ('roughness', 'crack', 'damp', 'moss', 'carved')
        pb.write_maps(args.out, OUT_SIZE[name], result['albedo'], result['normal'], {k: result[k] for k in masks})
        print(args.out)
    elif args.command == 'blockout':
        spec = json.loads(args.layout.read_text())
        passes = pb.read_passes(args.passes, spec['period'])
        width = passes['piece'].shape[1]
        pb.write_preview(passes, args.out, (width, round(width * spec['period'][1] / spec['period'][0])))
        print(args.out)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
