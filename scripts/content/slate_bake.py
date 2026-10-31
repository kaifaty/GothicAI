#!/usr/bin/env python3
"""Slate roof NW_CITY_ROOF_04 from a 3D model: slates laid in Blender, passes rendered from above,
weathering placed by the passes.

The user asked for the roof as a model (2026-10-21): the painted versions (slate_roof.py) stayed too
clean and too evenly laid. Here every slate is a solid with its own thickness and tilt. It rests on
the course below and on its right neighbour, so the steps at the course lines and joints, their
occlusion and the normal map come from geometry. Dirt, black moss and water streaks follow the
occlusion of those steps instead of noise.

Measured from the original (reports/content/remaster/city-roofs/intent.md): horizontal courses,
16 slates across and 18 courses on a 6.6 m square; slates wider than tall; every joint curves from
upper left to lower right, steep at the top and sweeping right at the tail. The curve is the convex
left edge of the slate to its right, which lies over its left neighbour: the dark grime sits on the
left of each joint and under each course edge. Original pixels are never used: the stone colour is an
image_gen surface.

The slates are pieces of piece_bake.py: the scene is the shared mesh3d/blender/piece_scene.py.

    slate_bake.py layout OUT.json [--seed N]
    slate_bake.py render LAYOUT.json OUTDIR STONE.png... [--resolution PX] [--samples N] [--sun]
    slate_bake.py compose LAYOUT.json PASSES.exr OUT.png [--seed N]
"""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

import numpy as np

import piece_bake as pb

PERIOD = 6.6            # metres per texture square
ACROSS, COURSES = 16, 18
SLANT = .95             # horizontal joint travel over one course, in course heights
SLANT_SPREAD = .08      # joints of one course differ in slant by this share
CURVE = .45             # joint slope at the top of a course, share of its mean slope: steep, then sweeping right
HEAD = 1.9              # slate length in course heights: the head lies under the course above
SIDE_LAP = .07          # hidden part of a slate under its right neighbour, metres
TAIL_R = .07            # radius of the rounded corners, metres
WIDTH_SPREAD = .22      # slate widths within a course vary by this share
COURSE_SPREAD = .04     # course heights vary by this share
DROP = .012             # bottom edges of single slates sit higher or lower by this much, metres
TILT = .1               # bottom edge slope across a slate, in course heights
THICKNESS = (.012, .022)  # heavy hand-split slate: 3/8 inch and more, up to an inch is not unusual
LATERAL = .024          # a slate's left side rests on its neighbour: lift of the left edge, metres
LIFT = .11              # a slate's tail rests on the course below: lift of the bottom edge, metres
WOBBLE = .004           # random tilt of a slate plane, metres per metre
RAGGED = .003           # hand-dressed edges wander by this much, metres
CHIPS = .8              # mean number of small chips on the visible edges of a slate
BROKEN = .12            # share of slates with a broken corner
MARGIN = .4             # copies of slates within this distance of the square are built too
BOW = (.004, .010)      # a slate bows up across its width by this much in the middle, metres
WAVE = (.0003, .001)    # cleavage waviness of the face, metres
ROUND = .03             # the dressed edge: hammered from the back, a broken bevel this wide, metres
ROUND_DROP = .8         # and falling by this share of the thickness at the very edge
POINT_STEP = .012       # mesh density of the visible part of a slate, metres
STONE_METRES = 2.6      # one image_gen stone surface covers this many metres

