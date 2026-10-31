#!/usr/bin/env python3
"""Underside of the roof NW_CITY_ROOF_INSIDE_01 from a 3D model (piece_bake.py): boards of the roof's
sheathing on heavy rafters, a rail along the top and the bottom, the dark underside of the shingles
showing through the gaps; seen from the room below, weathered by the passes.

Measured from the original (reports/content/remaster/city-roofs/intent.md): a 14.5 × 6.4 m square,
three fields across U between rafters about 4.8 m apart (U ≈ 0, 0.33, 0.67), twenty courses of narrow
boards down V, a heavier rail at the top and bottom edge. The rafters and rails stand in front of the
boards, towards the room. Original pixels are never used: the wood is an image_gen surface.

    inside_bake.py layout OUT.json [--seed N]
    inside_bake.py render LAYOUT.json OUTDIR [--grey PNG] [--dark PNG] [--resolution PX] [--samples N] [--street]
    inside_bake.py compose LAYOUT.json PASSES.exr OUT.png [--seed N]
    inside_bake.py blockout LAYOUT.json PASSES.exr OUT.png
"""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

import numpy as np

import piece_bake as pb

PERIOD = (14.48, 6.42)       # metres per texture square (data/content/city-materials.csv)
RAFTERS = (.0, .33, .67)     # U of the rafters: the first one stands on the square's edge
RAFTER = (.42, .22)          # rafter: width, depth in front of the boards, metres (heavier than a board)
RAIL = (.52, .25)            # the rail across the top and bottom edge: width, depth, metres
COURSES = 20                 # board courses down V between the rails, about 32 cm apart
BOARD_WIDTH = (.12, .17)     # spaced sheathing under wood shingles: narrow boards with wide gaps that
                             # let the shingles dry; the original is light boards on dark gaps
BOARD_THICK = (.022, .032)   # metres
BOARD_LENGTH = (.5, 1.6)     # short boards, butt-jointed along a course: the original's brick-like rows
SAG = (.002, .012)           # a board sags towards the room between its nails, metres
CUP = (-.004, .004)          # and cups across its width
WOBBLE = .006                # boards sit a little out of plane, metres
RAGGED = .002                # sawn and riven edges wander by this much, metres
LEAN = .01                   # grain lean, radians
BACK = -.07                  # the underside of the shingles behind the boards
SHINGLE = (9, .96, .045)     # seen through the gaps: courses down V, mean width, how far each lap steps
MARGIN = .3
WOOD_METRES = 2.
CAMERA_TILT = 20.            # degrees from straight up, looking along the slope

SURFACES = [  # name, plain colour of the block-out (linear), metres per surface image, bump in metres
    dict(name='board', colour=[.20, .17, .14], metres=WOOD_METRES, bump=.0012),
    dict(name='timber', colour=[.30, .26, .21], metres=WOOD_METRES, bump=.0015),
    dict(name='back', colour=[.06, .05, .045], metres=WOOD_METRES, bump=.001),
]
BOARD, TIMBER, BACK_SURFACE = 0, 1, 2
IMAGES = {'board': 'dark', 'timber': 'grey', 'back': 'dark'}   # surface -> image


def course_lines(rng) -> np.ndarray:
    """y of the centre of every board course between the rails, and the board's width."""
    px, py = PERIOD
    rail = RAIL[0]
    step = (py - rail) / COURSES
    centres = rail / 2 + step * (np.arange(COURSES) + .5) + rng.normal(0, .012, COURSES)
    return centres, rng.uniform(*BOARD_WIDTH, COURSES)


