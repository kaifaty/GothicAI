#!/usr/bin/env python3
"""Shingle roof NW_CITY_ROOF_01 from a 3D model (piece_bake.py): split spruce shingles laid in
Blender on laths and rafters, holes where shingles fell out, a verge board on each side and a ridge
board on top; passes rendered up the slope, weathering placed by the passes.

Measured from the original (reports/content/remaster/city-roofs/intent.md): a 13.4 × 6.8 m square,
14 shingles across and 10 courses, a shingle about 96 × 68 cm in sight. The course lines are the
original's (BUTT_V). Five holes at the original's places show the laths, a rafter in the big one,
and the dark of the attic. A dark verge board takes about 3% of the width on the left and right, a
lighter ridge board about 2% of the height at the top. Original pixels are never used: the wood
colour is an image_gen surface.

Thickness: hand-split shakes are 2–3 cm at the butt and thin to under 1 cm at the head; every course
lies on the one below, so the butt of each course stands as a step over the next.

    shingle_bake.py layout OUT.json [--seed N]
    shingle_bake.py render LAYOUT.json OUTDIR [--grey PNG] [--dark PNG] [--resolution PX] [--samples N] [--street]
    shingle_bake.py compose LAYOUT.json PASSES.exr OUT.png [--seed N]
    shingle_bake.py blockout LAYOUT.json PASSES.exr OUT.png
"""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

import numpy as np

import piece_bake as pb

PERIOD = (13.4354, 6.7872)   # metres per texture square (data/content/city-materials.csv)
ACROSS, COURSES = 14, 10
# Course lines of the original, V from the top: the dark line under each butt. The last one lies under
# the ridge board, V 0.018 of the square below.
BUTT_V = (1.018, .928, .830, .732, .631, .529, .418, .316, .211, .105)
RIDGE = .125                 # ridge board: this much of the top, metres (2% of the height)
VERGE = .40                  # verge board on each side, metres (3% of the width)
# Holes where shingles fell out (U, V of the original's square, intent.md «Главная деталь»):
# u0, u1, v0, v1, rafter seen in it
HOLES = ((.27, .38, .17, .51, True), (.41, .44, .63, .93, False), (.67, .70, .63, .78, False),
         (.672, .69, .13, .20, False), (.86, .88, .64, .72, False))
SNAP = .1                    # a hole edge this close to a course line ends at the line
STEP = .12                   # the sides of a hole step from course to course by this share of its width
REMNANTS = (1, 4)            # splinters left hanging in the top of a hole wider than 25 cm
REMNANT = (.30, .8)          # a long shingle left lying in the big hole: width, metres; share of the hole's height

HEAD = 2.3                   # shingle length in course heights: two layers and a lap everywhere
WIDTH_SPREAD = .3            # shingle widths within a course vary by this share
SLANT = .05                  # joints lean by up to this many radians
GAP = (.006, .016)           # keyway between neighbours, metres: wood swells when wet
THICK_BUTT = (.022, .032)    # heavy hand-split shake at the butt, metres
THICK_HEAD = (.006, .010)    # and at the hidden head
DROP = .012                  # butts sit higher or lower by this much, metres
TILT = .012                  # butt edge falls across a shingle by up to this much, metres
CORNER = .012                # radius of the worn butt corners, metres
RAGGED = .0025               # split edges wander by this much, metres
WOBBLE = .004                # random tilt of a shingle, metres per metre
CRACKS = .45                 # mean number of cracks running up from the butt, per shingle
BROKEN = .07                 # share of shingles with a butt corner broken off
CUP = (-.004, .007)          # bulge across a shingle (negative: cupped), metres
TWIST = (.3, (.004, .014))   # share of warped shingles, and how far a butt corner lifts, metres
GRAIN = (((.03, .06), (.0003, .0007)), ((.06, .12), (.0004, .0009)))  # split ridges: lengths, depths in metres;
                             # the fine grain is the surface image's bump: deep long waves read as corrugated sheet
ROUND = (.008, .2)           # weathered edge: width in metres, drop as a share of the butt; split wood stays sharp
LEAN = .02                   # grain leans from the slope line by up to this many radians
LATH = (.10, .04, .18, 2)    # laths: width, depth, metres above a course line, every second course:
                             # the original's holes show one lath each, a cross with the rafter in the big one
RAFTER = (.16, .20, 4.43, 4)  # rafters: width, depth, x of the first (in the big hole), count across the square
VOID = -.8                   # the dark under the roof
MARGIN = .3                  # copies of pieces within this distance of the square are built too
WOOD_METRES = 2.             # one image_gen wood surface covers this many metres
DARK_SHARE = .3              # shingles cut from the dark surface
CAMERA_TILT = 25.            # degrees from straight down, looking up the slope