# Weathering (compose)
DESATURATE = .7         # share of the generated stone's colour removed
STONE_CONTRAST = .65    # the generated stone is flaky; slates on a roof read smooth and mottled
TONE = .14              # slates differ in tone (log-normal spread)
PALE = .07              # share of paler weathered slates
BLACK = .03             # share of almost black slates, wet or newer replacements, in groups where it is damp
BLOTS = .9              # black lichen and grime blots over the slates, densest by the gaps (the original's darks)
BLOT_SHARE = .7         # threshold of the blot field in standard deviations: higher, fewer blots
SHEEN = .7              # strongest worn, lighter patch on a slate
DIRT = .75              # darkest dirt smudge
SKY = 1.                # how strongly the sky light of the model shades the albedo
SHADE = .95             # darkening under each course edge, where the light from the ridge is blocked
SHADE_ELEVATION = (17., 22., 29.)  # degrees above the roof plane of that light: a soft, broad sky
MOSS = (.004, .0045, .004)  # black moss and dirt packed in the gaps: as dark as the original's darkest tenth
JOINT_GRIME = (.012, .045)  # metres: solid grime, then fading, on the left of each joint under the raised neighbour
JOINT_LINE = (.012, .018)   # metres: a black line along every joint, solid, then fading out
DRIPS = .5              # share of joints whose tail drips water onto the courses below
DRIP = .75              # darkening of a fresh drip trail
FADE = .35              # washed-out regions
WET = .7                # darkening of the wet black patches that span several slates
# Surface (roughness for the engine's GGX: full highlight below 0.5, none above 0.8)
DRY_ROUGH = (.82, .92)  # dry slate: matte, no highlight in the engine
WORN_ROUGH = .25        # rain-polished worn patches are this much smoother: a faint sheen
WET_ROUGH = .36         # wet slate reflects the sky and the sun
DAMP = .1               # share of slates still wet from the last rain
OUT_SIZE = 1024
CAMERA_TILT = 25.       # degrees from straight down, looking up the slope: the thick tails show
SUN = 2.5               # energy of the sun for --sun, over the ridge a little from the left


def joint(x0, slant, height, v):
    """Joint x at depth v of the course: v = 0 at the top of the visible part, 1 at the bottom edge.
    Above the visible part (v < 0) the joint goes on straight under the course above."""
    v = np.asarray(v, dtype=float)
    return x0 + slant * height * np.where(v >= 0, CURVE * v + (1 - CURVE) * v ** 2, CURVE * v)


def slate_outline(rng, x_left, slant_left, x_right, slant_right, y_bottom, height):
    """Outline of one slate: curved left joint lying over the left neighbour, hidden right part under
    the right neighbour, uneven bottom edge, chips. Returns a shapely Polygon and whether a corner is
    broken off."""
    from shapely import affinity
    from shapely.geometry import Point, Polygon
    v = np.linspace(-(HEAD - 1), 1.35, 60)
    y = y_bottom + height * (1 - v)
    left = np.stack([joint(x_left, slant_left, height, v), y], axis=1)
    right = np.stack([joint(x_right, slant_right, height, v) + SIDE_LAP, y], axis=1)
    body = Polygon(np.concatenate([left, right[::-1]]))
    # bottom edge: a line through both sides, each end dropped or raised a little
    y_left = y_bottom + rng.normal(0, DROP)
    y_right = y_left + rng.uniform(-TILT, TILT) * height
    x_a, x_b = left[:, 0].min() - 1, right[:, 0].max() + 1
    slope = (y_right - y_left) / (x_right - x_left + SIDE_LAP)
    cut = Polygon([(x_a, y_left + slope * (x_a - x_left)), (x_b, y_left + slope * (x_b - x_left)),
                   (x_b, y_bottom + 3 * height), (x_a, y_bottom + 3 * height)])
    shape = pb.largest(body.intersection(cut))
    shape = pb.largest(shape.buffer(-TAIL_R, quad_segs=12).buffer(TAIL_R, quad_segs=12))
    shape = pb.ragged(shape, rng, RAGGED)
    visible_top = y_bottom + height
    bottom_at = lambda x: y_left + slope * (x - x_left)       # noqa: E731
    left_at = lambda y: joint(x_left, slant_left, height, 1 - (y - y_bottom) / height)   # noqa: E731
    right_at = lambda y: joint(x_right, slant_right, height, 1 - (y - y_bottom) / height)  # noqa: E731
    broken = rng.random() < BROKEN
    if broken:
        # the lower left corner is broken off along a straight crack
        x_low = left_at(y_left)
        a = (x_low + rng.uniform(.3, .55) * (right_at(y_left) - x_low), bottom_at(x_low) - .05)
        y_high = y_bottom + height * rng.uniform(.25, .5)
        b = (left_at(y_high) - .05, y_high)
        far = np.subtract(b, a) * 10
        side = np.array([-far[1], far[0]])          # towards the corner that breaks off
        crack = Polygon([np.subtract(a, far), np.add(b, far), np.add(np.add(b, far), side),
                         np.add(np.subtract(a, far), side)])
        shape = pb.largest(shape.difference(crack))
    for _ in range(rng.poisson(CHIPS)):
        # a small chip on the visible edges: the curved left edge or the bottom edge
        boundary = shape.exterior
        for _attempt in range(30):
            p = boundary.interpolate(rng.uniform(0, boundary.length))
            if p.y < visible_top - .03 and p.x < right_at(p.y) - .02 and (
                    p.y < bottom_at(p.x) + .03 or p.x < left_at(p.y) + .03):
                break
        radius = rng.uniform(.01, .03)
        chip = affinity.rotate(affinity.scale(Point(p.x, p.y).buffer(radius, quad_segs=2),
                                              rng.uniform(.6, 1.4), 1), rng.uniform(0, 180))
        shape = pb.largest(shape.difference(chip))
    return shape.simplify(.0015), broken