def board_terms(rng, x0, x1, y0, y1):
    """A board sags between its ends and cups across its width; the grain ripples along it."""
    length, width = x1 - x0, y1 - y0
    along = [1 / length, 0., -x0 / length]              # 0 at one end, 1 at the other
    across = [0., 1 / width, -y0 / width]
    terms = [dict(kind='bow', amp=float(rng.uniform(*SAG)), across=along),
             dict(kind='bow', amp=float(rng.uniform(*CUP)), across=across)]
    for length_range, amp_range in (((.03, .06), (.0002, .0005)), ((.06, .12), (.0003, .0007))):
        terms.append(dict(kind='ripple', amp=float(rng.uniform(*amp_range)), dir=[0., 1.],
                          length=float(rng.uniform(*length_range)), phase=float(rng.uniform(0, 2 * np.pi)),
                          bend=float(rng.uniform(.1, .4)), bend_length=float(rng.uniform(.5, 1.5)),
                          bend_phase=float(rng.uniform(0, 2 * np.pi))))
    terms.append(dict(kind='round', width=.006, drop=.004))
    return terms


def timber_terms(rng, across, length_axis):
    """A hewn timber: faces a little wavy from the axe, edges worn round."""
    terms = [dict(kind='bow', amp=float(rng.uniform(-.004, .006)), across=across)]
    for (l0, l1), (a0, a1) in (((.08, .2), (.0006, .0015)), ((.25, .6), (.001, .003))):
        terms.append(dict(kind='ripple', amp=float(rng.uniform(a0, a1)), dir=length_axis,
                          length=float(rng.uniform(l0, l1)), phase=float(rng.uniform(0, 2 * np.pi)),
                          bend=float(rng.uniform(.2, .8)), bend_length=float(rng.uniform(.3, .8)),
                          bend_phase=float(rng.uniform(0, 2 * np.pi))))
    terms.append(dict(kind='round', width=.03, drop=.02))
    return terms


def layout(seed: int = 0) -> tuple[dict, dict]:
    """Pieces of one texture square, with copies across its edges so the render tiles."""
    from shapely.geometry import box
    rng = np.random.default_rng(seed)
    px, py = PERIOD
    pieces, points = [], {}

    def add(shape, bottom, top, surface, uv, terms=(), kind='', mesh=None, **extra):
        index = len(pieces)
        pieces.append(pb.piece(index, shape, bottom, top, surface, uv, terms,
                               pb.copies(shape.bounds, PERIOD, MARGIN), kind=kind, **extra))
        if mesh is not None and len(mesh):
            points[index] = mesh
        return index

    # the underside of the shingles behind the boards: courses of wide shingles, each lap a step
    rows, mean_width, step = SHINGLE
    height = py / rows
    for row in range(rows):
        y0 = row * height
        x = rng.uniform(0, px)
        end = x + px
        while x < end - .1:
            width = min(mean_width * rng.uniform(.7, 1.3), end - x)
            if end - (x + width) < .3:
                width = end - x
            shape = box(x, y0, x + width - rng.uniform(.006, .015), y0 + height + .02)
            # from the room the butt of a shingle is its lower edge, nearest the boards
            bottom = pb.plane_through(BACK - step - .01, x, y0, 0., step / height)
            add(shape, bottom, [bottom[0] + .01, 0., bottom[2]], BACK_SURFACE,
                [*rng.uniform(0, 1, 2), rng.normal(0, LEAN)], kind='back', closed=True)
            x += width

    # board courses: short boards butt-jointed along the course, wide gaps between the courses
    centres, widths = course_lines(rng)
    for course, (centre, width) in enumerate(zip(centres, widths)):
        lo, hi = centre - width / 2, centre + width / 2
        x = rng.uniform(0, px)
        end = x + px
        while x < end - .05:
            length = min(rng.uniform(*BOARD_LENGTH), end - x)
            if end - (x + length) < .5:
                length = end - x
            joint = rng.uniform(.004, .02)
            x1 = x + length - joint
            shape = pb.ragged(box(x, lo + rng.normal(0, .004), x1, hi + rng.normal(0, .004)), rng, RAGGED,
                              wavelengths=(.08, .2, .6))
            thick = rng.uniform(*BOARD_THICK)
            tilt = rng.normal(0, WOBBLE / max(length, 1), 2)
            bottom = pb.plane_through(-thick - .01, (x + x1) / 2, (lo + hi) / 2, tilt[0], tilt[1])
            top = [bottom[0] + thick, bottom[1], bottom[2]]
            lean = math.pi / 2 + rng.normal(0, LEAN)          # grain along the board
            mesh = pb.mesh_points(shape, None, across=.01, along=.04, angle=lean, rings=(.003, .008))
            add(shape, bottom, top, BOARD, [*rng.uniform(0, 1, 2), lean], board_terms(rng, x, x1, lo, hi),
                kind='board', mesh=mesh, course=course, y_low=float(lo), y_high=float(hi))
            x += length

    # rafters in front of the boards, between the rails; the rail across the top and bottom edge
    width, depth = RAFTER
    for u in RAFTERS:
        x = u * px + rng.normal(0, .02)
        shape = box(x - width / 2, RAIL[0] / 2 - .02, x + width / 2, py - RAIL[0] / 2 + .02)
        shape = pb.ragged(shape, rng, .003, wavelengths=(.1, .3, .9))
        lean = rng.normal(0, LEAN)
        across = [1 / width, 0., -(x - width / 2) / width]
        mesh = pb.mesh_points(shape, None, across=.012, along=.04, angle=lean, rings=(.005, .015, .03))
        add(shape, [-.01, 0, 0], [depth, 0, 0], TIMBER, [*rng.uniform(0, 1, 2), lean],
            timber_terms(rng, across, [1., 0.]), kind='rafter', mesh=mesh)
    width, depth = RAIL
    x0 = RAFTERS[0] * px + RAFTER[0] / 4          # its joint hidden by the first rafter's corner
    shape = box(x0, -width / 2, x0 + px - .004, width / 2)
    shape = pb.ragged(shape, rng, .003, wavelengths=(.1, .3, .9))
    lean = math.pi / 2 + rng.normal(0, LEAN)
    mesh = pb.mesh_points(shape, None, across=.012, along=.04, angle=lean, rings=(.005, .015, .03))
    add(shape, [-.01, 0, 0], [depth + .02, 0, 0], TIMBER, [*rng.uniform(0, 1, 2), lean],
        timber_terms(rng, [0., 1 / width, .5], [0., 1.]), kind='rail', mesh=mesh)

    spec = dict(tool='scripts/content/inside_bake.py', seed=seed, period=list(PERIOD), courses=COURSES,
                camera=dict(tilt=CAMERA_TILT), sky=dict(pitch=0., ground=.08), ao=dict(distance=.15, crevice=.025),
                surfaces=SURFACES, rafters=[float(u) for u in RAFTERS], pieces=pieces)
    return spec, points