SURFACES = [  # name, plain colour of the block-out (linear), metres per surface image, bump in metres
    dict(name='shingle', colour=[.30, .29, .26], metres=WOOD_METRES, bump=.0015),
    dict(name='shingle-dark', colour=[.26, .25, .22], metres=WOOD_METRES, bump=.0015),
    dict(name='board', colour=[.17, .16, .14], metres=WOOD_METRES, bump=.0015),
    dict(name='lath', colour=[.42, .34, .25], metres=WOOD_METRES, bump=.001),
    dict(name='ridge', colour=[.36, .34, .30], metres=WOOD_METRES, bump=.0015),
]
SHINGLE, SHINGLE_DARK, BOARD, LATH_SURFACE, RIDGE_SURFACE = 0, 1, 2, 3, 4
IMAGES = {'shingle': 'grey', 'shingle-dark': 'dark', 'board': 'dark', 'lath': 'grey', 'ridge': 'grey'}   # surface -> image

# Weathering (compose). The original: pale grey-olive faces, each dark at its top under the butt of the
# course above, with dark wet streaks hanging from that line; a thin black line under every butt;
# green moss at the lower edges and in patches; black holes; a dark mossy verge board, a paler ridge
# board. Colours are linear RGB.
DESATURATE = .45        # share of the generated wood's colour removed
TONE = .12              # shingles differ in tone (log-normal spread)
PALE = (.12, 1.3)       # share of bleached shingles, and how much paler
DAMP_DARK = (.16, .55)  # share of dark damp shingles, in groups where the roof stays wet, and their tone
FRESH = 2               # shingles put in later, still a little brown
PALE_BUTT = .55         # the lower part of a face, out of the wet lap, is bleached paler towards the butt
SKY = 1.                # how strongly the sky light of the model shades the albedo
SHADE = .9              # darkening under each butt, where the light from the ridge is blocked
BAND = (.5, .18, .08, .8)   # the wet band under each butt: mean reach in face heights, spread, share
                        # of long drips, their reach
CORE = (.27, .04)       # its solid black top right under the course line: face heights, spread
KEYWAY = (.35, .025)    # water runs through the joints of the course above: longer streaks under each,
                        # in face heights, and how wide, metres
BAND_DARK = .95         # how much darker the wood gets in the band: the grain still shows through
BAND_MOSS = .35         # and how much black moss lies on it
MOSS_DARK = (.0062, .0058, .0034)  # black-olive moss and wet dirt: the original's darkest fifth
MOSS_GREEN = (.034, .052, .016)  # olive-green moss: a fifth of the original's lit roof is greener than red
FRINGE = (.12, .5)      # green moss along the butt: reach in face heights, strength
PATCHES = (.55, .65)    # moss patches over the faces: share of the face, strength
JOINT_DIRT = .9         # dirt and moss packed in the joints and cracks
WET = .35               # darkening of the wet patches over several shingles
BLOTS = (.14, .7)       # dark moss and grime blots across shingles: share of the roof, darkness
ROT = (.75, .3)         # rot around the holes: darkness, reach in metres
CONTRAST = 1.2          # tone curve around the mean: in the game the original's middle tones sit lower
                        # than ours at the same mean (median 51 against 61 in the close shot)
VERGE_TONE = .55        # the verge board is darker and mossier
RIDGE_TONE = 1.15       # the ridge board is paler
ATTIC = .35             # laths and rafters in the dark under the roof: dusty, unlit
# Surface (roughness for the engine's GGX: full highlight below 0.5, none above 0.8)
DRY_ROUGH = (.86, .95)  # dry weathered wood: matte
WET_ROUGH = .72         # wet wood: a faint sheen at most, wood never shines like slate
OUT_SIZE = (2048, 1024)


def course_lines() -> np.ndarray:
    """y of the butt of every course, from the eaves up (y runs up the slope, V down)."""
    return np.array([PERIOD[1] * (1 - v) for v in BUTT_V])