def face_points(shape, y_bottom, height):
    """Points inside a slate for its curved face: rings that follow the edge (for the rounded edge)
    and a grid, only where the slate can be seen; the hidden head stays a plane."""
    import shapely
    from shapely.geometry import box
    visible = box(-1e3, -1e3, 1e3, y_bottom + 1.35 * height)
    points = []
    for depth in (.004, .01, .018):
        inner = shape.buffer(-depth).intersection(visible)
        for part in getattr(inner, 'geoms', [inner]):
            if part.geom_type == 'Polygon' and not part.is_empty:
                points += list(shapely.segmentize(part.exterior, POINT_STEP).coords)[:-1]
    core = shape.buffer(-.026).intersection(visible)
    if not core.is_empty:
        minx, miny, maxx, maxy = core.bounds
        gx, gy = np.meshgrid(np.arange(minx, maxx, POINT_STEP * 1.6), np.arange(miny, maxy, POINT_STEP * 1.6))
        inside = shapely.contains_xy(core, gx.ravel(), gy.ravel())
        points += list(zip(gx.ravel()[inside], gy.ravel()[inside]))
    return np.round(np.asarray(points, dtype=np.float64).reshape(-1, 2), 4)


def layout(seed: int = 0) -> tuple[dict, dict]:
    """Slates of one texture square, with copies across its edges so the render tiles; and the mesh
    points of their faces."""
    rng = np.random.default_rng(seed)
    heights = (PERIOD / COURSES) * rng.uniform(1 - COURSE_SPREAD, 1 + COURSE_SPREAD, COURSES)
    heights *= PERIOD / heights.sum()
    bottoms = np.concatenate([[0], np.cumsum(heights)[:-1]])
    pieces, points = [], {}
    for course in range(COURSES):
        height, y_bottom = heights[course], bottoms[course]
        widths = rng.uniform(1 - WIDTH_SPREAD, 1 + WIDTH_SPREAD, ACROSS)
        widths *= PERIOD / widths.sum()
        x0 = rng.uniform(0, PERIOD / ACROSS) + np.concatenate([[0], np.cumsum(widths)])
        slants = SLANT * (1 + rng.normal(0, SLANT_SPREAD, ACROSS))
        slants = np.append(slants, slants[0])          # the last joint is the first one, a square later
        for k in range(ACROSS):
            shape, broken = slate_outline(rng, x0[k], slants[k], x0[k + 1], slants[k + 1], y_bottom, height)
            # The slate is a plane: its tail rests on the course below, its left side on the neighbour.
            # Lateral position runs along the chord of the joint, so the lift follows the curved joints.
            def bottom_z(x, y):
                v = 1 - (y - y_bottom) / height
                lateral = (x - x0[k] - slants[k] * height * v) / (widths[k] + SIDE_LAP)
                return LIFT * (1 - (y - y_bottom) / (HEAD * height)) + LATERAL * (1 - lateral)
            cx, cy = shape.centroid.x, shape.centroid.y
            tilt = rng.normal(0, WOBBLE, 2)
            z0 = bottom_z(cx, cy)                     # bottom_z is linear: its plane plus a small tilt
            bx = bottom_z(cx + 1, cy) - z0 + tilt[0]
            by = bottom_z(cx, cy + 1) - z0 + tilt[1]
            a = z0 - bx * cx - by * cy
            thickness = float(rng.uniform(*THICKNESS))
            stone = int(rng.integers(0, 2))
            uv_offset = rng.uniform(0, 1, 2).round(4).tolist()
            lateral_width = float(widths[k] + SIDE_LAP)
            bow, wave = float(rng.uniform(*BOW)), float(rng.uniform(*WAVE))
            wave_angle, wave_length = float(rng.uniform(0, np.pi)), float(rng.uniform(.08, .16))
            wave_phase = float(rng.uniform(0, 2 * np.pi))
            # The face: a bow across the slate along its curved joint, cleavage waviness, both only where
            # the slate can be seen; and the hand-dressed edge, falling towards the outline.
            seen = [0., -1 / (.3 * height), (y_bottom + 1.3 * height) / (.3 * height)]
            across = [1 / lateral_width, slants[k] / lateral_width,
                      -(x0[k] + slants[k] * (height + y_bottom)) / lateral_width]
            terms = [dict(kind='bow', amp=bow, across=[float(c) for c in across], mask=seen),
                     dict(kind='ripple', amp=wave, dir=[math.cos(wave_angle), math.sin(wave_angle)],
                          length=wave_length, phase=wave_phase, mask=seen),
                     dict(kind='round', width=ROUND, drop=ROUND_DROP * thickness)]
            index = len(pieces)
            pieces.append(pb.piece(
                index, shape, [a, bx, by], [a + thickness, bx, by], stone, [*uv_offset, 0.], terms,
                pb.copies(shape.bounds, (PERIOD, PERIOD), MARGIN), kind='slate',
                course=course, index_in_course=k, broken=bool(broken), thickness=thickness,
                y_bottom=float(y_bottom), height=float(height),
                joint_left=[float(x0[k]), float(slants[k])], joint_right=[float(x0[k + 1]), float(slants[k + 1])]))
            points[index] = face_points(shape, y_bottom, height)
    surfaces = [dict(name=f'stone{i}', colour=[.07, .07, .065], metres=STONE_METRES, bump=0.) for i in range(2)]
    spec = dict(tool='scripts/content/slate_bake.py', seed=seed, period=[PERIOD, PERIOD], across=ACROSS,
                courses=COURSES, camera=dict(tilt=CAMERA_TILT), sky=dict(pitch=45., ground=.08),
                ao=dict(distance=.15, crevice=.025), surfaces=surfaces,
                parameters=dict(slant=SLANT, slant_spread=SLANT_SPREAD, curve=CURVE, head=HEAD, side_lap=SIDE_LAP,
                                tail_r=TAIL_R, width_spread=WIDTH_SPREAD, course_spread=COURSE_SPREAD, drop=DROP,
                                tilt=TILT, thickness=THICKNESS, lateral=LATERAL, lift=LIFT, wobble=WOBBLE,
                                ragged=RAGGED, chips=CHIPS, broken=BROKEN, bow=BOW, wave=WAVE, round=ROUND,
                                round_drop=ROUND_DROP),
                pieces=pieces)
    return spec, points