# Weathering (compose). The original: pale grey-brown boards over almost black gaps, lighter hewn
# rafters, a darker rail, soot thickest towards the ridge and along the timbers, a few dark stains
# where water came through. Dry inside: no moss, no shine. Colours are linear RGB.
DESATURATE = .5
TONE = .12              # boards differ in tone (log-normal spread)
DUSTY = (.2, 1.15)      # share of paler, dusty boards and how much paler
BACK_TONE = .35         # the underside of the shingles in the gaps
TIMBER_TONE = 1.0       # hewn rafters read a little lighter than the boards
RAIL_TONE = .9
SOOT = (.45, .35)       # darkening towards the ridge (top of the square), and along the timbers
SOOT_REACH = .25        # metres from a timber where its soot fades out
DUST = (.25, .25)       # a pale dust film on parts of the boards: share, strength
STAINS = ((.285, .33, .55), (.42, .74, .45))  # water stains at the original's dark blots: u, v, size m
STAIN = .8              # their darkness
GAP_DARK = .65          # soot and dust packed in the gaps and joints
ROUGH = (.86, .95)      # dry wood indoors: matte
OUT_SIZE = (2048, 1024)


def compose(passes: dict, spec: dict, seed: int = 0) -> dict:
    """Albedo (linear RGB) and the masks it was made with, at the render size."""
    rng = np.random.default_rng(seed)
    piece = passes['piece']
    rows, cols = piece.shape
    period = spec['period']
    px, py = period
    table = spec['pieces']
    count = len(table)
    kinds = np.array([p['kind'] for p in table] + ['none'])
    kind = kinds[piece]
    board = kind == 'board'
    timber = np.isin(kind, ['rafter', 'rail'])

    colour = passes['colour']
    grey = colour.mean(axis=-1, keepdims=True)
    out = grey + (colour - grey) * (1 - DESATURATE)
    tone = np.exp(rng.normal(0, TONE, count)) * np.where(rng.random(count) < DUSTY[0], DUSTY[1], 1.)
    tone *= np.select([kinds[:-1] == 'back', kinds[:-1] == 'rafter', kinds[:-1] == 'rail'],
                      [BACK_TONE, TIMBER_TONE, RAIL_TONE], 1.)
    out *= np.append(tone, 1.)[piece][..., None]
    out *= (1 + .08 * pb.noise_metres(rng, (rows, cols), period, .15))[..., None]

    # light of the room, occlusion of the gaps
    sky = passes['sky'] / np.percentile(passes['sky'][board], 95)
    out *= np.clip(sky, 0, 1.2)[..., None]

    # soot: thickest towards the ridge and along the timbers, in clouds
    top = np.clip((passes['y'] % py) / py, 0, 1)                       # 0 at the bottom rail, 1 at the top
    near = np.zeros((rows, cols))
    for u in spec['rafters']:
        dx = np.abs(((passes['x'] - u * px + px / 2) % px) - px / 2)
        near = np.maximum(near, np.clip(1 - (dx - .21) / SOOT_REACH, 0, 1))
    dy = np.minimum(passes['y'] % py, py - passes['y'] % py)
    near = np.maximum(near, np.clip(1 - (dy - .26) / SOOT_REACH, 0, 1))
    clouds = np.clip(.6 + .5 * pb.noise_metres(rng, (rows, cols), period, .6), 0, 1)
    edges = np.clip((1 - pb.smooth(passes['ao'], 3)) / .3, 0, 1) * timber       # soot settles along a timber's edges
    soot = np.clip(SOOT[0] * top ** 1.5 * clouds + SOOT[1] * near * ~timber + .5 * edges, 0, 1)
    out *= (1 - soot)[..., None]

    # a pale dust film on parts of the boards
    field = pb.noise_metres(rng, (rows, cols), period, .3) + .5 * pb.noise_metres(rng, (rows, cols), period, .05)
    dust = np.clip((field - np.quantile(field, 1 - DUST[0])) * 2, 0, 1) * board * DUST[1]
    out = out * (1 - dust[..., None]) + (out.mean(axis=-1, keepdims=True) * 1.8 + .02) * dust[..., None]

    # water stains where it came through, running down the boards
    x = passes['x'] % px
    y = passes['y'] % py
    stain = np.zeros((rows, cols))
    for u, v, size in STAINS:
        cx, cy = u * px, py * (1 - v)
        ddx = ((x - cx + px / 2) % px) - px / 2
        ddy = ((y - cy + py / 2) % py) - py / 2
        blob = np.exp(-(ddx / size) ** 2 - (np.maximum(ddy, 0) / size) ** 2 - (np.minimum(ddy, 0) / (2.5 * size)) ** 2)
        stain = np.maximum(stain, blob)
    stain *= np.clip(.5 + .6 * pb.noise_metres(rng, (rows, cols), period, .08), 0, 1)
    out *= (1 - STAIN * stain)[..., None]

    # soot and dust packed in the gaps and joints
    gap = np.clip((1 - pb.smooth(passes['crevice'])) / .45, 0, 1)
    out *= (1 - GAP_DARK * gap)[..., None]

    rough = np.append(rng.uniform(*ROUGH, count), 1.)[piece]
    normal = passes['normal'] / (np.linalg.norm(passes['normal'], axis=-1, keepdims=True) + 1e-9)
    return dict(albedo=np.clip(out, 0, 1), soot=soot, dust=dust, stain=stain, roughness=rough, normal=normal)


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
    p.add_argument('--street', action='store_true', help='perspective views instead of the passes')
    p.add_argument('--close', help='x,y in metres of the close view')
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
                      {k: result[k] for k in ('soot', 'dust', 'stain', 'roughness')})
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