def hole_shape(hole, lines: np.ndarray, rng):
    """Outline of a hole and the highest course it cuts. Edges near a course line end at it: the
    top runs on under the butt of the course above, the bottom is the broken top of the course
    below. The sides are breaks along the grain: jagged, with splinters."""
    from shapely import unary_union
    from shapely.geometry import Polygon
    u0, u1, v0, v1, _ = hole
    px, py = PERIOD
    x0, x1 = u0 * px, u1 * px
    y_top, y_bottom = py * (1 - v0), py * (1 - v1)
    every = np.concatenate([lines - py, lines, lines + py])
    near_top = every[np.argmin(abs(every - y_top))]
    snapped_top = abs(near_top - y_top) < SNAP
    near_bottom = every[np.argmin(abs(every - y_bottom))]
    if abs(near_bottom - y_bottom) < SNAP:
        y_bottom = near_bottom
    # the highest course the hole cuts: under a snapped top, the course below the line
    top_course_line = every[every < near_top - 1e-6].max() if snapped_top else every[every < y_top].max()
    top = near_top + .10 if snapped_top else y_top

    def side(x, y_from, y_to):
        ys = np.arange(y_from, y_to, .03)
        offset = np.cumsum(rng.normal(0, .006, len(ys)))
        offset -= np.linspace(offset[0], offset[-1], len(ys))
        spikes = rng.random(len(ys)) < .12
        offset += spikes * rng.normal(0, .025, len(ys))
        return [(x + o, y) for o, y in zip(offset, ys)]

    def across(y, x_from, x_to, rough):
        xs = np.arange(x_from, x_to, .025)
        if not rough:
            return [(x, y) for x in xs]
        # a break across the grain: long splinters along it
        return [(x, y + rng.choice([-1, 1]) * rng.exponential(.025)) for x in xs]
    # Shingles fall out one by one: in every course the hole is as wide as what broke there, so its
    # sides step from course to course instead of a cut-out window.
    cuts = sorted([y_bottom, top] + [y for y in every if y_bottom + .05 < y < top - .05])
    width = x1 - x0
    bands = []
    for k, (low, high) in enumerate(zip(cuts[:-1], cuts[1:])):
        a = x0 + rng.normal(0, STEP * width)
        b = x1 + rng.normal(0, STEP * width)
        if b - a < .5 * width:
            a, b = x0, x1
        first, last = k == 0, k == len(cuts) - 2
        y_low = low if first else low - .02          # bands overlap a little: one hole
        ring = (side(a, y_low, high) + across(high, a, b, last and not snapped_top)
                + side(b, y_low, high)[::-1] + across(y_low, a, b, first)[::-1])
        bands.append(Polygon(ring).buffer(0))
    hole = pb.largest(unary_union(bands))
    # Splinters of the broken shingles still hang in the top of the hole, nailed under the course
    # above: the hole reads as rot, not as a window.
    low, high = cuts[-2], cuts[-1]
    for _ in range(rng.integers(*REMNANTS) if width > .25 else 0):
        x = rng.uniform(x0 + .04, x1 - .04)
        wide = rng.uniform(.025, .08)
        end = high - rng.uniform(.45, .95) * (high - low - (.10 if snapped_top else 0))
        tip = x + rng.normal(0, wide / 3)
        strip = Polygon([(x - wide / 2, high + .2), (x + wide / 2, high + .2), (x + wide / 2, end + rng.uniform(0, .06)),
                         (tip, end), (x - wide / 2, end + rng.uniform(0, .06))])
        hole = pb.largest(hole.difference(strip))
    return hole, top_course_line


def shingle_terms(rng, x_left, lean, width, y_bottom, height, visible_top, butt):
    """Shape of a shingle's face: cupping, split ridges along the grain, a warp, weathered edges.
    The hidden head stays plain."""
    seen = [0., -1 / .12, (visible_top + .12) / .12]
    tan = math.tan(lean)
    across = [1 / width, -tan / width, (tan * y_bottom - x_left) / width]   # 0 at the left side, 1 at the right
    terms = [dict(kind='bow', amp=float(rng.uniform(*CUP)), across=across, mask=seen)]
    grain = [-math.sin(lean), math.cos(lean)]
    for (low, high), (amp_low, amp_high) in GRAIN:
        terms.append(dict(kind='ripple', amp=float(rng.uniform(amp_low, amp_high)),
                          dir=[grain[1], -grain[0]], length=float(rng.uniform(low, high)),
                          phase=float(rng.uniform(0, 2 * np.pi)), bend=float(rng.uniform(.1, .5)),
                          bend_length=float(rng.uniform(.5, 1.5)), bend_phase=float(rng.uniform(0, 2 * np.pi)),
                          mask=seen))
    if rng.random() < TWIST[0]:
        if rng.random() < .5:
            across = [-c for c in across[:2]] + [1 - across[2]]
        terms.append(dict(kind='twist', amp=float(rng.uniform(*TWIST[1])), across=across,
                          along=[0., -1 / height, 1 + y_bottom / height], mask=seen))
    terms.append(dict(kind='round', width=ROUND[0], drop=float(ROUND[1] * butt), mask=seen))
    return terms