def local_coords(spec: dict, slate: np.ndarray, x: np.ndarray, y: np.ndarray):
    """Position inside the visible part of each pixel's slate, from the rendered world position:
    u from its left joint (0) to the next joint (1), v from the course edge above (0) to its own
    bottom edge (1)."""
    period = spec['period'][0]
    table = spec['pieces']
    y_bottom = np.array([d['y_bottom'] for d in table])[slate]
    height = np.array([d['height'] for d in table])[slate]
    left = np.array([d['joint_left'] for d in table])[slate]
    right = np.array([d['joint_right'] for d in table])[slate]
    v = 1 - ((y - y_bottom + period / 2) % period - period / 2) / height
    x_left = joint(left[..., 0], left[..., 1], height, v)
    x_right = joint(right[..., 0], right[..., 1], height, v)
    u = ((x - x_left + period / 2) % period - period / 2) / (x_right - x_left)
    return u, v, x_right - x_left


def drip_trails(spec: dict, size: int, rng) -> np.ndarray:
    """0..1 trails running down the slope from the tails of some joints: water leaves the roof
    there and stains the slates below, darkest at the start, widening and fading over 0.6–2.2
    courses. Pixel rows run down the slope; the render's small vertical shift is ignored."""
    period = spec['period'][0]
    px = size / period
    field = np.zeros((size, size))
    for slate in spec['pieces']:
        if rng.random() > DRIPS:
            continue
        x0, slant = slate['joint_left']
        height = slate['height']
        col0 = joint(x0, slant, height, 1.) * px
        row0 = (period - slate['y_bottom']) * px
        length = rng.uniform(.8, 3.) * height * px
        width = rng.uniform(.012, .035) * px
        strength = rng.uniform(.5, 1.)
        bend, phase = rng.uniform(3, 7), rng.uniform(0, 2 * np.pi)
        rows = np.arange(int(row0), int(row0 + length) + 1)
        cols = np.arange(int(col0 - 6 * width), int(col0 + 6 * width) + 1)
        rr, cc = np.meshgrid(rows, cols, indexing='ij')
        down = np.clip((rr - row0) / length, 0, 1)
        centre = col0 + .012 * px * np.sin(down * bend + phase)       # the trail wanders a little
        trail = np.exp(-((cc - centre) / (width * (1 + 1.5 * down))) ** 2) * (1 - down) ** 1.5 * strength
        index = (rr % size, cc % size)
        field[index] = np.maximum(field[index], trail)
    return field


def compose(passes: dict, spec: dict, seed: int = 0) -> dict:
    """Albedo (linear RGB) and the masks it was made with, at the render size."""
    rng = np.random.default_rng(seed)
    slate = passes['piece']
    size, count = slate.shape[0], len(spec['pieces'])
    u, v, width = local_coords(spec, slate, passes['x'], passes['y'])

    stone = passes['colour']
    grey = stone.mean(axis=-1, keepdims=True)
    out = grey + (stone - grey) * (1 - DESATURATE)
    mean = out.mean()
    out = mean + (out - mean) * STONE_CONTRAST
    tone = np.exp(rng.normal(0, TONE, count)) * np.where(rng.random(count) < PALE, 1.45, 1.)
    # Almost black slates come in groups where the roof stays damp, not scattered one by one:
    # scattered, they repeat with the texture as a lattice of dark diamonds (game shots 2026-10-22).
    damp_field = pb.periodic_noise(np.random.default_rng([seed, 2]), (size, size), 6, 6)
    centre = np.array([np.mean(s['outline'], axis=0) for s in spec['pieces']])
    period = spec['period'][0]
    rows = ((period - centre[:, 1]) / period * size).astype(int) % size
    cols = (centre[:, 0] / period * size).astype(int) % size
    damp_here = damp_field[rows, cols]
    black_odds = BLACK * np.exp(1.6 * damp_here) / np.exp(1.6 * damp_here).mean()
    tone *= np.where(rng.random(count) < black_odds, rng.uniform(.25, .45, count), 1.)
    out *= tone[slate][..., None]
    out *= (1 + .10 * pb.periodic_noise(rng, (size, size), 40, 40))[..., None]       # mottling

    def blob(cu, cv, ru, rv):
        return np.exp(-((u - cu[slate]) / ru[slate]) ** 2 - ((v - cv[slate]) / rv[slate]) ** 2)

    # worn sheen: a soft lighter, greyer patch on the upper part of many slates
    sheen = blob(rng.uniform(.2, .7, count), rng.uniform(.15, .5, count),
                 rng.uniform(.25, .5, count), rng.uniform(.2, .4, count)) * rng.uniform(0, SHEEN, count)[slate]
    out = out * (1 - sheen[..., None]) + (out.mean(axis=-1, keepdims=True) * 2.4 + .02) * sheen[..., None]
    # dirt: one or two ragged dark smudges per slate, more often by the joint and the bottom edge
    ragged = .55 + .45 * np.clip(pb.periodic_noise(rng, (size, size), 90, 90), -1, 1)
    for share in (1., .5):
        has = rng.random(count) < share
        dirt = blob(rng.choice([.1, .5, .85], count), rng.uniform(.3, .95, count),
                    rng.uniform(.12, .3, count), rng.uniform(.15, .35, count)) * ragged
        out *= 1 - (DIRT * rng.uniform(.3, 1, count) * has)[slate][..., None] * dirt[..., None]

    # light and shade from the model
    sky = passes['sky'] / np.percentile(passes['sky'], 95)
    out *= np.clip(sky, 0, 1.2)[..., None] ** SKY
    shade = pb.overhang_shade(passes['height'], spec['period'][0] / size, SHADE_ELEVATION)
    out *= (1 - SHADE * shade)[..., None]

    # black moss and dirt packed into the gaps, along the left of each joint and in the corners
    # under course edges, in clumps
    gap = np.clip((1 - pb.smooth(passes['crevice'])) / .45, 0, 1)
    solid, reach = JOINT_GRIME
    beside = np.clip(((reach - (1 - u) * width) / (reach - solid)), 0, 1) ** .7 * ((v > 0) & (v < 1))
    clumps = np.clip(.55 + .6 * pb.periodic_noise(rng, (size, size), 110, 110), 0, 1)
    moss = np.clip(gap + 1.3 * shade + beside, 0, 1) * (.7 + .3 * clumps)
    out = out * (1 - moss[..., None]) + np.array(MOSS) * moss[..., None]
    # every joint reads as a crisp black line, as in the original, whatever the moss does
    core, edge = JOINT_LINE
    line = np.clip((edge - (1 - u) * width) / (edge - core), 0, 1) * ((v > 0) & (v < 1))
    out *= (1 - .85 * line)[..., None]

    # Black lichen and grime in ragged blots over parts of slates, densest by the gaps and the
    # course edges: in the original most of the dark is this, not whole dark slates.
    blot_rng = np.random.default_rng([seed, 3])
    field = .75 * pb.periodic_noise(blot_rng, (size, size), 28, 28) + .45 * pb.periodic_noise(blot_rng, (size, size), 95, 95)
    near = np.clip(moss * 1.5 + .5 * (np.clip(v, 0, 1) > .55), 0, 1)
    blots = np.clip((field + .6 * near - BLOT_SHARE) * 1.3, 0, 1) ** 1.4      # soft, ragged edges
    depth = np.clip(.65 + .35 * pb.periodic_noise(blot_rng, (size, size), 20, 20), .3, 1)  # some blots only grime
    out *= (1 - BLOTS * blots * depth)[..., None]
    moss = np.maximum(moss, .6 * blots)          # lichen is matte like moss

    # water drips off the tail where a joint ends and leaves a dark trail over the courses below
    streak = drip_trails(spec, size, np.random.default_rng([seed, 1]))
    out *= (1 - DRIP * streak)[..., None]
    # wet black patches over several slates, ragged at the edge, with streaks running down from them
    wet = np.clip((pb.periodic_noise(rng, (size, size), 7, 7) - .55) * 1.4, 0, 1) * (.6 + .4 * ragged)
    out *= (1 - WET * wet)[..., None]
    fade = np.clip(pb.periodic_noise(rng, (size, size), 6, 6) - .6, 0, 1)
    out = out * (1 - FADE * fade[..., None]) + out.mean(axis=-1, keepdims=True) * 1.6 * FADE * fade[..., None]

    # Wet and dry parts: the wet patches, the water streaks and slates still wet from rain are
    # darker and smooth, so in the game they catch the sky and the sun; worn patches have a faint
    # sheen; moss and gaps stay matte. Drawn after everything above, so the colour keeps its seed.
    dry = rng.uniform(*DRY_ROUGH, count)
    damp = np.where(rng.random(count) < DAMP, rng.uniform(.4, .9, count), 0.)
    # a wet part of a slate: towards its tail or along a joint, where water lingers
    patch = blob(rng.uniform(.1, .9, count), rng.uniform(.55, 1., count),
                 rng.uniform(.25, .6, count), rng.uniform(.2, .45, count))
    patch = np.clip(1.6 * patch * (.5 + .5 * ragged) - .2, 0, 1) * (rng.random(count) < .25)[slate]
    by_slate = rng.uniform(.25, 1, count)[slate]       # the wet patches break up slate by slate
    wetness = np.clip(np.maximum.reduce([1.3 * wet * by_slate, damp[slate] * (.6 + .4 * ragged), patch,
                                         1.2 * streak]), 0, 1)
    out *= (1 - .22 * damp[slate] * (.6 + .4 * ragged))[..., None]
    rough = dry[slate] - (dry[slate] - WET_ROUGH) * wetness - WORN_ROUGH * sheen
    rough = np.clip(np.maximum(rough, moss), WET_ROUGH, 1)
    normal = passes['normal'] / (np.linalg.norm(passes['normal'], axis=-1, keepdims=True) + 1e-9)
    return dict(albedo=np.clip(out, 0, 1), shade=shade, moss=moss, sky=sky, u=u, v=v, wetness=wetness,
                roughness=rough, normal=normal)