def shingle_outline(rng, x_left, lean_left, x_right, lean_right, y_bottom, length):
    """Outline of one shingle: straight split sides along its joints, a butt edge a little uneven and
    worn at the corners, sometimes a corner broken off, cracks running up from the butt."""
    from shapely import affinity
    from shapely.geometry import Polygon
    y_left = y_bottom + rng.normal(0, DROP)
    y_right = y_left + rng.uniform(-TILT, TILT)

    def at(x0, lean, y):
        return x0 + math.tan(lean) * (y - y_bottom)
    top = y_bottom + length
    body = Polygon([(at(x_left, lean_left, y_left), y_left), (at(x_right, lean_right, y_right), y_right),
                    (at(x_right, lean_right, top), top), (at(x_left, lean_left, top), top)])
    shape = pb.largest(body.buffer(-CORNER, quad_segs=6).buffer(CORNER, quad_segs=6))
    shape = pb.ragged(shape, rng, RAGGED, wavelengths=(.04, .09, .2))
    width = x_right - x_left
    if rng.random() < BROKEN:
        # a butt corner broken off along a slanted crack
        left = rng.random() < .5
        reach = rng.uniform(.1, .25) * width
        rise = rng.uniform(.04, .14)
        x_side = x_left if left else x_right
        x_edge = x_side + (reach if left else -reach)
        corner = Polygon([(x_side - (.1 if left else -.1), y_bottom - .1), (x_edge, y_bottom - .1),
                          (x_edge, y_left), (x_side - (.1 if left else -.1), y_bottom + rise)])
        shape = pb.largest(shape.difference(pb.ragged(corner, rng, .004)))
    cracks = []
    for _ in range(rng.poisson(CRACKS)):
        # a crack along the grain from the butt: a narrow wedge
        xc = x_left + rng.uniform(.15, .85) * width
        wide = rng.uniform(.003, .012)
        reach = rng.uniform(.25, .85) * length / HEAD
        wedge = Polygon([(xc - wide / 2, y_bottom - .1), (xc + wide / 2, y_bottom - .1),
                         (xc + wide / 2, y_bottom), (xc + rng.normal(0, .01), y_bottom + reach),
                         (xc - wide / 2, y_bottom)])
        shape = pb.largest(shape.difference(affinity.rotate(wedge, math.degrees(rng.normal(0, .02)),
                                                            origin=(xc, y_bottom))))
        cracks.append([round(xc, 4), round(reach, 4)])
    return shape.simplify(.001), cracks


def layout(seed: int = 0) -> tuple[dict, dict]:
    """Pieces of one texture square, with copies across its edges so the render tiles."""
    from shapely import affinity, unary_union
    from shapely.geometry import Polygon, box
    rng = np.random.default_rng(seed)
    px, py = PERIOD
    lines = course_lines()
    heights = np.diff(np.append(lines, lines[0] + py))
    hole_rng = np.random.default_rng([seed, 1])
    holes = [hole_shape(h, lines, hole_rng) for h in HOLES]
    pieces, points = [], {}

    def add(shape, bottom, top, surface, uv, terms=(), kind='', mesh=None, **extra):
        index = len(pieces)
        pieces.append(pb.piece(index, shape, bottom, top, surface, uv, terms,
                               pb.copies(shape.bounds, PERIOD, MARGIN), kind=kind, **extra))
        if mesh is not None and len(mesh):
            points[index] = mesh
        return index

    # shingles, course by course from the eaves
    for course in range(COURSES):
        y_bottom, height = lines[course], heights[course]
        length = HEAD * height
        visible_top = y_bottom + height
        widths = rng.uniform(1 - WIDTH_SPREAD, 1 + WIDTH_SPREAD, ACROSS)
        widths *= px / widths.sum()
        x0 = rng.uniform(0, px / ACROSS) + np.concatenate([[0], np.cumsum(widths)])
        leans = rng.uniform(-SLANT, SLANT, ACROSS)
        leans = np.append(leans, leans[0])
        cut = [shape for shape, top_line in holes if top_line >= y_bottom - 1e-6]
        cut = unary_union([affinity.translate(s, dx, dy) for s in cut for dx in (-px, 0, px) for dy in (-py, 0, py)])
        for k in range(ACROSS):
            gap_left, gap_right = rng.uniform(*GAP, 2) / 2
            x_left, x_right = x0[k] + gap_left, x0[k + 1] - gap_right
            shape, cracks = shingle_outline(rng, x_left, leans[k], x_right, leans[k + 1], y_bottom, length)
            parts = [shape]
            if not cut.is_empty and shape.intersects(cut):
                # a hole takes the shingle or breaks it; what is left of it stays, on either side
                rest = shape.difference(cut)
                for ring in [r for g in getattr(rest, 'geoms', [rest]) if g.geom_type == 'Polygon' for r in g.interiors]:
                    # a hole inside the shingle: it has rotted away up to its hidden head
                    minx, miny, maxx, _ = ring.bounds
                    rest = rest.difference(box(minx, miny, maxx, 1e3))
                parts = [g for g in getattr(rest, 'geoms', [rest]) if g.geom_type == 'Polygon' and g.area > .01]
            if not parts:
                continue
            butt, head = rng.uniform(*THICK_BUTT), rng.uniform(*THICK_HEAD)
            # Each course rests on the one below: its butt is lifted by the shingles under it, its head
            # lies on a lath. The lift is the thickness under the butt times the length in courses.
            lift = (butt + (head - butt) / HEAD) * HEAD
            cx = (x_left + x_right) / 2
            tilt = rng.normal(0, WOBBLE, 2)
            bottom = pb.plane_through(lift, cx, y_bottom, tilt[0], -lift / length + tilt[1])
            taper = (head - butt) / length
            top = [bottom[0] + butt - taper * y_bottom, bottom[1], bottom[2] + taper]
            lean = (leans[k] + leans[k + 1]) / 2 + rng.normal(0, LEAN / 2)
            visible = box(-1e3, -1e3, 1e3, visible_top + .12)
            # the face in sight lies inside the surface image, across the grain: no seam on it
            c, s = math.cos(lean), math.sin(lean)
            face = np.asarray(shape.intersection(visible).envelope.exterior.coords)
            across_grain = (face[:, 0] * c + face[:, 1] * s) / WOOD_METRES
            low, high = .02 - across_grain.min(), .98 - across_grain.max()
            uv = [float(rng.uniform(low, max(low, high))), float(rng.uniform(0, 1)), lean]
            surface = SHINGLE_DARK if rng.random() < DARK_SHARE else SHINGLE
            terms = shingle_terms(rng, x_left, leans[k], x_right - x_left, y_bottom, height, visible_top, butt)
            for part in parts:
                mesh = pb.mesh_points(part, visible, across=.008, along=.03, angle=lean, rings=(.003, .008, .015))
                add(part, bottom, top, surface, uv, terms, kind='shingle', mesh=mesh, course=course,
                    index_in_course=k, y_bottom=float(y_bottom), height=float(height), butt=float(butt),
                    cracks=cracks, joint=[float(x0[k]), float(leans[k])], width=float(x_right - x_left))

    # In the big hole one long shingle survived, its head broken to a point: the original's hole is
    # a cross of wood in the dark, not a window.
    for (shape, _), hole in zip(holes, HOLES):
        if not hole[4]:
            continue
        minx, miny, maxx, maxy = shape.bounds
        wide, share = REMNANT
        x = RAFTER[2] + rng.normal(0, .03)
        top = miny + share * (maxy - miny)
        tip = x + rng.uniform(-.4, .4) * wide
        board = Polygon([(x - wide / 2, miny - .08), (x + wide / 2, miny - .08), (x + wide / 2, top - rng.uniform(.1, .25)),
                         (tip, top), (x - wide / 2, top - rng.uniform(.1, .3))])
        board = pb.ragged(board, rng, .004)
        lean = rng.normal(0, LEAN)
        mesh = pb.mesh_points(board, None, across=.008, along=.03, angle=lean, rings=(.003, .008, .015))
        terms = [dict(kind='ripple', amp=float(rng.uniform(a0, a1)), dir=[math.cos(lean), math.sin(lean)],
                      length=float(rng.uniform(l0, l1)), phase=float(rng.uniform(0, 2 * np.pi)))
                 for (l0, l1), (a0, a1) in GRAIN] + [dict(kind='round', width=ROUND[0], drop=.004)]
        bottom = pb.plane_through(.006, x, miny, 0., -.004)       # lying on the laths, its head a little lower
        add(board, bottom, [bottom[0] + .022, 0., bottom[2]], SHINGLE, [rng.uniform(.1, .5), rng.uniform(0, 1), lean],
            terms, kind='remnant', mesh=mesh)

    # laths under every course, joined over a rafter; rafters under them
    width, depth, above, every = LATH
    r_width, r_depth, r_first, r_count = RAFTER
    for line in lines[::every]:
        y = line + above
        shape = box(r_first + .002, y - width / 2, r_first + px - .002, y + width / 2)
        add(shape, [-depth, 0, 0], [0, 0, 0], LATH_SURFACE, [*rng.uniform(0, 1, 2), math.pi / 2], kind='lath')
    for k in range(r_count):
        x = r_first + k * px / r_count
        shape = box(x - r_width / 2, .002, x + r_width / 2, py - .002)
        add(shape, [-depth - r_depth, 0, 0], [-depth, 0, 0], LATH_SURFACE, [*rng.uniform(0, 1, 2), 0.], kind='rafter')

    # boards over the shingles: the ridge board along the top, a verge board down each side
    high = max(p['top'][0] + p['top'][1] * x + p['top'][2] * y for p in pieces if p['kind'] == 'shingle'
               for x, y in p['outline'])
    ridge = box(VERGE / 2, py - RIDGE, VERGE / 2 + px - .004, py + .04)
    # the boards rest on the butts of the courses under them
    add(pb.ragged(ridge, rng, .002), [high - .015, 0, 0], [high + .02, 0, 0], RIDGE_SURFACE, [*rng.uniform(0, 1, 2), math.pi / 2],
        [dict(kind='round', width=.012, drop=.008)], kind='ridge', closed=True)
    for x_from, x_to in ((.004, VERGE), (px - VERGE, px - .004)):
        verge = box(x_from, py - .06 - py + .002, x_to, py - .06)
        add(pb.ragged(verge, rng, .002), [high - .005, 0, 0], [high + .035, 0, 0], BOARD,
            [*rng.uniform(0, 1, 2), rng.normal(0, LEAN)], [dict(kind='round', width=.012, drop=.008)],
            kind='verge', closed=True)

    spec = dict(tool='scripts/content/shingle_bake.py', seed=seed, period=list(PERIOD), across=ACROSS,
                courses=COURSES, camera=dict(tilt=CAMERA_TILT), sky=dict(pitch=45., ground=.08),
                ao=dict(distance=.15, crevice=.025), void=dict(z=VOID), surfaces=SURFACES,
                holes=[dict(u=[h[0], h[1]], v=[h[2], h[3]], rafter=h[4]) for h in HOLES],
                course_lines=[float(v) for v in lines], pieces=pieces)
    return spec, points