def render(layout_path: Path, out_dir: Path, stones: list[Path], resolution: int, samples: int,
           sun: bool = False) -> Path:
    """Passes of the layout through the shared scene; the stone surfaces in the layout's order."""
    surfaces = {f'stone{i}': path for i, path in enumerate(stones)}
    return pb.render(layout_path, out_dir, surfaces, resolution, samples, {'sun': str(SUN if sun else 0)})


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    p = sub.add_parser('layout')
    p.add_argument('out', type=Path)
    p.add_argument('--seed', type=int, default=0)
    p = sub.add_parser('render')
    p.add_argument('layout', type=Path)
    p.add_argument('out_dir', type=Path)
    p.add_argument('stones', type=Path, nargs='+')
    p.add_argument('--resolution', type=int, default=2048)
    p.add_argument('--samples', type=int, default=64)
    p.add_argument('--sun', action='store_true', help='bake a sun too (the accepted slate has none)')
    p = sub.add_parser('compose')
    p.add_argument('layout', type=Path)
    p.add_argument('passes', type=Path)
    p.add_argument('out', type=Path)
    p.add_argument('--seed', type=int, default=0)
    args = parser.parse_args(argv)
    if args.command == 'layout':
        spec, points = layout(args.seed)
        pb.write_layout(args.out, spec, points)
        print(args.out)
    elif args.command == 'render':
        print(render(args.layout, args.out_dir, [p.resolve() for p in args.stones], args.resolution, args.samples,
                     sun=args.sun))
    elif args.command == 'compose':
        spec = json.loads(args.layout.read_text())
        result = compose(pb.read_passes(args.passes, spec['period']), spec, args.seed)
        pb.write_maps(args.out, (OUT_SIZE, OUT_SIZE), result['albedo'], result['normal'],
                      {k: result[k] for k in ('shade', 'moss', 'sky', 'wetness', 'roughness')})
        print(args.out)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