def _table(spec: dict, name: str, default: float, piece: np.ndarray) -> np.ndarray:
    """Per-pixel value of a piece field; the void (piece -1) gets DEFAULT."""
    values = np.array([float(p.get(name, default)) for p in spec['pieces']] + [default])
    return values[piece]


def _spread_columns(field: np.ndarray, reach: int) -> np.ndarray:
    """Widen sparse column values to their neighbours, wrapping (a drip is a few pixels wide)."""
    out = field.copy()
    for d in range(1, reach + 1):
        fade = 1 - d / (reach + 1)
        out = np.maximum(out, np.maximum(np.roll(field, d), np.roll(field, -d)) * fade)
    return out


def _to_joint_above(spec: dict, piece: np.ndarray, x: np.ndarray) -> np.ndarray:
    """Distance across the slope, metres, from each shingle pixel to the nearest joint between the
    shingles of the course above; far for everything else."""
    px = spec['period'][0]
    courses = max(p.get('course', 0) for p in spec['pieces']) + 1
    joints = {c: np.sort([p['joint'][0] % px for p in spec['pieces'] if p['kind'] == 'shingle' and p['course'] == c])
              for c in range(courses)}
    course = np.array([p.get('course', -1) if p['kind'] == 'shingle' else -1 for p in spec['pieces']] + [-1])[piece]
    distance = np.full(piece.shape, 1e3)
    for c in range(courses):
        here = course == c
        above = joints[(c + 1) % courses]
        if not here.any() or not len(above):
            continue
        ring = np.concatenate([above - px, above, above + px])
        xs = x[here] % px
        k = np.clip(np.searchsorted(ring, xs), 1, len(ring) - 1)
        distance[here] = np.minimum(abs(xs - ring[k - 1]), abs(ring[k] - xs))
    return distance


def _near_void(void: np.ndarray, metres_per_px: float, reach: float) -> np.ndarray:
    """1 at a hole's edge, falling to 0 at REACH metres, wrapping across the square's edges."""
    from scipy import ndimage
    if not void.any():
        return np.zeros(void.shape)
    rows, cols = void.shape
    step = 4                                    # a 1–2 cm grid is plenty for a 30 cm halo
    small = void[::step, ::step]
    h, w = small.shape
    distance = ndimage.distance_transform_edt(np.tile(~small, (3, 3)))[h:2 * h, w:2 * w] * metres_per_px * step
    distance = ndimage.zoom(distance, (rows / h, cols / w), order=1)[:rows, :cols]
    return np.clip(1 - distance / reach, 0, 1) ** 1.5 * ~void


def compose(passes: dict, spec: dict, seed: int = 0) -> dict:
    """Albedo (linear RGB) and the masks it was made with, at the render size."""
    rng = np.random.default_rng(seed)
    piece = passes['piece']
    rows, cols = piece.shape
    period = spec['period']
    px, py = period
    table = spec['pieces']
    count = len(table)
    kinds = np.array([p['kind'] for p in table] + ['void'])
    kind = kinds[piece]
    shingle = kind == 'shingle'
    # v: 0 at the shingle's butt, 1 at the top of its face under the butt of the course above
    y_bottom = _table(spec, 'y_bottom', 0., piece)
    height = _table(spec, 'height', 1., piece)
    v = np.where(shingle, (((passes['y'] - y_bottom + py / 2) % py) - py / 2) / height, 0.)

    colour = passes['colour']
    grey = colour.mean(axis=-1, keepdims=True)
    out = grey + (colour - grey) * (1 - DESATURATE)

    # shingles differ: some bleached, some dark and damp in groups, a couple put in later
    tone = np.exp(rng.normal(0, TONE, count)) * np.where(rng.random(count) < PALE[0], PALE[1], 1.)
    damp_field = pb.noise_metres(np.random.default_rng([seed, 2]), (rows, cols), period, 1.6)
    centre = np.array([np.mean(p['outline'], axis=0) for p in table])
    at = ((((py - centre[:, 1]) % py) / py * rows).astype(int) % rows, ((centre[:, 0] % px) / px * cols).astype(int) % cols)
    damp_here = damp_field[at]
    damp_odds = DAMP_DARK[0] * np.exp(1.5 * damp_here) / np.exp(1.5 * damp_here).mean()
    tone *= np.where(rng.random(count) < damp_odds, DAMP_DARK[1], 1.)
    is_shingle = np.array([p['kind'] == 'shingle' for p in table])
    fresh = rng.choice(np.flatnonzero(is_shingle), min(FRESH, int(is_shingle.sum())), replace=False)
    tint = np.ones((count + 1, 3))
    tint[fresh] = (1.12, 1.0, .84)
    tone *= np.select([kinds[:-1] == 'verge', kinds[:-1] == 'ridge', np.isin(kinds[:-1], ['lath', 'rafter'])],
                      [VERGE_TONE, RIDGE_TONE, ATTIC], 1.)
    out *= np.append(tone, 1.)[piece][..., None] * tint[piece]
    out *= (1 + .08 * pb.noise_metres(rng, (rows, cols), period, .12))[..., None]      # mottling
    out *= (1 + PALE_BUTT * np.clip(1 - v, 0, 1) * shingle)[..., None]

    # light and shade from the model
    sky = passes['sky'] / np.percentile(passes['sky'][shingle], 95)
    out *= np.clip(sky, 0, 1.2)[..., None] ** SKY
    shade = pb.overhang_shade(passes['height'], px / cols)
    out *= (1 - SHADE * shade)[..., None]

    # The wet band under every butt: dark moss and water hanging from the course line in streaks,
    # some running far down the face (the original's look, row after row).
    mean, spread, long_share, long_reach = BAND
    streaks = (.55 * pb.periodic_noise(rng, (1, cols), px / .035, 1)[0] + .35 * pb.periodic_noise(rng, (1, cols), px / .12, 1)[0]
               + .25 * pb.periodic_noise(rng, (1, cols), px / .012, 1)[0])
    reach = mean + spread * np.clip(streaks, -2, 2) / 2
    drips = np.where(rng.random(cols) < long_share / 8, rng.uniform(.4, 1, cols) * long_reach, 0.)
    reach = np.maximum(reach, _spread_columns(drips, max(1, round(.008 / px * cols))))
    reach = reach[None, :] * np.append(rng.uniform(.6, 1.4, count), 1.)[piece]
    reach = reach + KEYWAY[0] * np.exp(-(_to_joint_above(spec, piece, passes['x']) / KEYWAY[1]) ** 2)
    edge = 1 - reach + .04 * pb.noise_metres(rng, (rows, cols), period, .02)       # a ragged lower edge
    streak = np.clip((v - edge) / .08, 0, 1) * (.75 + .25 * np.clip((v - edge) / np.maximum(reach, .05), 0, 1))
    core = CORE[0] + CORE[1] * np.clip(.6 * pb.periodic_noise(rng, (1, cols), px / .15, 1)[0]
                                       + .5 * pb.periodic_noise(rng, (1, cols), px / .03, 1)[0], -2, 2) / 2
    solid = np.clip((v - (1 - core[None, :]) + .02 * pb.noise_metres(rng, (rows, cols), period, .015)) / .04, 0, 1)
    band = np.where(shingle, np.maximum(streak, solid), 0.)
    # the solid top is nearly black, the streaks a little less; the grain still shows through both
    dark = np.maximum(BAND_DARK * solid, .82 * BAND_DARK * streak) * (.93 + .07 * np.clip(
        pb.noise_metres(rng, (rows, cols), period, .06), -1, 1))
    dark = np.where(shingle, dark, 0.)
    out = out * (1 - dark[..., None]) + np.array(MOSS_DARK) * (BAND_MOSS * band)[..., None]

    # green moss: a fringe along each butt and patches over the faces, more on the lower part
    fringe = np.where(shingle, np.clip(1 - v / FRINGE[0], 0, 1), 0.)
    fringe *= np.clip(.6 + .6 * pb.noise_metres(rng, (rows, cols), period, .06), 0, 1)
    field = (.7 * pb.noise_metres(rng, (rows, cols), period, .5) + .4 * pb.noise_metres(rng, (rows, cols), period, .15)
             + .25 * pb.noise_metres(rng, (rows, cols), period, .03))          # ragged edges
    patches = np.clip((field - np.quantile(field, 1 - PATCHES[0]) + .3 * (.5 - v)) * 4, 0, 1) * shingle * (1 - band)
    green = np.clip(FRINGE[1] * fringe + PATCHES[1] * patches, 0, 1)
    out = out * (1 - green[..., None]) + np.array(MOSS_GREEN) * green[..., None]
    verge = np.isin(kind, ['verge'])
    out = np.where(verge[..., None], out * .6 + np.array(MOSS_GREEN) * .4, out)

    # dirt and moss packed in the joints, cracks and corners
    gap = np.clip((1 - pb.smooth(passes['crevice'])) / .45, 0, 1)
    out = out * (1 - JOINT_DIRT * gap[..., None]) + np.array(MOSS_DARK) * JOINT_DIRT * gap[..., None]

    # Dark moss and grime in blots across several shingles, and rot around the holes: the wood
    # next to a hole is the wettest on the roof (the original is darkest there).
    field = (.7 * pb.noise_metres(rng, (rows, cols), period, .35) + .45 * pb.noise_metres(rng, (rows, cols), period, .1)
             + .25 * pb.noise_metres(rng, (rows, cols), period, .03))
    blots = np.clip((field - np.quantile(field, 1 - BLOTS[0])) * 3, 0, 1) * shingle
    rot = _near_void(piece == -1, px / cols, ROT[1]) * np.clip(.55 + .6 * pb.noise_metres(rng, (rows, cols), period, .06), 0, 1)
    grime = np.clip(np.maximum(BLOTS[1] * blots, ROT[0] * rot) * np.isin(kind, ['shingle', 'remnant']), 0, 1)
    out = out * (1 - grime[..., None]) + np.array(MOSS_DARK) * (.5 * grime)[..., None]

    # wet patches over several shingles
    wet = np.clip((pb.noise_metres(rng, (rows, cols), period, 1.2) - .7) * 1.2, 0, 1)
    out *= (1 - WET * wet)[..., None]

    # wet and dry: the band and the wet patches are damp, moss is matte
    void = kind == 'void'
    luminance = out @ np.array([.2126, .7152, .0722])
    mean = luminance[~void].mean()
    out *= (((np.maximum(luminance, 1e-6) / mean) ** CONTRAST * mean) / np.maximum(luminance, 1e-6))[..., None]
    out[void] = .002                              # the dark of the attic through a hole
    dry = np.append(rng.uniform(*DRY_ROUGH, count), 1.)[piece]
    wetness = np.clip(np.maximum(.6 * wet, .8 * band), 0, 1) * ~void
    rough = np.clip(dry - (dry - WET_ROUGH) * wetness, WET_ROUGH, 1)
    rough = np.maximum(rough, np.clip(np.maximum(green, grime) * 1.2, 0, .97))
    normal = passes['normal'] / (np.linalg.norm(passes['normal'], axis=-1, keepdims=True) + 1e-9)
    moss = np.clip(np.maximum.reduce([green, gap, grime]), 0, 1)
    return dict(albedo=np.clip(out, 0, 1), shade=shade, band=band, moss=moss, wetness=wetness,
                roughness=rough, normal=normal)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    p = sub.add_parser('layout')
    p.add_argument('out', type=Path)
    p.add_argument('--seed', type=int, default=0)
    p = sub.add_parser('render')
    p.add_argument('layout', type=Path)
    p.add_argument('out_dir', type=Path)
    p.add_argument('--grey', type=Path, help='image_gen surface: weathered grey split spruce')
    p.add_argument('--dark', type=Path, help='image_gen surface: dark damp split spruce')
    p.add_argument('--resolution', type=int, default=4096)
    p.add_argument('--samples', type=int, default=64)
    p.add_argument('--street', action='store_true', help='perspective views from the street instead of the passes')
    p.add_argument('--close', help='x,y in metres of the close street view')
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
        spec, points = layout(args.seed)
        pb.write_layout(args.out, spec, points)
        print(args.out)
    elif args.command == 'render':
        images = {}
        for name, source in (('grey', args.grey), ('dark', args.dark)):
            if source:
                args.out_dir.mkdir(parents=True, exist_ok=True)
                images[name] = pb.seamless_along(source, args.out_dir / f'surface-{name}.png')
        surfaces = {surface: images[image] for surface, image in IMAGES.items() if image in images}
        options = {'view': 'street'} if args.street else {}
        if args.close:
            options['close'] = args.close
        print(pb.render(args.layout, args.out_dir, surfaces, args.resolution, args.samples, options))
    elif args.command == 'compose':
        spec = json.loads(args.layout.read_text())
        result = compose(pb.read_passes(args.passes, spec['period']), spec, args.seed)
        pb.write_maps(args.out, OUT_SIZE, result['albedo'], result['normal'],
                      {k: result[k] for k in ('shade', 'band', 'moss', 'wetness', 'roughness')})
        print(args.out)
    elif args.command == 'blockout':
        spec = json.loads(args.layout.read_text())
        passes = pb.read_passes(args.passes, spec['period'])
        width = passes['piece'].shape[1] // 2
        pb.write_preview(passes, args.out, (width, round(width * spec['period'][1] / spec['period'][0])))
        print(args.out)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
